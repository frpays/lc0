/*
 This file is part of Leela Zero.
 Copyright (C) 2017 Gian-Carlo Pascutto

 Leela Zero is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Leela Zero is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <array>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>

#include "neural/opencl/OpenCL.h"
#include "neural/opencl/OpenCLParams.h"
#include "neural/opencl/OpenCLTuner.h"

#include "utils/logging.h"
#include "utils/random.h"

static const auto kTunerFilename = std::string("leelaz_opencl_tuning");

// Maximum error from reference.
static constexpr auto kMaxError = 1e-4;
static constexpr auto kExpoDistroLambda = 0.5;
static constexpr auto kMinCutoffRatio = 1.05;
static constexpr int kMaxTuneIters = 10;

static const std::vector<Configurations> kParamsSize1 = {
    // Former smaller set
    {"MWG", {16, 32, 64}},  {"NWG", {16, 32, 64}},  {"KWG", {32}},
    {"MDIMC", {8, 16, 32}}, {"NDIMC", {8, 16, 32}}, {"MDIMA", {8, 16, 32}},
    {"NDIMB", {8, 16, 32}}, {"KWI", {2}},           {"VWM", {1, 2, 4}},
    {"VWN", {1, 2, 4}},     {"STRM", {0}},          {"STRN", {0}},
    {"SA", {0, 1}},         {"SB", {0, 1}},
};

static const std::vector<Configurations> kParamsSize2 = {
    // Former smaller set + KWG 32 and KWI 8
    {"MWG", {16, 32, 64}},  {"NWG", {16, 32, 64}},  {"KWG", {16, 32}},
    {"MDIMC", {8, 16, 32}}, {"NDIMC", {8, 16, 32}}, {"MDIMA", {8, 16, 32}},
    {"NDIMB", {8, 16, 32}}, {"KWI", {2, 8}},        {"VWM", {1, 2, 4}},
    {"VWN", {1, 2, 4, 8}},  {"STRM", {0}},          {"STRN", {0}},
    {"SA", {0, 1}},         {"SB", {0, 1}},
};

static const std::vector<Configurations> kParamsSize3 = {
    // Former smaller set + KWG 32, KWI 8, VWN 8
    {"MWG", {16, 32, 64}},  {"NWG", {16, 32, 64}},  {"KWG", {16, 32}},
    {"MDIMC", {8, 16, 32}}, {"NDIMC", {8, 16, 32}}, {"MDIMA", {8, 16, 32}},
    {"NDIMB", {8, 16, 32}}, {"KWI", {2, 8}},        {"VWM", {1, 2, 4, 8}},
    {"VWN", {1, 2, 4, 8}},  {"STRM", {0, 1}},       {"STRN", {0, 1}},
    {"SA", {0, 1}},         {"SB", {0, 1}},
};

static const std::vector<Configurations> kParamsSize4 = {
    // Former larger set
    {"MWG", {16, 32, 64}},  {"NWG", {16, 32, 64}},  {"KWG", {16, 32}},
    {"MDIMC", {8, 16, 32}}, {"NDIMC", {8, 16, 32}}, {"MDIMA", {8, 16, 32}},
    {"NDIMB", {8, 16, 32}}, {"KWI", {2, 8}},        {"VWM", {1, 2, 4, 8}},
    {"VWN", {1, 2, 4, 8}},  {"STRM", {0, 1}},       {"STRN", {0, 1}},
    {"SA", {0, 1}},         {"SB", {0, 1}},
};

// Stochastic search constants.
static constexpr auto kWalkLength = 100;
static constexpr auto kWalkMinChanges = 6;

static void sgemmBatched_ref(const std::vector<float>& a,
                             const std::vector<float>& b, std::vector<float>& c,
                             const int m, const int n, const int k,
                             const int batch_size) {
  for (auto batch = 0; batch < batch_size; batch++) {
    auto offset_u = batch * m * k;
    auto offset_v = batch * n * k;
    auto offset_m = batch * m * n;

    // Calculates C = transpose(tranpose(A) * B) in row major, or
    // C = A * transpose(B) in column major.
    for (auto i = 0; i < m; i++) {
      for (auto j = 0; j < n; j++) {
        auto acc = 0.0f;
        for (auto l = 0; l < k; l++) {
          acc += a[l * m + i + offset_u] * b[l * n + j + offset_v];
        }
        c[j * m + i + offset_m] = acc;
      }
    }
  }
}

static bool IsMultiple(const size_t a, const size_t b) { return (a % b == 0); }

bool OpenCLTuner::valid_config_sgemm(TuneParameters p, bool exhaustive) {
  if (!IsMultiple(p["MWG"], p["MDIMC"] * p["VWM"])) {
    return false;
  }
  if (!IsMultiple(p["NWG"], p["NDIMC"] * p["VWN"])) {
    return false;
  }
  if (!IsMultiple(p["MWG"], p["MDIMA"] * p["VWM"])) {
    return false;
  }
  if (!IsMultiple(p["NWG"], p["NDIMB"] * p["VWN"])) {
    return false;
  }
  if (!IsMultiple(p["KWG"], p["MDIMC"] * p["NDIMC"] / p["MDIMA"])) {
    return false;
  }
  if (!IsMultiple(p["KWG"], p["MDIMC"] * p["NDIMC"] / p["NDIMB"])) {
    return false;
  }
  // Extra restrictions for a fast tuning run
  if (!exhaustive) {
    if (p["MDIMC"] != p["MDIMA"]) {
      return false;
    }
    if (p["NDIMC"] != p["NDIMB"]) {
      return false;
    }
    if (p["SA"] != p["SB"]) {
      return false;
    }
  }
  return true;
}

TuneParameters OpenCLTuner::get_parameters_by_int(
    const std::vector<Configurations>& opts, const int n) {
  TuneParameters param;
  std::vector<size_t> choices(opts.size());

  auto cfgs = 1;
  for (auto c = size_t{0}; c < opts.size(); c++) {
    choices[c] = opts[c].second.size();
    cfgs *= choices[c];
  }
  auto j = n;

  for (auto c = size_t{0}; c < opts.size(); c++) {
    auto o = opts[c];
    auto s = o.first;
    auto v = o.second[j % choices[c]];
    j /= choices[c];
    param[s] = v;
  }

  return param;
}

std::string OpenCLTuner::parameters_to_defines(const TuneParameters& p) {
  std::string s;
  for (auto const& x : p) {
    s += " -D" + x.first + "=" + std::to_string(x.second);
  }
  return s;
}

std::string OpenCLTuner::parameters_to_string(const TuneParameters& p) {
  std::string s;
  for (auto const& x : p) {
    s += x.first + "=" + std::to_string(x.second) + " ";
  }
  if (s.size() > 0) {
    s.resize(s.size() - 1);
  }
  return s;
}

static size_t next_power_of_two(const size_t x) {
  return 2 << (size_t)(std::ceil(std::log2(x)) - 1);
}

static void sgemm_generate_data(std::vector<float>& x, const int m, const int n,
                                const int batch_size, const int m_ceil,
                                const int n_ceil) {
  for (auto batch = 0; batch < batch_size; batch++) {
    for (auto i = 0; i < n_ceil; i++) {
      if (i < n) {
        for (auto j = 0; j < m; j++) {
          x[batch * n_ceil * m_ceil + i * m_ceil + j] =
              0.01f * (((i ^ j) + batch - 50) % 100);
        }
        for (auto j = m; j < m_ceil; j++) {
          x[batch * n_ceil * m_ceil + i * m_ceil + j] = 0.0f;
        }
      } else {
        for (auto j = 0; j < m_ceil; j++) {
          x[batch * n_ceil * m_ceil + i * m_ceil + j] = 0.0f;
        }
      }
    }
  }
}

static float compare_ref(std::vector<float>& x, std::vector<float>& ref,
                         const int m, const int n, const int batch_size,
                         const int m_ceil, const int n_ceil) {
  auto sum = 0.0f;
  for (auto batch = 0; batch < batch_size; batch++) {
    for (auto j = 0; j < m; j++) {
      for (auto i = 0; i < n; i++) {
        auto r = ref[batch * n * m + j * n + i];
        auto y = x[batch * n_ceil * m_ceil + j * n_ceil + i];

        sum += (r - y) * (r - y);
      }
    }
  }
  return sum / (m * n * batch_size);
}

std::string OpenCLTuner::tune_sgemm(const int m, const int n, const int k,
                                    const int batch_size) {
  CERR << "Started OpenCL SGEMM tuner with batch size " << n / WINOGRAD_P
       << ".";

  std::string defines;
  switch (m_params.tune_algo) {
    case kTuneAlgoSystematic:
      defines = tune_sgemm_systematic(m, n, k, batch_size);
      break;
    case kTuneAlgoStochastic:
      defines = tune_sgemm_stochastic(m, n, k, batch_size);
      break;
  }
  return defines;
}

std::string OpenCLTuner::tune_sgemm_systematic(const int m, const int n,
                                               const int k,
                                               const int batch_size) {
  bool large = true;
  auto opts = std::vector<Configurations>();
  switch (m_params.tune_effort) {
    case kTuneEffortFaster:
      opts = kParamsSize1;
      large = false;
      break;

    case kTuneEffortNormal:
      opts = kParamsSize2;
      large = false;
      break;

    case kTuneEffortSlower:
      opts = kParamsSize3;
      large = false;
      break;

    case kTuneEffortSlowest:
      opts = kParamsSize4;
      large = true;
      break;
  }

  // This needs to be at minimum the maximum (MNK/WG) values above.
  auto m_max = std::max(64, m);
  auto n_max = std::max(64, n);
  auto k_max = std::max(32, k);

  auto at_size =
      batch_size * next_power_of_two(k_max) * next_power_of_two(m_max);
  auto b_size =
      batch_size * next_power_of_two(k_max) * next_power_of_two(n_max);
  auto c_size =
      batch_size * next_power_of_two(m_max) * next_power_of_two(n_max);

  auto total_flops = batch_size * 2.0 * m * n * k;

  auto at = std::vector<float>(at_size);
  auto b = std::vector<float>(b_size);
  auto c = std::vector<float>(c_size);
  auto c_ref = std::vector<float>(c_size);

  sgemm_generate_data(at, k, m, batch_size, k, m);
  sgemm_generate_data(b, n, k, batch_size, n, k);

  sgemmBatched_ref(at, b, c_ref, m, n, k, batch_size);

  auto aBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * at_size, nullptr, nullptr);
  auto bBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * b_size, nullptr, nullptr);
  auto cBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * c_size, nullptr, nullptr);

  auto valid_params = std::vector<int>{};
  auto cfgs = 1;
  for (auto c = size_t{0}; c < opts.size(); c++) {
    cfgs *= opts[c].second.size();
  }

  for (auto i = 0; i < cfgs; i++) {
    TuneParameters param = get_parameters_by_int(opts, i);
    if (valid_config_sgemm(param, large)) {
      valid_params.emplace_back(i);
    }
  }

  CERR << "Will try " << valid_params.size() << " valid configurations.";

  std::string best_params, best_params_str;

  auto queue = cl::CommandQueue(m_context, m_device, CL_QUEUE_PROFILING_ENABLE);
  auto event = cl::Event();
  auto program = cl::Program(m_context, sourceCode_sgemm);

  auto m_ceil_prev = 0;
  auto n_ceil_prev = 0;
  auto k_ceil_prev = 0;
  auto param_counter = size_t{0};
  double best_time_us = 0;
  auto last_log_counter = size_t{0};
  shuffle(valid_params.begin(), valid_params.end(),
          std::default_random_engine(0));
  for (const auto& i : valid_params) {
    param_counter++;

    auto p = get_parameters_by_int(opts, i);
    auto defines = parameters_to_defines(p);

    try {
      auto args = m_opencl.m_cl_args + " " + defines;
      program.build(args.c_str());
    } catch (const cl::Error&) {
      // Failed to compile, get next parameter.
      continue;
    }

    auto sgemm_kernel = cl::Kernel(program, "XgemmBatched");

    auto m_ceil = (int)ceilMultiple(ceilMultiple(m, p["MWG"]), p["VWM"]);
    auto n_ceil = (int)ceilMultiple(ceilMultiple(n, p["NWG"]), p["VWN"]);
    auto k_ceil = (int)ceilMultiple(ceilMultiple(k, p["KWG"]), p["VWM"]);

    if (m_ceil != m_ceil_prev || n_ceil != n_ceil_prev ||
        k_ceil != k_ceil_prev) {
      m_ceil_prev = m_ceil;
      n_ceil_prev = n_ceil;
      k_ceil_prev = k_ceil;

      sgemm_generate_data(at, k, m, batch_size, k_ceil, m_ceil);
      sgemm_generate_data(b, n, k, batch_size, n_ceil, k_ceil);

      queue.enqueueWriteBuffer(aBuffer, CL_FALSE, 0, at_size * sizeof(float),
                               at.data());
      queue.enqueueWriteBuffer(bBuffer, CL_FALSE, 0, b_size * sizeof(float),
                               b.data());
      queue.finish();
    }

    sgemm_kernel.setArg(0, m_ceil);
    sgemm_kernel.setArg(1, n_ceil);
    sgemm_kernel.setArg(2, k_ceil);
    sgemm_kernel.setArg(3, aBuffer);
    sgemm_kernel.setArg(4, bBuffer);
    sgemm_kernel.setArg(5, cBuffer);

    cl::NDRange local_sgemm = {p["MDIMC"], p["NDIMC"], 1};

    cl::NDRange size_sgemm = {(m_ceil * p["MDIMC"]) / p["MWG"],
                              (n_ceil * p["NDIMC"]) / p["NWG"],
                              (size_t)batch_size};
    float max_error = 0.0;
    cl_ulong min_elapsed = 0;

    for (auto r = size_t{0}; r < kMaxTuneIters; r++) {
      try {
        queue.enqueueNDRangeKernel(sgemm_kernel, cl::NullRange, size_sgemm,
                                   local_sgemm, nullptr, &event);
        queue.finish();
        event.wait();

        queue.enqueueReadBuffer(cBuffer, CL_FALSE, 0, c_size * sizeof(float),
                                c.data());
        queue.finish();

        auto this_error =
            compare_ref(c, c_ref, n, m, batch_size, n_ceil, m_ceil);
        max_error = std::max(max_error, this_error);

        auto elapsed = event.getProfilingInfo<CL_PROFILING_COMMAND_END>() -
                       event.getProfilingInfo<CL_PROFILING_COMMAND_START>();

        if (r == 0 || elapsed < min_elapsed) {
          min_elapsed = elapsed;
          double cutoffRatio =
              std::max(kMinCutoffRatio, 1 + kExpoDistroLambda / (r + 1));
          double time_us = 1e-3 * elapsed;
          if (best_time_us > 0 && time_us > best_time_us * cutoffRatio) {
            break;
          }
        }

      } catch (const cl::Error&) {
        // Failed to enqueue kernel. Set error to max.
        max_error = kMaxError;
        break;
      }
    }

    bool log_progress = ++last_log_counter >= 500 && param_counter % 1000 == 0;
    double time_us = 1e-3 * min_elapsed;
    if (max_error < kMaxError &&
        (best_time_us == 0 || time_us < best_time_us)) {
      best_time_us = time_us;
      best_params = defines;
      best_params_str = parameters_to_string(p);
      log_progress = true;
    }

    if (log_progress) {
      auto kernel_gflops = 1e-3 * total_flops / best_time_us;
      CERR << std::fixed << std::setprecision(1) << "(" << param_counter << "/"
           << valid_params.size() << ") " << best_params_str << " "
           << best_time_us << " us (" << kernel_gflops << " GFLOPS)";
      last_log_counter = 0;
    }
  }
  if (best_time_us == 0) {
    CERR << "Failed to find a working configuration." << std::endl
         << "Check your OpenCL drivers.";
    throw std::runtime_error("Tuner failed to find working configuration.");
  }
  return best_params;
}

std::string OpenCLTuner::tune_sgemm_stochastic(const int m, const int n,
                                               const int k,
                                               const int batch_size) {
  auto seeds = 1;
  auto walkLength = kWalkLength;

  switch (m_params.tune_effort) {
    case kTuneEffortFaster:
      seeds = 1;
      break;

    case kTuneEffortNormal:
      seeds = 5;
      break;

    case kTuneEffortSlower:
      seeds = 25;
      break;

    case kTuneEffortSlowest:
      seeds = 250;
      break;
  }

  // This needs to be at minimum the maximum (MNK/WG) values above.
  auto m_max = std::max(64, m);
  auto n_max = std::max(64, n);
  auto k_max = std::max(32, k);

  auto at_size =
      batch_size * next_power_of_two(k_max) * next_power_of_two(m_max);
  auto b_size =
      batch_size * next_power_of_two(k_max) * next_power_of_two(n_max);
  auto c_size =
      batch_size * next_power_of_two(m_max) * next_power_of_two(n_max);

  auto total_flops = batch_size * 2.0 * m * n * k;

  auto at = std::vector<float>(at_size);
  auto b = std::vector<float>(b_size);
  auto c = std::vector<float>(c_size);
  auto c_ref = std::vector<float>(c_size);

  sgemm_generate_data(at, k, m, batch_size, k, m);
  sgemm_generate_data(b, n, k, batch_size, n, k);

  sgemmBatched_ref(at, b, c_ref, m, n, k, batch_size);

  auto aBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * at_size, nullptr, nullptr);
  auto bBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * b_size, nullptr, nullptr);
  auto cBuffer = cl::Buffer(m_context, CL_MEM_READ_WRITE,
                            sizeof(float) * c_size, nullptr, nullptr);

  size_t cfgs = 1;
  const auto& opts = kParamsSize4;
  for (auto c = size_t{0}; c < opts.size(); c++) {
    cfgs *= opts[c].second.size();
  }
  CERR << "Stochastic search in " << cfgs << " configurations";

  std::string best_params;
  std::string best_string;
  double best_time_us = 0;
  double best_gflops = 0;

  auto queue = cl::CommandQueue(m_context, m_device, CL_QUEUE_PROFILING_ENABLE);
  auto event = cl::Event();
  auto program = cl::Program(m_context, sourceCode_sgemm);

  auto m_ceil_prev = 0;
  auto n_ceil_prev = 0;
  auto k_ceil_prev = 0;

  for (int seed = 0; seed < seeds; seed++) {
    int index = lczero::Random::Get().GetInt(0, cfgs);

    TuneParameters p = get_parameters_by_int(opts, index);

    std::string walk_best_params;
    std::string walk_best_string;
    double walk_best_time_us = 0;
    double walk_best_gflops = 0;

    for (int steps = 0; steps < walkLength; steps++) {
      TuneParameters p_old = p;

      int changes = 0;
      while (true) {
        const auto param_counts = opts.size();
        auto p0 = lczero::Random::Get().GetInt(0, param_counts - 1);

        auto b0 = 2 * lczero::Random::Get().GetInt(0, 2) - 1;

        auto name = opts[p0].first;
        auto value = p[name];

        auto values = opts[p0].second;
        int value_count = static_cast<int>(values.size());
        int value_index = -1;

        for (int k = 0; k < value_count; k++) {
          if (value == values[k]) {
            value_index = k;
            break;
          }
        }

        if (value_index < 0) continue;

        value_index += b0;
        if (value_index < 0) continue;

        if (value_index >= value_count) continue;

        p[name] = values[value_index];
        changes++;

        bool valid = valid_config_sgemm(p, true);
        if (!valid) continue;

        if (changes >= kWalkMinChanges) break;
      }

      auto defines = parameters_to_defines(p);

      try {
        auto args = m_opencl.m_cl_args + " " + defines;
        program.build(args.c_str());
      } catch (const cl::Error&) {
        continue;
      }

      auto sgemm_kernel = cl::Kernel(program, "XgemmBatched");

      auto m_ceil = (int)ceilMultiple(ceilMultiple(m, p["MWG"]), p["VWM"]);
      auto n_ceil = (int)ceilMultiple(ceilMultiple(n, p["NWG"]), p["VWN"]);
      auto k_ceil = (int)ceilMultiple(ceilMultiple(k, p["KWG"]), p["VWM"]);

      if (m_ceil != m_ceil_prev || n_ceil != n_ceil_prev ||
          k_ceil != k_ceil_prev) {
        m_ceil_prev = m_ceil;
        n_ceil_prev = n_ceil;
        k_ceil_prev = k_ceil;

        sgemm_generate_data(at, k, m, batch_size, k_ceil, m_ceil);
        sgemm_generate_data(b, n, k, batch_size, n_ceil, k_ceil);

        queue.enqueueWriteBuffer(aBuffer, CL_FALSE, 0, at_size * sizeof(float),
                                 at.data());
        queue.enqueueWriteBuffer(bBuffer, CL_FALSE, 0, b_size * sizeof(float),
                                 b.data());
        queue.finish();
      }

      sgemm_kernel.setArg(0, m_ceil);
      sgemm_kernel.setArg(1, n_ceil);
      sgemm_kernel.setArg(2, k_ceil);
      sgemm_kernel.setArg(3, aBuffer);
      sgemm_kernel.setArg(4, bBuffer);
      sgemm_kernel.setArg(5, cBuffer);

      cl::NDRange local_sgemm = {p["MDIMC"], p["NDIMC"], 1};

      cl::NDRange size_sgemm = {(m_ceil * p["MDIMC"]) / p["MWG"],
                                (n_ceil * p["NDIMC"]) / p["NWG"],
                                (size_t)batch_size};

      float max_error = 0.0;
      cl_ulong min_elapsed = 0;

      for (auto r = size_t{0}; r < kMaxTuneIters; r++) {
        try {
          queue.enqueueNDRangeKernel(sgemm_kernel, cl::NullRange, size_sgemm,
                                     local_sgemm, nullptr, &event);
          queue.finish();
          event.wait();

          queue.enqueueReadBuffer(cBuffer, CL_FALSE, 0, c_size * sizeof(float),
                                  c.data());
          queue.finish();

          auto this_error =
              compare_ref(c, c_ref, n, m, batch_size, n_ceil, m_ceil);
          max_error = std::max(max_error, this_error);

          auto elapsed = event.getProfilingInfo<CL_PROFILING_COMMAND_END>() -
                         event.getProfilingInfo<CL_PROFILING_COMMAND_START>();

          if (r == 0 || elapsed < min_elapsed) {
            min_elapsed = elapsed;
//            double cutoffRatio =
//                std::max(kMinCutoffRatio, 1 + kExpoDistroLambda / (r + 1));
//            double time_us = 1e-3 * elapsed;
//            if (walk_best_time_us > 0 &&
//                time_us > walk_best_time_us * cutoffRatio) {
//              break;
//            }
          }

        } catch (const cl::Error&) {
          // Failed to enqueue kernel. Set error to max.
          max_error = kMaxError;
          break;
        }
      }

      if (max_error >= kMaxError) {
        p = p_old;
        continue;
      }

      double time_us = 1e-3 * min_elapsed;
      if (walk_best_time_us==0 || time_us < walk_best_time_us) {
        walk_best_time_us = time_us;
        walk_best_params = defines;
        // Timing is in nanoseconds (10^-9), Giga = 10^9, so this works out.
        walk_best_gflops = total_flops / min_elapsed;
        walk_best_string = parameters_to_string(p);

        if (best_time_us == 0 || walk_best_time_us < best_time_us) {
          best_time_us = walk_best_time_us;
          best_params = walk_best_params;
          best_gflops = walk_best_gflops;
          best_string = walk_best_string;
        }

      } else
        p = p_old;

    }  // march

    CERR << std::fixed << std::setprecision(1) << "(" << (seed+1) << "/" << seeds
         << ") " << best_string << " " << best_time_us << " us (" << best_gflops
         << " GFLOPS)";

  }  // seed

  if (best_time_us == 0) {
    CERR << "Failed to find a working configuration." << std::endl
         << "Check your OpenCL drivers.";
    throw std::runtime_error("Tuner failed to find working configuration.");
  }

  return best_params;
}

void OpenCLTuner::store_sgemm_tuners(const int m, const int n, const int k,
                                     const int batch_size, std::string tuners) {
  auto file_contents = std::vector<std::string>();
  {
    // Read the previous contents to string.
    auto file = std::ifstream{kTunerFilename};
    if (file.good()) {
      auto line = std::string{};
      while (std::getline(file, line)) {
        file_contents.emplace_back(line);
      }
    }
  }
  auto file = std::ofstream{kTunerFilename};

  auto device_name = m_opencl.get_device_name();
  auto tuning_params = std::stringstream{};
  tuning_params << m << ";" << n << ";" << k << ";" << batch_size;

  auto tuning_line_prefix = std::to_string(TUNER_VERSION) + ";XgemmBatched;" +
                            tuning_params.str() + ";";
  auto tuning_line = tuning_line_prefix + tuners + ";" + device_name;

  // Write back previous data as long as it's not the device and
  // tuning we just tuned.
  for (const auto& line : file_contents) {
    if (line.find(tuning_line_prefix) == std::string::npos ||
        line.find(device_name) == std::string::npos) {
      file << line << std::endl;
    }
  }

  // Write new tuning.
  file << tuning_line << std::endl;

  if (file.fail()) {
    CERR << "Could not save the tuning result.";
    CERR << "Do I have write permissions on " << kTunerFilename << "?";
  }
}

std::string OpenCLTuner::sgemm_tuners_from_line(std::string line, const int m,
                                                const int n, const int k,
                                                const int batch_size) {
  auto s = std::vector<std::string>{};
  auto ss = std::stringstream{line};
  auto item = std::string{};

  while (std::getline(ss, item, ';')) {
    s.emplace_back(item);
  }

  if (s.size() != 8) {
    return "";
  }

  if (s[0] != std::to_string(TUNER_VERSION)) {
    return "";
  }

  if (s[1] != "XgemmBatched") {
    return "";
  }

  if (s[2] != std::to_string(m)) {
    return "";
  }

  if (s[3] != std::to_string(n)) {
    return "";
  }

  if (s[4] != std::to_string(k)) {
    return "";
  }

  if (s[5] != std::to_string(batch_size)) {
    return "";
  }

  if (s[7] != m_opencl.get_device_name()) {
    return "";
  }

  return s[6];
}

std::string OpenCLTuner::load_sgemm_tuners(const int m, const int n,
                                           const int k, const int batch_size) {
  if (!m_params.force_tune) {
    auto file = std::ifstream{kTunerFilename};
    if (file.good()) {
      auto line = std::string{};
      while (std::getline(file, line)) {
        auto tuners = sgemm_tuners_from_line(line, m, n, k, batch_size);
        if (tuners.size() != 0) {
          // batch_size argument is the number of batched sgemm calls, which
          // equals the number of elements in one tile.
          // Convolution batch size affects the "n" dimension of
          // the matrix multiplication (n = WINOGRAD_P * batch_size).
          CERR << "Loaded existing SGEMM tuning for batch size "
               << n / WINOGRAD_P << ".";
          return tuners;
        }
      }
    }
  }

  auto tuners = tune_sgemm(m, n, k, batch_size);
  store_sgemm_tuners(m, n, k, batch_size, tuners);

  // Exit immediately after tuning. Some NVIDIA drivers are buggy,
  // and will fail to compile the rest of the kernels after a tuning,
  // run. See #729.
  if (m_params.tune_only) {
    exit(EXIT_SUCCESS);
  }
  return tuners;
}

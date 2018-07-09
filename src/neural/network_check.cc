/*
 This file is part of Leela Chess Zero.
 Copyright (C) 2018 The LCZero Authors

 Leela Chess is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Leela Chess is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "neural/network.h"
#include "neural/factory.h"
#include "utils/histogram.h"
#include "utils/random.h"

#include <cmath>

namespace lczero {

namespace {

class CheckNetwork;

enum CheckMode {

  kCheckOnly,
  kErrorDisplay,
  kHistogram,
};

struct CheckParams {
  CheckMode mode;
  double absolute_tolerance;
  double relative_tolerance;
};

class CheckComputation : public NetworkComputation {
 public:
  CheckComputation(const CheckParams& params,
                   std::unique_ptr<NetworkComputation> refComp,
                   std::unique_ptr<NetworkComputation> checkComp)
      : params_(params),
        refComp_(std::move(refComp)),
        checkComp_(std::move(checkComp)) {}

  void AddInput(InputPlanes&& input) override {
    InputPlanes x = input;
    InputPlanes y = input;
    refComp_->AddInput(std::move(x));
    checkComp_->AddInput(std::move(y));
  }

  void ComputeBlocking() override {
    refComp_->ComputeBlocking();
    checkComp_->ComputeBlocking();
    switch (params_.mode) {
      case kCheckOnly:
        CheckOnly();
        break;
      case kErrorDisplay:
        DisplayError();
        break;
      case kHistogram:
        DisplayHistogram();
        break;
    }
  }

  int GetBatchSize() const override {
    return static_cast<int>(refComp_->GetBatchSize());
  }

  float GetQVal(int sample) const override { return refComp_->GetQVal(sample); }

  float GetPVal(int sample, int move_id) const override {
    return refComp_->GetPVal(sample, move_id);
  }

 private:
  static constexpr int kNumOutputPolicies = 1858;
  const CheckParams& params_;

  void CheckOnly() {
    bool valueAlmostEqual = true;
    int size = GetBatchSize();
    for (int i = 0; i < size && valueAlmostEqual; i++) {
      float v1 = refComp_->GetQVal(i);
      float v2 = checkComp_->GetQVal(i);
      valueAlmostEqual &= IsAlmostEqual(v1, v2);
    }

    bool policyAlmostEqual = true;
    for (int i = 0; i < size && policyAlmostEqual; i++) {
      for (int j = 0; j < kNumOutputPolicies; j++) {
        float v1 = refComp_->GetPVal(i, j);
        float v2 = checkComp_->GetPVal(i, j);
        policyAlmostEqual &= IsAlmostEqual(v1, v2);
      }
    }

    if (valueAlmostEqual && policyAlmostEqual) {
      fprintf(stderr, "Check passed for a batch of %d.\n", size);
      return;
    }

    if (!valueAlmostEqual && !policyAlmostEqual) {
      fprintf(stderr,
              "*** ERROR check failed for a batch of %d  both value and policy "
              "incorrect.\n",
              size);
      return;
    }

    if (!valueAlmostEqual) {
      fprintf(stderr,
              "*** ERROR check failed for a batch of %d value incorrect (but "
              "policy ok).\n",
              size);
      return;
    }

    fprintf(stderr,
            "*** ERROR check failed for a batch of %d policy incorrect (but "
            "value ok).",
            size);
  }

  bool IsAlmostEqual(double a, double b) {
    return std::abs(a - b) <= std::max(params_.relative_tolerance *
                                           std::max(std::abs(a), std::abs(b)),
                                       params_.absolute_tolerance);
  }

  void DisplayHistogram() {

    int size = GetBatchSize();
    for (int i = 0; i < size; i++) {
      
      Histogram histogram(-15, 1, 5);
      Histogram histogramA(-10, 6, 5);
      Histogram histogramB(-10, 6, 5);

      float qv1 = refComp_->GetQVal(i);
      float qv2 = checkComp_->GetQVal(i);
      histogram.Add(qv2 - qv1);
      histogramA.Add(qv1);
      histogramB.Add(qv2);

      for (int j = 0; j < kNumOutputPolicies; j++) {
        float pv1 = refComp_->GetPVal(i, j);
        float pv2 = checkComp_->GetPVal(i, j);
        histogram.Add(pv2 - pv1);
        histogramA.Add(pv1);
        histogramB.Add(pv2);
      }
      
      fprintf(stderr, "Absolute error histogram for batch %d:\n", i);
      histogram.Dump();
      
      fprintf(stderr, "histogram A for a batch of %d:\n", i);
      histogramA.Dump();
      
      fprintf(stderr, "histogram B for a batch of %d:\n", i);
      histogramB.Dump();
    }
 
  }

  // Compute maximum absolute/relative errors
  struct MaximumError {
    double max_absolute_error = 0;
    double max_relative_error = 0;

    void Add(double a, double b) {
      double absolute_error = GetAbsoluteError(a, b);
      if (absolute_error > max_absolute_error)
        max_absolute_error = absolute_error;
      double relative_error = GetRelativeError(a, b);
      if (relative_error > max_relative_error)
        max_relative_error = relative_error;
    }

    void Dump(const char* name) {
      fprintf(stderr, "%s: absolute: %.1e, relative: %.1e\n", name,
              max_absolute_error, max_relative_error);
    }

    static double GetRelativeError(double a, double b) {
      double max = std::max(std::abs(a), std::abs(b));
      return max == 0 ? 0 : std::abs(a - b) / max;
    }

    static double GetAbsoluteError(double a, double b) {
      return std::abs(a - b);
    }
  };

  void DisplayError() {
    MaximumError value_error;
    int size = GetBatchSize();
    for (int i = 0; i < size; i++) {
      float v1 = refComp_->GetQVal(i);
      float v2 = checkComp_->GetQVal(i);
      value_error.Add(v1, v2);
    }

    MaximumError policy_error;
    for (int i = 0; i < size; i++) {
      for (int j = 0; j < kNumOutputPolicies; j++) {
        float v1 = refComp_->GetPVal(i, j);
        float v2 = checkComp_->GetPVal(i, j);
        policy_error.Add(v1, v2);
      }
    }

    fprintf(stderr, "maximum error for a batch of %d:\n", size);
    value_error.Dump("  value");
    policy_error.Dump("  policy");
  }

  std::unique_ptr<NetworkComputation> refComp_;
  std::unique_ptr<NetworkComputation> checkComp_;
};

class CheckNetwork : public Network {
 public:
  static constexpr CheckMode kDefaultMode = kCheckOnly;
  static constexpr double kDefaultCheckFrequency = 0.2;
  static constexpr double kDefaultAbsoluteTolerance = 1e-5;
  static constexpr double kDefaultRelativeTolerance = 1e-4;

  CheckNetwork(const Weights& weights, const OptionsDict& options) {
    params_.mode = kDefaultMode;
    params_.absolute_tolerance = kDefaultAbsoluteTolerance;
    params_.relative_tolerance = kDefaultRelativeTolerance;
    checkFrequency_ = kDefaultCheckFrequency;

    OptionsDict dict1;
    std::string backendName1 = "opencl";
    OptionsDict& backend1_dict = dict1;

    OptionsDict dict2;
    std::string backendName2 = "blas";
    OptionsDict& backend2_dict = dict2;

    std::string mode = options.GetOrDefault<std::string>("mode", "check");
    if (mode == "check") {
      params_.mode = kCheckOnly;
    } else if (mode == "histo") {
      params_.mode = kHistogram;
    } else if (mode == "display") {
      params_.mode = kErrorDisplay;
    }

    params_.absolute_tolerance =
        options.GetOrDefault<float>("atol", kDefaultAbsoluteTolerance);
    params_.relative_tolerance =
        options.GetOrDefault<float>("rtol", kDefaultRelativeTolerance);

    const auto parents = options.ListSubdicts();
    if (parents.size() > 0) {
      backendName1 = parents[0];
      backend1_dict = options.GetSubdict(backendName1);
    }
    if (parents.size() > 1) {
      backendName2 = parents[1];
      backend2_dict = options.GetSubdict(backendName2);
    }
    if (parents.size() > 2) {
      fprintf(stderr, "Warning, cannot check more than two backends\n");
    }

    fprintf(stderr, "Working backend set to %s.\n", backendName1.c_str());
    fprintf(stderr, "Reference backend set to %s.\n", backendName2.c_str());

    workNet_ =
        NetworkFactory::Get()->Create(backendName1, weights, backend1_dict);
    checkNet_ =
        NetworkFactory::Get()->Create(backendName2, weights, backend2_dict);

    checkFrequency_ =
        options.GetOrDefault<float>("freq", kDefaultCheckFrequency);
    switch (params_.mode) {
      case kCheckOnly:
        fprintf(stderr,
                "Check mode: check only with relative tolerance  %.1e, "
                "absolute tolerance %.1e\n",
                params_.absolute_tolerance, params_.relative_tolerance);
        break;
      case kErrorDisplay:
        fprintf(stderr, "Check mode: error display\n");
        break;
      case kHistogram:
        fprintf(stderr, "Check mode: histogram\n");
        break;
    }
    fprintf(stderr, "Check rate: %.0f %%\n", 100.0 * checkFrequency_);
  }

  std::unique_ptr<NetworkComputation> NewComputation() override {
    double draw = Random::Get().GetDouble(1.0);
    bool check = draw < checkFrequency_;
    if (check) {
      std::unique_ptr<NetworkComputation> refComp = workNet_->NewComputation();
      std::unique_ptr<NetworkComputation> checkComp =
          checkNet_->NewComputation();
      return std::make_unique<CheckComputation>(params_, std::move(refComp),
                                                std::move(checkComp));
    }
    return workNet_->NewComputation();
  }

 private:
  CheckParams params_;
  double checkFrequency_;
  std::unique_ptr<Network> workNet_;
  std::unique_ptr<Network> checkNet_;
};

}  // namespace

REGISTER_NETWORK("check", CheckNetwork, -800)

}  // namespace lczero

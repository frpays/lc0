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

#include <algorithm>
#include <cmath>
#include <functional>

#include "engine.h"
#include "mcts/search.h"
#include "neural/factory.h"
#include "neural/loader.h"

namespace lczero {
namespace {
// TODO(mooskagh) Move threads parameter handling to search.
const int kDefaultThreads = 2;
const char* kThreadsOption = "Number of worker threads";
const char* kDebugLogStr = "Do debug logging into file";

// TODO(mooskagh) Move weights/backend/backend-opts parameter handling to
//                network factory.
const char* kWeightsStr = "Network weights file path";
const char* kNnBackendStr = "NN backend to use";
const char* kNnBackendOptionsStr = "NN backend parameters";
const char* kSlowMoverStr = "Scale thinking time";
const char* kMoveOverheadStr = "Move time overhead in milliseconds";
const char* kTimeCurvePeak = "Time weight curve peak ply";
const char* kTimeCurveRightWidth = "Time weight curve width right of peak";
const char* kTimeCurveLeftWidth = "Time weight curve width left of peak";

const char* kAutoDiscover = "<autodiscover>";

float ComputeMoveWeight(int ply, float peak, float left_width,
                        float right_width) {
  // Inflection points of the function are at ply = peak +/- width.
  // At these points the function is at 2/3 of its max value.
  const float width = ply > peak ? right_width : left_width;
  constexpr float width_scaler = 1.518651485f;  // 2 / log(2 + sqrt(3))
  return std::pow(std::cosh((ply - peak) / width / width_scaler), -2.0f);
}

}  // namespace

EngineController::EngineController(BestMoveInfo::Callback best_move_callback,
                                   ThinkingInfo::Callback info_callback,
                                   const OptionsDict& options)
    : options_(options),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback) {}

void EngineController::PopulateOptions(OptionsParser* options) {
  using namespace std::placeholders;

  options->Add<StringOption>(kWeightsStr, "weights", 'w') = kAutoDiscover;
  options->Add<IntOption>(kThreadsOption, 1, 128, "threads", 't') =
      kDefaultThreads;
  options->Add<IntOption>(
      "NNCache size", 0, 999999999, "nncache", '\0',
      std::bind(&EngineController::SetCacheSize, this, _1)) = 200000;

  const auto backends = NetworkFactory::Get()->GetBackendsList();
  options->Add<ChoiceOption>(kNnBackendStr, backends, "backend") =
      backends.empty() ? "<none>" : backends[0];
  options->Add<StringOption>(kNnBackendOptionsStr, "backend-opts");
  options->Add<FloatOption>(kSlowMoverStr, 0.0f, 100.0f, "slowmover") = 1.93f;
  options->Add<IntOption>(kMoveOverheadStr, 0, 10000, "move-overhead") = 100;
  options->Add<FloatOption>(kTimeCurvePeak, -1000.0f, 1000.0f,
                            "time-curve-peak") = 26.0f;
  options->Add<FloatOption>(kTimeCurveLeftWidth, 0.0f, 1000.0f,
                            "time-curve-left-width") = 67.0f;
  options->Add<FloatOption>(kTimeCurveRightWidth, 0.0f, 1000.0f,
                            "time-curve-right-width") = 76.0f;

  Search::PopulateUciParams(options);

  auto defaults = options->GetMutableDefaultsOptions();

  defaults->Set<int>(Search::kMiniBatchSizeStr, 256);    // Minibatch = 256
  defaults->Set<float>(Search::kFpuReductionStr, 0.9f);  // FPU reduction = 0.9
  defaults->Set<float>(Search::kCpuctStr, 3.4f);         // CPUCT = 3.4
  defaults->Set<float>(Search::kPolicySoftmaxTempStr, 2.2f);  // Psoftmax = 2.2
  defaults->Set<int>(Search::kAllowedNodeCollisionsStr, 32);  // Node collisions
}

SearchLimits EngineController::PopulateSearchLimits(int ply, bool is_black,
                                                    const GoParams& params) {
  SearchLimits limits;
  limits.visits = params.nodes;
  limits.time_ms = params.movetime;
  int64_t time = (is_black ? params.btime : params.wtime);
  limits.infinite = params.infinite;
  if (!params.searchmoves.empty()) {
    limits.searchmoves.reserve(params.searchmoves.size());
    for (const auto& move : params.searchmoves) {
      limits.searchmoves.emplace_back(move, is_black);
    }
  }
  if (params.infinite || time < 0) return limits;
  int increment = std::max(int64_t(0), is_black ? params.binc : params.winc);

  int movestogo = params.movestogo < 0 ? 50 : params.movestogo;
  // Fix non-standard uci command.
  if (movestogo == 0) movestogo = 1;

  // How to scale moves time.
  float slowmover = options_.Get<float>(kSlowMoverStr);
  int64_t move_overhead = options_.Get<int>(kMoveOverheadStr);
  float time_curve_peak = options_.Get<float>(kTimeCurvePeak);
  float time_curve_left_width = options_.Get<float>(kTimeCurveLeftWidth);
  float time_curve_right_width = options_.Get<float>(kTimeCurveRightWidth);

  // Total time till control including increments.
  auto total_moves_time =
      std::max(int64_t{0},
               time + increment * (movestogo - 1) - move_overhead * movestogo);

  constexpr int kSmartPruningToleranceMs = 200;
  float this_move_weight = ComputeMoveWeight(
      ply, time_curve_peak, time_curve_left_width, time_curve_right_width);
  float other_move_weights = 0.0f;
  for (int i = 1; i < movestogo; ++i)
    other_move_weights +=
        ComputeMoveWeight(ply + 2 * i, time_curve_peak, time_curve_left_width,
                          time_curve_right_width);
  // Compute the move time without slowmover.
  float this_move_time = total_moves_time * this_move_weight /
                         (this_move_weight + other_move_weights);

  // Only extend thinking time with slowmover if smart pruning can potentially
  // reduce it.
  if (slowmover < 1.0 ||
      this_move_time * slowmover > kSmartPruningToleranceMs) {
    this_move_time *= slowmover;
  }

  // Make sure we don't exceed current time limit with what we calculated.
  limits.time_ms = std::max(
      int64_t{0},
      std::min(static_cast<int64_t>(this_move_time), time - move_overhead));
  return limits;
}

void EngineController::UpdateNetwork() {
  SharedLock lock(busy_mutex_);
  std::string network_path = options_.Get<std::string>(kWeightsStr);
  std::string backend = options_.Get<std::string>(kNnBackendStr);
  std::string backend_options = options_.Get<std::string>(kNnBackendOptionsStr);

  if (network_path == network_path_ && backend == backend_ &&
      backend_options == backend_options_)
    return;

  network_path_ = network_path;
  backend_ = backend;
  backend_options_ = backend_options;

  std::string net_path = network_path;
  if (net_path == kAutoDiscover) {
    net_path = DiscoveryWeightsFile();
  }
  Weights weights = LoadWeightsFromFile(net_path);

  OptionsDict network_options =
      OptionsDict::FromString(backend_options, &options_);

  network_ = NetworkFactory::Get()->Create(backend, weights, network_options);
}

void EngineController::SetCacheSize(int size) { cache_.SetCapacity(size); }

void EngineController::EnsureReady() {
  UpdateNetwork();
  std::unique_lock<RpSharedMutex> lock(busy_mutex_);
}

void EngineController::NewGame() {
  SharedLock lock(busy_mutex_);
  cache_.Clear();
  search_.reset();
  tree_.reset();
  UpdateNetwork();
}

void EngineController::SetPosition(const std::string& fen,
                                   const std::vector<std::string>& moves_str) {
  SharedLock lock(busy_mutex_);
  search_.reset();

  if (!tree_) tree_ = std::make_unique<NodeTree>();

  std::vector<Move> moves;
  for (const auto& move : moves_str) moves.emplace_back(move);
  tree_->ResetToPosition(fen, moves);
  UpdateNetwork();
}

void EngineController::Go(const GoParams& params) {
  if (!tree_) {
    SetPosition(ChessBoard::kStartingFen, {});
  }

  auto limits = PopulateSearchLimits(tree_->GetPlyCount(),
                                     tree_->IsBlackToMove(), params);

  search_ =
      std::make_unique<Search>(*tree_, network_.get(), best_move_callback_,
                               info_callback_, limits, options_, &cache_);

  search_->StartThreads(options_.Get<int>(kThreadsOption));
}

void EngineController::Stop() {
  if (search_) {
    search_->Stop();
    search_->Wait();
  }
}

EngineLoop::EngineLoop()
    : engine_(std::bind(&UciLoop::SendBestMove, this, std::placeholders::_1),
              std::bind(&UciLoop::SendInfo, this, std::placeholders::_1),
              options_.GetOptionsDict()) {
  engine_.PopulateOptions(&options_);
  options_.Add<StringOption>(
      kDebugLogStr, "debuglog", 'l',
      [this](const std::string& filename) { SetLogFilename(filename); }) = "";
}

void EngineLoop::RunLoop() {
  if (!options_.ProcessAllFlags()) return;
  UciLoop::RunLoop();
}

void EngineLoop::CmdUci() {
  SendId();
  for (const auto& option : options_.ListOptionsUci()) {
    SendResponse(option);
  }
  SendResponse("uciok");
}

void EngineLoop::CmdIsReady() {
  engine_.EnsureReady();
  SendResponse("readyok");
}

void EngineLoop::CmdSetOption(const std::string& name, const std::string& value,
                              const std::string& context) {
  options_.SetOption(name, value, context);
  if (options_sent_) {
    options_.SendOption(name);
  }
}

void EngineLoop::EnsureOptionsSent() {
  if (!options_sent_) {
    options_.SendAllOptions();
    options_sent_ = true;
  }
}

void EngineLoop::CmdUciNewGame() {
  EnsureOptionsSent();
  engine_.NewGame();
}

void EngineLoop::CmdPosition(const std::string& position,
                             const std::vector<std::string>& moves) {
  EnsureOptionsSent();
  std::string fen = position;
  if (fen.empty()) fen = ChessBoard::kStartingFen;
  engine_.SetPosition(fen, moves);
}

void EngineLoop::CmdGo(const GoParams& params) {
  EnsureOptionsSent();
  engine_.Go(params);
}

void EngineLoop::CmdStop() { engine_.Stop(); }

}  // namespace lczero

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

#include "mcts/search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "mcts/node.h"
#include "neural/cache.h"
#include "neural/encoder.h"
#include "utils/random.h"

namespace lczero {

const char* Search::kMiniBatchSizeStr = "Minibatch size for NN inference";
const char* Search::kMaxPrefetchBatchStr = "Max prefetch nodes, per NN call";
const char* Search::kCpuctStr = "Cpuct MCTS option";
const char* Search::kTemperatureStr = "Initial temperature";
const char* Search::kTempDecayMovesStr = "Moves with temperature decay";
const char* Search::kNoiseStr = "Add Dirichlet noise at root node";
const char* Search::kVerboseStatsStr = "Display verbose move stats";
const char* Search::kSmartPruningStr = "Enable smart pruning";
const char* Search::kVirtualLossBugStr = "Virtual loss bug";
const char* Search::kFpuReductionStr = "First Play Urgency Reduction";
const char* Search::kCacheHistoryLengthStr =
    "Length of history to include in cache";
const char* Search::kPolicySoftmaxTempStr = "Policy softmax temperature";
const char* Search::kAllowedNodeCollisionsStr =
    "Allowed node collisions, per batch";
const char* Search::kBackPropagateBetaStr = "Backpropagation gamma";
const char* Search::kBackPropagateGammaStr = "Backpropagation beta";

namespace {
const int kSmartPruningToleranceNodes = 100;
const int kSmartPruningToleranceMs = 200;
// Maximum delay between outputting "uci info" when nothing interesting happens.
const int kUciInfoMinimumFrequencyMs = 5000;
}  // namespace

void Search::PopulateUciParams(OptionsParser* options) {
  // Here the "safe defaults" are listed.
  // Many of them are overridden with optimized defaults in engine.cc and
  // tournament.cc

  options->Add<IntOption>(kMiniBatchSizeStr, 1, 1024, "minibatch-size") = 1;
  options->Add<IntOption>(kMaxPrefetchBatchStr, 0, 1024, "max-prefetch") = 32;
  options->Add<FloatOption>(kCpuctStr, 0.0f, 100.0f, "cpuct") = 1.2f;
  options->Add<FloatOption>(kTemperatureStr, 0.0f, 100.0f, "temperature") =
      0.0f;
  options->Add<IntOption>(kTempDecayMovesStr, 0, 100, "tempdecay-moves") = 0;
  options->Add<BoolOption>(kNoiseStr, "noise", 'n') = false;
  options->Add<BoolOption>(kVerboseStatsStr, "verbose-move-stats") = false;
  options->Add<BoolOption>(kSmartPruningStr, "smart-pruning") = true;
  options->Add<FloatOption>(kVirtualLossBugStr, -100.0f, 100.0f,
                            "virtual-loss-bug") = 0.0f;
  options->Add<FloatOption>(kFpuReductionStr, -100.0f, 100.0f,
                            "fpu-reduction") = 0.0f;
  options->Add<IntOption>(kCacheHistoryLengthStr, 0, 7,
                          "cache-history-length") = 7;
  options->Add<FloatOption>(kPolicySoftmaxTempStr, 0.1f, 10.0f,
                            "policy-softmax-temp") = 1.0f;
  options->Add<IntOption>(kAllowedNodeCollisionsStr, 0, 1024,
                          "allowed-node-collisions") = 0;
  options->Add<FloatOption>(kBackPropagateBetaStr, 0.0f, 100.0f,
                            "backpropagate-beta") = 1.0f;
  options->Add<FloatOption>(kBackPropagateGammaStr, -100.0f, 100.0f,
                            "backpropagate-gamma") = 1.0f;
}

Search::Search(const NodeTree& tree, Network* network,
               BestMoveInfo::Callback best_move_callback,
               ThinkingInfo::Callback info_callback, const SearchLimits& limits,
               const OptionsDict& options, NNCache* cache)
    : root_node_(tree.GetCurrentHead()),
      cache_(cache),
      played_history_(tree.GetPositionHistory()),
      network_(network),
      limits_(limits),
      start_time_(std::chrono::steady_clock::now()),
      initial_visits_(root_node_->GetN()),
      best_move_callback_(best_move_callback),
      info_callback_(info_callback),
      kMiniBatchSize(options.Get<int>(kMiniBatchSizeStr)),
      kMaxPrefetchBatch(options.Get<int>(kMaxPrefetchBatchStr)),
      kCpuct(options.Get<float>(kCpuctStr)),
      kTemperature(options.Get<float>(kTemperatureStr)),
      kTempDecayMoves(options.Get<int>(kTempDecayMovesStr)),
      kNoise(options.Get<bool>(kNoiseStr)),
      kVerboseStats(options.Get<bool>(kVerboseStatsStr)),
      kSmartPruning(options.Get<bool>(kSmartPruningStr)),
      kVirtualLossBug(options.Get<float>(kVirtualLossBugStr)),
      kFpuReduction(options.Get<float>(kFpuReductionStr)),
      kCacheHistoryLength(options.Get<int>(kCacheHistoryLengthStr)),
      kPolicySoftmaxTemp(options.Get<float>(kPolicySoftmaxTempStr)),
      kAllowedNodeCollisions(options.Get<int>(kAllowedNodeCollisionsStr)),
      kBackPropagateBeta(options.Get<float>(kBackPropagateBetaStr)),
      kBackPropagateGamma(options.Get<float>(kBackPropagateGammaStr)) {}

namespace {
void ApplyDirichletNoise(Node* node, float eps, double alpha) {
  float total = 0;
  std::vector<float> noise;

  // TODO(mooskagh) remove this loop when we store number of children.
  for (Node* child : node->Children()) {
    (void)child;  // Silence the unused variable warning.
    float eta = Random::Get().GetGamma(alpha, 1.0);
    noise.emplace_back(eta);
    total += eta;
  }

  if (total < std::numeric_limits<float>::min()) return;

  int noise_idx = 0;
  for (Node* child : node->Children()) {
    child->SetP(child->GetP() * (1 - eps) + eps * noise[noise_idx++] / total);
  }
}
}  // namespace

void Search::SendUciInfo() REQUIRES(nodes_mutex_) {
  if (!best_move_node_) return;
  last_outputted_best_move_node_ = best_move_node_;
  uci_info_.depth = root_node_->GetFullDepth();
  uci_info_.seldepth = root_node_->GetMaxDepth();
  uci_info_.time = GetTimeSinceStart();
  uci_info_.nodes = total_playouts_ + initial_visits_;
  uci_info_.hashfull =
      cache_->GetSize() * 1000LL / std::max(cache_->GetCapacity(), 1);
  uci_info_.nps =
      uci_info_.time ? (total_playouts_ * 1000 / uci_info_.time) : 0;
  uci_info_.score = 290.680623072 * tan(1.548090806 * best_move_node_->GetQ(0));
  uci_info_.pv.clear();

  bool flip = played_history_.IsBlackToMove();
  for (Node* iter = best_move_node_; iter;
       iter = GetBestChildNoTemperature(iter), flip = !flip) {
    uci_info_.pv.push_back(iter->GetMove(flip));
  }
  uci_info_.comment.clear();
  info_callback_(uci_info_);
}

// Decides whether anything important changed in stats and new info should be
// shown to a user.
void Search::MaybeOutputInfo() {
  SharedMutex::Lock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  if (!responded_bestmove_ && best_move_node_ &&
      (best_move_node_ != last_outputted_best_move_node_ ||
       uci_info_.depth != root_node_->GetFullDepth() ||
       uci_info_.seldepth != root_node_->GetMaxDepth() ||
       uci_info_.time + kUciInfoMinimumFrequencyMs < GetTimeSinceStart())) {
    SendUciInfo();
  }
}

int64_t Search::GetTimeSinceStart() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start_time_)
      .count();
}

void Search::SendMovesStats() const {
  std::vector<const Node*> nodes;
  const float parent_q =
      -root_node_->GetQ(0) -
      kFpuReduction * std::sqrt(root_node_->GetVisitedPolicy());
  for (Node* child : root_node_->Children()) {
    nodes.emplace_back(child);
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const Node* a, const Node* b) { return a->GetN() < b->GetN(); });

  const bool is_black_to_move = played_history_.IsBlackToMove();
  ThinkingInfo info;
  for (const Node* node : nodes) {
    std::ostringstream oss;
    oss << std::fixed;
    oss << std::left << std::setw(5)
        << node->GetMove(is_black_to_move).as_string();
    oss << " (" << std::setw(4) << node->GetMove().as_nn_index() << ")";
    oss << " N: ";
    oss << std::right << std::setw(7) << node->GetN() << " (+" << std::setw(2)
        << node->GetNInFlight() << ") ";
    oss << "(V: " << std::setw(6) << std::setprecision(2) << node->GetV() * 100
        << "%) ";
    oss << "(P: " << std::setw(5) << std::setprecision(2) << node->GetP() * 100
        << "%) ";
    oss << "(Q: " << std::setw(8) << std::setprecision(5)
        << node->GetQ(parent_q) << ") ";
    oss << "(U: " << std::setw(6) << std::setprecision(5)
        << node->GetU() * kCpuct *
               std::sqrt(std::max(node->GetParent()->GetChildrenVisits(), 1u))
        << ") ";

    oss << "(Q+U: " << std::setw(8) << std::setprecision(5)
        << node->GetQ(parent_q) +
               node->GetU() * kCpuct *
                   std::sqrt(
                       std::max(node->GetParent()->GetChildrenVisits(), 1u))
        << ") ";
    info.comment = oss.str();
    info_callback_(info);
  }
}

void Search::MaybeTriggerStop() {
  SharedMutex::Lock nodes_lock(nodes_mutex_);
  Mutex::Lock lock(counters_mutex_);
  // Don't stop when the root node is not yet expanded.
  if (total_playouts_ == 0) return;
  // If smart pruning tells to stop (best move found), stop.
  if (found_best_move_) {
    stop_ = true;
  }
  // Stop if reached playouts limit.
  if (limits_.playouts >= 0 && total_playouts_ >= limits_.playouts) {
    stop_ = true;
  }
  // Stop if reached visits limit.
  if (limits_.visits >= 0 &&
      total_playouts_ + initial_visits_ >= limits_.visits) {
    stop_ = true;
  }
  // Stop if reached time limit.
  if (limits_.time_ms >= 0 && GetTimeSinceStart() >= limits_.time_ms) {
    stop_ = true;
  }
  // If we are the first to see that stop is needed.
  if (stop_ && !responded_bestmove_) {
    SendUciInfo();
    if (kVerboseStats) SendMovesStats();
    best_move_ = GetBestMoveInternal();
    best_move_callback_({best_move_.first, best_move_.second});
    responded_bestmove_ = true;
    best_move_node_ = nullptr;
  }
}

void Search::UpdateRemainingMoves() {
  if (!kSmartPruning) return;
  SharedMutex::Lock lock(nodes_mutex_);
  remaining_playouts_ = std::numeric_limits<int>::max();
  // Check for how many playouts there is time remaining.
  if (limits_.time_ms >= 0) {
    auto time_since_start = GetTimeSinceStart();
    if (time_since_start > kSmartPruningToleranceMs) {
      auto nps = (1000LL * total_playouts_ + kSmartPruningToleranceNodes) /
                     (time_since_start - kSmartPruningToleranceMs) +
                 1;
      int64_t remaining_time = limits_.time_ms - time_since_start;
      int64_t remaining_playouts = remaining_time * nps / 1000;
      // Don't assign directly to remaining_playouts_ as overflow is possible.
      if (remaining_playouts < remaining_playouts_)
        remaining_playouts_ = remaining_playouts;
    }
  }
  // Check how many visits are left.
  if (limits_.visits >= 0) {
    // Add kMiniBatchSize, as it's possible to exceed visits limit by that
    // number.
    auto remaining_visits =
        limits_.visits - total_playouts_ - initial_visits_ + kMiniBatchSize - 1;

    if (remaining_visits < remaining_playouts_)
      remaining_playouts_ = remaining_visits;
  }
  if (limits_.playouts >= 0) {
    // Add kMiniBatchSize, as it's possible to exceed visits limit by that
    // number.
    auto remaining_playouts =
        limits_.visits - total_playouts_ + kMiniBatchSize + 1;
    if (remaining_playouts < remaining_playouts_)
      remaining_playouts_ = remaining_playouts;
  }
  // Even if we exceeded limits, don't go crazy by not allowing any playouts.
  if (remaining_playouts_ <= 1) remaining_playouts_ = 1;
}

// Return the evaluation of the actual best child, regardless of temperature
// settings. This differs from GetBestMove, which does obey any temperature
// settings. So, somethimes, they may return results of different moves.
float Search::GetBestEval() const {
  SharedMutex::SharedLock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  float parent_q = -root_node_->GetQ(0);
  if (!root_node_->HasChildren()) return parent_q;
  Node* best_node = GetBestChildNoTemperature(root_node_);
  return best_node->GetQ(parent_q);
}

std::pair<Move, Move> Search::GetBestMove() const {
  SharedMutex::SharedLock lock(nodes_mutex_);
  Mutex::Lock counters_lock(counters_mutex_);
  return GetBestMoveInternal();
}

// Returns the best move, maybe with temperature (according to the settings).
std::pair<Move, Move> Search::GetBestMoveInternal() const
    REQUIRES_SHARED(nodes_mutex_) REQUIRES_SHARED(counters_mutex_) {
  if (responded_bestmove_) return best_move_;
  if (!root_node_->HasChildren()) return {};

  float temperature = kTemperature;
  if (temperature && kTempDecayMoves) {
    int moves = played_history_.Last().GetGamePly() / 2;
    if (moves >= kTempDecayMoves) {
      temperature = 0.0;
    } else {
      temperature *=
          static_cast<float>(kTempDecayMoves - moves) / kTempDecayMoves;
    }
  }

  Node* best_node = temperature && root_node_->GetN() > 1
                        ? GetBestChildWithTemperature(root_node_, temperature)
                        : GetBestChildNoTemperature(root_node_);

  Move ponder_move;  // Default is "null move" which means "don't display
                     // anything".
  return {best_node->GetMove(played_history_.IsBlackToMove()), ponder_move};
}

// Returns a child with most visits.
Node* Search::GetBestChildNoTemperature(Node* parent) const {
  Node* best_node = nullptr;
  // Best child is selected using the following criteria:
  // * Largest number of playouts.
  // * If two nodes have equal number:
  //   * If that number is 0, the one with larger prior wins.
  //   * If that number is larger than 0, the one with larger eval wins.
  std::tuple<int, float, float> best(-1, 0.0, 0.0);
  for (Node* node : parent->Children()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  node->GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    std::tuple<int, float, float> val(node->GetN(), node->GetQ(-10.0),
                                      node->GetP());
    if (val > best) {
      best = val;
      best_node = node;
    }
  }
  return best_node;
}

// Returns a child chosen according to weighted-by-temperature visit count.
Node* Search::GetBestChildWithTemperature(Node* parent,
                                          float temperature) const {
  std::vector<float> cumulative_sums;
  float sum = 0.0;
  const float n_parent = parent->GetN();

  for (Node* node : parent->Children()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  node->GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    sum += std::pow(node->GetN() / n_parent, 1 / temperature);
    cumulative_sums.push_back(sum);
  }

  float toss = Random::Get().GetFloat(cumulative_sums.back());
  int idx =
      std::lower_bound(cumulative_sums.begin(), cumulative_sums.end(), toss) -
      cumulative_sums.begin();

  for (Node* node : parent->Children()) {
    if (parent == root_node_ && !limits_.searchmoves.empty() &&
        std::find(limits_.searchmoves.begin(), limits_.searchmoves.end(),
                  node->GetMove()) == limits_.searchmoves.end()) {
      continue;
    }
    if (idx-- == 0) return node;
  }
  assert(false);
  return nullptr;
}

void Search::StartThreads(size_t how_many) {
  Mutex::Lock lock(threads_mutex_);
  while (threads_.size() < how_many) {
    threads_.emplace_back([this]() {
      SearchWorker worker(this);
      worker.RunBlocking();
    });
  }
}

void Search::RunSingleThreaded() {
  SearchWorker worker(this);
  worker.RunBlocking();
}

void Search::RunBlocking(size_t threads) {
  if (threads == 1) {
    RunSingleThreaded();
  } else {
    StartThreads(threads);
    Wait();
  }
}

void Search::Stop() {
  Mutex::Lock lock(counters_mutex_);
  stop_ = true;
}

void Search::Abort() {
  Mutex::Lock lock(counters_mutex_);
  responded_bestmove_ = true;
  stop_ = true;
}

void Search::Wait() {
  Mutex::Lock lock(threads_mutex_);
  while (!threads_.empty()) {
    threads_.back().join();
    threads_.pop_back();
  }
}

Search::~Search() {
  Abort();
  Wait();
}

//////////////////////////////////////////////////////////////////////////////
// SearchWorker
//////////////////////////////////////////////////////////////////////////////

void SearchWorker::ExecuteOneIteration() {
  // 1. Initialize internal structures.
  InitializeIteration(search_->network_->NewComputation());

  // 2. Gather minibatch.
  GatherMinibatch();

  // 3. Prefetch into cache.
  MaybePrefetchIntoCache();

  // 4. Run NN computation.
  RunNNComputation();

  // 5. Populate computed nodes with results of the NN computation.
  FetchNNResults();

  // 6. Update nodes.
  DoBackupUpdate();

  // 7. Update status/counters.
  UpdateCounters();
}

bool SearchWorker::IsSearchActive() const {
  Mutex::Lock lock(search_->counters_mutex_);
  return !search_->stop_;
}

// 1. Initialize internal structures.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::InitializeIteration(
    std::unique_ptr<NetworkComputation> computation) {
  nodes_to_process_.clear();
  computation_ = std::make_unique<CachingComputation>(std::move(computation),
                                                      search_->cache_);
}

// 2. Gather minibatch.
// ~~~~~~~~~~~~~~~~~~~~
void SearchWorker::GatherMinibatch() {
  int nodes_found = 0;
  int collisions_found = 0;

  // Gather nodes to process in the current batch.
  while (nodes_found < search_->kMiniBatchSize) {
    // If there's something to process without touching slow neural net, do it.
    if (nodes_found > 0 && computation_->GetCacheMisses() == 0) return;
    // Pick next node to extend.
    nodes_to_process_.emplace_back(PickNodeToExtend());
    auto* node = nodes_to_process_.back().node;

    // There was a collision. If limit has been reached, return, otherwise
    // just start search of another node.
    if (nodes_to_process_.back().is_collision) {
      if (++collisions_found > search_->kAllowedNodeCollisions) return;
      continue;
    }
    ++nodes_found;

    // If node is already known as terminal (win/loss/draw according to rules
    // of the game), it means that we already visited this node before.
    if (node->IsTerminal()) continue;

    // Node was never visited, extending.
    ExtendNode(node);

    // Only send non-terminal nodes to neural network
    if (!node->IsTerminal()) {
      nodes_to_process_.back().nn_queried = true;
      AddNodeToComputation(node);
    }
  }
}

// Returns node and whether there's been a search collision on the node.
SearchWorker::NodeToProcess SearchWorker::PickNodeToExtend() {
  // Starting from search_->root_node_, generate a playout, choosing a
  // node at each level according to the MCTS formula. n_in_flight_ is
  // incremented for each node in the playout (via TryStartScoreUpdate()).

  Node* node = search_->root_node_;
  // Initialize position sequence with pre-move position.
  history_.Trim(search_->played_history_.GetLength());
  // Fetch the current best root node visits for possible smart pruning.
  int best_node_n = 0;
  {
    SharedMutex::Lock lock(search_->nodes_mutex_);
    if (search_->best_move_node_)
      best_node_n = search_->best_move_node_->GetN();
  }

  // True on first iteration, false as we dive deeper.
  bool is_root_node = true;
  while (true) {
    // First, terminate if we find collisions or leaf nodes.
    {
      SharedMutex::Lock lock(search_->nodes_mutex_);
      // n_in_flight_ is incremented. If the method returns false, then there is
      // a search collision, and this node is already being expanded.
      if (!node->TryStartScoreUpdate()) return {node, true};
      // Unexamined leaf node. We've hit the end of this playout.
      if (!node->HasChildren()) return {node, false};
      // If we fall through, then n_in_flight_ has been incremented but this
      // playout remains incomplete; we must go deeper.
    }

    SharedMutex::SharedLock lock(search_->nodes_mutex_);
    float puct_mult =
        search_->kCpuct * std::sqrt(std::max(node->GetChildrenVisits(), 1u));
    float best = -100.0f;
    int possible_moves = 0;
    float parent_q =
        (is_root_node && search_->kNoise)
            ? -node->GetQ(0)
            : -node->GetQ(0) -
                  search_->kFpuReduction * std::sqrt(node->GetVisitedPolicy());
    for (Node* child : node->Children()) {
      if (is_root_node) {
        // If there's no chance to catch up to the current best node with
        // remaining playouts, don't consider it.
        // best_move_node_ could have changed since best_node_n was retrieved.
        // To ensure we have at least one node to expand, always include
        // current best node.
        if (child != search_->best_move_node_ &&
            search_->remaining_playouts_ <
                best_node_n - static_cast<int>(child->GetN())) {
          continue;
        }
        // If searchmoves was sent, restrict the search only in that moves
        if (!search_->limits_.searchmoves.empty() &&
            std::find(search_->limits_.searchmoves.begin(),
                      search_->limits_.searchmoves.end(),
                      child->GetMove()) == search_->limits_.searchmoves.end()) {
          continue;
        }
        ++possible_moves;
      }
      float Q = child->GetQ(parent_q);
      if (search_->kVirtualLossBug && child->GetN() == 0) {
        Q = (Q * child->GetParent()->GetN() - search_->kVirtualLossBug) /
            (child->GetParent()->GetN() + std::fabs(search_->kVirtualLossBug));
      }
      const float score = puct_mult * child->GetU() + Q;
      if (score > best) {
        best = score;
        node = child;
      }
    }
    history_.Append(node->GetMove());
    if (is_root_node && possible_moves <= 1 && !search_->limits_.infinite) {
      // If there is only one move theoretically possible within remaining time,
      // output it.
      Mutex::Lock counters_lock(search_->counters_mutex_);
      search_->found_best_move_ = true;
    }
    is_root_node = false;
  }
}

void SearchWorker::ExtendNode(Node* node) {
  // We don't need the mutex because other threads will see that N=0 and
  // N-in-flight=1 and will not touch this node.
  const auto& board = history_.Last().GetBoard();
  auto legal_moves = board.GenerateLegalMoves();

  // Check whether it's a draw/lose by position. Importantly, we must check
  // these before doing the by-rule checks below.
  if (legal_moves.empty()) {
    // Could be a checkmate or a stalemate
    if (board.IsUnderCheck()) {
      node->MakeTerminal(GameResult::WHITE_WON);
    } else {
      node->MakeTerminal(GameResult::DRAW);
    }
    return;
  }

  // We can shortcircuit these draws-by-rule only if they aren't root;
  // if they are root, then thinking about them is the point.
  if (node != search_->root_node_) {
    if (!board.HasMatingMaterial()) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }

    if (history_.Last().GetNoCapturePly() >= 100) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }

    if (history_.Last().GetRepetitions() >= 2) {
      node->MakeTerminal(GameResult::DRAW);
      return;
    }
  }

  // Add legal moves as children to this node.
  for (const auto& move : legal_moves) node->CreateChild(move);
}

// Returns whether node was already in cache.
bool SearchWorker::AddNodeToComputation(Node* node, bool add_if_cached) {
  auto hash = history_.HashLast(search_->kCacheHistoryLength + 1);
  // If already in cache, no need to do anything.
  if (add_if_cached) {
    if (computation_->AddInputByHash(hash)) return true;
  } else {
    if (search_->cache_->ContainsKey(hash)) return true;
  }
  auto planes = EncodePositionForNN(history_, 8);

  std::vector<uint16_t> moves;

  if (node->HasChildren()) {
    // Legal moves are known, use them.
    for (Node* child : node->Children()) {
      moves.emplace_back(child->GetMove().as_nn_index());
    }
  } else {
    // Cache pseudolegal moves. A bit of a waste, but faster.
    const auto& pseudolegal_moves =
        history_.Last().GetBoard().GeneratePseudolegalMoves();
    moves.reserve(pseudolegal_moves.size());
    // As an optimization, store moves in reverse order in cache, because
    // that's the order nodes are listed in nodelist.
    for (auto iter = pseudolegal_moves.rbegin(), end = pseudolegal_moves.rend();
         iter != end; ++iter) {
      moves.emplace_back(iter->as_nn_index());
    }
  }

  computation_->AddInput(hash, std::move(planes), std::move(moves));
  return false;
}

// 3. Prefetch into cache.
// ~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::MaybePrefetchIntoCache() {
  // TODO(mooskagh) Remove prefetch into cache if node collisions work well.
  // If there are requests to NN, but the batch is not full, try to prefetch
  // nodes which are likely useful in future.
  if (computation_->GetCacheMisses() > 0 &&
      computation_->GetCacheMisses() < search_->kMaxPrefetchBatch) {
    history_.Trim(search_->played_history_.GetLength());
    SharedMutex::SharedLock lock(search_->nodes_mutex_);
    PrefetchIntoCache(search_->root_node_, search_->kMaxPrefetchBatch -
                                               computation_->GetCacheMisses());
  }
}

// Prefetches up to @budget nodes into cache. Returns number of nodes
// prefetched.
int SearchWorker::PrefetchIntoCache(Node* node, int budget) {
  if (budget <= 0) return 0;

  // We are in a leaf, which is not yet being processed.
  if (node->GetNStarted() == 0) {
    if (AddNodeToComputation(node, false)) {
      // Make it return 0 to make it not use the slot, so that the function
      // tries hard to find something to cache even among unpopular moves.
      // In practice that slows things down a lot though, as it's not always
      // easy to find what to cache.
      return 1;
    }
    return 1;
  }

  // n = 0 and n_in_flight_ > 0, that means the node is being extended.
  if (node->GetN() == 0) return 0;
  // The node is terminal; don't prefetch it.
  if (node->IsTerminal()) return 0;

  // Populate all subnodes and their scores.
  typedef std::pair<float, Node*> ScoredNode;
  std::vector<ScoredNode> scores;
  float puct_mult =
      search_->kCpuct * std::sqrt(std::max(node->GetChildrenVisits(), 1u));
  // FPU reduction is not taken into account.
  const float parent_q = -node->GetQ(0);
  for (Node* child : node->Children()) {
    if (child->GetP() == 0.0f) continue;
    // Flip the sign of a score to be able to easily sort.
    scores.emplace_back(-puct_mult * child->GetU() - child->GetQ(parent_q),
                        child);
  }

  size_t first_unsorted_index = 0;
  int total_budget_spent = 0;
  int budget_to_spend = budget;  // Initialize for the case where there's only
                                 // one child.
  for (size_t i = 0; i < scores.size(); ++i) {
    if (budget <= 0) break;

    // Sort next chunk of a vector. 3 at a time. Most of the time it's fine.
    if (first_unsorted_index != scores.size() &&
        i + 2 >= first_unsorted_index) {
      const int new_unsorted_index =
          std::min(scores.size(), budget < 2 ? first_unsorted_index + 2
                                             : first_unsorted_index + 3);
      std::partial_sort(scores.begin() + first_unsorted_index,
                        scores.begin() + new_unsorted_index, scores.end());
      first_unsorted_index = new_unsorted_index;
    }

    Node* n = scores[i].second;
    // Last node gets the same budget as prev-to-last node.
    if (i != scores.size() - 1) {
      // Sign of the score was flipped for sorting, so flip it back.
      const float next_score = -scores[i + 1].first;
      const float q = n->GetQ(-parent_q);
      if (next_score > q) {
        budget_to_spend = std::min(
            budget,
            int(n->GetP() * puct_mult / (next_score - q) - n->GetNStarted()) +
                1);
      } else {
        budget_to_spend = budget;
      }
    }
    history_.Append(n->GetMove());
    const int budget_spent = PrefetchIntoCache(n, budget_to_spend);
    history_.Pop();
    budget -= budget_spent;
    total_budget_spent += budget_spent;
  }
  return total_budget_spent;
}

// 4. Run NN computation.
// ~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::RunNNComputation() {
  // This function is so small as to be silly, but its parent function is
  // conceptually cleaner for it.
  if (computation_->GetBatchSize() != 0) computation_->ComputeBlocking();
}

// 5. Populate computed nodes with results of the NN computation.
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
void SearchWorker::FetchNNResults() {
  if (computation_->GetBatchSize() == 0) return;

  // Copy NN results into nodes.
  int idx_in_computation = 0;
  for (auto& node_to_process : nodes_to_process_) {
    if (!node_to_process.nn_queried) continue;
    Node* node = node_to_process.node;
    // Populate V value.
    node->SetV(-computation_->GetQVal(idx_in_computation));
    // Populate P values.
    float total = 0.0;
    for (Node* n : node->Children()) {
      float p =
          computation_->GetPVal(idx_in_computation, n->GetMove().as_nn_index());
      if (search_->kPolicySoftmaxTemp != 1.0f) {
        p = pow(p, 1 / search_->kPolicySoftmaxTemp);
      }
      total += p;
      n->SetP(p);
    }
    // Scale P values to add up to 1.0.
    if (total > 0.0f) {
      float scale = 1.0f / total;
      for (Node* n : node->Children()) n->SetP(n->GetP() * scale);
    }
    // Add Dirichlet noise if enabled and at root.
    if (search_->kNoise && node == search_->root_node_) {
      ApplyDirichletNoise(node, 0.25, 0.3);
    }
    ++idx_in_computation;
  }
}

// 6. Update nodes.
// ~~~~~~~~~~~~~~
void SearchWorker::DoBackupUpdate() {
  // Update nodes.
  SharedMutex::Lock lock(search_->nodes_mutex_);
  for (NodeToProcess& node_to_process : nodes_to_process_) {
    Node* node = node_to_process.node;
    if (node_to_process.is_collision) {
      // If it was a collision, just undo counters.
      for (node = node->GetParent(); node != search_->root_node_->GetParent();
           node = node->GetParent()) {
        node->CancelScoreUpdate();
      }
      continue;
    }

    // Backup V value up to a root. After 1 visit, V = Q.
    float v = node->GetV();
    // Maximum depth the node is explored.
    uint16_t depth = 0;
    // If the node is terminal, mark it as fully explored to an "infinite"
    // depth.
    uint16_t cur_full_depth = node->IsTerminal() ? 999 : 0;
    bool full_depth_updated = true;
    for (Node* n = node; n != search_->root_node_->GetParent();
         n = n->GetParent()) {
      ++depth;
      n->FinalizeScoreUpdate(v, search_->kBackPropagateGamma,
                             search_->kBackPropagateBeta);
      // Q will be flipped for opponent.
      v = -v;

      // Update the stats.
      // Max depth.
      n->UpdateMaxDepth(depth);
      // Full depth.
      if (full_depth_updated)
        full_depth_updated = n->UpdateFullDepth(&cur_full_depth);
      // Best move.
      if (n->GetParent() == search_->root_node_) {
        if (!search_->best_move_node_ ||
            search_->best_move_node_->GetN() < n->GetN()) {
          search_->best_move_node_ = n;
        }
      }
    }
    ++search_->total_playouts_;
  }
}

// 7. UpdateCounters()
//~~~~~~~~~~~~~~~~~~~~
void SearchWorker::UpdateCounters() {
  search_->UpdateRemainingMoves();  // Updates smart pruning counters.
  search_->MaybeOutputInfo();
  search_->MaybeTriggerStop();

  if (nodes_to_process_.empty()) {
    // If this thread had no work, sleep for some milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

}  // namespace lczero

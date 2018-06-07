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

#include "neural/factory.h"
#include "neural/network.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <thread>
#include <queue>

#include "utils/exception.h"
#include "utils/random.h"

namespace lczero {
  
  
  class StreamNetwork;
  
  class StreamComputation : public NetworkComputation {
  public:
    
    StreamComputation(StreamNetwork& network):
    network_(network),
    remaining_(0),
    planes_(),
    peers_()
    {}
    
    void AddInput(InputPlanes&& input) override {
      planes_.emplace_back(input);
    }
    
    void ComputeBlocking() override;
    
    // Returns how many times AddInput() was called.
    int GetBatchSize() const override { return static_cast<int>(planes_.size()); }
    
    // Returns Q value of @sample.
    float GetQVal(int sample) const override {
      return peers_[sample]->GetQVal(0);
    }
    
    // Returns P value @move_id of @sample.
    float GetPVal(int sample, int move_id) const override {
      return peers_[sample]->GetPVal(0, move_id);
    }
    
    
  private:
    
    friend StreamNetwork;
    
    void Receive(int index, std::unique_ptr<NetworkComputation> cmp);
    
    
    StreamNetwork& network_;
    std::vector<InputPlanes> planes_;
    std::vector<std::unique_ptr<NetworkComputation>> peers_;
    
    std::mutex mutex_;
    std::condition_variable condition_;
    int remaining_;

  };
  
  class StreamNetwork : public Network {
  public:
    
    StreamNetwork(const Weights& weights, const OptionsDict& options);
    
    void Compute(StreamComputation& computation, int index) ;
    
    std::unique_ptr<NetworkComputation> NewComputation() override;
    
  private:
    
    struct Task {
      StreamComputation& computation;
      int index;
      
      Task(StreamComputation& c, int i) : computation(c), index(i) {}
    };
    
    
    std::vector<std::thread> threads_;
    std::queue<Task> tasks_;
    std::mutex queue_mutex;
    std::condition_variable condition_;

    std::unique_ptr<Network> peer_;
    
    
    void ThreadLoop();
    Task ThreadStep();

  };
  
  
  void StreamComputation::ComputeBlocking()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    int batch_size=static_cast<int>(planes_.size());
    remaining_=batch_size;
    peers_.resize(batch_size);
    for (int i=0; i<batch_size; i++) {
      network_.Compute(*this, i);
    }
    condition_.wait(lock);
  }
  
  void StreamComputation::Receive(int index, std::unique_ptr<NetworkComputation> cmp) {
    std::unique_lock<std::mutex> lock(mutex_);
    peers_[index]=std::move(cmp);
    remaining_--;
    if (remaining_==0)
      condition_.notify_one();
  }
  
  
  StreamNetwork::StreamNetwork(const Weights& weights, const OptionsDict& options) {
    OptionsDict blas_options;
    peer_ = NetworkFactory::Get()->Create("blas", weights, blas_options);
    
    int threadCount=6;
    for (int i=0; i<threadCount; i++) {
      threads_.emplace_back(std::thread(&StreamNetwork::ThreadLoop, this));
    }
  }
  
  void StreamNetwork::Compute(StreamComputation& computation, int index) {
    
    auto peerComp=peer_->NewComputation();
    peerComp->AddInput(std::move(computation.planes_[index]));
    peerComp->ComputeBlocking();
    computation.peers_[index]=std::move(peerComp);
    
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      tasks_.push(Task(computation, index));
      condition_.notify_all();
    }
    
  }

  std::unique_ptr<NetworkComputation> StreamNetwork::NewComputation()  {
    return std::make_unique<StreamComputation>(*this);
  }
  
  void
  StreamNetwork::ThreadLoop() {
    while(true)
    {
      Task task=ThreadStep();
 //     std::cerr<< "running " << task.index << std::endl;
      
      auto peerComp=peer_->NewComputation();
      peerComp->AddInput(std::move(task.computation.planes_[task.index]));
      peerComp->ComputeBlocking();
      task.computation.Receive(task.index, std::move(peerComp));
     // task.computation.peers_[task.index]=std::move(peerComp);

    }
  }
  
  StreamNetwork::Task
  StreamNetwork::ThreadStep() {
    std::unique_lock<std::mutex> lock(queue_mutex);
    condition_.wait(lock, [this]{ return !tasks_.empty(); });
    Task task=std::move(tasks_.front());
    tasks_.pop();
    return task;
  }
  
  
  
  REGISTER_NETWORK("stream", StreamNetwork, -750)
  
}  // namespace lczero

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
      return peers_[sample].GetQVal();
    }
    
    // Returns P value @move_id of @sample.
    float GetPVal(int sample, int move_id) const override {
      return peers_[sample].GetPVal(move_id);
    }
    
    
  private:
    
    friend StreamNetwork;
    
    void Receive(int index, std::shared_ptr<NetworkComputation> cmp, int cmp_index);
    
    struct Lookup {
      
      
    public:
      
      Lookup() {}
      
      Lookup(std::shared_ptr<NetworkComputation> cmp, int cmp_index) : cmp_(std::move(cmp)), cmp_index_(cmp_index) {
        
      }
      
      std::shared_ptr<NetworkComputation> cmp_;
      int cmp_index_;
                                                                              
      inline float GetQVal() const {
        return cmp_->GetQVal(cmp_index_);
      }
      
      inline float GetPVal(int move_id) const {
        return cmp_->GetPVal(cmp_index_, move_id);
      }
      
    };
    
    
    
    StreamNetwork& network_;
    std::vector<InputPlanes> planes_;
    std::vector<Lookup> peers_;
    
    std::mutex mutex_;
    std::condition_variable condition_;
    int remaining_;

  };
  
  class StreamNetwork : public Network {
  public:
    
    StreamNetwork(const Weights& weights, const OptionsDict& options);
    
    void Add(StreamComputation& computation, int index) ;
    void Flush();

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
    
    int min_batch_size_;
    
    void ThreadLoop();
    std::vector<Task> ThreadStep();

  };
  
  
  void StreamComputation::ComputeBlocking()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    int batch_size=static_cast<int>(planes_.size());
    remaining_=batch_size;
    peers_.resize(batch_size);
    for (int i=0; i<batch_size; i++) {
      network_.Add(*this, i);
    }
    network_.Flush();
    condition_.wait(lock, [this]{ return remaining_==0; });

  }
  
  void StreamComputation::Receive(int index, std::shared_ptr<NetworkComputation> cmp, int cmp_index) {
    std::unique_lock<std::mutex> lock(mutex_);
    peers_[index]=Lookup(cmp, cmp_index);
    remaining_--;
    condition_.notify_one();
  }
  
  
  StreamNetwork::StreamNetwork(const Weights& weights, const OptionsDict& options) {
    std::string peer=options.GetOrDefault<std::string>("peer", "blas");
    bool verbose=options.GetOrDefault<bool>("verbose", true);
    int blas_cores = options.GetOrDefault<int>("blas_cores", 1);

    int threadCount=options.GetOrDefault<int>("threads", 2);
    
    int min_batch_size=options.GetOrDefault<int>("min_batch_size", 32);
    int max_batch_size=options.GetOrDefault<int>("max_batch_size", 256);

    OptionsDict blas_options;
    blas_options.Set("blas_core", blas_cores);
    blas_options.Set("verbose", verbose);
    blas_options.Set("batch_size", max_batch_size);
    
    min_batch_size_=min_batch_size;
    
    fprintf(stderr, "Stream: threads <%d>\n", threadCount);
    fprintf(stderr, "Stream: min_batch_size <%d>\n", min_batch_size);
    fprintf(stderr, "Stream: max_batch_size <%d>\n", max_batch_size);
    fprintf(stderr, "Stream: blas_core <%d>\n", blas_cores);

    peer_ = NetworkFactory::Get()->Create(peer, weights, blas_options);

    if (verbose) {
      fprintf(stderr, "Stream: creating %d threads for backend <%s>\n", threadCount, peer.c_str());
    }
    for (int i=0; i<threadCount; i++) {
      threads_.emplace_back(std::thread(&StreamNetwork::ThreadLoop, this));
    }
    

  }
  
  void StreamNetwork::Add(StreamComputation& computation, int index) {
    
    std::unique_lock<std::mutex> lock(queue_mutex);
    tasks_.push(Task(computation, index));
  }
  
  
  void StreamNetwork::Flush() {
    condition_.notify_all();
  }

  std::unique_ptr<NetworkComputation> StreamNetwork::NewComputation()  {
    return std::make_unique<StreamComputation>(*this);
  }
  
  void
  StreamNetwork::ThreadLoop() {
    while(true)
    {

      std::vector<Task> tasks=ThreadStep();
      std::shared_ptr<NetworkComputation> peerComp=peer_->NewComputation();
      for (const auto& task : tasks)
        peerComp->AddInput(std::move(task.computation.planes_[task.index]));
      peerComp->ComputeBlocking();
      int cmp_index=0;
      for (const auto& task : tasks)
        task.computation.Receive(task.index, peerComp, cmp_index++);

    }
  }
  
  std::vector<StreamNetwork::Task>
  StreamNetwork::ThreadStep() {
    
    
    while (true) {
      std::unique_lock<std::mutex> lock(queue_mutex);
      condition_.wait(lock, [this]{ return !tasks_.empty(); });
      const int min_batch=std::min(32, (int) tasks_.size());
      if (min_batch==0)
        continue;
      
      std::vector<StreamNetwork::Task> tasks;
      for (int i=0; i<min_batch; i++) {
        tasks.emplace_back(std::move(tasks_.front()));
        tasks_.pop();
      }
      condition_.notify_all();
      return tasks;
    }
    
  }
  
  
  
  REGISTER_NETWORK("stream", StreamNetwork, -750)
  
}  // namespace lczero

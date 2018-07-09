//
//  network_profiler.hpp
//  lc0
//
//  Created by François on 10/06/2018.
//  Copyright © 2018 François. All rights reserved.
//

#ifndef network_profiler_hpp
#define network_profiler_hpp

#include <stdio.h>
#include <chrono>
#include <string>


enum NetworkStep {
  
  NetworkStepEncode,
  NetworkStepInit,
  NetworkStepFirstConvolve3,
  NetworkStepFirstBatchNorm,
  NetworkStepInit2,
  NetworkStepResCopy,
  NetworkStepResConvolve3,
  NetworkStepResBatchNorm1,
  NetworkStepResBatchNorm2,
  NetworkStepConvolveP1,
  NetworkStepConvolveV1,
  NetworkStepBatchNormP1,
  NetworkStepBatchNormV1,
  NetworkStepInneproductP1,
  NetworkStepInneproductV1,
  NetworkStepEnd,
  NetworkStepEnd2,
  NetworkStepEnd3,
  NetworkStepEnd4,
  NetworkStepEnd5,
  NetworkStepEnd6,
  NetworkStepEnd7,
  NetworkStepEnd8,
  NetworkStepEnd9,

  NetworkStepWinogradTransformIn,
  NetworkStepWinogradTransformSgemm,
  NetworkStepWinogradTransformOut,
  
  NetworkStepEncoding,
  NetworkStepForward,

  NetworkStepCOUNT
  
};


class NetworkProfiler
{
private:
  
  static const char* LABELS[];
  
  int batch_size;
  
  uint64_t ticks[NetworkStepCOUNT];
  int counts[NetworkStepCOUNT];
  
  bool started;
  uint64_t last;
  
  
public:
  
  NetworkProfiler() {
    started=false;
    for (int i=0; i<NetworkStepCOUNT; i++) {
      ticks[i]=0;
      counts[i]=0;
    }
//    memset(ticks, sizeof(ticks), 0);
//    memset(counts,sizeof(counts), 0);
   last=0;
  }
  
  
  void Step(NetworkStep step) {
    clock_t now=getTicks();
    if (started) {
      ticks[step]+=now-last;
      counts[step]+=batch_size;
    }
    last=now;
  }
  
  
  void Start(int batch_size) {
    last=getTicks();
    started=true;
    this->batch_size=batch_size;
  }
  
  
  void Stop() {
    last=0;
    started=false;
  }
  
  
  uint64_t getTicks() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return 1000000000LL*ts.tv_sec + ts.tv_nsec;
  }
  
  double getMicroSecs(uint64_t ticks) {
    return ticks/1000.0;
  }
  
  void Dump() {
    
    uint64_t total=0;
    for (int i=0; i<NetworkStepCOUNT; i++) {
      total+=ticks[i];
    }
    
    printf("    Operation        count     total us    us  percent \n");
    for (int i=0; i<NetworkStepCOUNT; i++) {
      if (counts[i]==0)
        continue;
      double us=getMicroSecs(ticks[i]/counts[i]);
      double total_us=getMicroSecs(ticks[i]);
      double percent=100.0*ticks[i]/(double) total;
      printf("%15.15s    %5.0d   %9.0f  %8.0f  %2.2f \n", LABELS[i], counts[i], total_us, us, percent);
    }
  }
  
 
  
  
  
  
  
};

#endif /* network_profiler_hpp */

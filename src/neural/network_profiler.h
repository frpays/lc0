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

#ifndef network_profiler_hpp
#define network_profiler_hpp

#include <stdio.h>
#include <chrono>
#include <string>


enum NetworkStep {
  
  NetworkStepWinogradTransformIn,
  NetworkStepWinogradTransformSgemm,
  NetworkStepWinogradTransformOut,
  
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
  
  int total;
  
  
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
    total=0;
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
    total++;
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
    
    if (total%1000!=0)
      return;
    
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

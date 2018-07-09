//
//  network_profiler.cpp
//  lc0
//
//  Created by François on 10/06/2018.
//  Copyright © 2018 François. All rights reserved.
//

#include "network_profiler.h"

 const char*  NetworkProfiler:: LABELS[] {
   "Encode",
   "Init",
   "FirstConvolve3",
   "FirstBatchNorm",
   "Init2",
   "ResCopy",
   "ResConvolve3",
   "ResBatchNorm1",
   "ResBatchNorm2",
   "ConvolveP1",
   "ConvolveV1",
   "BatchNormP1",
   "BatchNormV1",
   "InneproductP1",
   "InneproductV1",
   "End",
   "End2",
   "End3",
   "End4",
   "End5",
   "End6",
   "End7",
   "End8",
   "End9",

   "WinogradTransformIn",
   "WinogradTransformSgemm",
   "WinogradTransformOut",

   "Encoding",
   "Forward",

};


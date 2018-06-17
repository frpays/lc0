//
//  blas.cpp
//  lc0
//
//  Created by François on 17/06/2018.
//  Copyright © 2018 François. All rights reserved.
//

#include "blas.h"

#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

void handler(const char* where, int line) {
  void *array[10];
  size_t size;
  
  // get void*'s for all entries on the stack
  size = backtrace(array, 10);
  
  // print out all the frames to stderr
  fprintf(stderr, "Error %s %d\n", where, line);
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  exit(1);
}


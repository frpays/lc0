//
//  Regress.cpp
//  lc0
//
//  Created by François on 28/07/2018.
//  Copyright © 2018 François. All rights reserved.
//

#include "utils/Regress.h"

#include <stdio.h>

namespace lczero {
  
  Regress::Regress()
  {
    reset();
  }
  
  void
  Regress::reset()
  {
     n=0;
     sx=0;
     sx2=0;
     sy=0;
     sy2=0;
    
     alpha=0;
     beta=0;
  }
  
  
  void Regress::add(double x, double y) {
    
    n+=1;
    sx+=x;
    sy+=y;
    sx2+=x*x;
    sy2+=y*y;
    sxy+=x*y;

  }
  
  
  void Regress::compute() {
    
    double denom=sx*sx-n*sx2;
    alpha=(sx*sxy-sx2*sy)/denom;
    beta=(sx*sy-n*sxy)/denom;
  }
  
  
  void Regress::dump() {
    fprintf(stderr, "alpha = %f\n", alpha);
    fprintf(stderr, "beta = %f\n", beta);
 }
}

//
//  Regress.hpp
//  lc0
//
//  Created by François on 28/07/2018.
//  Copyright © 2018 François. All rights reserved.
//

#pragma once
namespace lczero {

class Regress
{
public:
  
  
  Regress();
  
  void reset();
  
  void add(double x, double y);
  
  void compute();
  
  void   dump();

  
private:
  
  
  double n;
  double sx;
  double sx2;
  double sy;
  double sy2;
  double sxy;

  double alpha;
  double beta;
  
  
  
};
  
}


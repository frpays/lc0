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

#pragma once

// Select the BLAS vendor based on defines

#include <vector>
#include "neural/network.h"


#ifdef USE_MKL
#include <mkl.h>
#else

#ifdef USE_OPENBLAS
#include <cblas.h>

// Specific openblas routines.
extern "C" {
int openblas_get_num_procs(void);
void openblas_set_num_threads(int num_threads);
char* openblas_get_corename(void);
char* openblas_get_config(void);
}

#else

#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#define USE_ACCELERATE
#endif

#endif  // USE_OPENBLAS

#endif  // USE_MKL

template<typename T>
class SafePtr
{
  public:
  
  SafePtr(T* ptr, size_t size, int offset=0):
  ptr_(ptr),
  size_(size),
  offset_(offset) {
  }
  

  SafePtr(std::vector<T> &x):
  ptr_( x.data()),
  size_(x.size()),
  offset_(0) {
  }

  
  operator SafePtr<const T>() const { return SafePtr<const T>(ptr_, size_, offset_);  }
  
  SafePtr<T>& operator+=(int idx);
  SafePtr<T>& operator-=(int idx);
  SafePtr<T>& operator++();
  SafePtr<T>& operator--();

  friend SafePtr<T> operator+(const SafePtr<T> &x, int val) {
    return SafePtr<T>(x.ptr_, x.size_, x.offset_+val);
  }
  
  friend SafePtr<T> operator-(const SafePtr<T> &x, int val) {
    return SafePtr<T>(x.ptr_, x.size_, x.offset_-val);
  }


  T& operator*();
  T operator*() const;

  T& operator[](int idx);
  T operator[](int idx) const;

  
  private:
  
  T* ptr_;
  size_t size_;
  int offset_;
};


void handler(const char* where, int line);



template<typename T>
T&
SafePtr<T>::operator[](int idx) {
  
  if (ptr_==0 || (idx+offset_)<0 || (idx+offset_)>size_) {
    handler(__FILE__, __LINE__);
  }
  return ptr_[offset_+idx];
}



template<typename T>
T
SafePtr<T>::operator[](int idx) const {
  
  if (ptr_==0 || (idx+offset_)<0 || (idx+offset_)>size_) {
    handler(__FILE__, __LINE__);
  }
  return ptr_[offset_+idx];
}



template<typename T>
T
SafePtr<T>::operator*() const {
  return ptr_[offset_];
}


template<typename T>
T&
SafePtr<T>::operator*() {
  return ptr_[offset_];
}



template<typename T>
SafePtr<T>&
SafePtr<T>::operator+=(int idx) {
  offset_+=idx;
  return *this;
}


template<typename T>
SafePtr<T>&
SafePtr<T>::operator-=(int idx){
  offset_-=idx;
  return *this;
}

template<typename T>
SafePtr<T>&
SafePtr<T>::operator++(){
  offset_++;
  return *this;
}

template<typename T>
SafePtr<T>&
SafePtr<T>::operator--(){
  offset_--;
  return *this;
}


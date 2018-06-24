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

#include "neural/blas/convolution1.h"
#include "neural/blas/blas.h"

namespace lczero {

void Convolution1::Forward(const size_t batch_size, const size_t input_channels,
                           const size_t output_channels, const float* input,
                           const float* weights, const float* biases,
                           float* output) {
  for (size_t i = 0; i < batch_size; i++) {
    // C←αAB + βC
    // M Number of rows in matrices A and C.
    // N Number of columns in matrices B and C.
    // K Number of columns in matrix A; number of rows in matrix B.
    // lda The size of the first dimension of matrix A; if you are
    // passing a matrix A[m][n], the value should be m.
    //    cblas_sgemm(CblasRowMajor, TransA, TransB, M, N, K, alpha, A, lda, B,
    //                ldb, beta, C, N);

    //             C                               A                     B
    //
    //           outputs             :=          weights        x      input
    //
    //   cols:  kSquares (N)                 input_channels (K)      kSquares
    //   (N)
    //
    //   rows:  output_channels (M)         output_channels (M)
    //   input_channels (K)

    const float* batch_input = input + i * kSquares * input_channels;
    float* batch_output = output + i * kSquares * output_channels;

    cblas_sgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                // M              N         K         alpha
                (int)output_channels, kSquares, (int)input_channels, 1.0f,
                // A     lda
                weights, (int)input_channels,
                // B    ldb   beta,
                batch_input, kSquares, 0.0f,
                // C   ldc
                batch_output, kSquares);

    auto index = 0;
    for (size_t o = 0; o < output_channels; o++) {
      const auto bias = biases[o];
      for (size_t b = 0; b < kSquares; b++) {
        batch_output[index++] += bias;
      }
    }
  }
}

}  // namespace lczerp

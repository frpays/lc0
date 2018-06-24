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

#include <cstddef>
#include <vector>

namespace lczero {

class Batchnorm {
 public:
  static void Apply(const size_t batch_size, const size_t channels, float* data,
                    const float* means, const float* stddivs,
                    const float* eltwise = nullptr);

  static void OffsetMeans(std::vector<float>& bn_means,
                          const std::vector<float>& biases);

  static void InvertStddev(std::vector<float>& weights);

 private:
  static constexpr float kEpsilon = 1e-5f;

  static constexpr auto kWidth = 8;
  static constexpr auto kHeight = 8;
  static constexpr auto kSquares = kWidth * kHeight;
};

}  // namespace lczero

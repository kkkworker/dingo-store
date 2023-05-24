// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef DINGODB_EXPR_OPERATORS_ARITHMETIC_H_
#define DINGODB_EXPR_OPERATORS_ARITHMETIC_H_

#include <string>

#include "defs.h"

namespace dingodb::expr {

template <typename T>
T CalcPos(T v) {
  return v;
}

DECLARE_INVALID_UNARY_OP(CalcPos, std::string)

template <typename T>
T CalcNeg(T v) {
  return -v;
}

DECLARE_INVALID_UNARY_OP(CalcNeg, std::string)

template <typename T>
T CalcAdd(T v0, T v1) {
  return v0 + v1;
}

DECLARE_INVALID_BINARY_OP(CalcAdd, std::string)

template <typename T>
T CalcSub(T v0, T v1) {
  return v0 - v1;
}

DECLARE_INVALID_BINARY_OP(CalcSub, std::string)

template <typename T>
T CalcMul(T v0, T v1) {
  return v0 * v1;
}

DECLARE_INVALID_BINARY_OP(CalcMul, std::string)

template <typename T>
T CalcDiv(T v0, T v1) {
  return v0 / v1;
}

DECLARE_INVALID_BINARY_OP(CalcDiv, std::string)

template <typename T>
T CalcMod(T v0, T v1) {
  return v0 % v1;
}

DECLARE_INVALID_BINARY_OP(CalcMod, float)
DECLARE_INVALID_BINARY_OP(CalcMod, double)
DECLARE_INVALID_BINARY_OP(CalcMod, std::string)

}  // namespace dingodb::expr

#endif  // DINGODB_EXPR_OPERATORS_ARITHMETIC_H_
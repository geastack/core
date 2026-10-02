// SPDX-License-Identifier: Apache-2.0
#pragma once
#ifndef GEA_HOST_DECLARED
#define GEA_HOST_DECLARED 1
#include "gea/embedded.h"
#endif
#include "gea_runtime.h"

namespace gea::runtime::hostbase64 {
inline std::string encode(const std::string &value) {
  return gea::runtime::base64::encode(value);
}
inline std::string encode(const gea::Ref<gea::ArrayBuffer> &value) {
  value->requireAttached();
  return gea::runtime::base64::encodeBytes(value->data(), value->size());
}
inline std::string encode(const gea::Ref<gea::TypedArray<std::uint8_t>> &value) {
  return gea::runtime::base64::encodeBytes(value->data(), value->size());
}
inline std::string encode(const gea::TypedArray<std::uint8_t> &value) {
  return gea::runtime::base64::encodeBytes(value.data(), value.size());
}
inline std::string decode(const std::string &value) {
  return gea::runtime::base64::decode(value);
}
}  // namespace gea::runtime::hostbase64

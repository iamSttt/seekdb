#pragma once
// Rejected d227204 build encoder, retained only for historical regression.
// Production translation units do not include this file.
#include "quantized_bound.h"
namespace mvdb {
// Row-sized index build operation. The caller must not change FP controls
// during this call; no allocation or callback occurs inside the loop. Keep the
// scalar helper and query encoder independent of this build-only acceleration.
inline bool quantize_coordinates(const float* values,size_t count,double step,int8_t* out) {
  if(!values || !out || !quantized_detail::normal_fp_environment() ||
     !quantized_detail::power_of_two(step))return false;
  // Decide before taking a reciprocal: a tiny step could otherwise overflow
  // even when the original value/step (for example zero/step) would not. Also
  // avoid adding subnormal operands to the FP operation sequence. All steps
  // selected from finite float32 index values are inside this normal range.
  if(step<0x1p-1022 || step>0x1p1022) {
    for(size_t i=0;i<count;++i)
      if(!quantized_detail::encode(values[i],step,&out[i]))return false;
    return true;
  }
  const double inverse=1.0/step; // Exact, normal reciprocal of a power of two.
  for(size_t i=0;i<count;++i) {
    const float value=values[i];
    if(!std::isfinite(value))return false;
    const double scaled=static_cast<double>(value)*inverse;
    if(!std::isfinite(scaled) || scaled < -127 || scaled > 127)return false;
    const int code=static_cast<int>(scaled+(scaled>=0 ? 0.5 : -0.5));
    out[i]=static_cast<int8_t>(code);
  }
  return true;
}

} // namespace mvdb

#pragma once
#include "compensated_distance.h"
#include <cstdint>
#include <cstring>

namespace comparison {
inline uint64_t bits(double value) { uint64_t out; std::memcpy(&out,&value,8); return out; }
// Both measured entry points retain their complete calls and cannot be folded
// or inlined into the timing loop. The new wrapper includes attempt guards and
// the unchanged original scalar fallback, never just the bare SIMD kernel.
__attribute__((noinline,noipa)) double old_distance(const float* a,const float* b,size_t n) {
  return mvdb::distance_scalar(a,b,n);
}
__attribute__((noinline,noipa)) double new_distance(const float* a,const float* b,size_t n) {
  return mvdb::compensated_draft::distance(a,b,n);
}
}

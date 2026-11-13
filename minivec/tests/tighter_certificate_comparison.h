#pragma once
#define mvdb retained_mvdb
#include "tighter_certificate_reference_distance.h"
#undef mvdb
#include <distance.h>
#include <cstring>
#include <cstdint>

namespace comparison {
inline uint64_t bits(double value){uint64_t out;std::memcpy(&out,&value,8);return out;}
inline float value(uint32_t bits){float out;std::memcpy(&out,&bits,4);return out;}
// Avoid CSE/IPA across changed FP environments and timed repetitions.
__attribute__((noinline,noipa)) double old_distance(const float* a,const float* b,size_t n){return retained_mvdb::distance_scalar(a,b,n);}
__attribute__((noinline,noipa)) double new_distance(const float* a,const float* b,size_t n){return mvdb::distance_scalar(a,b,n);}
}

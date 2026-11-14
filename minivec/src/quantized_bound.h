#pragma once
#include "distance.h"
#include <vector>

namespace mvdb {
namespace quantized_detail {
inline bool normal_fp_environment() {
#if defined(__FAST_MATH__)
  return false;
#elif defined(MVDB_SIMD_BOUND_X86)
  return (_mm_getcsr() & ((1u<<6)|(1u<<15)|(3u<<13)))==0;
#else
  // No portable C++ query for flush-to-zero controls. Preserve exactness by
  // disabling this optional acceleration on unverified floating environments.
  return false;
#endif
}
inline bool power_of_two(double step) {
  if(!(step>0) || !std::isfinite(step))return false;
  uint64_t bits;std::memcpy(&bits,&step,sizeof(bits));
  const uint64_t fraction=bits&0x000fffffffffffffull;
  return (bits>>52) ? fraction==0 : (fraction && !(fraction&(fraction-1)));
}
inline bool encode(float value,double step,int8_t* out) {
  if(!out || !std::isfinite(value))return false;
  const double scaled=static_cast<double>(value)/step;
  if(!std::isfinite(scaled) || scaled < -127 || scaled > 127)return false;
  // Binary32 scaled by a power of two retains its significand. Around a half
  // integer the addition is exact in double; tiny values still encode as zero.
  // Truncation therefore rounds nearest with half ties away from zero.
  const int code=static_cast<int>(scaled+(scaled>=0 ? 0.5 : -0.5));
  *out=static_cast<int8_t>(code);
  return true;
}
inline uint32_t distance_scalar(const int8_t* a,const int8_t* b,size_t n) {
  uint32_t sum=0;
  for(size_t i=0;i<n;++i){const int d=int(a[i])-int(b[i]);sum+=static_cast<uint32_t>(d*d);}
  return sum;
}
#if defined(MVDB_SIMD_BOUND_X86)
__attribute__((target("sse2,no-avx,no-fma")))
inline uint32_t distance_sse2(const int8_t* a,const int8_t* b,size_t n) {
  const __m128i zero=_mm_setzero_si128();
  __m128i sums=zero;size_t i=0;
  for(;i+16<=n;i+=16){
    const __m128i x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+i));
    const __m128i y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+i));
    const __m128i xs=_mm_cmpgt_epi8(zero,x),ys=_mm_cmpgt_epi8(zero,y);
    const __m128i lo=_mm_sub_epi16(_mm_unpacklo_epi8(x,xs),_mm_unpacklo_epi8(y,ys));
    const __m128i hi=_mm_sub_epi16(_mm_unpackhi_epi8(x,xs),_mm_unpackhi_epi8(y,ys));
    sums=_mm_add_epi32(sums,_mm_madd_epi16(lo,lo));
    sums=_mm_add_epi32(sums,_mm_madd_epi16(hi,hi));
  }
  // Each lane is below 2^31 for n<=65536. Horizontal additions are unsigned
  // modulo 2^32; the final full sum is below 2^32 even for int8 endpoints.
  sums=_mm_add_epi32(sums,_mm_srli_si128(sums,8));
  sums=_mm_add_epi32(sums,_mm_srli_si128(sums,4));
  uint32_t result=static_cast<uint32_t>(_mm_cvtsi128_si32(sums));
  for(;i<n;++i){const int d=int(a[i])-int(b[i]);result+=static_cast<uint32_t>(d*d);}
  return result;
}
__attribute__((target("avx2,no-fma")))
inline uint32_t distance_avx2(const int8_t* a,const int8_t* b,size_t n) {
  __m256i sums=_mm256_setzero_si256();size_t i=0;
  for(;i+16<=n;i+=16){
    const __m256i x=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+i)));
    const __m256i y=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+i)));
    const __m256i d=_mm256_sub_epi16(x,y);
    sums=_mm256_add_epi32(sums,_mm256_madd_epi16(d,d));
  }
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(sums),_mm256_extracti128_si256(sums,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  uint32_t result=static_cast<uint32_t>(_mm_cvtsi128_si32(total));
  for(;i<n;++i){const int d=int(a[i])-int(b[i]);result+=static_cast<uint32_t>(d*d);}
  return result;
}
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
__attribute__((target("avx512f,avx512bw,no-fma")))
inline uint32_t distance_avx512(const int8_t* a,const int8_t* b,size_t n) {
  if(n<32)return distance_avx2(a,b,n);
  __m512i sums=_mm512_setzero_si512();size_t i=0;
  for(;i+32<=n;i+=32){
    const __m512i x=_mm512_cvtepi8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i)));
    const __m512i y=_mm512_cvtepi8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i)));
    const __m512i d=_mm512_sub_epi16(x,y);
    sums=_mm512_add_epi32(sums,_mm512_madd_epi16(d,d));
  }
  // Each lane is <= (65536/16)*255^2 < 2^31. Reduce with hardware
  // modulo additions, preserving the exact unsigned full sum below 2^32.
  const __m256i halves=_mm256_add_epi32(_mm512_castsi512_si256(sums),_mm512_extracti64x4_epi64(sums,1));
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(halves),_mm256_extracti128_si256(halves,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  uint32_t result=static_cast<uint32_t>(_mm_cvtsi128_si32(total));
  for(;i<n;++i){const int d=int(a[i])-int(b[i]);result+=static_cast<uint32_t>(d*d);}
  return result;
}
#endif
#endif
// Exactly 128 readable bytes per operand. Query initialization selects these
// kernels only for n==128; all other lengths keep the original dynamic kernel.
// Independent partial sums remove the loop-carried accumulation dependency.
// Even the int8 endpoints give 128*255^2=8323200, so every partial sum and the
// final reduction are exact in uint32 (and below INT32_MAX).
#if defined(MVDB_SIMD_BOUND_X86)
template<size_t Offset>
__attribute__((target("sse2,no-avx,no-fma"),always_inline))
inline __m128i fixed128_block_sse2(const int8_t* a,const int8_t* b) {
  const __m128i zero=_mm_setzero_si128();
  const __m128i x=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+Offset));
  const __m128i y=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+Offset));
  const __m128i xs=_mm_cmpgt_epi8(zero,x),ys=_mm_cmpgt_epi8(zero,y);
  const __m128i lo=_mm_sub_epi16(_mm_unpacklo_epi8(x,xs),_mm_unpacklo_epi8(y,ys));
  const __m128i hi=_mm_sub_epi16(_mm_unpackhi_epi8(x,xs),_mm_unpackhi_epi8(y,ys));
  return _mm_add_epi32(_mm_madd_epi16(lo,lo),_mm_madd_epi16(hi,hi));
}
__attribute__((target("sse2,no-avx,no-fma")))
inline uint32_t distance_sse2_128(const int8_t* a,const int8_t* b,size_t) {
  const __m128i s0=_mm_add_epi32(fixed128_block_sse2<0>(a,b),fixed128_block_sse2<64>(a,b));
  const __m128i s1=_mm_add_epi32(fixed128_block_sse2<16>(a,b),fixed128_block_sse2<80>(a,b));
  const __m128i s2=_mm_add_epi32(fixed128_block_sse2<32>(a,b),fixed128_block_sse2<96>(a,b));
  const __m128i s3=_mm_add_epi32(fixed128_block_sse2<48>(a,b),fixed128_block_sse2<112>(a,b));
  __m128i sum=_mm_add_epi32(_mm_add_epi32(s0,s1),_mm_add_epi32(s2,s3));
  sum=_mm_add_epi32(sum,_mm_srli_si128(sum,8));
  sum=_mm_add_epi32(sum,_mm_srli_si128(sum,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(sum));
}
template<size_t Offset>
__attribute__((target("avx2,no-fma"),always_inline))
inline __m256i fixed128_block_avx2(const int8_t* a,const int8_t* b) {
  const __m256i x=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+Offset)));
  const __m256i y=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+Offset)));
  const __m256i d=_mm256_sub_epi16(x,y);
  return _mm256_madd_epi16(d,d);
}
__attribute__((target("avx2,no-fma")))
inline uint32_t distance_avx2_128(const int8_t* a,const int8_t* b,size_t) {
  const __m256i s0=_mm256_add_epi32(fixed128_block_avx2<0>(a,b),fixed128_block_avx2<64>(a,b));
  const __m256i s1=_mm256_add_epi32(fixed128_block_avx2<16>(a,b),fixed128_block_avx2<80>(a,b));
  const __m256i s2=_mm256_add_epi32(fixed128_block_avx2<32>(a,b),fixed128_block_avx2<96>(a,b));
  const __m256i s3=_mm256_add_epi32(fixed128_block_avx2<48>(a,b),fixed128_block_avx2<112>(a,b));
  const __m256i sum=_mm256_add_epi32(_mm256_add_epi32(s0,s1),_mm256_add_epi32(s2,s3));
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(sum),_mm256_extracti128_si256(sum,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(total));
}
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
template<size_t Offset>
__attribute__((target("avx512f,avx512bw,no-fma"),always_inline))
inline __m512i fixed128_block_avx512(const int8_t* a,const int8_t* b) {
  const __m512i x=_mm512_cvtepi8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+Offset)));
  const __m512i y=_mm512_cvtepi8_epi16(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+Offset)));
  const __m512i d=_mm512_sub_epi16(x,y);
  return _mm512_madd_epi16(d,d);
}
__attribute__((target("avx512f,avx512bw,no-fma")))
inline uint32_t distance_avx512_128(const int8_t* a,const int8_t* b,size_t) {
  const __m512i s0=fixed128_block_avx512<0>(a,b),s1=fixed128_block_avx512<32>(a,b);
  const __m512i s2=fixed128_block_avx512<64>(a,b),s3=fixed128_block_avx512<96>(a,b);
  const __m512i sum=_mm512_add_epi32(_mm512_add_epi32(s0,s1),_mm512_add_epi32(s2,s3));
  const __m256i halves=_mm256_add_epi32(_mm512_castsi512_si256(sum),_mm512_extracti64x4_epi64(sum,1));
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(halves),_mm256_extracti128_si256(halves,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(total));
}
#endif
#endif
using Distance = uint32_t(*)(const int8_t*,const int8_t*,size_t);
inline Distance choose_distance() {
#if defined(MVDB_SIMD_BOUND_X86)
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
     __builtin_cpu_supports("avx2"))return distance_avx512;
#endif
  if(__builtin_cpu_supports("avx2"))return distance_avx2;
  if(__builtin_cpu_supports("sse2"))return distance_sse2;
#endif
  return distance_scalar;
}
inline Distance choose_query_distance(size_t n) {
  const auto original=choose_distance();
#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_FIXED128_QUANTIZED)
  if(n!=128)return original;
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  if(original==distance_avx512)return distance_avx512_128;
#endif
  if(original==distance_avx2)return distance_avx2_128;
  if(original==distance_sse2)return distance_sse2_128;
#else
  (void)n;
#endif
  return original;
}
inline double up(double x) {
  return std::isfinite(x) ? distance_detail::positive_neighbors(x).upper : x;
}
} // namespace quantized_detail

// Zero signals unavailable/invalid input; zero-valued data uses unit step.
inline double quantization_step(double max_abs) {
  if(!quantized_detail::normal_fp_environment() ||
     !std::isfinite(max_abs) || max_abs<0)return 0;
  if(max_abs==0)return 1;
  int exponent=0;std::frexp(max_abs,&exponent);
  double step=std::ldexp(1.0,exponent-7);
  if(!(step>0) || !std::isfinite(step))return 0;
  // This division is exact and avoids overflow of 127*step for large inputs.
  if(max_abs/step>127)step*=2;
  return quantized_detail::power_of_two(step) ? step : 0;
}
inline bool quantize_coordinate(float value,double step,int8_t* out) {
  return quantized_detail::normal_fp_environment() &&
         quantized_detail::power_of_two(step) &&
         quantized_detail::encode(value,step,out);
}

// Exact integer distance for n<=65536. Invalid length returns conservative zero.
inline uint32_t quantized_distance_squared(const int8_t* a,const int8_t* b,size_t n) {
  if(n>65536 || (!a && n) || (!b && n))return 0;
  static const auto fn=quantized_detail::choose_distance();
  return fn(a,b,n);
}

// The encoded row must use this same step and be produced by quantize_coordinate.
// Query-scoped: FP controls remain unchanged for its lifetime. Threshold cache
// is mutable and this object must not be shared between concurrent threads.
class QuantizedQueryBound {
 public:
  QuantizedQueryBound(const float* query,size_t n,double step):n_(n) {
#if !defined(MVDB_DISABLE_QUANTIZED_BOUND)
    if(!query || n<16 || n>65536 || !quantized_detail::normal_fp_environment() ||
       !quantized_detail::power_of_two(step))return;
    step_squared_=step*step;
    if(!(step_squared_>0) || !std::isfinite(step_squared_))return;
    try {query_.resize(n);} catch(const std::bad_alloc&) {return;}
    for(size_t i=0;i<n;++i) {
      if(!quantized_detail::encode(query[i],step,&query_[i])) {
        std::vector<int8_t>().swap(query_);return;
      }
    }
    root_n_=quantized_detail::up(std::sqrt(static_cast<double>(n)));
    kernel_=quantized_detail::choose_query_distance(n);
    enabled_=true;
#else
    (void)query;(void)step;
#endif
  }
  bool enabled() const {return enabled_;}
  const std::vector<int8_t>& coded_query() const {return query_;}
  bool definitely_greater(const int8_t* row,double threshold) const {
    if(!enabled_ || !row || !std::isfinite(threshold) || threshold<0)return false;
    if(!cached_ || threshold!=cached_threshold_)update_cap(threshold);
    if(!usable_cap_)return false;
    return kernel_(row,query_.data(),n_)>integer_cap_;
  }
 private:
  void update_cap(double threshold) const {
    cached_=true;cached_threshold_=threshold;usable_cap_=false;
    // X=c*A+eX and Q=c*B+eQ, with each coordinate error <=c/2.
    // Hence ||X-Q|| >= c*(sqrt(Qint)-sqrt(n)).
    // Tu is the successor of the canonical threshold. If Qint exceeds this
    // outward-rounded cap, the real squared distance exceeds Tu, so its
    // correctly rounded double is strictly greater than threshold.
    const double tu=quantized_detail::up(threshold);
    if(!std::isfinite(tu))return;
    const double scaled=quantized_detail::up(tu/step_squared_);
    if(!std::isfinite(scaled))return;
    const double root=quantized_detail::up(std::sqrt(scaled));
    if(!std::isfinite(root) || root<4*root_n_)return; // performance fallback only
    const double sum=quantized_detail::up(root+root_n_);
    cap_=quantized_detail::up(sum*sum);
    usable_cap_=std::isfinite(cap_);
    // Every exact uint32_t kernel sum is representable in double. For this
    // nonnegative cap, integer > cap is exactly integer > floor(cap). Saturate
    // before conversion and still run the kernel when rejection is impossible.
    if(usable_cap_)integer_cap_=
        cap_>=static_cast<double>(std::numeric_limits<uint32_t>::max())
        ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(cap_);
  }
  size_t n_=0;
  bool enabled_=false;
  double step_squared_=0,root_n_=0;
  quantized_detail::Distance kernel_=nullptr;
  std::vector<int8_t> query_;
  mutable bool cached_=false,usable_cap_=false;
  mutable uint32_t integer_cap_=0;
  mutable double cached_threshold_=0,cap_=0;
};
} // namespace mvdb


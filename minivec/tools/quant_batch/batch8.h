#pragma once
#include "reference/quantized_bound.h"
#include <limits>

// Independent prototype. No production source is included by relative path.
namespace batch8 {
constexpr size_t width=8, maximum_dimension=65536;
using Kernel=void(*)(const int8_t*,size_t,const int8_t*,size_t,size_t,uint32_t*);

inline bool valid(const int8_t* rows,size_t stride,const int8_t* query,
                  size_t n,size_t count,uint32_t* out) {
  if(count>width || n>maximum_dimension)return false;
  if(!count)return true;
  if(!out)return false;
  if(!n)return true;
  if(!rows || !query || stride<n)return false;
  if(count>1 && stride>(std::numeric_limits<size_t>::max()-n)/(count-1))return false;
  return true;
}

// Unchecked kernels have the same buffer-validity contract as the old row
// kernels. count is 0..8, n is 0..65536, output does not alias any input.
inline void scalar(const int8_t* rows,size_t stride,const int8_t* query,
                   size_t n,size_t count,uint32_t* out) {
  if(!n){for(size_t r=0;r<count;++r)out[r]=0;return;}
  for(size_t r=0;r<count;++r)
    out[r]=mvdb::quantized_detail::distance_scalar(rows+r*stride,query,n);
}

#if defined(MVDB_SIMD_BOUND_X86)
__attribute__((target("avx2,no-fma")))
inline uint32_t horizontal(__m256i sum) {
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(sum),_mm256_extracti128_si256(sum,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(total));
}

__attribute__((target("avx2,no-fma")))
inline void avx2(const int8_t* rows,size_t stride,const int8_t* query,
                 size_t n,size_t count,uint32_t* out) {
  if(!n){for(size_t r=0;r<count;++r)out[r]=0;return;}
  // A short final batch uses unchanged same-ISA row kernels; it never reads
  // nonexistent rows and does not write output slots count..7.
  if(count!=width){
    for(size_t r=0;r<count;++r)
      out[r]=mvdb::quantized_detail::distance_avx2(rows+r*stride,query,n);
    return;
  }
  const int8_t *r0=rows,*r1=rows+stride,*r2=rows+2*stride,*r3=rows+3*stride;
  const int8_t *r4=rows+4*stride,*r5=rows+5*stride,*r6=rows+6*stride,*r7=rows+7*stride;
  __m256i s0=_mm256_setzero_si256(),s1=s0,s2=s0,s3=s0,s4=s0,s5=s0,s6=s0,s7=s0;
  size_t i=0;
  for(;i+16<=n;i+=16){
    const __m256i q=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(query+i)));
#define BATCH8_UPDATE(k) do { \
    const __m256i x=_mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i*>(r##k+i))); \
    const __m256i d=_mm256_sub_epi16(x,q); \
    s##k=_mm256_add_epi32(s##k,_mm256_madd_epi16(d,d)); \
  } while(false)
    BATCH8_UPDATE(0);BATCH8_UPDATE(1);BATCH8_UPDATE(2);BATCH8_UPDATE(3);
    BATCH8_UPDATE(4);BATCH8_UPDATE(5);BATCH8_UPDATE(6);BATCH8_UPDATE(7);
#undef BATCH8_UPDATE
  }
  out[0]=horizontal(s0);out[1]=horizontal(s1);out[2]=horizontal(s2);out[3]=horizontal(s3);
  out[4]=horizontal(s4);out[5]=horizontal(s5);out[6]=horizontal(s6);out[7]=horizontal(s7);
  for(;i<n;++i){
    const int q=int(query[i]);
#define BATCH8_TAIL(k) do {const int d=int(r##k[i])-q;out[k]+=static_cast<uint32_t>(d*d);} while(false)
    BATCH8_TAIL(0);BATCH8_TAIL(1);BATCH8_TAIL(2);BATCH8_TAIL(3);
    BATCH8_TAIL(4);BATCH8_TAIL(5);BATCH8_TAIL(6);BATCH8_TAIL(7);
#undef BATCH8_TAIL
  }
}
#endif

inline Kernel choose() {
#if defined(MVDB_SIMD_BOUND_X86)
  if(__builtin_cpu_supports("avx2"))return avx2;
#endif
  return scalar;
}
inline bool compute(const int8_t* rows,size_t stride,const int8_t* query,
                    size_t n,size_t count,uint32_t* out) {
  if(!valid(rows,stride,query,n,count,out))return false;
  if(!count)return true;
  static const Kernel kernel=choose();
  kernel(rows,stride,query,n,count,out);
  return true;
}
} // namespace batch8

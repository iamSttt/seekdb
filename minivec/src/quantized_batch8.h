#pragma once
#include <cstddef>
#include <cstdint>
#include <immintrin.h>

// Explicit opt-in only. Existing pilot, coordinate-block and local-scale
// variants retain their complete old scan implementation.
#if defined(MVDB_ENABLE_BATCH8_QUANTIZED) && defined(MVDB_SIMD_BOUND_X86) && \
    !defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && \
    !(defined(MVDB_PILOT_COUNT) && MVDB_PILOT_COUNT > 0) && \
    !(defined(MVDB_QUANTIZED_BLOCK_SIZE) && MVDB_QUANTIZED_BLOCK_SIZE > 0)
#define MVDB_BATCH8_GLOBAL_SCAN 1
namespace mvdb { namespace quantized_batch8 {
__attribute__((target("avx2,no-fma")))
inline uint32_t horizontal(__m256i sum) {
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(sum),_mm256_extracti128_si256(sum,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(total));
}

// Exactly eight existing rows, n <= 65536, stride >= n; output is disjoint.
__attribute__((target("avx2,no-fma")))
inline void compute(const int8_t* rows,size_t stride,const int8_t* query,
                    size_t n,uint32_t* out) {
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
} }
#endif

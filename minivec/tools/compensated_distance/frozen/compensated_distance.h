#pragma once
// EXPERIMENTAL, NOT PRODUCTION. Compile with strict FP and -ffp-contract=off.
#include "reference_distance.h"
#include <cfenv>
#include <cfloat>
#include <cstdint>
#include <limits>

namespace mvdb { namespace compensated_draft {
enum class Outcome { certified, unsupported, environment, shape, coordinate, boundary };
struct Attempt {
  Outcome outcome=Outcome::unsupported;
  double value=0;
  long double approximation=0,error_bound=0;
};

inline bool coordinate_admitted(uint32_t a,uint32_t b) {
  const unsigned ea=(a>>23)&255,eb=(b>>23)&255;
  if(ea==255 || eb==255)return false;
  if(!(a&0x7fffffff) || !(b&0x7fffffff))return true;
  // Binary32 = signed M * 2^(max(E,1)-150), M has <=24 bits.
  const unsigned x=ea?ea:1,y=eb?eb:1;
  return x<=y+28 && y<=x+28;
}

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__FAST_MATH__) && LDBL_MANT_DIG == 64
inline bool available() {
  static const bool yes=[] {__builtin_cpu_init();return
      bool(__builtin_cpu_supports("avx2")) && bool(__builtin_cpu_supports("fma"));}();
  return yes;
}
inline bool environment_admitted() {
  const unsigned mx=_mm_getcsr();
  unsigned short cw=0;
  __asm__ __volatile__("fnstcw %0" : "=m"(cw));
  // RN, no DAZ/FTZ, all exceptions masked; x87 must use full 64-bit precision.
  return !(mx&((1u<<6)|(1u<<15)|(3u<<13))) && (mx&0x1f80)==0x1f80 &&
      (cw&0x0f00)==0x0300 && (cw&0x3f)==0x3f;
}

#if defined(__clang__)
__attribute__((target("avx2,fma"),noinline))
#else
__attribute__((target("avx2,fma"),optimize("no-fast-math","fp-contract=off"),noinline,noclone))
#endif
inline Attempt kernel(const float* a,const float* b,size_t n) {
  const __m128i zero=_mm_setzero_si128(),abs_mask=_mm_set1_epi32(0x7fffffff);
  const __m128i exponent_mask=_mm_set1_epi32(255),one=_mm_set1_epi32(1);
  const __m128i gap_limit=_mm_set1_epi32(28);
  __m256d high=_mm256_setzero_pd(),low=_mm256_setzero_pd();
  for(size_t i=0;i<n;i+=4) {
    const __m128i ab=_mm_loadu_si128(reinterpret_cast<const __m128i*>(a+i));
    const __m128i bb=_mm_loadu_si128(reinterpret_cast<const __m128i*>(b+i));
    const __m128i aa=_mm_and_si128(ab,abs_mask),ba=_mm_and_si128(bb,abs_mask);
    const __m128i ae=_mm_and_si128(_mm_srli_epi32(ab,23),exponent_mask);
    const __m128i be=_mm_and_si128(_mm_srli_epi32(bb,23),exponent_mask);
    const __m128i nonfinite=_mm_or_si128(_mm_cmpeq_epi32(ae,exponent_mask),_mm_cmpeq_epi32(be,exponent_mask));
    const __m128i either_zero=_mm_or_si128(_mm_cmpeq_epi32(aa,zero),_mm_cmpeq_epi32(ba,zero));
    const __m128i gap=_mm_abs_epi32(_mm_sub_epi32(_mm_max_epi32(ae,one),_mm_max_epi32(be,one)));
    const __m128i bad_gap=_mm_andnot_si128(either_zero,_mm_cmpgt_epi32(gap,gap_limit));
    if(_mm_movemask_epi8(_mm_or_si128(nonfinite,bad_gap)))
      return {Outcome::coordinate};
    const __m256d d=_mm256_sub_pd(_mm256_cvtps_pd(_mm_castsi128_ps(ab)),
                                _mm256_cvtps_pd(_mm_castsi128_ps(bb)));
    const __m256d product=_mm256_mul_pd(d,d);
    const __m256d residual=_mm256_fmsub_pd(d,d,product);
    // Knuth TwoSum, no magnitude ordering assumption and no reassociation.
    const __m256d sum=_mm256_add_pd(high,product);
    const __m256d z=_mm256_sub_pd(sum,high);
    const __m256d error=_mm256_add_pd(_mm256_sub_pd(high,_mm256_sub_pd(sum,z)),
                                    _mm256_sub_pd(product,z));
    low=_mm256_add_pd(low,_mm256_add_pd(error,residual));
    high=sum;
  }
  alignas(32) double h[4],l[4];
  _mm256_store_pd(h,high);_mm256_store_pd(l,low);
  const long double hs=(static_cast<long double>(h[0])+h[1])+
                       (static_cast<long double>(h[2])+h[3]);
  const long double ls=(static_cast<long double>(l[0])+l[1])+
                       (static_cast<long double>(l[2])+l[3]);
  const long double sum=hs+ls;
  Attempt answer;answer.outcome=Outcome::boundary;answer.approximation=sum;
  if(sum==0) {answer.outcome=Outcome::certified;return answer;}
  if(!(sum>0) || !std::isfinite(sum))return answer;
  const long double epsd=std::numeric_limits<double>::epsilon();
  const long double epsl=std::numeric_limits<long double>::epsilon();
  const long double length=static_cast<long double>(n)+16;
  const long double infinity=std::numeric_limits<long double>::infinity();
  const long double factor=std::nextafter(32*epsl+32*length*length*epsd*epsd,infinity);
  const long double error=std::nextafter(sum*factor,infinity);
  answer.error_bound=error;
  const double rounded=static_cast<double>(sum);
  if(!(rounded>0) || !std::isfinite(rounded))return answer;
  const auto neighbours=distance_detail::positive_neighbors(rounded);
  const long double lower=(static_cast<long double>(rounded)+neighbours.lower)*0.5L;
  const long double upper=(static_cast<long double>(rounded)+neighbours.upper)*0.5L;
  if(sum-lower>error && upper-sum>error) {
    answer.value=rounded;answer.outcome=Outcome::certified;
  }
  return answer;
}
#else
inline bool available(){return false;}
inline bool environment_admitted(){return false;}
#endif

inline Attempt attempt(const float* a,const float* b,size_t n) {
  if(!a || !b || n<16 || n>4096 || n%4)return {Outcome::shape};
  if(!available())return {Outcome::unsupported};
  if(!environment_admitted())return {Outcome::environment};
#if defined(__x86_64__) && defined(__GNUC__) && !defined(__FAST_MATH__) && LDBL_MANT_DIG == 64
  return kernel(a,b,n);
#else
  return {Outcome::unsupported};
#endif
}

inline double distance(const float* a,const float* b,size_t n) {
  const Attempt result=attempt(a,b,n);
  return result.outcome==Outcome::certified ? result.value : mvdb::distance_scalar(a,b,n);
}
}} // namespace mvdb::compensated_draft

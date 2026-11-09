#pragma once
#include <cstddef>
#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

namespace mvdb {
inline double distance_scalar(const float* a,const float* b,size_t n) {
  double s0=0,s1=0,s2=0,s3=0;
  size_t i=0;
  for(;i+4<=n;i+=4) {
    double d0=double(a[i])-double(b[i]);
    double d1=double(a[i+1])-double(b[i+1]);
    double d2=double(a[i+2])-double(b[i+2]);
    double d3=double(a[i+3])-double(b[i+3]);
    s0+=d0*d0;s1+=d1*d1;s2+=d2*d2;s3+=d3*d3;
  }
  for(;i<n;++i){double d=double(a[i])-double(b[i]);s0+=d*d;}
  return (s0+s1)+(s2+s3);
}
#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2"))) inline double distance_avx2(const float* a,const float* b,size_t n) {
  __m256d sum=_mm256_setzero_pd();size_t i=0;
  for(;i+4<=n;i+=4) {
    auto d=_mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(a+i)),_mm256_cvtps_pd(_mm_loadu_ps(b+i)));
    sum=_mm256_add_pd(sum,_mm256_mul_pd(d,d));
  }
  double lanes[4];_mm256_storeu_pd(lanes,sum);
  for(;i<n;++i){double d=double(a[i])-double(b[i]);lanes[0]+=d*d;}
  return (lanes[0]+lanes[1])+(lanes[2]+lanes[3]);
}
#endif
inline double distance_squared(const float* a,const float* b,size_t n) {
  using Kernel=double(*)(const float*,const float*,size_t);
#if defined(__x86_64__) && defined(__GNUC__)
  static Kernel kernel=__builtin_cpu_supports("avx2") ? distance_avx2 : distance_scalar;
#else
  static Kernel kernel=distance_scalar;
#endif
  return kernel(a,b,n);
}
}

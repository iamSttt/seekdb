#pragma once
#include <cmath>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define MVDB_SIMD_BOUND_X86 1
#endif

namespace mvdb {
namespace distance_detail {
struct Neighbors { double lower, upper; };
// Precondition: finite nonnegative input. IEEE binary64 encodings are ordered
// by value here, including the subnormal/normal transition and max -> infinity.
inline Neighbors positive_neighbors(double value) {
  static_assert(sizeof(double)==8 && std::numeric_limits<double>::is_iec559 &&
                std::numeric_limits<double>::digits==53,"IEEE binary64 required");
  uint64_t bits=0;
  if(value!=0)std::memcpy(&bits,&value,sizeof(bits));
  const uint64_t lo=bits ? bits-1 : 0,hi=bits+1;
  Neighbors result;
  std::memcpy(&result.lower,&lo,sizeof(lo));
  std::memcpy(&result.upper,&hi,sizeof(hi));
  return result;
}
// A finite binary32 is an integer times 2^-149. Its squared distance is
// therefore an integer times 2^-298. Ten limbs cover every such sum even
// for SIZE_MAX coordinates on a 64-bit platform: the sum is below 2^620.
// No floating-point subtraction, multiplication or partial summation can
// lose bits here; only the final conversion rounds, to nearest/even.
struct ExactSum {
  uint64_t words[10]{};

  void add_word(size_t i,uint64_t value) {
    while(value) {
      const uint64_t before=words[i];
      words[i]=before+value;
      value=words[i]<before;
      ++i;
    }
  }
  void subtract_word(size_t i,uint64_t value) {
    while(value) {
      const uint64_t before=words[i];
      words[i]=before-value;
      value=before<value;
      ++i;
    }
  }
  void add(uint64_t value,unsigned shift) {
    const unsigned offset=shift%64;
    add_word(shift/64,value<<offset);
    if(offset)add_word(shift/64+1,value>>(64-offset));
  }
  void subtract(uint64_t value,unsigned shift) {
    const unsigned offset=shift%64;
    subtract_word(shift/64,value<<offset);
    if(offset)subtract_word(shift/64+1,value>>(64-offset));
  }
  double rounded() const {
    size_t high=10;
    while(high && words[high-1]==0)--high;
    if(!high)return 0;
    --high;
    unsigned bit=0;
#if defined(__GNUC__)
    bit=63-static_cast<unsigned>(__builtin_clzll(words[high]));
#else
    for(uint64_t v=words[high];v>>1;v>>=1)++bit;
#endif
    const unsigned highest=static_cast<unsigned>(high*64)+bit;
    if(highest<=52)return std::ldexp(static_cast<double>(words[0]),-298);
    const unsigned shift=highest-52,word=shift/64,offset=shift%64;
    uint64_t top=words[word]>>offset;
    if(offset && word+1<10)top|=words[word+1]<<(64-offset);
    const unsigned round_word=(shift-1)/64,round_bit=(shift-1)%64;
    if((words[round_word]>>round_bit)&1) {
      bool sticky=(words[round_word]&((uint64_t(1)<<round_bit)-1))!=0;
      for(unsigned i=0;i<round_word;++i)sticky=sticky || words[i]!=0;
      if(sticky || (top&1))++top;
    }
    return std::ldexp(static_cast<double>(top),static_cast<int>(shift)-298);
  }
};
}

inline double distance_exact(const float* a,const float* b,size_t n) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559 &&
                std::numeric_limits<float>::digits==24,"binary32 coordinates required");
  static_assert(std::numeric_limits<double>::digits==53 && sizeof(size_t)<=8,
                "binary64 distances and at most 64-bit sizes required");
  distance_detail::ExactSum sum;
  for(size_t i=0;i<n;++i) {
    uint32_t x,y;
    std::memcpy(&x,a+i,sizeof(x));std::memcpy(&y,b+i,sizeof(y));
    const unsigned ex=(x>>23)&255,ey=(y>>23)&255;
    if(ex==255 || ey==255) {
      // Database inputs are finite; retain ordinary IEEE behavior for callers
      // that use this helper directly with infinities or NaNs.
      double result=0;
      for(size_t j=0;j<n;++j){const double d=double(a[j])-double(b[j]);result+=d*d;}
      return result;
    }
    if(x==y)continue;
    const uint64_t mx=(x&0x7fffff)|(ex?0x800000:0);
    const uint64_t my=(y&0x7fffff)|(ey?0x800000:0);
    const unsigned sx=ex?ex-1:0,sy=ey?ey-1:0;
    if(sx<=sy+7 && sy<=sx+7) {
      // Nearby exponents permit an exact aligned difference whose square
      // fits in one uint64_t, avoiding three products and a subtraction.
      const unsigned shift=sx<sy?sx:sy;
      const uint64_t ax=mx<<(sx-shift),by=my<<(sy-shift);
      const uint64_t delta=(x^y)>>31 ? ax+by : (ax>=by?ax-by:by-ax);
      sum.add(delta*delta,2*shift);
      continue;
    }
    // Add the squares before subtracting 2ab, keeping every intermediate
    // accumulator nonnegative, including when a and b are nearly equal.
    sum.add(mx*mx,2*sx);sum.add(my*my,2*sy);
    if((x^y)>>31)sum.add(mx*my,sx+sy+1);
    else sum.subtract(mx*my,sx+sy+1);
  }
  return sum.rounded();
}

inline double distance_scalar(const float* a,const float* b,size_t n) {
#if LDBL_MANT_DIG >= 64
  // Extended precision usually determines the correctly rounded double
  // without integer accumulation. Certify its entire error interval; ties
  // and uncertain cases still use the exact finite-float32 calculation.
  if(n<=uint64_t(1)<<32) {
    long double s0=0,s1=0,s2=0,s3=0;
    size_t i=0;
    for(;i+4<=n;i+=4) {
      const long double d0=static_cast<long double>(a[i])-b[i];
      const long double d1=static_cast<long double>(a[i+1])-b[i+1];
      const long double d2=static_cast<long double>(a[i+2])-b[i+2];
      const long double d3=static_cast<long double>(a[i+3])-b[i+3];
      s0+=d0*d0;s1+=d1*d1;s2+=d2*d2;s3+=d3*d3;
    }
    for(;i<n;++i){const long double d=static_cast<long double>(a[i])-b[i];s0+=d*d;}
    const long double sum=(s0+s1)+(s2+s3);
    const double result=static_cast<double>(sum);
    if(sum>0 && std::isfinite(result)) {
      // Each term has three rounding-error factors (subtraction twice,
      // then multiplication), plus at most n/4+3 lane additions and two
      // reduction additions. With n<=2^32 and at least 64 mantissa bits,
      // this deliberately loose bound also covers rounding of the bound
      // itself, and conversion from relative exact to computed-sum error.
      const long double error=sum*(static_cast<long double>(n/4)+8)*
        (2*std::numeric_limits<long double>::epsilon());
      const auto neighbors=distance_detail::positive_neighbors(result);
      const long double lower=(static_cast<long double>(result)+neighbors.lower)*0.5L;
      const long double upper=(static_cast<long double>(result)+
        neighbors.upper)*0.5L;
      if(sum-lower>error && upper-sum>error)return result;
    }
  }
#endif
  return distance_exact(a,b,n);
}

// Conservative rejection only: true proves distance_exact(a,b,n) > threshold.
// Uses the same round-to-nearest IEEE model as distance_scalar; no fast-math.
inline bool distance_definitely_greater_double(const float* a,const float* b,size_t n,
                                        double threshold) {
  if(!n || n>(uint64_t(1)<<32) || !std::isfinite(threshold) || threshold<0)
    return false;
  double s0=0,s1=0,s2=0,s3=0;
  size_t i=0;
  for(;i+4<=n;i+=4) {
    const double d0=static_cast<double>(a[i])-b[i];
    const double d1=static_cast<double>(a[i+1])-b[i+1];
    const double d2=static_cast<double>(a[i+2])-b[i+2];
    const double d3=static_cast<double>(a[i+3])-b[i+3];
    s0+=d0*d0;s1+=d1*d1;s2+=d2*d2;s3+=d3*d3;
  }
  for(;i<n;++i){const double d=static_cast<double>(a[i])-b[i];s0+=d*d;}
  const double sum=(s0+s1)+(s2+s3);
  if(!std::isfinite(sum) || !(sum>threshold))return false;
  // For finite float32 inputs, every nonzero difference is >=2^-149;
  // squares and sums are normal binary64, and n<=2^32 cannot overflow.
  // Set u=epsilon/2, k=n+6. Counting subtraction twice through its square,
  // multiplication, lane accumulation and reduction gives relative error
  // <=gamma_k<=2*k*u against the exact real sum D. Since k*u<2^-20,
  // conversion to computed-sum error gives |sum-D|<=4*k*u*sum.
  // Rounding D once to binary64 adds <=2*u*sum, so the total is bounded
  // by (4*n+26)*u*sum. We use the larger 16*(n+8)*u*sum instead.
  // The factor is exact (small integer times a power of two); next-up of
  // the product and next-down of the subtraction make the bound outward.
  const double factor=(static_cast<double>(n)+8)*
                      (8*std::numeric_limits<double>::epsilon());
  const double error=distance_detail::positive_neighbors(sum*factor).upper;
  const double lower_candidate=sum-error;
  if(!(lower_candidate>0) || !std::isfinite(lower_candidate))return false;
  const double lower=distance_detail::positive_neighbors(lower_candidate).lower;
  return lower>threshold;
}

namespace distance_detail {
#if defined(MVDB_SIMD_BOUND_X86)
__attribute__((target("sse2,no-avx,no-fma")))
inline double bound_sum_sse2(const float* a,const float* b,size_t n) {
  __m128 s0=_mm_setzero_ps(),s1=_mm_setzero_ps();
  size_t i=0;
  for(;i+8<=n;i+=8) {
    const __m128 d0=_mm_sub_ps(_mm_loadu_ps(a+i),_mm_loadu_ps(b+i));
    const __m128 d1=_mm_sub_ps(_mm_loadu_ps(a+i+4),_mm_loadu_ps(b+i+4));
    s0=_mm_add_ps(s0,_mm_mul_ps(d0,d0));s1=_mm_add_ps(s1,_mm_mul_ps(d1,d1));
  }
#if defined(MVDB_SIMD_DOUBLE_REDUCTION)
  float lanes[8];_mm_storeu_ps(lanes,s0);_mm_storeu_ps(lanes+4,s1);
  double sum=0;for(float lane:lanes)sum+=lane;
#else
  // Positive-term tree: SSE2 shuffle/add only, no SSE3 horizontal add or
  // lane spill. If the float sum overflows, the caller uses the double bound.
  __m128 total=_mm_add_ps(s0,s1);
  total=_mm_add_ps(total,_mm_movehl_ps(total,total));
  total=_mm_add_ss(total,_mm_shuffle_ps(total,total,_MM_SHUFFLE(1,1,1,1)));
  double sum=static_cast<double>(_mm_cvtss_f32(total));
#endif
  for(;i<n;++i){const float d=a[i]-b[i];const float term=d*d;sum+=term;}
  return sum;
}
__attribute__((target("avx2,no-fma")))
inline double bound_sum_avx2(const float* a,const float* b,size_t n) {
  __m256 s0=_mm256_setzero_ps(),s1=_mm256_setzero_ps();
  size_t i=0;
  for(;i+16<=n;i+=16) {
    const __m256 d0=_mm256_sub_ps(_mm256_loadu_ps(a+i),_mm256_loadu_ps(b+i));
    const __m256 d1=_mm256_sub_ps(_mm256_loadu_ps(a+i+8),_mm256_loadu_ps(b+i+8));
    s0=_mm256_add_ps(s0,_mm256_mul_ps(d0,d0));s1=_mm256_add_ps(s1,_mm256_mul_ps(d1,d1));
  }
#if defined(MVDB_SIMD_DOUBLE_REDUCTION)
  float lanes[16];_mm256_storeu_ps(lanes,s0);_mm256_storeu_ps(lanes+8,s1);
  double sum=0;for(float lane:lanes)sum+=lane;
#else
  const __m256 combined=_mm256_add_ps(s0,s1);
  __m128 total=_mm_add_ps(_mm256_castps256_ps128(combined),
                         _mm256_extractf128_ps(combined,1));
  total=_mm_add_ps(total,_mm_movehl_ps(total,total));
  total=_mm_add_ss(total,_mm_shuffle_ps(total,total,_MM_SHUFFLE(1,1,1,1)));
  double sum=static_cast<double>(_mm_cvtss_f32(total));
#endif
  for(;i<n;++i){const float d=a[i]-b[i];const float term=d*d;sum+=term;}
  return sum;
}
using BoundSum = double(*)(const float*,const float*,size_t);
inline BoundSum choose_bound_sum() {
  if(__builtin_cpu_supports("avx2"))return bound_sum_avx2;
  if(__builtin_cpu_supports("sse2"))return bound_sum_sse2;
  return nullptr;
}
#endif

inline bool float_sum_definitely_greater(double sum,size_t n,double threshold) {
  if(!std::isfinite(sum) || !(sum>threshold))return false;
  // u=2^-24, eta=2^-150. A subnormal float32 subtraction is exact;
  // multiplication contributes at most eta absolute rounding error. Positive
  // float additions that remain subnormal are exact. Either lane reduction
  // (float tree or reference double serial) forms a positive-term addition
  // tree with at most n-1 nonzero additions on any path. Thus the computed
  // S <= (D+n*eta)*(1+u)^(n+12), for exact D.
  // Bernoulli gives D >= S*(1-(n+12)*u)-n*eta. The deliberately looser error
  // 4*(n+12)*u*S + 4*n*eta also covers rounding D to binary64. n<=65536,
  // so the relative factor stays below .016. All bound arithmetic is double,
  // and each error operation rounds upward, the final subtraction downward.
  const double relative=(static_cast<double>(n)+12)*0x1p-22;
  const double absolute=static_cast<double>(n)*0x1p-148;
  const double product=positive_neighbors(sum*relative).upper;
  const double error=positive_neighbors(product+absolute).upper;
  const double lower=sum-error;
  return lower>0 && positive_neighbors(lower).lower>threshold;
}
}

inline bool distance_definitely_greater(const float* a,const float* b,size_t n,
                                        double threshold) {
#if defined(MVDB_SIMD_BOUND_X86)
  // The fallback double bound assumes the same floating-point environment.
  // DAZ can change float inputs even during conversion to double, so guard
  // every dimension, including paths that never execute the SIMD kernel.
  if(_mm_getcsr() & ((1u<<6)|(1u<<15)|(3u<<13)))return false;
#endif
#if defined(MVDB_DISABLE_SIMD_BOUND)
  return distance_definitely_greater_double(a,b,n,threshold);
#else
#if defined(MVDB_SIMD_BOUND_X86) && !defined(__FAST_MATH__)
  if(n>=16 && n<=65536 && std::isfinite(threshold) && threshold>=0) {
    static const auto sum_function=distance_detail::choose_bound_sum();
    if(sum_function) {
      const double sum=sum_function(a,b,n);
      if(std::isfinite(sum))
        return distance_detail::float_sum_definitely_greater(sum,n,threshold);
      // Float overflow is common for valid extreme coordinates. The original
      // double bound remains safe over the complete finite float32 domain.
    }
  }
#endif
  return distance_definitely_greater_double(a,b,n,threshold);
#endif
}

inline double distance_squared(const float* a,const float* b,size_t n) {
  // Exact for all finite float32 coordinates, then rounded once to double.
  // Different exact distances that round to the same double remain tied.
  return distance_scalar(a,b,n);
}
}

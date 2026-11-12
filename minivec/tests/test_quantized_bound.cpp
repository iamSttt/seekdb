#include "quantized_bound.h"
#include "distance.h"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace mvdb;
static uint64_t random_state=0xa29f461c37d85b60ull;
static uint64_t random_bits() {
  random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;
  return random_state;
}
static void require(bool condition,const char* message) {
  if(!condition){std::cerr<<"FAIL "<<message<<"\n";std::exit(1);}
}
static float float_bits(uint32_t bits) {
  float value;std::memcpy(&value,&bits,sizeof(value));return value;
}
// Independent wide scalar oracle: no SIMD, horizontal reduction, cached cap or
// floating-point approximation is shared with the implementation under test.
static uint64_t integer_oracle(const int8_t* a,const int8_t* b,size_t n) {
  uint64_t sum=0;
  for(size_t i=0;i<n;++i) {
    const int64_t difference=int64_t(a[i])-int64_t(b[i]);
    sum+=static_cast<uint64_t>(difference*difference);
  }
  return sum;
}
static uint32_t norm_oracle(const int8_t* a,size_t n) {
  uint64_t sum=0;
  for(size_t i=0;i<n;++i) {const int64_t x=a[i];sum+=static_cast<uint64_t>(x*x);}
  require(sum<=UINT32_MAX,"code norm fits uint32");
  return static_cast<uint32_t>(sum);
}
static void check_integer_result(const std::vector<int8_t>& a,const std::vector<int8_t>& b) {
  const uint64_t expected=integer_oracle(a.data(),b.data(),a.size());
  require(quantized_distance_squared(a.data(),b.data(),a.size())==expected,
          "integer kernel differs from uint64 scalar oracle");
#if defined(MVDB_SIMD_BOUND_X86)
  if(__builtin_cpu_supports("sse2"))
    require(quantized_detail::distance_sse2(a.data(),b.data(),a.size())==expected,"SSE2 uint64 oracle");
  if(__builtin_cpu_supports("avx2"))
    require(quantized_detail::distance_avx2(a.data(),b.data(),a.size())==expected,"AVX2 uint64 oracle");
#ifndef MVDB_DISABLE_QUANTIZED_DOT
  // -128 is deliberately tested by the old distance API, but is outside the
  // quantizer/dot contract. Never silently extend the saturation proof to it.
  if(__builtin_cpu_supports("avx2") &&
     std::all_of(a.begin(),a.end(),[](int8_t x){return x>=-127;}) &&
     std::all_of(b.begin(),b.end(),[](int8_t x){return x>=-127;}))
    require(quantized_detail::distance_dot_avx2(a.data(),b.data(),a.size(),
                norm_oracle(a.data(),a.size()),norm_oracle(b.data(),b.size()))==expected,
            "AVX2 dot differs from independent uint64 squared-distance oracle");
#endif
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx2"))
    require(quantized_detail::distance_avx512(a.data(),b.data(),a.size())==expected,"AVX512 uint64 oracle");
#endif
#endif
}
static void check_integer_kernel() {
  for(size_t n:{size_t(0),size_t(1),size_t(15),size_t(16),size_t(17),size_t(31),
                size_t(32),size_t(127),size_t(128),size_t(129),size_t(65535),size_t(65536)}) {
    std::vector<int8_t> a(n),b(n);
    for(size_t i=0;i<n;++i) {
      a[i]=static_cast<int8_t>(int(random_bits()%255)-127);
      b[i]=static_cast<int8_t>(int(random_bits()%255)-127);
    }
    check_integer_result(a,b);
    std::fill(a.begin(),a.end(),int8_t(-127));std::fill(b.begin(),b.end(),int8_t(127));
    const uint64_t expected=integer_oracle(a.data(),b.data(),n);
    require(quantized_distance_squared(a.data(),b.data(),n)==expected,
            "maximum signed-code difference or unsigned reduction overflow");
    if(n==65536) {
      require(expected==uint64_t(65536)*254*254,"maximum legal integer distance");
      require(expected>uint64_t(INT32_MAX) && expected<UINT32_MAX,
              "test crosses signed32 and approaches unsigned32 boundary");
    }
    require(quantized_distance_squared(a.data(),a.data(),n)==0,"identical code distance");
    check_integer_result(a,b);
    check_integer_result(a,a);check_integer_result(b,b);
    std::fill(a.begin(),a.end(),int8_t(-128));check_integer_result(a,b);
  }
}
static void check_dot_unaligned() {
#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_QUANTIZED_DOT)
  if(!__builtin_cpu_supports("avx2"))return;
  for(size_t n:{size_t(0),size_t(1),size_t(17),size_t(31),size_t(32),size_t(33),
                size_t(127),size_t(129),size_t(65535),size_t(65536)}) {
    std::vector<int8_t> a(n+3),b(n+3);
    for(size_t i=0;i<n;++i) {
      a[i+1]=static_cast<int8_t>(int(random_bits()%255)-127);
      b[i+2]=static_cast<int8_t>(int(random_bits()%255)-127);
    }
    require(quantized_detail::distance_dot_avx2(a.data()+1,b.data()+2,n,
                norm_oracle(a.data()+1,n),norm_oracle(b.data()+2,n))==
                integer_oracle(a.data()+1,b.data()+2,n),"unaligned/tail dot uint64 oracle");
  }
#endif
}
static std::vector<int8_t> encode(const std::vector<float>& values,double step) {
  std::vector<int8_t> result(values.size());
  for(size_t i=0;i<values.size();++i) {
    require(quantize_coordinate(values[i],step,&result[i]),"generated step must encode finite input");
    require(result[i]>=-127 && result[i]<=127,"legal signed quantization range");
    // A float32 and an integer times a power of two are dyadic numbers.
    // Long double is used only for this independent error check, never to
    // reconstruct the implementation's rejection threshold.
    const long double error=std::fabs(static_cast<long double>(values[i])-
        static_cast<long double>(result[i])*static_cast<long double>(step));
    require(error<=static_cast<long double>(step)*0.5L,"coordinate error exceeds half a step");
  }
  return result;
}
static size_t rejections=0,checked_pairs=0;
static void check_pair(const std::vector<float>& row,const std::vector<float>& query,double forced_step=0) {
  const size_t n=row.size();require(query.size()==n,"test dimension agreement");
  double max_abs=0;
  for(float value:row)max_abs=std::max(max_abs,std::fabs(double(value)));
  for(float value:query)max_abs=std::max(max_abs,std::fabs(double(value)));
  const double step=forced_step>0 ? forced_step : quantization_step(max_abs);
  require(step>0 && std::isfinite(step),"finite data produces positive finite step");
  int exponent=0;require(std::frexp(step,&exponent)==0.5,"step is a power of two");
  const auto coded_row=encode(row,step),coded_query=encode(query,step);
  if(n<=65536)require(quantized_distance_squared(coded_row.data(),coded_query.data(),n)==
      integer_oracle(coded_row.data(),coded_query.data(),n),"pair integer distance oracle");
  const QuantizedQueryBound bound(query.data(),n,step);
  const QuantizedQueryBound dot(query.data(),n,step,true);
  const uint32_t row_norm=norm_oracle(coded_row.data(),n);
  bool supported=n>=16 && n<=65536;
#ifdef MVDB_DISABLE_QUANTIZED_BOUND
  supported=false;
#endif
  require(bound.enabled()==supported,"default-FP generated-step dimension eligibility");
  require(dot.enabled()==supported && dot.dot_enabled()==(supported && n>=32 && quantized_dot_available()),
          "dot eligibility follows optional AVX2-only dispatch");
  if(supported)require(bound.coded_query()==coded_query,"query code equals independently encoded coordinates");
  const double exact=distance_exact(row.data(),query.data(),n);
  const std::vector<double> thresholds={0.0,-0.0,exact*0.25,std::nextafter(exact,0.0),exact,
      std::nextafter(exact,std::numeric_limits<double>::infinity()),exact*2,
      std::numeric_limits<double>::max(),std::numeric_limits<double>::min(),
      -1.0,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()};
  // Tighten and loosen repeatedly, including the same cap twice. This catches
  // stale cached thresholds without duplicating their calculation in the test.
  for(size_t pass=0;pass<2;++pass)for(size_t i=0;i<thresholds.size();++i) {
    const double threshold=thresholds[pass?thresholds.size()-1-i:i];
    const bool rejected=bound.definitely_greater(coded_row.data(),threshold);
    require(dot.definitely_greater(coded_row.data(),threshold,row_norm)==rejected,
            "dot rejection differs from the exact integer distance kernel");
    require(dot.definitely_greater(coded_row.data(),threshold)==rejected,
            "old rejection overload remains valid with a dot-enabled context");
    require(!rejected || exact>threshold,"quantized bound rejected canonical exact <= threshold");
    if(!supported || !std::isfinite(threshold) || threshold<0)
      require(!rejected,"unsupported query or threshold must not reject");
    require(rejected==QuantizedQueryBound(query.data(),n,step).definitely_greater(coded_row.data(),threshold),
            "cached threshold and a fresh query context differ");
    rejections+=rejected;
  }
  ++checked_pairs;
}
static void check_coordinates() {
  for(int exponent:{-150,-100,-30,0,30,100,122}) {
    const double step=std::ldexp(1.0,exponent);
    for(int integer=-126;integer<=126;++integer) {
      const float midpoint=static_cast<float>((double(integer)+0.5)*step);
      if(!std::isfinite(midpoint))continue;
      for(float value:{std::nextafter(midpoint,-std::numeric_limits<float>::infinity()),
                       midpoint,std::nextafter(midpoint,std::numeric_limits<float>::infinity())}) {
        if(std::fabs(double(value))>127*step)continue;
        encode({value},step);
      }
    }
  }
  for(float value:{0.5f,-0.5f,1.5f,-1.5f,126.5f,-126.5f}) {
    int8_t code=0;require(quantize_coordinate(value,1,&code),"midpoint must encode");
    const int expected=value>0 ? int(value+0.5f) : int(value-0.5f);
    require(code==expected,"midpoint ties are away from zero");
  }
  for(float value:{127.0f,-127.0f})encode({value},1);
  int8_t code=0;
  for(float value:{std::nextafter(127.0f,std::numeric_limits<float>::infinity()),
                   std::nextafter(-127.0f,-std::numeric_limits<float>::infinity()),
                   std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
    require(!quantize_coordinate(value,1,&code),"out-of-range or non-finite coordinate");
}
static void check_pairs() {
  const size_t dimensions[]={16,17,31,128,129,511};
  for(size_t trial=0;trial<2400;++trial) {
    const size_t n=dimensions[(trial/8)%6];std::vector<float> row(n),query(n);
    for(size_t i=0;i<n;++i) {
      const uint32_t x=random_bits(),y=random_bits();
      switch(trial%8) {
        case 0: row[i]=float_bits((x&0x807fffff)|((x%255)<<23));
                query[i]=float_bits((y&0x807fffff)|((y%255)<<23));break;
        case 1: row[i]=float(int(x%20001)-10000)/10000;
                query[i]=float(int(y%20001)-10000)/10000;break;
        case 2: row[i]=float_bits(x&0x807fffff);query[i]=float_bits(y&0x807fffff);break;
        case 3: row[i]=std::ldexp(float(1+x%16),-100-int(x%40));query[i]=-row[i];break;
        case 4: row[i]=float_bits((x&0x807fffff)|((x%254+1)<<23));
                query[i]=std::nextafter(row[i],0.0f);break;
        case 5: row[i]=std::numeric_limits<float>::max();query[i]=-row[i];break;
        case 6: row[i]=float_bits((x&0x807fffff)|((x%255)<<23));query[i]=row[i];break;
        default: row[i]=(i%3)?0x1p-120f:0x1p120f;query[i]=(i%2)?-row[i]:row[i];break;
      }
    }
    check_pair(row,query);
  }
  for(size_t n:{size_t(0),size_t(1),size_t(15),size_t(16),size_t(17),size_t(128),
                size_t(65535),size_t(65536),size_t(65537)}) {
    check_pair(std::vector<float>(n,0),std::vector<float>(n,0));
    check_pair(std::vector<float>(n,1),std::vector<float>(n,-1));
    check_pair(std::vector<float>(n,std::numeric_limits<float>::denorm_min()),
               std::vector<float>(n,-std::numeric_limits<float>::denorm_min()));
    check_pair(std::vector<float>(n,std::numeric_limits<float>::max()),
               std::vector<float>(n,-std::numeric_limits<float>::max()));
  }
  // Put both vectors on opposite sides of actual quantizer cell midpoints,
  // including exact ties and neighboring float32 values. The supplied step
  // prevents automatic scale selection from moving these cell boundaries.
  for(size_t n:{size_t(16),size_t(17),size_t(128)})
    for(int exponent:{-140,-100,-20,0,20,120})for(int side:{-1,0,1}) {
      const double step=std::ldexp(1.0,exponent);std::vector<float> row(n),query(n);
      for(size_t i=0;i<n;++i) {
        float value=static_cast<float>((double(int(i%127)-63)+0.5)*step);
        if(side)value=std::nextafter(value,side<0 ? -std::numeric_limits<float>::infinity()
                                                        : std::numeric_limits<float>::infinity());
        row[i]=value;query[i]=-value;
      }
      check_pair(row,query,step);
    }
  // A coarse scale and tiny query radius must disable rejection, not turn a
  // weak code distance into a false proof. Exact ties are checked above.
  std::vector<float> zero(128,0),one(128,1);std::vector<int8_t> far(128,127);
  const QuantizedQueryBound coarse(zero.data(),zero.size(),16);
  require(!coarse.definitely_greater(far.data(),32),"coarse-scale small-radius fallback");
  const auto codes=encode(one,quantization_step(1));
  const QuantizedQueryBound useful(zero.data(),zero.size(),quantization_step(1));
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  require(useful.definitely_greater(codes.data(),32),"nontrivial ordinary-data rejection");
#else
  require(!useful.definitely_greater(codes.data(),32),"disabled build retains exact fallback");
#endif
}
static void check_invalid_and_environment() {
  std::vector<float> query(128,0);std::vector<int8_t> row(128,127);int8_t code=0;
  for(double step:{0.0,-0.0,-1.0,0.3,std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::quiet_NaN(),std::ldexp(1.0,-600),std::ldexp(1.0,600)}) {
    const QuantizedQueryBound invalid(query.data(),query.size(),step);
    require(!invalid.enabled() && !invalid.definitely_greater(row.data(),0),"invalid step disables bound");
    const QuantizedQueryBound invalid_dot(query.data(),query.size(),step,true);
    require(!invalid_dot.enabled() && !invalid_dot.dot_enabled() &&
            !invalid_dot.definitely_greater(row.data(),0,norm_oracle(row.data(),row.size())),
            "invalid step disables dot context");
  }
  for(double step:{0.0,-1.0,0.3,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    require(!quantize_coordinate(1,step,&code),"invalid coordinate step");
  for(double maximum:{-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    require(quantization_step(maximum)==0,"invalid maximum disables quantization");
  for(float invalid:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN(),128.0f}) {
    query[11]=invalid;const QuantizedQueryBound bound(query.data(),query.size(),1,true);
    require(!bound.enabled() && !bound.definitely_greater(row.data(),0),"invalid or unencodable query disables bound");
    require(!bound.dot_enabled(),"failed query encoding cannot enable dot");
  }
  query[11]=0;
#if defined(MVDB_SIMD_BOUND_X86)
  const unsigned csr=_mm_getcsr();
  for(unsigned flags:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}) {
    _mm_setcsr((csr&~(3u<<13))|flags);
    const QuantizedQueryBound disabled(query.data(),query.size(),1,true);
    const bool enabled=disabled.enabled(),reject=disabled.definitely_greater(row.data(),0);
    _mm_setcsr(csr);
    require(!enabled && !reject && !disabled.dot_enabled(),"non-default MXCSR disables quantized bound");
  }
#else
  const int rounding=std::fegetround();
  for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
    require(std::fesetround(mode)==0,"test rounding mode supported");
    const QuantizedQueryBound disabled(query.data(),query.size(),1,true);
    const bool enabled=disabled.enabled(),reject=disabled.definitely_greater(row.data(),0);
    std::fesetround(rounding);
    require(!enabled && !reject && !disabled.dot_enabled(),"non-nearest rounding disables quantized bound");
  }
#endif
}
// Preserve the prior floating-cap decision exactly, including its performance
// fallback. nextafter and the wide scalar distance are independent of the new
// integer cap and production bit-neighbor helpers.
static bool legacy_reject(uint64_t distance,size_t n,double step,double threshold) {
  if(!std::isfinite(threshold) || threshold<0)return false;
  const auto up=[](double value) {
    return std::isfinite(value)
        ? std::nextafter(value,std::numeric_limits<double>::infinity()) : value;
  };
  const double root_n=up(std::sqrt(static_cast<double>(n)));
  const double tu=up(threshold);
  if(!std::isfinite(tu))return false;
  const double scaled=up(tu/(step*step));
  if(!std::isfinite(scaled))return false;
  const double root=up(std::sqrt(scaled));
  if(!std::isfinite(root) || root<4*root_n)return false;
  const double sum=up(root+root_n);
  const double cap=up(sum*sum);
  return std::isfinite(cap) && static_cast<double>(distance)>cap;
}
static void check_integer_cap_equivalence() {
  for(size_t n:{size_t(16),size_t(128),size_t(65536)})
    for(int exponent:{-100,0,100}) {
      const double step=std::ldexp(1.0,exponent);
      std::vector<float> query(n,static_cast<float>(-127*step));
      std::vector<int8_t> row(n,127),coded_query(n,-127);
      const uint64_t integer=integer_oracle(row.data(),coded_query.data(),n);
      const QuantizedQueryBound bound(query.data(),n,step);
      const QuantizedQueryBound dot(query.data(),n,step,true);
      const uint32_t row_norm=norm_oracle(row.data(),n);
      const auto check=[&](double threshold) {
        const bool expected=bound.enabled() && legacy_reject(integer,n,step,threshold);
        // Repeat to exercise both cache miss and the validated cached path.
        for(int repeat=0;repeat<2;++repeat) {
          require(bound.definitely_greater(row.data(),threshold)==expected,
                  "integer cap differs from legacy floating cap");
          require(dot.definitely_greater(row.data(),threshold,row_norm)==expected,
                  "dot cap differs near signed/unsigned integer boundaries");
        }
      };
      for(double target:{double(integer)-1,double(integer),double(integer)+1,
                         2147483647.0,2147483648.0,2147483649.0,
                         4294967294.0,4294967295.0,4294967296.0}) {
        const double radius=std::sqrt(target)-std::sqrt(static_cast<double>(n));
        const double threshold=(radius*radius)*(step*step);
        // Rounding can put the cap immediately above/below an integer. Sweep
        // adjacent representable thresholds on both sides of that boundary.
        double lower=threshold,upper=threshold;
        for(int i=0;i<24;++i) {
          check(lower);check(upper);
          check(std::numeric_limits<double>::quiet_NaN());
          check(std::numeric_limits<double>::infinity());check(-1);
          check(lower); // invalid input must not corrupt a reusable cached cap
          lower=std::nextafter(lower,0.0);
          upper=std::nextafter(upper,std::numeric_limits<double>::infinity());
        }
      }
      check(0);check(-0.0);check(std::numeric_limits<double>::max());
    }
}
int main() {
  check_integer_kernel();check_coordinates();check_pairs();check_invalid_and_environment();
  check_integer_cap_equivalence();check_dot_unaligned();
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  require(rejections>100,"test exercises nontrivial rejection");
#endif
  std::cout<<"PASS quantized uint64 oracle, coordinate error, exact-distance thresholds, cache and FP environment; pairs="
           <<checked_pairs<<" safe_rejections="<<rejections<<"\n";
}

#include "distance.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using namespace mvdb;
// Pre-cutoff query context retained as the same-source performance reference.
class ReferenceBoundContext {
 public:
  explicit ReferenceBoundContext(size_t n):n_(n) {
    if(!n || n>(uint64_t(1)<<32))return;
#if defined(MVDB_SIMD_BOUND_X86)
    if(_mm_getcsr() & ((1u<<6)|(1u<<15)|(3u<<13)))return;
#else
    if(std::fegetround()!=FE_TONEAREST)return;
#endif
    enabled_=true;
#if defined(MVDB_SIMD_BOUND_X86) && !defined(__FAST_MATH__) && !defined(MVDB_DISABLE_SIMD_BOUND)
    if(n>=16 && n<=65536) {
      static const auto selected=distance_detail::choose_bound_sum();
      kernel_=selected;
      relative_=(static_cast<double>(n)+12)*0x1p-22;
      absolute_=static_cast<double>(n)*0x1p-148;
    }
#endif
  }
  bool definitely_greater(const float* a,const float* b,double threshold) const {
    if(!enabled_ || !std::isfinite(threshold) || threshold<0)return false;
#if defined(MVDB_SIMD_BOUND_X86)
    if(kernel_) {
      const double sum=kernel_(a,b,n_);
      if(std::isfinite(sum))
        return distance_detail::float_sum_definitely_greater_cached(sum,relative_,absolute_,threshold);
      // Float overflow is common for valid extreme coordinates. The original
      // double bound remains safe over the complete finite float32 domain.
    }
#endif
    return distance_definitely_greater_double(a,b,n_,threshold);
  }
 private:
  size_t n_;
  bool enabled_=false;
  double relative_=0,absolute_=0;
#if defined(MVDB_SIMD_BOUND_X86)
  distance_detail::BoundSum kernel_=nullptr;
#endif
};

static uint64_t rng=0xb2c51e876da4930full;
static uint64_t next(){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static float fbits(uint32_t x){float f;std::memcpy(&f,&x,4);return f;}
static void require(bool ok,const char* message){
  if(!ok){std::cerr<<"FAIL "<<message<<"\n";std::exit(1);}
}
static size_t rejected=0;
static void check(const std::vector<float>& a,const std::vector<float>& b){
  const size_t n=a.size();
  const DistanceBoundContext context(n);
  const ReferenceBoundContext reference(n);
  const double exact=distance_exact(a.data(),b.data(),n);
  for(double threshold:{0.0,exact*0.5,std::nextafter(exact,0.0),exact,
      std::nextafter(exact,std::numeric_limits<double>::infinity()),exact*2}){
    const bool fast=distance_definitely_greater(a.data(),b.data(),n,threshold);
    require(context.definitely_greater(a.data(),b.data(),threshold)==fast,
            "query context and per-call wrapper decisions differ");
    require(!fast || exact>threshold,"SIMD rejected exact <= threshold");
    require(!reference.definitely_greater(a.data(),b.data(),threshold) || exact>threshold,
            "reference context rejected exact <= threshold");
    rejected+=fast;
#if defined(MVDB_SIMD_BOUND_X86)
    if(n && n<=65536){
      if(__builtin_cpu_supports("sse2")){
        const double sum=distance_detail::bound_sum_sse2(a.data(),b.data(),n);
        require(!distance_detail::float_sum_definitely_greater(sum,n,threshold) ||
                exact>threshold,"SSE2 bound unsafe");
      }
      if(__builtin_cpu_supports("avx2")){
        const double sum=distance_detail::bound_sum_avx2(a.data(),b.data(),n);
        require(!distance_detail::float_sum_definitely_greater(sum,n,threshold) ||
                exact>threshold,"AVX2 bound unsafe");
      }
    }
#endif
  }
}
static void tests(){
#if defined(MVDB_SIMD_DOUBLE_REDUCTION)
  std::cout<<"reduction=reference_double_serial\n";
#else
  std::cout<<"reduction=float_tree\n";
#endif
  const size_t dims[]={0,1,2,7,15,16,17,31,32,127,128,129,511,1024};
  for(size_t trial=0;trial<7200;++trial){
    const size_t n=dims[(trial/8)%14];std::vector<float>a(n),b(n);
    for(size_t j=0;j<n;++j){
      const uint32_t x=next(),y=next();
      switch(trial%8){
        case 0:
          a[j]=fbits((x&0x807fffff)|((x%255)<<23));
          b[j]=fbits((y&0x807fffff)|((y%255)<<23));break;
        case 1:
          a[j]=float(int(x%20001)-10000)/10000;
          b[j]=float(int(y%20001)-10000)/10000;break;
        case 2:
          a[j]=fbits(x&0x807fffff);b[j]=fbits(y&0x807fffff);break;
        case 3:
          a[j]=std::ldexp(float(1+(x%16))/16,-70-int(x%12));
          b[j]=-a[j];break;
        case 4:
          a[j]=fbits((x&0x807fffff)|((x%254+1)<<23));
          b[j]=std::nextafter(a[j],0.0f);break;
        case 5:
          a[j]=std::numeric_limits<float>::max();
          b[j]=-a[j];break;
        case 6:
          a[j]=fbits((x&0x807fffff)|((x%255)<<23));b[j]=a[j];break;
        default:
          a[j]=(j%3)?0x1p-120f:0x1p120f;b[j]=(j%2)?-a[j]:a[j];break;
      }
    }
    check(a,b);
  }
  for(int e=-100;e<=100;e+=10){
    std::vector<float>a(128),b(128);
    a[0]=std::ldexp(1.0f,e);a[1]=a[2]=std::ldexp(1.0f,e-27);
    check(a,b);
    a[3]=std::ldexp(1.0f,e-40);check(a,b);
  }
  for(size_t n:{size_t(65535),size_t(65536),size_t(65537)}){
    std::vector<float>a(n,0.75f),b(n,0.25f);check(a,b);
    std::fill(a.begin(),a.end(),0x1p-75f);std::fill(b.begin(),b.end(),0);check(a,b);
  }
  // Products fit in float, but their full reduction overflows. The wrapper
  // must safely use the double bound; also cover subnormal reduction sums.
  for(size_t n:{size_t(16),size_t(17),size_t(32),size_t(128)}){
    std::vector<float>x(n,0x1p63f),y(n,0);check(x,y);
    std::fill(x.begin(),x.end(),0x1p-74f);check(x,y);
    for(size_t i=0;i<n;++i)x[i]=static_cast<float>(i+1);
    const double expected=distance_exact(x.data(),y.data(),n);
#if defined(MVDB_SIMD_BOUND_X86)
    if(__builtin_cpu_supports("sse2"))
      require(distance_detail::bound_sum_sse2(x.data(),y.data(),n)==expected,"SSE2 reduction retains every lane");
    if(__builtin_cpu_supports("avx2"))
      require(distance_detail::bound_sum_avx2(x.data(),y.data(),n)==expected,"AVX2 reduction retains every lane");
#endif
    check(x,y);
  }
  std::vector<float>a(128,1),b(128,0);
  require(!distance_definitely_greater(a.data(),b.data(),128,-1),"negative threshold");
  require(!distance_definitely_greater(a.data(),b.data(),128,
    std::numeric_limits<double>::quiet_NaN()),"NaN threshold");
  require(!distance_definitely_greater(a.data(),b.data(),128,
    std::numeric_limits<double>::infinity()),"infinite threshold");
  a[0]=std::numeric_limits<float>::infinity();
  require(!distance_definitely_greater(a.data(),b.data(),128,1),"infinite coordinate");
  a[0]=std::numeric_limits<float>::quiet_NaN();
  require(!distance_definitely_greater(a.data(),b.data(),128,1),"NaN coordinate");
#if defined(MVDB_SIMD_BOUND_X86)
  a[0]=1;const unsigned csr=_mm_getcsr();
  for(unsigned flags:{1u<<6,1u<<15,1u<<13}){
    _mm_setcsr(csr|flags);
    const DistanceBoundContext disabled_context(128);
    const bool result=distance_definitely_greater(a.data(),b.data(),128,1);
    const bool cached=disabled_context.definitely_greater(a.data(),b.data(),1);
    _mm_setcsr(csr);require(!result,"non-default FP environment fallback");
    require(!cached,"context constructed in non-default environment is disabled");
  }
  // DAZ turns the largest subnormal input into zero during float->double
  // conversion. Without a wrapper-wide guard even the old double bound can
  // mistake this one-ulp difference for the entire smallest normal value.
  for(size_t n:{size_t(1),size_t(65537)}){
    std::vector<float>x(n,0),y(n,0);
    x[0]=std::numeric_limits<float>::min();
    y[0]=std::nextafter(x[0],0.0f);
    const double expected=distance_exact(x.data(),y.data(),n);
    require(expected>0,"DAZ oracle calculated in default environment");
    _mm_setcsr(csr|(1u<<6));
    const DistanceBoundContext disabled_context(n);
    const bool result=distance_definitely_greater(x.data(),y.data(),n,expected);
    const bool cached=disabled_context.definitely_greater(x.data(),y.data(),expected);
    _mm_setcsr(csr);
    require(!result,"DAZ guard covers non-SIMD dimensions and canonical tie");
    require(!cached,"disabled context covers non-SIMD dimensions");
  }
#endif
  require(rejected>1000,"nontrivial pruning");
  // One context sees many different rows and thresholds. Repeat thresholds,
  // include signed zero and extremes, and alternate tightening/loosening.
  DistanceBoundContext changing(128);
  for(size_t iteration=0;iteration<2000;++iteration){
    std::vector<float>x(128),y(128);
    for(size_t j=0;j<x.size();++j){
      x[j]=float(int(next()%20001)-10000)/10000;
      y[j]=float(int(next()%20001)-10000)/10000;
    }
    const double exact=distance_exact(x.data(),y.data(),x.size());
    const double threshold=double(next()%20000)/100;
    for(double t:{threshold,threshold,exact,std::nextafter(exact,0.0),
        std::nextafter(exact,std::numeric_limits<double>::infinity()),0.0,-0.0,
        std::numeric_limits<double>::max(),std::numeric_limits<double>::min(),
        std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN(),-1.0}) {
      const bool result=changing.definitely_greater(x.data(),y.data(),t);
      require(!result || exact>t,"cached cutoff threshold-change safety");
      require(result==distance_definitely_greater(x.data(),y.data(),128,t),
              "cached cutoff and fresh wrapper disagree");
    }
  }
  std::cout<<"PASS 7200 cases + midpoint/extreme dimension cases; safe_rejections="<<rejected<<"\n";
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool old_bound(const float*a,const float*b,size_t n,double t){
  return distance_definitely_greater_double(a,b,n,t);
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static bool new_bound(const float*a,const float*b,size_t n,double t){
  return distance_definitely_greater(a,b,n,t);
}
static volatile size_t sink=0;
using Bound=bool(*)(const float*,const float*,size_t,double);
static double timed(Bound fn,const std::vector<float>& a,const std::vector<float>& b){
  const auto start=std::chrono::steady_clock::now();size_t count=0;
  for(size_t pass=0;pass<128;++pass) {
#if defined(__GNUC__)
    // Keep each pass real even if interprocedural analysis marks old_bound pure.
    asm volatile("" : : "r"(a.data()),"r"(b.data()) : "memory");
#endif
    for(size_t row=0;row<2048;++row)count+=fn(a.data()+row*128,b.data(),128,70.0);
  }
  sink=count;
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
template<class Context>
static double timed_context(const std::vector<float>& a,const std::vector<float>& b){
  const Context context(128);
  const auto start=std::chrono::steady_clock::now();size_t count=0;
  for(size_t pass=0;pass<128;++pass){
#if defined(__GNUC__)
    asm volatile("" : : "r"(a.data()),"r"(b.data()) : "memory");
#endif
    for(size_t row=0;row<2048;++row)
      count+=context.definitely_greater(a.data()+row*128,b.data(),70.0);
  }
  sink=count;
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
static void bench(){
  std::vector<float>a(2048*128),b(128);
  for(float&v:a)v=float(int(next()%20001)-10000)/10000;
  for(float&v:b)v=float(int(next()%20001)-10000)/10000;
  timed(old_bound,a,b);timed(new_bound,a,b);
  std::vector<double>old_times,new_times;
  for(int i=0;i<5;++i){
    double old_ms,new_ms;
    if(i%2){new_ms=timed(new_bound,a,b);old_ms=timed(old_bound,a,b);}
    else{old_ms=timed(old_bound,a,b);new_ms=timed(new_bound,a,b);}
    old_times.push_back(old_ms);new_times.push_back(new_ms);
    std::cout<<"round="<<i+1<<" old_ms="<<old_ms<<" new_ms="<<new_ms<<" count="<<sink<<"\n";
  }
  std::sort(old_times.begin(),old_times.end());std::sort(new_times.begin(),new_times.end());
  std::cout<<"median_old_ms="<<old_times[2]<<" median_new_ms="<<new_times[2]
           <<" speedup="<<old_times[2]/new_times[2]<<"\n";
  timed_context<ReferenceBoundContext>(a,b);timed_context<DistanceBoundContext>(a,b);
  old_times.clear();new_times.clear();
  for(int i=0;i<5;++i){
    double old_ms,new_ms;
    if(i%2){new_ms=timed_context<DistanceBoundContext>(a,b);old_ms=timed_context<ReferenceBoundContext>(a,b);}
    else{old_ms=timed_context<ReferenceBoundContext>(a,b);new_ms=timed_context<DistanceBoundContext>(a,b);}
    old_times.push_back(old_ms);new_times.push_back(new_ms);
    std::cout<<"context_round="<<i+1<<" reference_ms="<<old_ms<<" cutoff_ms="<<new_ms<<" count="<<sink<<"\n";
  }
  std::sort(old_times.begin(),old_times.end());std::sort(new_times.begin(),new_times.end());
  std::cout<<"context_reference_median_ms="<<old_times[2]<<" context_cutoff_median_ms="<<new_times[2]
           <<" context_speedup="<<old_times[2]/new_times[2]<<"\n";
}
int main(int argc,char**argv){
  tests();if(argc==2 && std::string(argv[1])=="--bench")bench();
  else if(argc!=1){std::cerr<<"usage: test_distance_simd [--bench]\n";return 2;}
}


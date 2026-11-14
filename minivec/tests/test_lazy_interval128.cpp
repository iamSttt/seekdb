// Standalone prototype oracle test. See README.md for native commands.
#define MVDB_LAZY128_TEST_KERNEL 1
#include "lazy_interval128.h"
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <iostream>
#include <random>
#include <thread>

using namespace mvdb;
using Vector=std::array<float,128>;
using Key=std::pair<double,int64_t>;
static uint64_t rng=0xe5e1281114a9d361ull;
static uint64_t next() {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static void require(bool value,const char* message) {
  if(!value)throw std::runtime_error(message);
}
static float from_bits(uint32_t bits) {
  if((bits&0x7f800000u)==0x7f800000u)bits^=0x00800000u;
  float value;std::memcpy(&value,&bits,4);return value;
}
static double oracle(const float* a,const float* b) {
  // Existing integer ExactSum path is independent of the new float envelope.
  const double exact=distance_exact(a,b,128);
  const double old=distance_squared(a,b,128);
  require(exact==old,"canonical long-double path differs from ExactSum oracle");
  return exact;
}
static void check_x87_precision_independence() {
#if defined(__x86_64__) && defined(MVDB_SIMD_BOUND_X86)
  unsigned short saved;
  __asm__ volatile("fnstcw %0":"=m"(saved));
  Vector query{},row{};
  row[0]=0x1p30f;row[1]=16;row[2]=8;row[3]=8;
  query[3]=std::numeric_limits<float>::denorm_min();
  const double wanted=distance_exact(row.data(),query.data(),128);
  for(unsigned short precision:{static_cast<unsigned short>(0),static_cast<unsigned short>(0x200)}) {
    // Do not call the old long-double shortcut in this deliberately nondefault
    // environment. The new materialization is the integer ExactSum path.
    const unsigned short changed=static_cast<unsigned short>((saved&~0x300u)|precision|0x3fu);
    __asm__ volatile("fldcw %0"::"m"(changed));
    lazy128::TopK128<true> heap(query.data(),1);
    heap.offer(1,row.data());
    double actual=-1;
    heap.for_each_materialized([&](int64_t,double distance){actual=distance;});
    __asm__ volatile("fldcw %0"::"m"(saved));
    require(actual==wanted,"materialization depends on x87 precision control");
  }
#endif
}
static void check_envelopes(lazy128::EnvelopeContext128 envelope,const char* label,bool compare_old) {
  DistanceBoundContext old(128);
  uint64_t bounded=0,rejected=0,fallback=0;
  for(unsigned trial=0;trial<2048;++trial) {
    // Float-aligned but deliberately not SIMD-aligned starts.
    std::vector<float> aa(129),bb(130);
    float* a=aa.data()+1;float* b=bb.data()+2;
    for(size_t i=0;i<128;++i) {
      switch(trial%8) {
       case 0: a[i]=float(int(next()%8193)-4096)/1024;
               b[i]=float(int(next()%8193)-4096)/1024;break;
       case 1: a[i]=from_bits(uint32_t(next()));b[i]=from_bits(uint32_t(next()));break;
       case 2: a[i]=from_bits(uint32_t(next())&0x807fffffu);
               b[i]=from_bits(uint32_t(next())&0x807fffffu);break;
       case 3: a[i]=std::numeric_limits<float>::max();b[i]=-a[i];break;
       case 4: a[i]=std::ldexp(float(int(next()%33)-16),-100);
               b[i]=std::ldexp(float(int(next()%33)-16),-100);break;
       case 5: a[i]=i==0 ? 0x1p30f : i==1 ? 16.0f : i<4 ? 8.0f : 0.0f;
               b[i]=i==3 ? std::numeric_limits<float>::denorm_min() : 0.0f;break;
       case 6: a[i]=i%2 ? -0.0f : 0.0f;b[i]=-a[i];break;
       default: a[i]=std::ldexp(float(int(next()%257)-128),int(next()%241)-120);
                b[i]=std::nextafter(a[i],0.0f);break;
      }
    }
    const double exact=oracle(a,b);
    const double inf=std::numeric_limits<double>::infinity();
    // Check every row, including rows subsequently rejected without heap use.
    lazy128::Interval no_cutoff;
    const auto no_cutoff_kind=envelope.sample(a,b,inf,&no_cutoff);
    require(no_cutoff_kind!=lazy128::SampleKind::Reject,"infinite cutoff rejected row");
    if(no_cutoff_kind==lazy128::SampleKind::Interval)
      require(no_cutoff.lower<=exact && exact<=no_cutoff.upper,
              "unconditional envelope missed canonical distance");
    for(double threshold:{inf,0.0,-0.0,-1.0,exact*0.5,std::nextafter(exact,0.0),
                          exact,std::nextafter(exact,inf),std::numeric_limits<double>::quiet_NaN()}) {
      lazy128::Interval interval;
      const auto kind=envelope.sample(a,b,threshold,&interval);
      if(kind==lazy128::SampleKind::Reject) {
        require(exact>threshold,"false rejection of canonical distance");
        require(no_cutoff_kind==lazy128::SampleKind::Interval,
                "rejected row lacks independently checked envelope");
        ++rejected;
      } else if(kind==lazy128::SampleKind::Interval) {
        require(interval.lower<=exact && exact<=interval.upper,"canonical outside interval");
        ++bounded;
      } else ++fallback;
      // The old context may additionally reject through its double fallback.
      if(compare_old && kind!=lazy128::SampleKind::ExactFallback)
        require(old.definitely_greater(a,b,threshold)==(kind==lazy128::SampleKind::Reject),
                "cached float rejection differs from original context");
    }
  }
  if(envelope.enabled())require(bounded && rejected && fallback,"missing numeric path coverage");
  std::cout<<"PASS numeric kernel="<<label<<" envelopes="<<bounded<<" rejects="<<rejected<<" exact_fallback="<<fallback<<'\n';
}

static void check_directed_bins(lazy128::EnvelopeContext128 envelope,const char* label) {
  Vector zero{},a{};
  size_t cases=0,finite=0,overflow=0;
  auto check=[&](bool expect_finite) {
    const double exact=oracle(a.data(),zero.data());
    lazy128::Interval interval;
    const auto kind=envelope.sample(a.data(),zero.data(),std::numeric_limits<double>::infinity(),&interval);
    require(kind!=lazy128::SampleKind::Reject,"directed unbounded query rejected");
    if(envelope.enabled()) {
      require((kind==lazy128::SampleKind::Interval)==expect_finite,"directed bin hit unexpected finite/fallback path");
      if(expect_finite) {
        require(interval.lower<=exact && exact<=interval.upper,"directed envelope missed canonical");
        ++finite;
      } else ++overflow;
    }
    for(double threshold:{0.0,exact*0.5,exact,std::nextafter(exact,0.0)}) {
      const auto tested=envelope.sample(a.data(),zero.data(),threshold,&interval);
      if(tested==lazy128::SampleKind::Reject)require(exact>threshold,"directed false reject");
      if(tested==lazy128::SampleKind::Interval)
        require(interval.lower<=exact && exact<=interval.upper,"directed cutoff envelope missed canonical");
    }
    ++cases;
  };
  for(int exponent:{-149,-126,-100,-76,-75,-74,-64,-1,0,31,60,61,62,63,64,126,127}) {
    for(size_t active:{size_t(1),size_t(128)}) {
      a.fill(0);
      for(size_t i=0;i<active;++i)a[i]=std::ldexp(1.0f,exponent);
      check(exponent<=(active==1?63:60));
    }
  }
  // 2^-75 squared is exactly the half-ULP underflow boundary, tying to zero.
  for(float v:{std::nextafter(0x1p-75f,0.0f),0x1p-75f,std::nextafter(0x1p-75f,1.0f)}) {
    a.fill(0);a[0]=v;check(true);
  }
  a.fill(0);a[0]=std::nextafter(0x1p64f,0.0f);check(true);
  a[0]=0x1p64f;check(false);
  // Four 2^63 squares sum to 2^128; just smaller values remain finite.
  a.fill(0);for(size_t i=0;i<4;++i)a[i]=std::nextafter(0x1p63f,0.0f);check(true);
  for(size_t i=0;i<4;++i)a[i]=0x1p63f;check(false);
  if(envelope.enabled())require(finite && overflow,"directed bin coverage missing");
  std::cout<<"PASS directed kernel="<<label<<" cases="<<cases<<" interval="<<finite<<" overflow="<<overflow<<'\n';
}

static void check_all_kernels() {
  check_envelopes(lazy128::EnvelopeContext128{},"selected",true);
  check_directed_bins(lazy128::EnvelopeContext128{},"selected");
#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_SIMD_BOUND)
  auto test=[](distance_detail::BoundSum kernel,const char* label) {
    check_envelopes(lazy128::EnvelopeContext128{kernel},label,false);
    check_directed_bins(lazy128::EnvelopeContext128{kernel},label);
  };
  if(__builtin_cpu_supports("sse2"))test(distance_detail::bound_sum_sse2,"sse2");
  if(__builtin_cpu_supports("avx2"))test(distance_detail::bound_sum_avx2,"avx2");
#if !defined(MVDB_DISABLE_AVX512_BOUND)
  if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx2"))
    test(distance_detail::bound_sum_avx512,"avx512");
#endif
#endif
}

struct Row {int64_t pk;Vector value;};
static std::vector<Row> rows(size_t count,unsigned pattern) {
  std::vector<Row> result(count);
  for(size_t i=0;i<count;++i) {
    result[i].pk=(i%2 ? -9007199254741000ll : 9007199254741000ll)+int64_t(i);
    for(size_t d=0;d<128;++d) {
      if(pattern==0)result[i].value[d]=float(int(next()%1025)-512)/128;
      else if(pattern==1)result[i].value[d]=d==0 ? float(i%7) : 0;
      else if(pattern==2)result[i].value[d]=from_bits(uint32_t(next()));
      else if(pattern==3)result[i].value[d]=d==0 ? 0x1p30f : d==1 ? 16 : d<4 ? float((i%3)*8) : 0;
      else if(pattern==4)result[i].value[d]=from_bits(uint32_t(next())&0x807fffffu);
      else if(pattern==5)result[i].value[d]=float(i+1)/16;
      else if(pattern==6)result[i].value[d]=d==0 ? 0x1p30f : d==1 ? 16 : d==2 ? 8 :
          d==3 ? (i%3==0 ? std::nextafter(8.0f,0.0f) :
                  i%3==1 ? 8.0f : std::nextafter(8.0f,16.0f)) : 0;
      else result[i].value[d]=(next()&1) ? 1.0f : -1.0f;
    }
  }
  // Shuffle physical order independently from PK and distance.
  for(size_t i=count;i>1;--i)std::swap(result[i-1],result[next()%i]);
  return result;
}
static std::vector<Key> expected(const std::vector<Row>& input,const Vector& query,size_t k) {
  std::vector<Key> answer;
  for(const auto& row:input)answer.emplace_back(oracle(row.value.data(),query.data()),row.pk);
  std::sort(answer.begin(),answer.end());
  if(answer.size()>k)answer.resize(k);
  return answer;
}
static void compare_result(std::vector<Key> actual,const std::vector<Key>& wanted,size_t k) {
  std::sort(actual.begin(),actual.end());
  if(actual.size()>k)actual.resize(k);
  require(actual==wanted,"lazy heap winners differ from full ExactSum sort");
}
static void single(const std::vector<Row>& input,const Vector& query,size_t k) {
  lazy128::TopK128<true> heap(query.data(),k);
  double old_upper=std::numeric_limits<double>::infinity();
  for(const auto& row:input) {
    heap.offer(row.pk,row.value.data());
    require(heap.local_upper()<=old_upper,"historical witness upper increased");
    old_upper=heap.local_upper();
  }
  std::vector<Key> actual;
  heap.for_each_materialized([&](int64_t pk,double d){actual.emplace_back(d,pk);});
  require(heap.statistics().final_exact_calls<=k,"more than K final exact calls");
  compare_result(std::move(actual),expected(input,query,k),k);
}
static void paired(const std::vector<Row>& input,const Vector& query,size_t k,unsigned order) {
  lazy128::TopK128<true> left(query.data(),k),right(query.data(),k);
  lazy128::TopK128<true>* heaps[2]={&left,&right};
  const size_t middle=input.size()/2,first[2]={0,middle},last[2]={middle,input.size()};
  size_t cursor[2]={0,middle};
  double published[2]={std::numeric_limits<double>::infinity(),std::numeric_limits<double>::infinity()};
  while(cursor[0]<last[0] || cursor[1]<last[1]) {
    unsigned worker=order==0 ? 0 : order==1 ? 1 : unsigned(next()&1);
    if(cursor[worker]>=last[worker])worker^=1;
    auto& heap=*heaps[worker];
    if(((cursor[worker]-first[worker])&63)==0) {
      published[worker]=std::min(published[worker],heap.local_upper());
      heap.set_peer_upper(published[worker^1]);
    }
    const auto& row=input[cursor[worker]++];
    heap.offer(row.pk,row.value.data());
    if(cursor[worker]==last[worker])published[worker]=std::min(published[worker],heap.local_upper());
  }
  std::vector<Key> actual;
  for(auto* heap:heaps) {
    heap->for_each_materialized([&](int64_t pk,double d){actual.emplace_back(d,pk);});
    require(heap->statistics().final_exact_calls<=k,"paired final materialization exceeds K");
  }
  compare_result(std::move(actual),expected(input,query,k),k);
}
static void threaded(const std::vector<Row>& input,const Vector& query,size_t k) {
  struct alignas(64) Published {std::atomic<double> value{std::numeric_limits<double>::infinity()};};
  Published published[2];
  std::vector<Key> answers[2];
  std::exception_ptr errors[2];
  auto run=[&](unsigned worker) {
    try {
      lazy128::TopK128<true> heap(query.data(),k);
      const size_t middle=input.size()/2,first=worker?middle:0,last=worker?input.size():middle;
      double last_published=std::numeric_limits<double>::infinity();
      auto publish=[&] {
        const double value=heap.local_upper();
        if(value<last_published) {
          published[worker].value.store(value,std::memory_order_relaxed);
          last_published=value;
        }
      };
      for(size_t i=first;i<last;++i) {
        if(((i-first)&63)==0) {publish();heap.set_peer_upper(published[worker^1].value.load(std::memory_order_relaxed));}
        heap.offer(input[i].pk,input[i].value.data());
      }
      publish();
      heap.for_each_materialized([&](int64_t pk,double d){answers[worker].emplace_back(d,pk);});
    } catch(...) {errors[worker]=std::current_exception();}
  };
  std::thread worker(run,1);run(0);worker.join();
  for(const auto& e:errors)if(e)std::rethrow_exception(e);
  answers[0].insert(answers[0].end(),answers[1].begin(),answers[1].end());
  compare_result(std::move(answers[0]),expected(input,query,k),k);
}
static void check_environment() {
  Vector a{},b{};lazy128::Interval interval;
  std::fenv_t saved;require(std::fegetenv(&saved)==0,"fegetenv");
  for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
    require(std::fesetround(mode)==0,"fesetround");
    require(!lazy128::supported(),"non-RNE environment admitted");
    lazy128::EnvelopeContext128 context;
    require(context.sample(a.data(),b.data(),0,&interval)==lazy128::SampleKind::ExactFallback,
            "non-RNE envelope did not fall back");
  }
  require(std::fesetenv(&saved)==0,"fesetenv");
#if defined(MVDB_SIMD_BOUND_X86)
  const unsigned csr=_mm_getcsr();
  for(unsigned flag:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}) {
    _mm_setcsr(csr|flag);
    require(!lazy128::supported(),"DAZ/FTZ/non-RNE SIMD environment admitted");
    lazy128::EnvelopeContext128 context;
    require(!context.enabled(),"disabled SIMD environment enabled context");
  }
  for(unsigned bit=7;bit<=12;++bit) {
    // Clear exception status before unmasking; no floating operations follow.
    _mm_setcsr((csr&~0x3fu)&~(1u<<bit));
    const bool admitted=lazy128::supported();
    _mm_setcsr(csr);
    require(!admitted,"unmasked SIMD exception environment admitted");
  }
  _mm_setcsr(csr);
#endif
}
int main() {
  try {
    check_environment();check_all_kernels();check_x87_precision_independence();
    size_t cases=0;
    for(unsigned pattern=0;pattern<8;++pattern) {
      for(size_t n:{size_t(0),size_t(1),size_t(3),size_t(17),size_t(65),size_t(257)}) {
        auto input=rows(n,pattern);Vector query{};
        if(n>1 && pattern%2==0 && pattern!=6)query=input[n/2].value;
        for(size_t k:{size_t(1),size_t(2),size_t(10),size_t(32),size_t(1024)}) {
          single(input,query,k);
          for(unsigned schedule=0;schedule<3;++schedule)paired(input,query,k,schedule);
          ++cases;
        }
      }
    }
    auto input=rows(1025,0);Vector query{};
    threaded(input,query,1);threaded(input,query,10);threaded(input,query,1024);
    auto separated=rows(4096,5);
    lazy128::TopK128<true> diagnostics(query.data(),10);
    for(const auto& row:separated)diagnostics.offer(row.pk,row.value.data());
    diagnostics.for_each_materialized([](int64_t,double){});
    const auto& s=diagnostics.statistics();
    std::cout<<"PASS heap fixture families="<<cases<<" (single + three schedules), threaded=3\n";
    std::cout<<"DIAGNOSTIC separated offered="<<s.offered<<" float_reject="<<s.float_rejects
             <<" interval_compares="<<s.interval_comparisons<<" overlap_compares="<<s.overlap_comparisons
             <<" canonical_calls="<<s.exact_calls<<" final_calls="<<s.final_exact_calls<<'\n';
    std::cout<<"No throughput/official score is measured by this test.\n";
  } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}

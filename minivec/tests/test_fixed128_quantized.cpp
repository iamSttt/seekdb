// Correctness and same-process diagnostics for the optional fixed-128 kernel.
// Compile: g++ -O3 -std=c++17 -Iminivec/src minivec/tests/test_fixed128_quantized.cpp -o /tmp/test-fixed128
// Run: /tmp/test-fixed128 [--bench]
// Repeat with -DMVDB_DISABLE_AVX512_QUANTIZED and under ASan/UBSan.
// Timings are kernel/whole-bound diagnostics, not database or official scores.
#include "quantized_bound.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace mvdb;
using Kernel=quantized_detail::Distance;
static uint64_t rng=0x936bd21d4e991017ull;
static uint64_t next() {rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static void require(bool yes,const char* why) {
  if(!yes) {std::cerr<<"FAIL fixed128: "<<why<<'\n';std::exit(1);}
}
static uint64_t oracle(const int8_t* a,const int8_t* b,size_t n) {
  uint64_t sum=0;
  for(size_t i=0;i<n;++i) {const int64_t d=int64_t(a[i])-int64_t(b[i]);sum+=uint64_t(d*d);}
  return sum;
}
struct KernelPair {const char* name;Kernel dynamic,fixed;};
static std::vector<KernelPair> kernels() {
  std::vector<KernelPair> all;
#if defined(MVDB_SIMD_BOUND_X86)
  if(__builtin_cpu_supports("sse2"))
    all.push_back({"sse2",quantized_detail::distance_sse2,quantized_detail::distance_sse2_128});
  if(__builtin_cpu_supports("avx2"))
    all.push_back({"avx2",quantized_detail::distance_avx2,quantized_detail::distance_avx2_128});
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  if(__builtin_cpu_supports("avx512f")&&__builtin_cpu_supports("avx512bw")&&__builtin_cpu_supports("avx2"))
    all.push_back({"avx512",quantized_detail::distance_avx512,quantized_detail::distance_avx512_128});
#endif
#endif
  return all;
}

// Baseline QueryBound from a90b2b750e40f020793cddefe01d5b66631afc63, with only
// the class name changed. Keep the original dynamic kernel and cap arithmetic
// for whole-call differential tests. Independent integer/true-distance checks
// also run below and in test_quantized_bound.cpp, so equality is not the oracle.
class DynamicBoundReference {
 public:
  DynamicBoundReference(const float* query,size_t n,double step):n_(n) {
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
    kernel_=quantized_detail::choose_distance();
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
    return static_cast<double>(kernel_(row,query_.data(),n_))>cap_;
  }
 private:
  void update_cap(double threshold) const {
    cached_=true;cached_threshold_=threshold;usable_cap_=false;
    const double tu=quantized_detail::up(threshold);
    if(!std::isfinite(tu))return;
    const double scaled=quantized_detail::up(tu/step_squared_);
    if(!std::isfinite(scaled))return;
    const double root=quantized_detail::up(std::sqrt(scaled));
    if(!std::isfinite(root) || root<4*root_n_)return;
    const double sum=quantized_detail::up(root+root_n_);
    cap_=quantized_detail::up(sum*sum);
    usable_cap_=std::isfinite(cap_);
  }
  size_t n_=0;
  bool enabled_=false;
  double step_squared_=0,root_n_=0;
  quantized_detail::Distance kernel_=nullptr;
  std::vector<int8_t> query_;
  mutable bool cached_=false,usable_cap_=false;
  mutable double cached_threshold_=0,cap_=0;
};

static void check_integer_kernels() {
  const auto all=kernels();
  for(size_t offset_a=0;offset_a<64;++offset_a)for(size_t offset_b=0;offset_b<64;++offset_b) {
    std::vector<int8_t> aa(128+offset_a),bb(128+offset_b);
    int8_t* a=aa.data()+offset_a;int8_t* b=bb.data()+offset_b;
    for(int trial=0;trial<5;++trial) {
      for(size_t i=0;i<128;++i) {
        a[i]=trial==0 ? int8_t(-128) : trial==1 ? int8_t(127) : static_cast<int8_t>(int(next()%256)-128);
        b[i]=trial==0 ? int8_t(127) : trial==1 ? int8_t(-128) : trial==2 ? a[i] : static_cast<int8_t>(int(next()%256)-128);
      }
      const uint64_t expected=oracle(a,b,128);
      for(const auto& k:all) {
        require(k.dynamic(a,b,128)==expected,"dynamic uint64 oracle");
        require(k.fixed(a,b,128)==expected,"fixed uint64 oracle/unaligned buffer");
      }
    }
  }
  for(size_t n:{size_t(0),size_t(1),size_t(15),size_t(16),size_t(31),size_t(32),size_t(127),size_t(129),size_t(256),size_t(65535),size_t(65536)}) {
    require(quantized_detail::choose_query_distance(n)==quantized_detail::choose_distance(),"non128 dispatch unchanged");
    std::vector<int8_t> a(n,-128),b(n,127);
    require(quantized_detail::choose_query_distance(n)(a.data(),b.data(),n)==oracle(a.data(),b.data(),n),"non128 exact distance");
  }
  require(quantized_distance_squared(nullptr,nullptr,0)==0,"null empty public distance");
  require(quantized_distance_squared(nullptr,nullptr,128)==0,"invalid pointers rejected");
  require(quantized_distance_squared(nullptr,nullptr,65537)==0,"invalid dimension rejected");
#if defined(MVDB_DISABLE_FIXED128_QUANTIZED)
  require(quantized_detail::choose_query_distance(128)==quantized_detail::choose_distance(),"disabled dispatch unchanged");
#else
  for(const auto& k:all)if(quantized_detail::choose_distance()==k.dynamic)
    require(quantized_detail::choose_query_distance(128)==k.fixed,"same ISA selected once for128");
#endif
}

static void check_guard_page() {
#if defined(__linux__)
  const size_t page=static_cast<size_t>(::sysconf(_SC_PAGESIZE));
  require(page>=128,"page size");
  void* memory=::mmap(nullptr,6*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  require(memory!=MAP_FAILED,"mmap");
  char* base=static_cast<char*>(memory);
  require(::mprotect(base+page,page,PROT_READ|PROT_WRITE)==0,"first readable page");
  require(::mprotect(base+4*page,page,PROT_READ|PROT_WRITE)==0,"second readable page");
  for(bool at_end:{false,true}) {
    auto* a=reinterpret_cast<int8_t*>(base+page+(at_end?page-128:0));
    auto* b=reinterpret_cast<int8_t*>(base+4*page+(at_end?page-128:0));
    std::fill(a,a+128,int8_t(-128));std::fill(b,b+128,int8_t(127));
    for(const auto& k:kernels())require(k.fixed(a,b,128)==128u*255u*255u,"guarded128 exact bounds");
  }
  require(::munmap(memory,6*page)==0,"munmap");
#endif
}

static void check_bound_equivalence() {
  size_t decisions=0,rejected=0;
  for(size_t n:{size_t(16),size_t(127),size_t(128),size_t(129),size_t(512)}) {
    for(int trial=0;trial<40;++trial) {
      std::vector<float> q(n),row(n);
      double max_abs=0;
      for(size_t i=0;i<n;++i) {
        q[i]=trial==0 ? -0.0f : static_cast<float>(int(next()%513)-256)/64;
        row[i]=trial==0 ? 0.0f : static_cast<float>(int(next()%513)-256)/64;
        if(trial==1) {q[i]=std::numeric_limits<float>::max();row[i]=-q[i];}
        if(trial==2) {q[i]=std::numeric_limits<float>::denorm_min();row[i]=-q[i];}
        max_abs=std::max(max_abs,std::max(std::abs(double(q[i])),std::abs(double(row[i]))));
      }
      const double step=quantization_step(max_abs);
      QuantizedQueryBound current(q.data(),n,step);
      DynamicBoundReference original(q.data(),n,step);
      require(current.enabled()==original.enabled(),"bound enabled parity");
      require(current.coded_query()==original.coded_query(),"coded query parity");
      std::vector<int8_t> coded(n);
      if(!current.enabled())continue;
      for(size_t i=0;i<n;++i)require(quantize_coordinate(row[i],step,&coded[i]),"row encode");
      const double exact=distance_squared(row.data(),q.data(),n);
      const std::vector<double> thresholds={0.0,-0.0,exact*0.1,exact*0.5,std::nextafter(exact,0.0),exact,
        std::nextafter(exact,std::numeric_limits<double>::infinity()),exact*2,-1,
        std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()};
      for(int pass=0;pass<3;++pass)for(size_t j=0;j<thresholds.size();++j) {
        const double threshold=thresholds[pass==1 ? thresholds.size()-1-j : j];
        const bool actual=current.definitely_greater(coded.data(),threshold);
        require(actual==original.definitely_greater(coded.data(),threshold),"whole-bound baseline equality");
        require(!actual || exact>threshold,"no false exact-distance rejection");
        ++decisions;rejected+=actual;
      }
      require(!current.definitely_greater(nullptr,exact),"null row conservative");
    }
  }
#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
  require(decisions>0,"tested whole-bound decisions");
  require(rejected>0,"exercise positive reject path");
#endif
  std::cout<<"PASS fixed128 whole-bound decisions="<<decisions<<" rejected="<<rejected<<'\n';
}

static volatile uint64_t consumed=0;
#if defined(__GNUC__) && !defined(__clang__)
#define TEST_NOINLINE __attribute__((noinline,noclone))
#elif defined(__clang__)
#define TEST_NOINLINE __attribute__((noinline))
#else
#define TEST_NOINLINE
#endif
TEST_NOINLINE static double time_kernel(Kernel fn,const std::vector<int8_t>& rows,const int8_t* query,size_t rounds) {
  const auto start=std::chrono::steady_clock::now();
  uint64_t sum=0;
  for(size_t rep=0;rep<rounds;++rep)for(size_t i=0;i<rows.size();i+=128)sum+=fn(rows.data()+i,query,128);
  consumed=sum;
  return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
}
template<class Bound>
TEST_NOINLINE static double time_bound(const std::vector<int8_t>& rows,const std::vector<float>& query,size_t rounds,bool changing) {
  Bound bound(query.data(),128,0.0625);
  const auto start=std::chrono::steady_clock::now();
  uint64_t sum=0;
  for(size_t rep=0;rep<rounds;++rep)for(size_t i=0;i<rows.size()/128;++i) {
    const double threshold=changing ? 2400.0-double((i/16)%128)*8 : 1400.0;
    sum+=bound.definitely_greater(rows.data()+i*128,threshold);
  }
  consumed=sum;
  return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
}
static void benchmark() {
  std::vector<int8_t> rows(4096*128),query(128);
  std::vector<float> float_query(128);
  for(auto& x:rows)x=static_cast<int8_t>(int(next()%201)-100);
  for(size_t i=0;i<128;++i) {query[i]=static_cast<int8_t>(int(next()%129)-64);float_query[i]=float(query[i])*0.0625f;}
  for(const auto& k:kernels()) {
    time_kernel(k.dynamic,rows,query.data(),8);time_kernel(k.fixed,rows,query.data(),8);
    for(int pair=0;pair<8;++pair) {
      double old_time,new_time;
      if(pair%2==0) {old_time=time_kernel(k.dynamic,rows,query.data(),128);new_time=time_kernel(k.fixed,rows,query.data(),128);}
      else {new_time=time_kernel(k.fixed,rows,query.data(),128);old_time=time_kernel(k.dynamic,rows,query.data(),128);}
      std::cout<<"{\"scope\":\"integer_kernel_only\",\"isa\":\""<<k.name<<"\",\"pair\":"<<pair<<",\"baseline_seconds\":"<<old_time<<",\"candidate_seconds\":"<<new_time<<",\"ratio\":"<<old_time/new_time<<"}\n";
    }
  }
  for(bool changing:{false,true}) {
    time_bound<DynamicBoundReference>(rows,float_query,8,changing);time_bound<QuantizedQueryBound>(rows,float_query,8,changing);
    for(int pair=0;pair<8;++pair) {
      double old_time,new_time;
      if(pair%2==0) {old_time=time_bound<DynamicBoundReference>(rows,float_query,128,changing);new_time=time_bound<QuantizedQueryBound>(rows,float_query,128,changing);}
      else {new_time=time_bound<QuantizedQueryBound>(rows,float_query,128,changing);old_time=time_bound<DynamicBoundReference>(rows,float_query,128,changing);}
      std::cout<<"{\"scope\":\"whole_query_bound\",\"changing_cap\":"<<(changing?"true":"false")<<",\"pair\":"<<pair<<",\"baseline_seconds\":"<<old_time<<",\"candidate_seconds\":"<<new_time<<",\"ratio\":"<<old_time/new_time<<"}\n";
    }
  }
}
int main(int argc,char** argv) {
  require(argc==1 || (argc==2 && std::string(argv[1])=="--bench"),"usage: test-fixed128 [--bench]");
  check_integer_kernels();check_guard_page();check_bound_equivalence();
  std::cout<<"PASS fixed128 uint64 oracle, all supported ISAs, unaligned/end-guard buffers, non128 fallback\n";
  if(argc==2)benchmark();
}

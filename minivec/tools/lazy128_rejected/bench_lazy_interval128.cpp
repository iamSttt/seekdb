// Standalone mechanism benchmark, not a database/official-score measurement.
// Compile this file with -O3 -std=c++17 -fno-math-errno -pthread,
// -I PATH/minivec/src, and output name bench-lazy.
// ./bench-lazy [--pairs 8] [--target-ms 25]
// Counters and full-sort oracle checks run separately from timed calls.
#include "lazy_interval128.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <time.h>

using namespace mvdb;
using Vector=std::array<float,128>;
using Key=std::pair<double,int64_t>;
struct Row { int64_t pk; Vector value; };
struct Dataset { std::string name; std::vector<Row> rows; Vector query{}; };
constexpr size_t kRows=10000;
constexpr size_t kMaximumRepeats=512;
static volatile uint64_t consumed=0;
static uint64_t random_state=0xb619821114e73a5dull;
static uint64_t next_random() {
  random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;
  return random_state;
}
static void require(bool value,const char* message) {
  if(!value)throw std::runtime_error(message);
}
static float from_bits(uint32_t bits) {float value;std::memcpy(&value,&bits,4);return value;}
static uint64_t mix(uint64_t state,uint64_t value) {
  return (state^(value+0x9e3779b97f4a7c15ull+(state<<6)+(state>>2)))*0xbf58476d1ce4e5b9ull;
}
static uint64_t hash_keys(const std::vector<Key>& keys) {
  uint64_t value=0x356ea91c432d67bfull;
  for(const auto& key:keys) {
    uint64_t bits;std::memcpy(&bits,&key.first,8);
    value=mix(value,bits);value=mix(value,static_cast<uint64_t>(key.second));
  }
  return value;
}
static std::vector<Dataset> datasets() {
  std::vector<Dataset> all;
  for(unsigned family=0;family<4;++family) {
    Dataset data;
    data.name=family==0 ? "ordinary_dispersed" : family==1 ? "dense_sign_equal_norm" :
              family==2 ? "canonical_near_ties" : "float_sum_overflow";
    data.rows.resize(kRows);
    if(family==0)for(auto& x:data.query)x=float(int(next_random()%8193)-4096)/1024;
    if(family==2)data.query[3]=std::numeric_limits<float>::denorm_min();
    for(size_t i=0;i<kRows;++i) {
      auto& row=data.rows[i];
      row.pk=(i&1 ? -9007199254742000ll : 9007199254742000ll)+static_cast<int64_t>(i);
      for(size_t d=0;d<128;++d) {
        if(family==0)row.value[d]=float(int(next_random()%8193)-4096)/1024;
        else if(family==1)row.value[d]=(next_random()&1) ? 1.0f : -1.0f;
        else if(family==2) {
          row.value[d]=d==0 ? 0x1p30f : d==1 ? 16.0f : 0.0f;
          if(i%3==1 && (d==2 || d==3))row.value[d]=8;
          if(i%3==2 && (d==2 || d==4))row.value[d]=8;
        } else {
          // Finite, varied large magnitudes: each float square overflows, while
          // real/canonical double distances are finite. The baseline's existing
          // double rejection fallback is deliberately retained in run_old.
          const uint32_t sign=uint32_t(next_random()&1)<<31;
          const uint32_t exponent=220u+uint32_t(next_random()%35);
          row.value[d]=from_bits(sign|(exponent<<23)|uint32_t(next_random()&0x7fffffu));
        }
      }
    }
    for(size_t i=kRows;i>1;--i)std::swap(data.rows[i-1],data.rows[next_random()%i]);
    all.push_back(std::move(data));
  }
  return all;
}

struct Counters { uint64_t bound_rejections=0,distance_squared_calls=0; };
struct OldResult { std::vector<Key> keys; Counters counts; };
#if defined(__GNUC__) && !defined(__clang__)
#define NOINLINE __attribute__((noinline,noclone))
#elif defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

template<bool Instrument>
NOINLINE static OldResult run_old(const Dataset& data,size_t k) {
  OldResult result;result.keys.reserve(k);
  DistanceBoundContext bound(128);
  for(const auto& row:data.rows) {
    if(result.keys.size()==k && bound.definitely_greater(row.value.data(),data.query.data(),result.keys.front().first)) {
      if constexpr(Instrument)++result.counts.bound_rejections;
      continue;
    }
    if constexpr(Instrument)++result.counts.distance_squared_calls;
    const Key candidate{distance_squared(row.value.data(),data.query.data(),128),row.pk};
    if(result.keys.size()<k) {
      result.keys.push_back(candidate);
      std::push_heap(result.keys.begin(),result.keys.end());
    } else if(candidate<result.keys.front()) {
      std::pop_heap(result.keys.begin(),result.keys.end());
      result.keys.back()=candidate;
      std::push_heap(result.keys.begin(),result.keys.end());
    }
  }
  std::sort(result.keys.begin(),result.keys.end());
  return result;
}
struct LazyResult { std::vector<Key> keys; lazy128::Statistics counts; };
template<bool Instrument>
NOINLINE static LazyResult run_lazy(const Dataset& data,size_t k) {
  lazy128::TopK128<Instrument> heap(data.query.data(),k);
  for(const auto& row:data.rows)heap.offer(row.pk,row.value.data());
  LazyResult result;result.keys.reserve(k);
  heap.for_each_materialized([&](int64_t pk,double value){result.keys.emplace_back(value,pk);});
  std::sort(result.keys.begin(),result.keys.end());
  if constexpr(Instrument)result.counts=heap.statistics();
  return result;
}
static std::vector<Key> oracle(const Dataset& data,size_t k) {
  std::vector<Key> keys;keys.reserve(data.rows.size());
  for(const auto& row:data.rows)keys.emplace_back(distance_exact(row.value.data(),data.query.data(),128),row.pk);
  std::sort(keys.begin(),keys.end());keys.resize(std::min(k,keys.size()));return keys;
}
static double thread_seconds() {
  timespec stamp{};
  if(::clock_gettime(CLOCK_THREAD_CPUTIME_ID,&stamp)!=0)throw std::runtime_error("thread clock unavailable");
  return double(stamp.tv_sec)+double(stamp.tv_nsec)*1e-9;
}
struct Timing { double wall=0,cpu=0; uint64_t digest=0; };
static uint64_t expected_digest(uint64_t once,size_t repeats) {
  uint64_t digest=0x725aad76f30e193bull;
  for(size_t i=0;i<repeats;++i)digest=mix(digest,once);
  return digest;
}
NOINLINE static Timing measure(const Dataset& data,size_t k,bool lazy,size_t repeats) {
  Timing result;result.digest=0x725aad76f30e193bull;
  const double cpu_start=thread_seconds();
  const auto wall_start=std::chrono::steady_clock::now();
  // The choice is outside the repeated query loop; both include construction,
  // ranking, final K materialization, winner sorting, checksum and destruction.
  if(lazy) {
    for(size_t i=0;i<repeats;++i) {
      const auto answer=run_lazy<false>(data,k);
      result.digest=mix(result.digest,hash_keys(answer.keys));
    }
  } else {
    for(size_t i=0;i<repeats;++i) {
      const auto answer=run_old<false>(data,k);
      result.digest=mix(result.digest,hash_keys(answer.keys));
    }
  }
  result.wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-wall_start).count();
  result.cpu=thread_seconds()-cpu_start;
  consumed=result.digest;
  return result;
}
static double median(std::vector<double> values) {
  std::sort(values.begin(),values.end());
  const size_t n=values.size();require(n!=0,"empty median");
  return n&1 ? values[n/2] : (values[n/2-1]+values[n/2])*0.5;
}
static size_t calibrate(const Dataset& data,size_t k,double target,uint64_t expected) {
  std::vector<double> baseline,candidate;
  for(unsigned pass=0;pass<3;++pass) {
    Timing old_time,new_time;
    if(pass&1) {new_time=measure(data,k,true,1);old_time=measure(data,k,false,1);}
    else {old_time=measure(data,k,false,1);new_time=measure(data,k,true,1);}
    require(old_time.digest==expected_digest(expected,1) && new_time.digest==expected_digest(expected,1),
            "calibration winner checksum differs from oracle");
    baseline.push_back(old_time.wall);candidate.push_back(new_time.wall);
  }
  const double faster=std::max(1e-6,std::min(median(baseline),median(candidate)));
  const size_t repeats=std::min(kMaximumRepeats,std::max(size_t(1),static_cast<size_t>(std::ceil(target/faster))));
  std::cout<<"{\"type\":\"calibration\",\"case\":\""<<data.name<<"\",\"k\":"<<k
           <<",\"pilot_old_wall_seconds\":"<<median(baseline)<<",\"pilot_lazy_wall_seconds\":"<<median(candidate)
           <<",\"repeats\":"<<repeats<<",\"target_segment_ms\":"<<target*1000<<"}\n";
  return repeats;
}
static void benchmark(const Dataset& data,size_t k,unsigned pairs,double target) {
  const auto wanted=oracle(data,k);
  // Full result comparisons and instrumentation are outside all measured loops.
  const auto old_check=run_old<true>(data,k);
  const auto lazy_check=run_lazy<true>(data,k);
  require(old_check.keys==wanted,"baseline full winner list differs from ExactSum full sort");
  require(lazy_check.keys==wanted,"lazy full winner list differs from ExactSum full sort");
  require(run_old<false>(data,k).keys==wanted && run_lazy<false>(data,k).keys==wanted,
          "uninstrumented full winners differ from ExactSum full sort");
  const uint64_t expected=hash_keys(wanted);
  const auto& s=lazy_check.counts;
  std::cout<<"{\"type\":\"untimed_oracle_counts\",\"case\":\""<<data.name<<"\",\"rows\":"<<data.rows.size()
           <<",\"k\":"<<k<<",\"winner_equality\":true,\"winner_checksum\":"<<expected
           <<",\"old_bound_rejections\":"<<old_check.counts.bound_rejections
           <<",\"old_distance_squared_calls\":"<<old_check.counts.distance_squared_calls
           <<",\"lazy_float_sums\":"<<s.float_sums<<",\"lazy_float_rejections\":"<<s.float_rejects
           <<",\"lazy_interval_comparisons\":"<<s.interval_comparisons<<",\"lazy_overlap_comparisons\":"<<s.overlap_comparisons
           <<",\"lazy_exact_fallbacks\":"<<s.exact_fallbacks<<",\"lazy_distance_exact_calls\":"<<s.exact_calls
           <<",\"lazy_final_exact_calls\":"<<s.final_exact_calls<<"}\n";
  const size_t repeats=calibrate(data,k,target,expected);
  const uint64_t required_digest=expected_digest(expected,repeats);
  std::vector<double> wall_ratios,cpu_ratios,old_wall,new_wall,old_cpu,new_cpu;
  for(unsigned pair=0;pair<pairs;++pair) {
    Timing old_time,new_time;
    if(pair&1) {new_time=measure(data,k,true,repeats);old_time=measure(data,k,false,repeats);}
    else {old_time=measure(data,k,false,repeats);new_time=measure(data,k,true,repeats);}
    require(old_time.digest==required_digest && new_time.digest==required_digest,"timed winner checksum changed");
    require(old_time.wall>0 && new_time.wall>0 && old_time.cpu>0 && new_time.cpu>0,"nonpositive duration");
    const double wall=old_time.wall/new_time.wall,cpu=old_time.cpu/new_time.cpu;
    wall_ratios.push_back(wall);cpu_ratios.push_back(cpu);
    old_wall.push_back(old_time.wall);new_wall.push_back(new_time.wall);
    old_cpu.push_back(old_time.cpu);new_cpu.push_back(new_time.cpu);
    std::cout<<"{\"type\":\"raw_pair\",\"case\":\""<<data.name<<"\",\"k\":"<<k<<",\"pair\":"<<pair
             <<",\"order\":\""<<(pair&1 ? "lazy_then_old" : "old_then_lazy")<<"\",\"repeats\":"<<repeats
             <<",\"old_wall_seconds\":"<<old_time.wall<<",\"lazy_wall_seconds\":"<<new_time.wall
             <<",\"old_cpu_seconds\":"<<old_time.cpu<<",\"lazy_cpu_seconds\":"<<new_time.cpu
             <<",\"wall_ratio_old_over_lazy\":"<<wall<<",\"cpu_ratio_old_over_lazy\":"<<cpu
             <<",\"winner_checksum_verified\":true,\"digest\":"<<required_digest<<"}\n";
  }
  std::cout<<"{\"type\":\"median\",\"case\":\""<<data.name<<"\",\"k\":"<<k<<",\"pairs\":"<<pairs
           <<",\"repeats\":"<<repeats<<",\"wall_ratio_old_over_lazy\":"<<median(wall_ratios)
           <<",\"cpu_ratio_old_over_lazy\":"<<median(cpu_ratios)
           <<",\"old_wall_seconds\":"<<median(old_wall)<<",\"lazy_wall_seconds\":"<<median(new_wall)
           <<",\"old_cpu_seconds\":"<<median(old_cpu)<<",\"lazy_cpu_seconds\":"<<median(new_cpu)<<"}\n";
}
int main(int argc,char** argv) {
  try {
    unsigned pairs=8;double target_ms=25;
    for(int i=1;i<argc;++i) {
      const std::string flag=argv[i];
      if(flag=="--pairs" && i+1<argc)pairs=static_cast<unsigned>(std::stoul(argv[++i]));
      else if(flag=="--target-ms" && i+1<argc)target_ms=std::stod(argv[++i]);
      else throw std::invalid_argument("usage: bench-lazy [--pairs 8] [--target-ms 25]");
    }
    require(pairs>=2 && pairs<=32 && pairs%2==0,"pairs must be an even number in 2..32");
    require(std::isfinite(target_ms) && target_ms>=5 && target_ms<=1000,"target-ms must be 5..1000");
    if(!lazy128::supported()) {
      std::cout<<"{\"type\":\"unsupported\",\"reason\":\"lazy128 admission unavailable; no timing run\"}\n";
      return 77;
    }
    std::cout<<std::setprecision(17)
             <<"{\"type\":\"scope\",\"rows\":10000,\"dimensions\":128,\"pairs\":"<<pairs
             <<",\"target_segment_ms\":"<<target_ms<<",\"max_repeats\":"<<kMaximumRepeats
             <<",\"single_thread\":true,\"quantized_prefilter\":false,\"shared_bound\":false"
             <<",\"instrumentation_in_timing\":false,\"short_screen\":"<<(pairs<8 ? "true" : "false")
             <<",\"oracle\":\"unchanged ExactSum full sort, independent of float envelope but not a second ExactSum implementation\""
             <<",\"ratio\":\"old/lazy; above 1 favors lazy\",\"claim\":\"mechanism only; not database or official score\"}\n";
    const auto all=datasets();
    for(const auto& data:all)for(size_t k:{size_t(10),size_t(32)})benchmark(data,k,pairs,target_ms/1000);
    std::cout<<"{\"type\":\"complete\",\"fixtures\":8,\"all_winner_checks_passed\":true}\n";
  } catch(const std::exception& error) {
    std::cerr<<"FAIL benchmark: "<<error.what()<<'\n';return 1;
  }
}

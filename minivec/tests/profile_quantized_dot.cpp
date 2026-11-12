// Diagnostic only; not linked into the submitted database. Quantized codes must
// be in [-127,127], as produced by quantize_coordinate (not arbitrary int8_t).
#include "quantized_bound.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
static volatile uint64_t observed=0;
static uint32_t norm(const int8_t* a,size_t n) {
  uint32_t sum=0;for(size_t i=0;i<n;++i){const int x=a[i];sum+=uint32_t(x*x);}return sum;
}
static uint32_t oracle(const int8_t* a,const int8_t* b,size_t n) {
  uint64_t sum=0;for(size_t i=0;i<n;++i){const int64_t d=int64_t(a[i])-b[i];sum+=uint64_t(d*d);}
  return static_cast<uint32_t>(sum);
}
#if defined(MVDB_SIMD_BOUND_X86)
// abs(a)*sign(b,a) equals a*b. No PMADDUBSW saturation: each signed
// pair sum lies within +/-2*127*127 = +/-32258. Four products fit int32;
// the entire dot product for n<=65536 has magnitude <=1,057,030,144.
// Intel intrinsic semantics: unsigned-byte * signed-byte, adjacent pair sums
// with signed saturation; VPSIGNB negates/zeros/retains its first operand.
// https://www.intel.com/content/dam/develop/external/us/en/documents/cpp_compiler_classic.pdf
__attribute__((target("avx2,no-fma"),noinline))
static uint32_t dot_distance(const int8_t* a,const int8_t* b,size_t n,uint32_t an,uint32_t bn) {
  __m256i sums=_mm256_setzero_si256();
  const __m256i ones=_mm256_set1_epi16(1);
  size_t i=0;
  for(;i+32<=n;i+=32) {
    const __m256i x=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a+i));
    const __m256i y=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(b+i));
    const __m256i pairs=_mm256_maddubs_epi16(_mm256_abs_epi8(x),_mm256_sign_epi8(y,x));
    sums=_mm256_add_epi32(sums,_mm256_madd_epi16(pairs,ones));
  }
  __m128i total=_mm_add_epi32(_mm256_castsi256_si128(sums),_mm256_extracti128_si256(sums,1));
  total=_mm_add_epi32(total,_mm_srli_si128(total,8));
  total=_mm_add_epi32(total,_mm_srli_si128(total,4));
  int64_t dot=_mm_cvtsi128_si32(total);
  for(;i<n;++i)dot+=int64_t(a[i])*b[i];
  return static_cast<uint32_t>(int64_t(an)+bn-2*dot);
}
static void check() {
  std::mt19937 rng(193771);std::uniform_int_distribution<int> value(-127,127);
  for(size_t n:{size_t(0),size_t(1),size_t(15),size_t(16),size_t(31),size_t(32),
               size_t(33),size_t(128),size_t(768),size_t(65535),size_t(65536)}) {
    std::vector<int8_t> a(n+3),b(n+3); // unaligned spans, odd tails
    for(int pattern=0;pattern<7;++pattern) {
      for(size_t i=0;i<n;++i) {
        a[i+1]=static_cast<int8_t>(pattern<4?((pattern&1)?127:-127):value(rng));
        b[i+2]=static_cast<int8_t>(pattern<4?((pattern&2)?127:-127):(pattern==4?0:value(rng)));
      }
      const auto* x=a.data()+1;const auto* y=b.data()+2;
      if(dot_distance(x,y,n,norm(x,n),norm(y,n))!=oracle(x,y,n))
        throw std::runtime_error("dot kernel differs from uint64 oracle");
    }
  }
}
static Json measure(unsigned seed) {
  const size_t rows=10000,dimension=128,queries=256;
  std::mt19937 rng(seed);std::normal_distribution<double> normal(0,16);
  std::vector<int8_t> data(rows*dimension),query(queries*dimension);
  for(auto* buffer:{&data,&query})for(auto& v:*buffer)
    v=static_cast<int8_t>(std::max(-127,std::min(127,int(std::lround(normal(rng))))));
  std::vector<uint32_t> norms(rows);
  const auto build=Clock::now();
  for(size_t i=0;i<rows;++i)norms[i]=norm(data.data()+i*dimension,dimension);
  const double build_seconds=std::chrono::duration<double>(Clock::now()-build).count();
  const auto baseline=mvdb::quantized_detail::choose_distance();
  auto run=[&](bool dot) {
    uint64_t checksum=0;const auto begin=Clock::now();
    for(size_t j=0;j<queries;++j) {
      const int8_t* q=query.data()+j*dimension;
      const uint32_t qn=dot?norm(q,dimension):0;
      for(size_t i=0;i<rows;++i) {
        const int8_t* row=data.data()+i*dimension;
        checksum+=dot?dot_distance(row,q,dimension,norms[i],qn):baseline(row,q,dimension);
      }
    }
    const double seconds=std::chrono::duration<double>(Clock::now()-begin).count();
    observed=checksum;return std::make_pair(seconds,checksum);
  };
  run(false);run(true);
  Json pairs=Json::array();
  for(int i=0;i<5;++i) {
    std::pair<double,uint64_t> before,after;
    if(i%2){after=run(true);before=run(false);}else{before=run(false);after=run(true);}
    if(before.second!=after.second)throw std::runtime_error("microbenchmark checksum mismatch");
    pairs.push_back({{"baseline_seconds",before.first},{"dot_seconds",after.first},
        {"baseline_over_dot",before.first/after.first},{"checksum",before.second}});
  }
  return {{"seed",seed},{"rows",rows},{"dimension",dimension},{"queries",queries},
          {"row_norm_build_seconds",build_seconds},{"row_norm_bytes",norms.size()*sizeof(uint32_t)},
          {"pairs",pairs}};
}
#endif
int main() {
  try {
    Json report={{"scope","isolated integer kernel diagnostic; not database/end-to-end performance"},
                 {"limitation","precomputed row norms add memory and build cost; integration/budget not implemented"}};
#if defined(MVDB_SIMD_BOUND_X86)
    report["baseline_kernel"]=__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx2")
        ? "avx512" : (__builtin_cpu_supports("avx2") ? "avx2" : "sse2_or_scalar");
    if(__builtin_cpu_supports("avx2")) {
      check();report["status"]="passed";report["oracle"]="uint64 full scan, endpoints, zero, unaligned and odd-length spans";
      report["runs"]=Json::array();for(unsigned seed:{7319u,8911u,13337u})report["runs"].push_back(measure(seed));
    } else report["status"]="skipped_no_avx2";
#else
    report["status"]="skipped_non_x86";
#endif
    std::cout<<report.dump(2)<<'\n';return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}

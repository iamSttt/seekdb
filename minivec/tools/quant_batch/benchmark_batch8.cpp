#include "batch8.h"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>
#include <initializer_list>
#include <string>

#if defined(__GNUC__) && !defined(__clang__)
#define OPAQUE __attribute__((noinline,noclone,noipa))
#else
#define OPAQUE __attribute__((noinline))
#endif
static volatile uint64_t sink=0;
OPAQUE uint64_t baseline(mvdb::quantized_detail::Distance kernel,const int8_t* rows,const int8_t* queries,size_t n,size_t count,size_t query_count){
  uint64_t sum=0;
  for(size_t q=0;q<query_count;++q)
    for(size_t row=0;row<count;++row)sum+=kernel(rows+row*n,queries+q*n,n);
  return sum;
}
OPAQUE uint64_t candidate(batch8::Kernel kernel,const int8_t* rows,const int8_t* queries,size_t n,size_t count,size_t query_count){
  uint64_t sum=0;uint32_t distances[8];
  for(size_t q=0;q<query_count;++q)
    for(size_t row=0;row<count;row+=8){
      const size_t batch=std::min<size_t>(8,count-row);
      kernel(rows+row*n,n,queries+q*n,n,batch,distances);
      for(size_t r=0;r<batch;++r)sum+=distances[r];
    }
  return sum;
}
struct Timing{double wall,cpu;uint64_t checksum;};
static double cpu_clock(){timespec t{};if(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t))throw std::runtime_error("clock_gettime");return double(t.tv_sec)+t.tv_nsec*1e-9;}
template<class F>Timing measure(F fn){
  const auto w=std::chrono::steady_clock::now();const double c=cpu_clock();
  const uint64_t checksum=fn();
  const double elapsed=cpu_clock()-c;
  const double wall=std::chrono::duration<double>(std::chrono::steady_clock::now()-w).count();
  sink^=checksum;return {wall,elapsed,checksum};
}
int main(int argc,char** argv){
  if(argc!=3)return 2;
  const size_t pairs=std::stoul(argv[1]),query_count=std::stoul(argv[2]);
  if(pairs!=4 || query_count!=64)return 2;
#if defined(MVDB_SIMD_BOUND_X86)
  if(!__builtin_cpu_supports("avx2")){std::cerr<<"AVX2 unavailable\n";return 77;}
  auto original_avx2=mvdb::quantized_detail::distance_avx2;
  auto original_native=mvdb::quantized_detail::choose_distance();
  const char* native_name=original_native==original_avx2?"avx2":"avx512";
  std::mt19937 random(0x327a88);
  std::cout<<std::setprecision(17);
  for(size_t n:std::initializer_list<size_t>{128,512})for(size_t rows_count:std::initializer_list<size_t>{10000,10003}){
    std::vector<int8_t> rows(n*rows_count),queries(n*query_count);
    for(auto& v:rows)v=int8_t(int(random()%256)-128);
    for(auto& v:queries)v=int8_t(int(random()%256)-128);
    for(int comparison=0;comparison<(original_native==original_avx2?1:2);++comparison){
      auto original=comparison?original_native:original_avx2;
      const char* label=comparison?"native_baseline_vs_avx2_batch":"same_isa_avx2";
      // Both paths warm exactly the same full immutable corpus before timing.
      const auto b=baseline(original,rows.data(),queries.data(),n,rows_count,query_count);
      const auto c=candidate(batch8::avx2,rows.data(),queries.data(),n,rows_count,query_count);
      if(b!=c)throw std::runtime_error("warmup checksum mismatch");sink^=b;sink^=c;
      for(size_t pair=0;pair<pairs;++pair){
        Timing before{},after{};
        auto run_b=[&]{return measure([&]{return baseline(original,rows.data(),queries.data(),n,rows_count,query_count);});};
        auto run_c=[&]{return measure([&]{return candidate(batch8::avx2,rows.data(),queries.data(),n,rows_count,query_count);});};
        if(pair%2==0){before=run_b();after=run_c();}else{after=run_c();before=run_b();}
        if(before.checksum!=after.checksum || before.checksum!=b)throw std::runtime_error("measured checksum mismatch");
        std::cout<<"{\"comparison\":\""<<label<<"\",\"baseline_isa\":\""<<(comparison?native_name:"avx2")
          <<"\",\"candidate_isa\":\"avx2\",\"dimension\":"<<n<<",\"rows\":"<<rows_count
          <<",\"queries\":"<<query_count<<",\"pair\":"<<pair<<",\"order\":\""<<(pair%2?"BA":"AB")
          <<"\",\"baseline_wall\":"<<before.wall<<",\"candidate_wall\":"<<after.wall
          <<",\"baseline_cpu\":"<<before.cpu<<",\"candidate_cpu\":"<<after.cpu
          <<",\"baseline_checksum\":"<<before.checksum<<",\"candidate_checksum\":"<<after.checksum<<"}\n";
      }
    }
  }
  return 0;
#else
  std::cerr<<"x86 GNU compatible compiler required\n";return 77;
#endif
}

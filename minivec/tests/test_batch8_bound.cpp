#include "quantized_bound.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
#ifndef MVDB_BATCH8_GLOBAL_SCAN
#error This differential test requires the actual global batch8 API
#endif
using namespace mvdb;
static uint64_t checks=0,groups=0,usable=0,unusable=0,rejections=0,changes=0;
static void need(bool value,const char* why){++checks;if(!value){std::cerr<<why<<'\n';std::exit(1);}}
static uint64_t wide(const int8_t* a,const int8_t* b,size_t n){
  uint64_t sum=0;for(size_t i=0;i<n;++i){const int64_t d=int64_t(a[i])-int64_t(b[i]);sum+=uint64_t(d*d);}return sum;
}
static void domain(size_t n,double step){
  std::vector<float> query(n),values(8*n);std::vector<int8_t> query_codes(n),codes(8*n);
  for(size_t i=0;i<n;++i){
    query[i]=float((int((i*17)%161)-80+.25)*step);
    need(quantize_coordinate(query[i],step,&query_codes[i]),"query encode");
    for(size_t row=0;row<8;++row){
      values[row*n+i]=float((int((i*23+row*37)%241)-120-.25)*step);
      need(quantize_coordinate(values[row*n+i],step,&codes[row*n+i]),"row encode");
    }
  }
  QuantizedQueryBound original(query.data(),n,step),batched(query.data(),n,step);
  need(original.enabled() && batched.enabled(),"eligible bound disabled");
  need(batched.coded_query()==query_codes,"coded query differs");
  bool expected_support=quantized_detail::choose_distance()==quantized_detail::distance_avx2;
#if defined(MVDB_BATCH8_ALLOW_NATIVE_AVX512) && !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  expected_support=expected_support || quantized_detail::choose_distance()==quantized_detail::distance_avx512;
#endif
  need(batched.batch8_supported()==expected_support,"batch dispatch eligibility differs");
  // AVX2 availability is checked in main. Calling this internal raw helper is
  // safe here even when production elects to retain a native AVX512 row path.
  std::array<uint32_t,10> staged{};staged.front()=staged.back()=0xdeadbeef;
  batched.compute_batch8(codes.data(),staged.data()+1);++groups;
  need(staged.front()==0xdeadbeef && staged.back()==0xdeadbeef,"staging overwrote guards");
  std::array<double,8> exact{};
  for(size_t row=0;row<8;++row){
    need(staged[row+1]==wide(codes.data()+row*n,query_codes.data(),n),"batch integer distance differs from uint64 oracle");
    exact[row]=distance_exact(values.data()+row*n,query.data(),n);
  }
  const double infinity=std::numeric_limits<double>::infinity();
  for(size_t pass=0;pass<2;++pass)for(size_t row=0;row<8;++row){
    const double d=exact[row];
    std::vector<double> thresholds={-1,-0.0,0,std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(),std::nextafter(d,0.0),d,std::nextafter(d,infinity),
      d*.25,d*2,std::numeric_limits<double>::max(),infinity,std::numeric_limits<double>::quiet_NaN()};
    if(pass)std::reverse(thresholds.begin(),thresholds.end());
    for(double threshold:thresholds){
      const bool cap=batched.batch8_cap_usable(threshold);cap?++usable:++unusable;
      const bool old=original.definitely_greater(codes.data()+row*n,threshold);
      const bool now=batched.reject_precomputed(staged[row+1],threshold);
      need(now==old,"old row and precomputed decision differ");
      need(now==QuantizedQueryBound(query.data(),n,step).definitely_greater(codes.data()+row*n,threshold),"fresh/cached decision differs");
      need(!now || d>threshold,"rejected exact <= threshold");rejections+=now;
    }
  }
  // The raw sums are reused while the caller changes the cap after every row.
  double threshold=exact[0]*2;
  for(size_t round=0;round<8;++round)for(size_t row=0;row<8;++row){
    const bool old=original.definitely_greater(codes.data()+row*n,threshold);
    need(old==batched.reject_precomputed(staged[row+1],threshold),"dynamic threshold uses stale batch-start cap");
    const double next=round%2 ? exact[row]*2 : std::nextafter(std::min(threshold,exact[row]),0.0);
    changes+=next!=threshold;threshold=next;
  }
}
static void disabled(){
  std::vector<float> q(16,.5f);std::array<int8_t,16> row{};
  auto check=[&](const QuantizedQueryBound& bound){
    need(!bound.enabled() && !bound.batch8_supported(),"disabled bound supports batch");
    for(double cap:{0.0,1.0,std::numeric_limits<double>::infinity()})
      need(!bound.batch8_cap_usable(cap) && !bound.reject_precomputed(UINT32_MAX,cap) &&
           !bound.definitely_greater(row.data(),cap),"disabled bound rejects");
  };
  check(QuantizedQueryBound(q.data(),15,1));check(QuantizedQueryBound(q.data(),65537,1));
  check(QuantizedQueryBound(nullptr,16,1));
  check(QuantizedQueryBound(q.data(),16,0));check(QuantizedQueryBound(q.data(),16,3));
  check(QuantizedQueryBound(q.data(),16,std::numeric_limits<double>::infinity()));
  check(QuantizedQueryBound(q.data(),16,0x1p-600));
  q[0]=std::numeric_limits<float>::infinity();check(QuantizedQueryBound(q.data(),16,1));q[0]=.5f;
  const unsigned original=_mm_getcsr();
  for(unsigned mask:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}){
    _mm_setcsr(original|mask);QuantizedQueryBound bound(q.data(),16,1);check(bound);_mm_setcsr(original);
  }
}
int main(){
  if(!__builtin_cpu_supports("avx2")){std::cout<<"batch8_bound=UNSUPPORTED\n";return 77;}
  for(size_t n:{size_t(16),size_t(17),size_t(31),size_t(128),size_t(129),size_t(512),size_t(65535),size_t(65536)})
    for(double step:{0x1p-6,1.0,0x1p20})domain(n,step);
  disabled();need(groups==24 && usable && unusable && rejections && changes,"meaningful differential branches not exercised");
  std::cout<<"batch8_bound_groups="<<groups<<" usable="<<usable<<" unusable="<<unusable<<" rejections="<<rejections<<" changes="<<changes
    <<"\nbatch8_bound_checks="<<checks<<"\nbatch8_bound=PASS\n";
}

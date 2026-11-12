// g++ -std=c++17 -O2 -pthread -Isrc -DMVDB_ENABLE_BLOCK_QUANT_SCALE \
//   tests/test_quantized_scales.cpp src/flat_index.cpp -o test_quantized_scales
// Also run with MVDB_QUANTIZED_BLOCK_SIZE=64 and with the I11 macro omitted.
#include "flat_index.h"
#include "quantized_bound.h"
#include <algorithm>
#include <cassert>
#include <cfenv>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <vector>

// Failure injection touches only array allocation in the optional index/query
// structures. Ordinary vectors use operator new and remain independent.
static size_t fail_array=0;
void* operator new[](std::size_t bytes) {
  if(void* p=std::malloc(bytes ? bytes : 1))return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {
  if(fail_array && !--fail_array)return nullptr;
  try {return ::operator new[](bytes);}catch(...) {return nullptr;}
}
void operator delete[](void* p,const std::nothrow_t&) noexcept {std::free(p);}

using namespace mvdb;
static constexpr size_t generous=16u<<20;
static size_t checked=0,rejected=0;
static uint64_t integer_oracle(const int8_t* a,const int8_t* b,size_t n) {
  uint64_t sum=0;
  for(size_t i=0;i<n;++i) {
    const int64_t delta=int64_t(a[i])-int64_t(b[i]);
    sum+=uint64_t(delta*delta);
  }
  return sum;
}
static std::unique_ptr<FlatIndex> build(std::vector<std::vector<float>>& rows,
                                      size_t budget=generous,size_t fail=0) {
  auto index=std::make_unique<FlatIndex>(".",rows.front().size(),rows.size(),budget);
  for(size_t i=0;i<rows.size();++i)
    index->add(int64_t(rows.size()-i),i,rows[i].data(),rows[i].size());
  fail_array=fail;index->finish();fail_array=0;
  assert(index->resident_bytes()<=budget);
  return index;
}
static bool local_expected() {
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
  return true;
#else
  return false;
#endif
}
static void verify(std::vector<std::vector<float>>& rows,bool expect_local) {
  auto index=build(rows);
  FlatIndex::ResidentView view;assert(index->resident_view(&view));
  assert(view.quantization_scales.present()==(expect_local && local_expected()));
#ifdef MVDB_DISABLE_QUANTIZED_BOUND
  assert(!view.quantized);return;
#endif
  assert(view.quantized);
  if(view.quantization_scales.present())assert(view.quantization_step==0);
  const size_t n=rows[0].size();
  std::vector<std::vector<float>> queries{std::vector<float>(n,0),rows[0],rows.back()};
  for(float value:{-0.015625f,0.5f,127.0f})
    queries.emplace_back(n,value);
  for(const auto& query:queries) {
    const QuantizedQueryBound bound(query.data(),n,view.quantization_step,view.quantization_scales);
    for(size_t i=0;i<rows.size();++i) {
      const auto& scales=view.quantization_scales;
      const double step=scales.present() ? scales.steps[scales.block_ids[i/scales.block_rows]] :
                                          view.quantization_step;
      std::vector<int8_t> qcodes(n);bool encodable=true;
      for(size_t d=0;d<n;++d) {
        const long double error=std::fabs(static_cast<long double>(rows[i][d])-
            static_cast<long double>(view.quantized[i*n+d])*step);
        assert(error<=static_cast<long double>(step)*0.5L);
        encodable=quantize_coordinate(query[d],step,&qcodes[d]) && encodable;
      }
      if(encodable)
        assert(quantized_distance_squared(view.quantized+i*n,qcodes.data(),n)==
               integer_oracle(view.quantized+i*n,qcodes.data(),n));
      const double exact=distance_exact(rows[i].data(),query.data(),n);
      const QuantizedQueryBound single(query.data(),n,step);
      for(double threshold:{exact*0.25,std::nextafter(exact,0.0),exact,
                            std::nextafter(exact,std::numeric_limits<double>::infinity()),
                            exact*2,0.0,-1.0,std::numeric_limits<double>::infinity()}) {
        const bool got=bound.definitely_greater(view.quantized+i*n,threshold,i);
        assert(!got || exact>threshold);
        assert(got==single.definitely_greater(view.quantized+i*n,threshold));
        if(!encodable)assert(!got);
        checked++;rejected+=got;
      }
      if(scales.present())assert(!bound.definitely_greater(view.quantized+i*n,exact*0.25));
    }
    if(view.quantization_scales.present())
      assert(!bound.definitely_greater(view.quantized,1,rows.size()));
  }
}
static void distributions() {
  constexpr size_t count=769,n=65;
  std::mt19937 random(713);std::normal_distribution<float> gaussian(0,1);
  for(int family=0;family<3;++family) {
    std::vector<std::vector<float>> rows(count,std::vector<float>(n));
    for(size_t i=0;i<count;++i)for(size_t d=0;d<n;++d) {
      const float noise=std::max(-3.0f,std::min(3.0f,gaussian(random)));
      rows[i][d]=family==0 ? noise : family==1 ? noise*0.125f+float(i/256)*16 :
                     (i>=512 ? noise*128 : noise*0.125f);
    }
    // A fixed maximum per block makes the Gaussian case deliberately uniform;
    // the untruncated production benchmark separately measures natural data.
    for(size_t i=0;i<count;i+=256)
      rows[i][0]=family==0 ? 3 : family==1 ? float(i/256)*16+0.375f :
                            (i>=512 ? 384 : 0.375f);
    verify(rows,family!=0);
  }
  for(float extreme:{std::numeric_limits<float>::denorm_min(),
                     std::numeric_limits<float>::min(),std::numeric_limits<float>::max()}) {
    std::vector<std::vector<float>> rows(513,std::vector<float>(17,extreme));
    std::fill(rows.back().begin(),rows.back().end(),1.0f);
    verify(rows,true);
  }
  std::vector<std::vector<float>> zero(513,std::vector<float>(17,0));
  verify(zero,false);
  // Nine distinct binary scales must abandon all local metadata.
  std::vector<std::vector<float>> many(9*256,std::vector<float>(16));
  for(size_t i=0;i<many.size();++i)std::fill(many[i].begin(),many[i].end(),std::ldexp(1.0f,int(i/256)));
  verify(many,false);
}
static void budget_and_failures() {
  constexpr size_t n=16,count=513;
  std::vector<std::vector<float>> rows(count,std::vector<float>(n,1));
  std::fill(rows.back().begin(),rows.back().end(),32);
  const size_t base=count*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t)+
                           2*sizeof(int64_t)+sizeof(uint32_t)+n);
  const size_t metadata=3+QuantizedScaleView::max_steps*sizeof(double);
  const size_t cache=QuantizedQueryBound::scale_cache_bytes(n,2);
  const size_t edge=base+(2u<<20)+metadata+2*cache;
  for(size_t budget:{base,edge-1,edge,generous}) {
    auto index=build(rows,budget);
    FlatIndex::ResidentView view;assert(index->resident_view(&view));
    assert(view.quantization_scales.present()==(local_expected() && budget>=edge));
  }
  if(!local_expected())return;
  for(size_t fail:{size_t(1),size_t(2)}) {
    auto index=build(rows,generous,fail);
    FlatIndex::ResidentView view;assert(index->resident_view(&view));
    assert(!view.quantization_scales.present());
    assert(fail==1 ? view.quantized && view.quantization_step>0 : !view.quantized);
  }
  auto index=build(rows);FlatIndex::ResidentView view;assert(index->resident_view(&view));
  std::vector<float> query(n,0);
  for(size_t fail:{size_t(1),size_t(2)}) {
    fail_array=fail;
    QuantizedQueryBound bound(query.data(),n,0,view.quantization_scales);fail_array=0;
    assert(!bound.enabled() && !bound.definitely_greater(view.quantized,1,0));
  }
  auto bad=view.quantization_scales;--bad.query_cache_bytes;
  QuantizedQueryBound too_small(query.data(),n,0,bad);assert(!too_small.enabled());
  bad=view.quantization_scales;bad.step_count=9;
  QuantizedQueryBound too_many(query.data(),n,0,bad);assert(!too_many.enabled());
  bad=view.quantization_scales;bad.block_count=1;
  QuantizedQueryBound wrong_count(query.data(),n,0,bad);assert(!wrong_count.enabled());
  rows[0][0]=std::numeric_limits<float>::infinity();
  auto nonfinite=build(rows);assert(nonfinite->resident_view(&view) && !view.quantized);
  rows[0][0]=std::numeric_limits<float>::quiet_NaN();
  auto nan=build(rows);assert(nan->resident_view(&view) && !view.quantized);
}
static void fp_modes() {
  std::vector<std::vector<float>> rows(513,std::vector<float>(16,1));
  rows.back()[0]=64;
  const int saved=std::fegetround();
  assert(std::fesetround(FE_DOWNWARD)==0);
  auto index=build(rows);FlatIndex::ResidentView view;
  assert(index->resident_view(&view) && !view.quantized);
  assert(std::fesetround(saved)==0);
#ifdef MVDB_SIMD_BOUND_X86
  const unsigned saved_mxcsr=_mm_getcsr();_mm_setcsr(saved_mxcsr|(1u<<15)|(1u<<6));
  index=build(rows);assert(index->resident_view(&view) && !view.quantized);
  _mm_setcsr(saved_mxcsr);
#endif
}
int main() {
  if(!quantized_detail::normal_fp_environment()) {
    std::cout<<"SKIP unsupported floating environment\n";return 0;
  }
  distributions();budget_and_failures();fp_modes();
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  assert(rejected>0);
#endif
  std::cout<<"quantized_scales OK checked="<<checked<<" strict_rejections="<<rejected<<"\n";
}

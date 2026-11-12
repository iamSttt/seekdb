// Build with no macros, SELECTIVE alone, ENABLE alone, and ENABLE+SELECTIVE.
// Link flat_index.cpp. Selective only changes admission, never the exact bound.
#include "flat_index.h"
#include "quantized_bound.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>
using namespace mvdb;

static size_t checks=0;
static void check(size_t count,size_t dimension,size_t fine_rows,float fine,float coarse,
                  bool selective_expected,bool broad_expected=true,size_t budget=16u<<20) {
  std::vector<std::vector<float>> rows(count,std::vector<float>(dimension,coarse));
  for(size_t i=0;i<fine_rows;++i)std::fill(rows[i].begin(),rows[i].end(),fine);
  FlatIndex index(".",dimension,count,budget);
  for(size_t i=0;i<count;++i)index.add(int64_t(count-i),i,rows[i].data(),dimension);
  index.finish();
  FlatIndex::ResidentView view;assert(index.resident_view(&view));
  bool expected=false;
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
#ifdef MVDB_BLOCK_SCALE_SELECTIVE
  expected=selective_expected;
#else
  expected=broad_expected;
#endif
#endif
  (void)selective_expected;(void)broad_expected;
  assert(view.quantization_scales.present()==expected);
  assert(index.resident_bytes()<=budget);
#ifdef MVDB_DISABLE_QUANTIZED_BOUND
  assert(!view.quantized);return;
#endif
  assert(view.quantized && (expected ? view.quantization_step==0 : view.quantization_step>0));
  std::vector<float> query(dimension,0);
  QuantizedQueryBound bound(query.data(),dimension,view.quantization_step,view.quantization_scales);
  assert(bound.enabled());
  // Check edge ordinals, short-tail admission, exact ties and neighboring
  // thresholds. The reference is the full integer exact-distance oracle.
  for(size_t i:{size_t(0),fine_rows-1,fine_rows,count-1}) {
    if(i>=count)continue;
    const double exact=distance_exact(rows[i].data(),query.data(),dimension);
    const auto& scales=view.quantization_scales;
    const double step=expected ? scales.steps[scales.block_ids[i/scales.block_rows]] : view.quantization_step;
    QuantizedQueryBound single(query.data(),dimension,step);
    for(double threshold:{0.0,exact*0.25,std::nextafter(exact,0.0),exact,
        std::nextafter(exact,std::numeric_limits<double>::infinity()),std::numeric_limits<double>::infinity()}) {
      const bool rejected=bound.definitely_greater(view.quantized+i*dimension,threshold,i);
      assert(!rejected || exact>threshold);
      assert(rejected==single.definitely_greater(view.quantized+i*dimension,threshold));
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && defined(MVDB_BLOCK_SCALE_SELECTIVE)
      if(!bound.local_scales())assert(rejected==bound.definitely_greater_global(view.quantized+i*dimension,threshold));
#endif
      ++checks;
    }
    for(size_t d=0;d<dimension;++d)
      assert(std::fabs(double(rows[i][d])-view.quantized[i*dimension+d]*step)<=step*0.5);
  }
  if(expected) {
    assert(!bound.definitely_greater(view.quantized,1));
    assert(!bound.definitely_greater(view.quantized,1,count));
    auto invalid=view.quantization_scales;invalid.query_cache_bytes=0;
    QuantizedQueryBound unavailable(query.data(),dimension,0,invalid);
    assert(!unavailable.enabled() && !unavailable.definitely_greater(view.quantized,1,0));
  }
}

int main() {
  // Four complete fine blocks / five blocks: exactly 80%, exactly 4x finer.
  check(1280,128,1024,31.75f,127.0f,true);
  check(1280,127,1024,31.75f,127.0f,false);
  check(1280,16,1024,31.75f,127.0f,false);
  check(1280,768,1024,31.75f,127.0f,true);
  // Actual row coverage, not proportion of block IDs. The extra coarse row
  // flips the precise 80% boundary; a 1-row coarse tail must not get 50% weight.
  check(1279,128,1024,31.75f,127.0f,true);
  check(1281,128,1024,31.75f,127.0f,false);
  check(257,128,256,31.75f,127.0f,true);
  check(1025,128,769,31.75f,127.0f,false);
  check(1280,128,1024,63.5f,127.0f,false); // only 2x finer
  check(1280,128,1024,127.0f,127.0f,false,false); // no finer scale
  // No extra local metadata quota may override an otherwise viable global index.
  check(1280,128,1024,31.75f,127.0f,false,false,1280*(24+20+128));
  std::cout<<"selective_scale OK exact admission/tail/dimension/cache gates; bound_checks="<<checks<<"\n";
}

// Standalone diagnostic, NOT an official-score or whole-request benchmark.
// Compile paired binaries with exactly the same source/flags except
// MVDB_ENABLE_BLOCK_QUANT_SCALE. Optional I10 block size must match both.
#include "flat_index.h"
#include "quantized_bound.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <vector>
using namespace mvdb;
using Clock=std::chrono::steady_clock;
using Candidate=std::pair<double,int64_t>;
static double seconds(Clock::time_point a,Clock::time_point b) {
  return std::chrono::duration<double>(b-a).count();
}
int main(int argc,char** argv) {
  const size_t rows=argc>1 ? std::stoull(argv[1]) : 20000;
  const size_t dimension=argc>2 ? std::stoull(argv[2]) : 128;
  const size_t queries=argc>3 ? std::stoull(argv[3]) : 20;
  if(!rows || !dimension || dimension>65536)return 2;
  for(const std::string family:{"gaussian","clustered","outlier","dispersed_outlier"}) {
    std::mt19937 random(20261113);
    std::normal_distribution<float> gaussian(0,1);
    std::vector<std::vector<float>> data(rows,std::vector<float>(dimension));
    std::vector<double> norms(rows);
    for(size_t i=0;i<rows;++i) {
      const bool outlier=family=="outlier" ? i+rows/100>=rows :
                         family=="dispersed_outlier" ? i%100==0 : false;
      const float scale=outlier ? 128 : family=="clustered" ? 0.125f : 1;
      const float center=family=="clustered" ? float(i%8)*4 : 0;
      for(float& value:data[i])value=gaussian(random)*scale+center;
      long double norm=0;
      for(float value:data[i])norm+=static_cast<long double>(value)*value;
      norms[i]=static_cast<double>(norm);
    }
    std::vector<size_t> order(rows);std::iota(order.begin(),order.end(),0);
    // The normal index path orders low-norm rows first. Also test deliberately
    // dispersed outliers in unsorted physical blocks to expose its limitation.
    if(family!="dispersed_outlier")
      std::sort(order.begin(),order.end(),[&](size_t a,size_t b){return norms[a]<norms[b];});
    const auto build_start=Clock::now();
    FlatIndex index(".",dimension,rows,16u<<20);
    for(size_t i:order)index.add(int64_t(i),i,data[i].data(),dimension);
    index.finish();const auto build_end=Clock::now();
    FlatIndex::ResidentView view;
    if(!index.resident_view(&view))return 3;
    long double error2=0;double error_max=0;
    if(view.quantized)for(size_t i=0;i<rows;++i) {
      const auto& scales=view.quantization_scales;
      const double step=scales.present() ? scales.steps[scales.block_ids[i/scales.block_rows]] :
                                          view.quantization_step;
      for(size_t d=0;d<dimension;++d) {
        const long double e=static_cast<long double>(view.rows[i][d])-
                             static_cast<long double>(view.quantized[i*dimension+d])*step;
        if(std::fabs(e)>step*0.5L) {std::cerr<<"envelope violation\n";return 4;}
        error2+=e*e;error_max=std::max(error_max,static_cast<double>(std::fabs(e)));
      }
    }
    size_t rejected=0,exact_count=0;double query_seconds=0,cache_seconds=0;
    uint64_t checksum=0;
    for(size_t q=0;q<queries;++q) {
      std::vector<float> query(dimension);
      for(float& value:query)value=gaussian(random)*(family=="clustered" ? 0.125f : 1);
      // Independent exhaustive oracle includes PK tie ordering and canonical
      // exact distance; it is excluded from the measured candidate path.
      std::priority_queue<Candidate> reference;
      for(size_t i=0;i<rows;++i) {
        Candidate value(distance_exact(view.rows[i],query.data(),dimension),view.keys[i]);
        if(reference.size()<10)reference.push(value);
        else if(value<reference.top()) {reference.pop();reference.push(value);}
      }
      const auto start=Clock::now();
      const QuantizedQueryBound bound(query.data(),dimension,view.quantization_step,
                                      view.quantization_scales);
      const auto cache_end=Clock::now();
      std::priority_queue<Candidate> heap;
      for(size_t i=0;i<rows;++i) {
        if(heap.size()==10 && bound.definitely_greater(view.quantized ? view.quantized+i*dimension : nullptr,
                                                      heap.top().first,i)) {++rejected;continue;}
        Candidate value(distance_exact(view.rows[i],query.data(),dimension),view.keys[i]);++exact_count;
        if(heap.size()<10)heap.push(value);
        else if(value<heap.top()) {heap.pop();heap.push(value);}
      }
      const auto end=Clock::now();query_seconds+=seconds(start,end);cache_seconds+=seconds(start,cache_end);
      if(heap.size()!=reference.size())return 5;
      while(!heap.empty()) {
        if(heap.top()!=reference.top()) {std::cerr<<"top-k mismatch\n";return 6;}
        checksum=checksum*1315423911ull+uint64_t(heap.top().second);
        heap.pop();reference.pop();
      }
    }
    std::cout<<std::setprecision(10)<<"{\"family\":\""<<family<<"\",\"rows\":"<<rows
      <<",\"dimension\":"<<dimension<<",\"queries\":"<<queries
      <<",\"local\":"<<(view.quantization_scales.present()?"true":"false")
      <<",\"steps\":"<<view.quantization_scales.step_count
      <<",\"resident_bytes\":"<<index.resident_bytes()
      <<",\"build_s\":"<<seconds(build_start,build_end)<<",\"query_s\":"<<query_seconds
      <<",\"cache_s\":"<<cache_seconds<<",\"error_rmse\":"
      <<std::sqrt(static_cast<double>(error2/(rows*dimension)))
      <<",\"error_max\":"<<error_max<<",\"rejected\":"<<rejected
      <<",\"exact\":"<<exact_count<<",\"checksum\":"<<checksum<<"}\n";
  }
}

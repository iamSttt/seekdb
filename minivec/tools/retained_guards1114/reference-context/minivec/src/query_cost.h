#pragma once
#include "query_pool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sched.h>
#include <string>

namespace mvdb {
namespace query_cost_detail {
// Query execution is single-client. Only the main thread touches these bounded
// samples; worker heaps and pruning state remain private to each scan half.
struct Sample { unsigned count=0; double ns_per_row=0; };
inline std::array<Sample,32>& samples() {static std::array<Sample,32> value{};return value;}
inline size_t bucket(size_t dim,size_t k,bool quantized) {
  const size_t d=dim<=32?0:dim<=128?1:dim<=512?2:3;
  const size_t top=k<=1?0:k<=10?1:k<=100?2:3;
  return (quantized?16:0)+4*d+top;
}
inline bool quota_allows_two(const char* text) {
  if(!text)return true;
  if(std::strncmp(text,"max ",4)==0)return true;
  unsigned long long quota=0,period=0;
  return std::sscanf(text,"%llu %llu",&quota,&period)!=2 || !period ||
      static_cast<long double>(quota)/period>=2;
}
inline bool read_quota(const char* path) {
  FILE* file=std::fopen(path,"r");
  if(!file)return true; // Unknown is not a fabricated zero CPU limit.
  char value[128]={};const bool got=std::fgets(value,sizeof(value),file)!=nullptr;
  std::fclose(file);return !got || quota_allows_two(value);
}
inline bool two_cpus_available() {
  cpu_set_t affinity;CPU_ZERO(&affinity);
  if(sched_getaffinity(0,sizeof(affinity),&affinity)==0 && CPU_COUNT(&affinity)<2)return false;
  // Docker's cgroup namespace normally exposes the container at this root.
  if(!read_quota("/sys/fs/cgroup/cpu.max"))return false;
  // Also inspect the unified membership and its visible ancestors on hosts
  // without a private cgroup namespace. Refuse path traversal components.
  FILE* file=std::fopen("/proc/self/cgroup","r");
  if(!file)return true;
  char line[4096]={};std::string member;
  while(std::fgets(line,sizeof(line),file))if(std::strncmp(line,"0::/",4)==0) {
    member=line+3;break;
  }
  std::fclose(file);
  while(!member.empty() && (member.back()=='\n' || member.back()=='\r'))member.pop_back();
  if(member.empty() || member[0]!='/' || member.find("/../")!=std::string::npos ||
      (member.size()>=3 && member.compare(member.size()-3,3,"/..")==0))return true;
  while(member.size()>1) {
    if(!read_quota((std::string("/sys/fs/cgroup")+member+"/cpu.max").c_str()))return false;
    const size_t slash=member.find_last_of('/');member.resize(slash?slash:1);
  }
  return true;
}
inline bool enabled() {
#ifdef MVDB_ENABLE_WORKLOAD_FANOUT
  return true;
#else
  return false;
#endif
}
inline bool predicts_gain(double ns_per_row,size_t rows,size_t k,double wake) {
  if(!std::isfinite(ns_per_row) || ns_per_row<=0 || !std::isfinite(wake) || wake<0)return false;
  // Reserve 15% of ideal split saving for duplicate heap setup and imbalance;
  // K-dependent merge allowance is deliberately conservative and measured in
  // the paired experiment, never treated as a formal CPU cost model.
  const double scan_us=ns_per_row*static_cast<double>(rows)*1e-3;
  const double merge_us=static_cast<double>(k)*std::log2(double(k)+1)*0.004;
  return scan_us*0.425-2*wake-merge_us>=query_pool_detail::kMinGainUs;
}
} // namespace query_cost_detail

inline bool query_workload_fanout(size_t rows,size_t dimension,size_t k,bool quantized) {
  using namespace query_cost_detail;
  if(!enabled())return query_fanout_wanted(rows);
  static const bool capacity=two_cpus_available();
  if(!capacity || query_fanout_mode()==FanoutMode::Never)return false;
  if(query_fanout_mode()==FanoutMode::Force)return query_fanout_wanted(rows);
  if(rows<query_fanout_min_rows())return false;
  const Sample& sample=samples()[bucket(dimension,k,quantized)];
  if(sample.count<3)return false; // Measure real serial requests, no extra scan.
  const bool wanted=predicts_gain(sample.ns_per_row,rows,k,query_wake_us());
  static unsigned logged=0;
  if(logged++<8)std::fprintf(stderr,"mvdb: workload fanout rows=%zu dim=%zu k=%zu coded=%d measured_ns_per_row=%.3f -> %u threads\n",
      rows,dimension,k,int(quantized),sample.ns_per_row,wanted?2:1);
  return wanted;
}

class QueryCostObservation {
 public:
  QueryCostObservation(size_t rows,size_t dimension,size_t k,bool quantized,bool fanout,bool eligible)
      :rows_(rows),bucket_(query_cost_detail::bucket(dimension,k,quantized)) {
    collect_=query_cost_detail::enabled() && eligible && !fanout && rows>0 &&
        query_fanout_mode()==FanoutMode::Adaptive && query_cost_detail::samples()[bucket_].count<3;
    if(collect_)start_=std::chrono::steady_clock::now();
  }
  void finish() {
    if(!collect_)return;
    collect_=false;
    const double ns=std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start_).count()/rows_;
    if(!std::isfinite(ns) || ns<=0)return;
    auto& sample=query_cost_detail::samples()[bucket_];
    sample.ns_per_row=(sample.ns_per_row*sample.count+ns)/(sample.count+1);
    ++sample.count;
  }
 private:
  size_t rows_,bucket_;
  bool collect_=false;
  std::chrono::steady_clock::time_point start_;
};
} // namespace mvdb

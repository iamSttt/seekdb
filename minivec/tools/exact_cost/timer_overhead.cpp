#include "cost_diagnostic.h"
#include <algorithm>
#include <vector>
int main(){
  // Raw back-to-back clock delta. It is reported, never subtracted from samples.
  std::vector<uint64_t> x;x.reserve(20000);
  for(unsigned i=0;i<20000;++i){auto a=mvdb_cost::now();auto b=mvdb_cost::now();x.push_back(b-a);}
  std::sort(x.begin(),x.end());uint64_t sum=0;for(auto v:x)sum+=v;
  std::printf("{\"clock\":\"CLOCK_THREAD_CPUTIME_ID\",\"count\":20000,\"sum_ns\":%llu,\"min_ns\":%llu,\"median_ns\":%llu,\"p95_ns\":%llu,\"subtracted\":false}\n",
    (unsigned long long)sum,(unsigned long long)x.front(),(unsigned long long)x[10000],(unsigned long long)x[19000]);
}

#include "query_cost.h"
#include <cassert>
#include <iostream>
#include <limits>

int main() {
  using namespace mvdb::query_cost_detail;
  assert(quota_allows_two("max 100000"));
  assert(quota_allows_two("200000 100000"));
  assert(!quota_allows_two("100000 100000"));
  assert(!quota_allows_two("150000 100000"));
  assert(!quota_allows_two("199999 100000"));
  assert(quota_allows_two("300000 100000"));
  assert(bucket(16,1,false)!=bucket(768,1,false));
  assert(bucket(128,1,false)!=bucket(128,100,false));
  assert(bucket(128,10,false)!=bucket(128,10,true));
  for(size_t d:{1u,16u,128u,512u,768u,65536u})for(size_t k:{1u,10u,100u,1024u})
    for(bool q:{false,true})assert(bucket(d,k,q)<samples().size());
  assert(!predicts_gain(1,1000,10,10));
  assert(predicts_gain(100,10000,10,10));
  assert(!predicts_gain(100,10000,10,1000));
  assert(!predicts_gain(0,10000,10,0));
  assert(!predicts_gain(std::numeric_limits<double>::infinity(),10000,10,0));
  assert(!predicts_gain(100,10000,10,std::numeric_limits<double>::quiet_NaN()));
  std::cout<<"PASS bounded workload buckets, CPU quota and conservative fanout decisions\n";
}

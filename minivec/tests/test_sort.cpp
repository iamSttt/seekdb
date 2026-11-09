#include "result_sort.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>
using namespace mvdb;
int main(int argc,char** argv) {
  const bool stress=argc>1 && std::string(argv[1])=="--stress-fanin";
  if (stress) {
    struct rlimit cap{96ull<<20,96ull<<20}; assert(::setrlimit(RLIMIT_AS,&cap)==0);
  }
  char path[]="/tmp/mvdb-sort-test-XXXXXX"; assert(::mkdtemp(path));
  try {
    for (bool descending : {false,true}) {
      const size_t count=stress?192:96;
      const size_t payload_size=stress?(2u<<20):(256u<<10);
      auto less=[&](const SortRow& a,const SortRow& b) {
        if(a.integer==b.integer)return a.pk<b.pk;
        return descending?a.integer>b.integer:a.integer<b.integer;
      };
      std::vector<SortRow> expected;
      ResultSorter sorter(path,less);
      for(size_t i=count;i-->0;) {
        SortRow row;row.pk=static_cast<int64_t>(i);
        row.integer=std::numeric_limits<int64_t>::max()-static_cast<int64_t>(i%7);
        SortRow key;key.pk=row.pk;key.integer=row.integer;expected.push_back(std::move(key));
        row.payload=std::to_string(i)+":"+std::string(payload_size,'x');
        sorter.add(std::move(row));
      }
      std::sort(expected.begin(),expected.end(),less);
      size_t seen=0;
      sorter.emit(count,[&](const std::string& row) {
        const size_t colon=row.find(':');assert(colon!=std::string::npos);
        assert(std::stoll(row.substr(0,colon))==expected.at(seen).pk);
        assert(row.size()-colon-1==payload_size);++seen;
      });
      assert(seen==count);
    }
    ResultSorter empty(path,[](const SortRow&a,const SortRow&b){return a.pk<b.pk;});
    empty.emit(0,[](const std::string&){std::abort();});
    std::cout<<"PASS external run merge, signed BIGINT keys, DESC ties, payload integrity"
             <<(stress?", fan-in under 96MiB address cap":"")<<'\n';
  } catch(const std::exception& e) {
    std::cerr<<"FAIL sort "<<(stress?"fan-in resource gate: ":"correctness: ")<<e.what()<<'\n';return 1;
  }
}

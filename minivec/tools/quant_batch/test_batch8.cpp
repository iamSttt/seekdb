#include "batch8.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <initializer_list>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

static size_t checks=0;
static void require(bool value,const char* text){++checks;if(!value){std::cerr<<text<<'\n';std::exit(1);}}
static uint32_t oracle(const int8_t* row,const int8_t* query,size_t n){
  uint64_t result=0;
  for(size_t i=0;i<n;++i){const int64_t d=int64_t(row[i])-int64_t(query[i]);result+=uint64_t(d*d);}
  require(result<=UINT32_MAX,"oracle overflow");return static_cast<uint32_t>(result);
}
static int8_t random_code(std::mt19937& random){return static_cast<int8_t>(int(random()%256)-128);}
static void compare(const int8_t* rows,size_t stride,const int8_t* query,size_t n,size_t count){
  std::array<uint32_t,10> output;output.fill(0xdeadbeef);
  require(batch8::compute(rows,stride,query,n,count,output.data()+1),"valid input rejected");
  require(output.front()==0xdeadbeef && output.back()==0xdeadbeef,"output guards overwritten");
  for(size_t r=0;r<count;++r){
    const auto expected=oracle(n?rows+r*stride:nullptr,query,n);
    require(output[r+1]==expected,"batch differs from independent uint64 oracle");
    if(n)require(mvdb::quantized_detail::choose_distance()(rows+r*stride,query,n)==expected,"original native differs");
#if defined(MVDB_SIMD_BOUND_X86)
    if(n && __builtin_cpu_supports("avx2"))require(mvdb::quantized_detail::distance_avx2(rows+r*stride,query,n)==expected,"original AVX2 differs");
#endif
  }
  for(size_t r=count;r<8;++r)require(output[r+1]==0xdeadbeef,"unused output overwritten");
}
static void cases(){
  std::mt19937 random(0x89ae5271);
  for(size_t n:std::initializer_list<size_t>{0,1,7,15,16,17,31,32,33,127,128,129,511,512,513,65535,65536}){
    for(size_t padding:std::initializer_list<size_t>{0,3}){
      const size_t stride=n+padding;
      std::vector<int8_t> rows(stride*8+3),query(n+3);
      for(int pattern=0;pattern<4;++pattern){
        for(auto& v:rows)v=pattern==0 ? int8_t(-128) : pattern==1 ? int8_t(127) : pattern==2 ? int8_t(0) : random_code(random);
        for(auto& v:query)v=pattern==0 ? int8_t(127) : pattern==1 ? int8_t(-128) : pattern==2 ? int8_t(0) : random_code(random);
        for(size_t count=0;count<=8;++count)compare(rows.data()+1,stride,query.data()+2,n,count);
      }
    }
  }
  for(size_t attempt=0;attempt<64;++attempt){
    const size_t n=random()%2049,stride=n+random()%19,count=random()%9;
    std::vector<int8_t> rows(stride*8+1),query(n+1);
    for(auto& v:rows)v=random_code(random);for(auto& v:query)v=random_code(random);
    compare(rows.data(),stride,query.data(),n,count);
  }
  compare(nullptr,0,nullptr,0,8);compare(nullptr,0,nullptr,0,0);
  require(batch8::compute(nullptr,65536,nullptr,65536,0,nullptr),"zero count should need no buffers");
  std::array<uint32_t,8> output;output.fill(0xdeadbeef);
  int8_t value=0;
  require(!batch8::compute(&value,1,&value,1,9,output.data()),"row count accepted");
  require(!batch8::compute(&value,65537,&value,65537,1,output.data()),"dimension accepted");
  require(!batch8::compute(&value,0,&value,1,1,output.data()),"bad stride accepted");
  require(!batch8::compute(nullptr,1,&value,1,1,output.data()),"null rows accepted");
  require(!batch8::compute(&value,1,nullptr,1,1,output.data()),"null query accepted");
  require(!batch8::compute(&value,1,&value,1,1,nullptr),"null output accepted");
  require(!batch8::compute(&value,SIZE_MAX,&value,1,8,output.data()),"overflow stride accepted");
  for(auto value:output)require(value==0xdeadbeef,"invalid input modified output");
}
struct Guarded {
  size_t allocated;void* base;int8_t* data;
  explicit Guarded(size_t bytes){
    const auto page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    allocated=((bytes+page-1)/page+1)*page;
    base=mmap(nullptr,allocated,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    require(base!=MAP_FAILED,"mmap failed");
    auto end=static_cast<int8_t*>(base)+allocated-page;
    require(mprotect(end,page,PROT_NONE)==0,"mprotect failed");data=end-bytes;
  }
  ~Guarded(){munmap(base,allocated);}
};
static void no_overread(){
  for(size_t n:std::initializer_list<size_t>{1,15,16,17,31,32,127,128,129,511,512,513}){
    for(size_t count=1;count<=8;++count){
      Guarded rows(n*count),query(n);
      for(size_t i=0;i<n*count;++i)rows.data[i]=int8_t(int(i%256)-128);
      for(size_t i=0;i<n;++i)query.data[i]=int8_t(127-int(i%256));
      compare(rows.data,n,query.data,n,count);
    }
  }
}
// This is a generic integer threshold/order model, NOT the database's float
// bound or TopK implementation. Every row consumes its current cap, which can
// shrink after any accepted row; all rejection decisions must match exactly.
static void dynamic_threshold_order(){
  std::mt19937 random(128);
  for(size_t n:std::initializer_list<size_t>{17,128,512}){
    const size_t count=67;std::vector<int8_t> rows(n*count),query(n);
    for(auto& v:rows)v=random_code(random);for(auto& v:query)v=random_code(random);
    std::vector<uint32_t> expected(count),actual(count);
    for(size_t r=0;r<count;++r)expected[r]=oracle(rows.data()+r*n,query.data(),n);
    for(size_t r=0;r<count;r+=8)require(batch8::compute(rows.data()+r*n,n,query.data(),n,std::min<size_t>(8,count-r),actual.data()+r),"order compute");
    uint32_t old_cap=UINT32_MAX,new_cap=UINT32_MAX;
    for(size_t r=0;r<count;++r){
      const bool old_reject=expected[r]>old_cap,new_reject=actual[r]>new_cap;
      require(old_reject==new_reject && old_cap==new_cap,"dynamic cap decision/order differs");
      if(!old_reject)old_cap=expected[r];if(!new_reject)new_cap=actual[r];
    }
  }
}
int main(){
  cases();no_overread();dynamic_threshold_order();
  std::cout<<"batch8=PASS\nbatch8_checks="<<checks<<'\n';
}

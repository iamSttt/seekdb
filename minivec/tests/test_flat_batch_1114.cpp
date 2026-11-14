// Isolated A: R33 sequential build batching on 760f. Native execution pending.
// g++ -std=c++17 -O2 -pthread -Isrc -DMVDB_SPILL_BYTES=1
// tests/test_flat_batch_1114.cpp src/{db,flat_index,value}.cpp -Wl,--wrap=pwrite
#include "db.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <new>
#include <stdexcept>
#include <unistd.h>

namespace {
constexpr size_t dimension=128, stride=(dimension+4)*sizeof(float);
constexpr size_t block_records=(256u<<10)/stride, rows=2*block_records+7;
bool trace=false, interrupt_once=false, allocation_fail=false;
size_t calls=0, fail_call=0, zero_call=0, short_limit=0, partial_first=0;
void reset() {trace=true;interrupt_once=false;calls=fail_call=zero_call=short_limit=partial_first=0;}
}
void* operator new(size_t n) {
  if(allocation_fail && n==block_records*stride) {allocation_fail=false;throw std::bad_alloc();}
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* p,size_t n,off_t off) {
  char link[64],target[512];std::snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);
  const ssize_t len=::readlink(link,target,sizeof(target)-1);
  if(len>=0)target[len]=0;else target[0]=0;
  if(trace && std::strstr(target,"/.flat-")) {
    ++calls;
    if(interrupt_once) {interrupt_once=false;errno=EINTR;return -1;}
    if(calls==fail_call) {errno=ENOSPC;return -1;}
    if(calls==zero_call)return 0;
    if(calls==1 && partial_first)n=std::min(n,partial_first);
    if(short_limit)n=std::min(n,short_limit);
  }
  return __real_pwrite(fd,p,n,off);
}
using namespace mvdb;
template<class F> void fails(F action) {bool failed=false;try {action();}catch(const std::exception&) {failed=true;}assert(failed);}
struct Directory {
  std::string path;
  Directory() {char p[]="/tmp/mvdb-flat-batch-XXXXXX";assert(::mkdtemp(p));path=p;}
  ~Directory() {std::filesystem::remove_all(path);}
};
void add(FlatIndex& index,size_t i) {
  float v[dimension];for(size_t j=0;j<dimension;++j)v[j]=float(i%31)+float(j)*0.25f;
  const int64_t scalar=int64_t(i%17);
  index.add(int64_t(i)-19,i*3,v,dimension,&scalar,1);
}
void unreadable(FlatIndex& index) {
  FlatIndex::ResidentView view;assert(!index.resident_view(&view));
  int64_t scalar;assert(!index.has_spill_integer(1));assert(!index.spill_integer_value(0,1,&scalar));
  fails([&]{index.scan([](int64_t,uint64_t,const float*,size_t){assert(false);return true;});});
  fails([&]{index.scan_filtered([](size_t,int64_t,uint64_t){assert(false);return true;},
    [](int64_t,uint64_t,const float*,size_t){assert(false);return true;});});
}
void verify(FlatIndex& index,size_t n) {
  size_t seen=0;index.scan([&](int64_t pk,uint64_t slot,const float* v,size_t d) {
    assert(pk==int64_t(seen)-19 && slot==seen*3 && d==dimension);
    for(size_t j=0;j<d;++j)assert(v[j]==float(seen%31)+float(j)*0.25f);
    int64_t scalar;assert(index.spill_integer_value(seen,1,&scalar) && scalar==int64_t(seen%17));
    ++seen;return true;
  });assert(seen==n);
}
void physical(const std::string& path) {
  const size_t column=1;
  for(size_t n:{size_t(1),block_records,rows}) {
    FlatIndex index(path,dimension,n,0);assert(index.configure_spill_integers(&column,1,65536)==1);
    reset();for(size_t i=0;i<n;++i)add(index,i);unreadable(index);
    index.finish();index.finish();assert(calls==(n+block_records-1)/block_records);verify(index,n);
  }
  {
    FlatIndex index(path,dimension,rows,0);assert(index.configure_spill_integers(&column,1,65536)==1);
    reset();interrupt_once=true;short_limit=4093;
    for(size_t i=0;i<rows;++i)add(index,i);index.finish();
    assert(!interrupt_once && calls>3);verify(index,rows);
  }
  // First full block, middle full block, and final partial block at finish().
  for(size_t failing:{size_t(1),size_t(2),size_t(3)}) for(bool zero:{false,true}) {
    FlatIndex index(path,dimension,rows,0);assert(index.configure_spill_integers(&column,1,65536)==1);
    reset();if(zero)zero_call=failing;else fail_call=failing;
    fails([&]{for(size_t i=0;i<rows;++i)add(index,i);index.finish();});
    assert(calls==failing);trace=false;unreadable(index);
    fails([&]{index.finish();});fails([&]{add(index,index.size());});
  }
  {
    FlatIndex index(path,dimension,rows,0);assert(index.configure_spill_integers(&column,1,65536)==1);
    reset();partial_first=17;fail_call=2;
    fails([&]{for(size_t i=0;i<rows;++i)add(index,i);});assert(calls==2);
    trace=false;unreadable(index);fails([&]{index.finish();});
  }
  {
    FlatIndex index(path,dimension,rows,0);assert(index.configure_spill_integers(&column,1,65536)==1);
    reset();allocation_fail=true;fails([&]{add(index,0);});assert(!allocation_fail && calls==0 && index.size()==0);
    unreadable(index); // R33 OOM throws; it does not silently disable batching.
    for(size_t i=0;i<rows;++i)add(index,i);index.finish();verify(index,rows);
  }
  trace=false;
}
void database() {
  Directory dir;Database db;std::string err;int code=0;assert(db.open(dir.path,&err));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column g;g.name="g";g.type=Type::BIGINT;
  Column v;v.name="v";v.type=Type::VECTOR;v.len=dimension;
  assert(db.create_table("t",{id,g,v},&err,&code));assert(db.begin(&err,&code));
  for(size_t i=0;i<rows;++i)assert(db.insert("t",{Value::ofInt(i),Value::ofInt(i%17),Value::ofVec(std::vector<float>(dimension,float(i)))},&err,&code));
  assert(db.commit(&err,&code));assert(db.table("t")->payload_store);
  auto check=[&] {
    size_t n=0;db.scan_vectors("t",{},[&](int64_t key,const float* v,size_t d) {
      assert(d==dimension);for(size_t j=0;j<d;++j)assert(v[j]==float(key));++n;return true;
    });assert(n==rows);
  };
  // Failed CREATE must publish neither physical index nor metadata.
  for(bool oom:{false,true}) {
    assert(db.begin_statement(&err,&code));reset();if(oom)allocation_fail=true;else fail_call=3;
    assert(!db.create_vector_index("t","v","vi",&err,&code));
    assert(oom ? !allocation_fail : calls==3);trace=false;
    assert(!db.end_statement(false,&err,&code));
    assert(!db.has_vector_index("t") && !db.table("t")->vector_index);check();
  }
  assert(db.create_vector_index("t","v","vi",&err,&code));check();
  Cond key;key.column="id";key.value=Value::ofInt(7);
  assert(db.begin(&err,&code));assert(db.update_rows("t",{key},{{"v",Value::ofVec(std::vector<float>(dimension,-7))}},&err,&code)==1);
  reset();fail_call=3;fails([&]{db.scan_vectors("t",{},[](int64_t,const float*,size_t){assert(false);return true;});});
  trace=false;assert(!db.table("t")->vector_index);assert(db.rollback(&err,&code));check();
  db.close();reset();fail_call=3;assert(!db.open(dir.path,&err));assert(calls==3);
  trace=false;assert(db.open(dir.path,&err));check();assert(db.checkpoint(&err));db.close();assert(db.open(dir.path,&err));check();
}
int main() {alarm(180);Directory dir;physical(dir.path);database();std::cout<<"PASS flat batch boundaries, short/EINTR/ENOSPC/zero/OOM, failed publication, rollback and restart\n";}

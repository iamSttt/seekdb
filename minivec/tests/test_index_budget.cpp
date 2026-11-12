// g++ -std=c++17 -O2 -Isrc tests/test_index_budget.cpp src/flat_index.cpp -o test_index_budget
// Add -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite for EINTR/short/error injection.
#include "flat_index.h"
#include "quantized_bound.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using mvdb::FlatIndex;
namespace {
constexpr size_t metadata_stride=sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t);
constexpr size_t posting_peak=2*sizeof(int64_t)+sizeof(uint32_t);
size_t fail_array_bytes=0;
#ifdef MVDB_TEST_WRAP_PWRITE
size_t writes=0;
bool interrupt_next=false,short_writes=false,fail_writes=false,zero_write=false;
#endif
template<class F> void throws(F&& function) {
  bool caught=false;
  try {function();} catch(const std::runtime_error&) {caught=true;}
  assert(caught);
}
void check_budget(const std::string& dir) {
  // 35k x 128 crossed the former copied-payload admission threshold, despite
  // borrowing every vector. It now fits without changing the 16 MiB budget.
  constexpr size_t count=35000,dimension=128,budget=16u<<20;
  std::vector<float> vector(dimension,0.25f);
  FlatIndex index(dir,dimension,count,budget);
  assert(!index.disk_backed() && index.resident_bytes()<=budget);
  for(size_t i=0;i<count;++i)index.add(int64_t(i)-1000,i,vector.data(),dimension);
  index.finish();
  FlatIndex::ResidentView view;
  assert(index.resident_view(&view) && view.count==count);
  for(size_t i=0;i<count;++i) {
    assert(view.rows[i]==vector.data());
    assert(view.keys[i]==int64_t(i)-1000 && view.slots[i]==i);
  }
  assert(index.resident_bytes()<=budget);
  const uint32_t* ordinals=nullptr;
  size_t matches=0;
  assert(index.equal_ordinals(1,7,[](uint64_t slot){return int64_t(slot%11);},
                             &ordinals,&matches));
  for(size_t i=0;i<matches;++i)assert(view.slots[ordinals[i]]%11==7);
  assert(index.resident_bytes()<=budget);

  // Admission depends on owned metadata, not vector width. A one-byte deficit
  // still selects disk, and budget zero continues to force all nonempty spills.
  const size_t rows=7,base=rows*metadata_stride;
  FlatIndex exact(dir,dimension,rows,base);
  FlatIndex below(dir,dimension,rows,base-1);
  FlatIndex disk(dir,dimension,rows,0);
  assert(!exact.disk_backed() && exact.resident_bytes()==base);
  assert(below.disk_backed() && disk.disk_backed());
  for(size_t i=0;i<rows;++i)exact.add(int64_t(i),i,vector.data(),dimension);
  exact.finish();
  assert(!exact.equal_ordinals(0,0,[](uint64_t){return int64_t(0);},&ordinals,&matches));

  // A lazy posting reserves its sorting scratch before other tables can claim
  // that space. Failed scalar extraction cannot expose the previous column.
  const size_t posting_budget=rows*(metadata_stride+posting_peak);
  FlatIndex posting(dir,5,rows,posting_budget);
  for(size_t i=0;i<rows;++i)posting.add(int64_t(i),i,vector.data(),5);
  posting.finish();assert(posting.resident_bytes()==posting_budget);
  assert(posting.equal_ordinals(0,2,[](uint64_t slot){return int64_t(slot);},&ordinals,&matches));
  assert(matches==1 && ordinals[0]==2);
  throws([&]{posting.equal_ordinals(1,0,[](uint64_t)->int64_t {
    throw std::runtime_error("injected scalar read failure");
  },&ordinals,&matches);});
  assert(posting.equal_ordinals(1,1,[](uint64_t slot){return int64_t(slot%2);},&ordinals,&matches));
  assert(matches==3 && ordinals[0]==1 && ordinals[1]==3 && ordinals[2]==5);
  assert(posting.resident_bytes()<=posting_budget);

  // Several indexes share one unchanged budget via the public accounting API.
  size_t remaining=posting_budget*2;
  FlatIndex first(dir,5,rows,remaining);
  for(size_t i=0;i<rows;++i)first.add(int64_t(i),i,vector.data(),5);
  first.finish();remaining-=first.resident_bytes();
  FlatIndex second(dir,5,rows,remaining);
  for(size_t i=0;i<rows;++i)second.add(int64_t(i),i,vector.data(),5);
  second.finish();remaining-=second.resident_bytes();
  assert(!first.disk_backed() && !second.disk_backed() && remaining==0);
  FlatIndex third(dir,5,1,remaining);assert(third.disk_backed());

  // Charging the 20-byte construction peak must not shrink the old 65,536-row
  // posting coverage. A 60k low-dimensional table still caches its filter.
  constexpr size_t large_count=60000;
  FlatIndex large_posting(dir,5,large_count,budget);
  for(size_t i=0;i<large_count;++i)large_posting.add(int64_t(i),i,vector.data(),5);
  large_posting.finish();
  size_t reads=0;
  auto scalar=[&](uint64_t slot){++reads;return int64_t(slot%100);};
  assert(large_posting.equal_ordinals(0,7,scalar,&ordinals,&matches));
  assert(matches==600 && reads==large_count);
  for(size_t i=0;i<matches;++i)assert(ordinals[i]%100==7);
  assert(large_posting.equal_ordinals(0,8,scalar,&ordinals,&matches));
  assert(matches==600 && reads==large_count);
  assert(large_posting.resident_bytes()==large_count*(metadata_stride+posting_peak));
  assert(large_posting.resident_bytes()<=budget);
}
void check_disk(const std::string& dir,size_t dimension,size_t count) {
  FlatIndex disk(dir,dimension,count,0);
  assert(disk.disk_backed());
  std::vector<float> values(dimension);
#ifdef MVDB_TEST_WRAP_PWRITE
  writes=0;
#endif
  for(size_t i=0;i<count;++i) {
    for(size_t d=0;d<dimension;++d)values[d]=float(int((i*13+d)%1021)-510);
    disk.add(int64_t(i)-500,i*17,values.data(),dimension);
  }
  disk.finish();disk.finish();
  assert(disk.resident_bytes()==0);
#ifdef MVDB_TEST_WRAP_PWRITE
  if(!short_writes) {
    const size_t records=std::max<size_t>(1,(256u<<10)/(dimension*sizeof(float)+16));
    assert(writes==(count+records-1)/records);
  }
#endif
  size_t visited=0;
  disk.scan([&](int64_t pk,uint64_t slot,const float* row,size_t dim) {
    assert(dim==dimension && pk==int64_t(visited)-500 && slot==visited*17);
    for(size_t d=0;d<dimension;++d)assert(row[d]==float(int((visited*13+d)%1021)-510));
    ++visited;return true;
  });
  assert(visited==count);
  visited=0;disk.scan([&](int64_t,uint64_t,const float*,size_t){++visited;return false;});
  assert(visited==1);
}
void check_dot_budget(const std::string& dir) {
  constexpr size_t count=33,dimension=128,ann_reservation=2u<<20;
  size_t base=count*(metadata_stride+posting_peak);
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  base+=count*dimension;
#endif
  const size_t norms=count*sizeof(uint32_t);
  std::vector<float> values(count*dimension);
  for(size_t row=0;row<count;++row)for(size_t d=0;d<dimension;++d)
    values[row*dimension+d]=float(int((row*17+d*5)%127)-63)*0.123f;
  // Norms may use only space beyond the existing full ANN reservation. One
  // byte short must keep all old codes/postings, and discard only the dot path.
  for(size_t extra:{size_t(0),ann_reservation+norms-1,ann_reservation+norms}) {
    FlatIndex index(dir,dimension,count,base+extra);
    assert(index.resident_bytes()<=base+extra);
    for(size_t row=0;row<count;++row)index.add(row,row,values.data()+row*dimension,dimension);
    index.finish();FlatIndex::ResidentView view;
    assert(index.resident_view(&view));
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
    assert(view.quantized);
#else
    assert(!view.quantized);
#endif
    const bool have_norms=mvdb::quantized_dot_available() && extra==ann_reservation+norms;
    assert(bool(view.quantized_norms)==have_norms);
    assert(index.resident_bytes()==base+(have_norms?norms:0));
    if(have_norms)for(size_t row=0;row<count;++row) {
      uint64_t expected=0;
      for(size_t d=0;d<dimension;++d) {
        const int64_t code=view.quantized[row*dimension+d];expected+=uint64_t(code*code);
      }
      assert(view.quantized_norms[row]==expected);
    }
    const uint32_t* ordinals=nullptr;size_t matches=0;
    assert(index.equal_ordinals(1,2,[](uint64_t slot){return int64_t(slot%3);},&ordinals,&matches));
    assert(matches==11 && index.resident_bytes()<=base+extra);
  }
  if(mvdb::quantized_dot_available()) {
    FlatIndex index(dir,dimension,count,base+ann_reservation+norms);
    for(size_t row=0;row<count;++row)index.add(row,row,values.data()+row*dimension,dimension);
    // Fail only the uint32 norm array (132 bytes), not the int8 code array
    // (4224 bytes). This optional failure must retain exact quantized scanning.
    fail_array_bytes=norms;index.finish();fail_array_bytes=0;
    FlatIndex::ResidentView view;assert(index.resident_view(&view));
    assert(view.quantized && !view.quantized_norms && index.resident_bytes()==base);
  }
}
void check_invalid(const std::string& dir) {
  throws([&]{FlatIndex bad(dir,0,1,0);});
  throws([&]{FlatIndex bad(dir,std::numeric_limits<size_t>::max(),1,0);});
  throws([&]{FlatIndex bad(dir,1,std::numeric_limits<size_t>::max(),0);});
  FlatIndex incomplete(dir,16,2,0);
  throws([&]{incomplete.finish();});
  throws([&]{incomplete.add(0,0,nullptr,16);});
  float vector[16]={};
  incomplete.add(0,0,vector,16);incomplete.add(1,1,vector,16);incomplete.finish();
  throws([&]{incomplete.add(2,2,vector,16);});
}
void check_partition_budget(const std::string& dir) {
  // Run in a separate process because ANN configuration is cached. Its full
  // training peak must fit, not merely the retained centroid/member arrays.
  assert(::setenv("MVDB_ANN_PROBE_PERCENT","50",1)==0);
  constexpr size_t count=512,dimension=16,clusters=32;
  size_t base=count*(metadata_stride+posting_peak);
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  base+=count*dimension;
#endif
  const size_t peak=clusters*dimension*(sizeof(float)+sizeof(double))+
                    count*sizeof(uint32_t)+clusters*sizeof(uint32_t);
  std::vector<float> vector(dimension,0.5f);
  for(size_t extra:{peak-1,peak}) {
    FlatIndex index(dir,dimension,count,base+extra);
    for(size_t i=0;i<count;++i)index.add(int64_t(i),i,vector.data(),dimension);
    index.finish();
    assert(index.ann_clusters()==(extra==peak?clusters:0));
    assert(index.resident_bytes()<=base+extra);
  }
  // Repeat at a dot-eligible dimension. The original ANN construction boundary
  // must not move to make space for norms; those norms simply remain absent.
  constexpr size_t wide_dimension=128;
  size_t wide_base=count*(metadata_stride+posting_peak);
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  wide_base+=count*wide_dimension;
#endif
  const size_t wide_peak=clusters*wide_dimension*(sizeof(float)+sizeof(double))+
                        count*sizeof(uint32_t)+clusters*sizeof(uint32_t);
  vector.resize(wide_dimension,0.5f);
  for(size_t extra:{wide_peak-1,wide_peak,size_t(2u<<20)+count*sizeof(uint32_t)}) {
    FlatIndex index(dir,wide_dimension,count,wide_base+extra);
    for(size_t i=0;i<count;++i)index.add(int64_t(i),i,vector.data(),wide_dimension);
    index.finish();FlatIndex::ResidentView view;assert(index.resident_view(&view));
    assert(index.ann_clusters()==(extra>=wide_peak?clusters:0));
    assert(bool(view.quantized_norms)==(mvdb::quantized_dot_available() && extra>wide_peak));
    assert(index.resident_bytes()<=wide_base+extra);
  }
}
#ifdef MVDB_TEST_WRAP_PWRITE
void check_failures(const std::string& dir) {
  interrupt_next=true;short_writes=true;
  check_disk(dir,128,1001);
  assert(!interrupt_next);short_writes=false;
  // Flush a residual tail in finish(), fail after successful complete blocks,
  // and verify that no partial build can be queried or reused.
  for(bool zero:{false,true}) {
    FlatIndex index(dir,128,1001,0);
    float vector[128]={};
    for(size_t i=0;i<1001;++i)index.add(int64_t(i),i,vector,128);
    fail_writes=!zero;zero_write=zero;
    throws([&]{index.finish();});
    fail_writes=false;zero_write=false;
    throws([&]{index.finish();});
    throws([&]{index.scan([](int64_t,uint64_t,const float*,size_t){return true;});});
    FlatIndex::ResidentView view;assert(!index.resident_view(&view));
  }
  // An error on a full-block flush during add() also poisons the build.
  FlatIndex index(dir,128,1001,0);float vector[128]={};
  fail_writes=true;
  throws([&]{for(size_t i=0;i<1001;++i)index.add(int64_t(i),i,vector,128);});
  fail_writes=false;
  throws([&]{index.add(0,0,vector,128);});
  throws([&]{index.finish();});
}
#endif
}
// Test-only allocation fault. All successful requests still use the matching
// ordinary array-new/delete implementation, including sanitizer interceptors.
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {
  if(fail_array_bytes && bytes==fail_array_bytes)return nullptr;
  try {return ::operator new[](bytes);} catch(...) {return nullptr;}
}
#ifdef MVDB_TEST_WRAP_PWRITE
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t bytes,off_t offset) {
  ++writes;
  if(interrupt_next) {interrupt_next=false;errno=EINTR;return -1;}
  if(fail_writes) {errno=EIO;return -1;}
  if(zero_write)return 0;
  if(short_writes)bytes=std::min<size_t>(bytes,37);
  return __real_pwrite(fd,data,bytes,offset);
}
#endif
int main(int argc,char** argv) {
  char path[]="/tmp/mvdb-index-budget-XXXXXX";assert(::mkdtemp(path));
  if(argc==2 && std::string(argv[1])=="--ann-budget") {
    check_partition_budget(path);assert(::rmdir(path)==0);
    std::cout<<"PASS optional partition construction peak budget\n";
    return 0;
  }
  check_budget(path);
  check_dot_budget(path);
  check_disk(path,128,1001);
  check_disk(path,65537,3); // one record exceeds the batching buffer target
  check_invalid(path);
#ifdef MVDB_TEST_WRAP_PWRITE
  check_failures(path);
#endif
  assert(::rmdir(path)==0); // temporary index files are unlinked immediately
  std::cout<<"PASS resident admission/accounting and batched disk construction\n";
}

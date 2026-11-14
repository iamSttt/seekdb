// Source-factor safety regression: no timing or performance claims here.
#include "flat_index.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
thread_local size_t allocation_target=0;
thread_local bool allocation_fail=false;
thread_local size_t matching_allocations=0;
void* allocate(size_t bytes) {
  if(allocation_target && bytes==allocation_target) {
    ++matching_allocations;
    if(allocation_fail)throw std::bad_alloc();
  }
  void* pointer=std::malloc(bytes ? bytes : 1);
  if(!pointer)throw std::bad_alloc();
  // Poison all fresh storage in this test. Production has no such memset.
  std::memset(pointer,0xa5,bytes);
  return pointer;
}
struct ReadControl {
  bool active=false,interrupt=false;
  size_t cap=SIZE_MAX,fail_at=SIZE_MAX,calls=0,bytes=0,interrupts=0;
  int failure=0; // 0 => EOF, EIO => error once fail_at bytes were delivered.
  void* first_destination=nullptr;
};
thread_local ReadControl io;
size_t completed_cases=0;
struct Directory {
  std::string path;
  Directory() {char value[]="/tmp/mvdb-scan-noinit-XXXXXX";assert(::mkdtemp(value));path=value;}
  ~Directory() {std::filesystem::remove_all(path);}
};
uint32_t float_bits(size_t row,size_t column) {
  switch((row+column)%23) {
    case 0:return 0;
    case 1:return 0x80000000u;
    case 2:return 1;
    case 3:return 0x80000001u;
    case 4:return 0x7f7fffffu;
    case 5:return 0xff7fffffu;
    default:return 0x3f000000u | (uint32_t(row*131+column*17)&0x007fffffu);
  }
}
int64_t key(size_t row) {
  if(row==0)return INT64_MIN;
  if(row==1)return INT64_MAX;
  return -int64_t(row*17);
}
uint64_t slot(size_t row) {return UINT64_MAX-uint64_t(row)*65537u;}
void check_row(size_t row,int64_t pk,uint64_t where,const float* vector,size_t dim,size_t expected_dim) {
  assert(pk==key(row) && where==slot(row) && dim==expected_dim);
  for(size_t j=0;j<dim;++j) {
    uint32_t bits=0;std::memcpy(&bits,vector+j,sizeof(bits));
    assert(bits==float_bits(row,j));
  }
}
void populate(mvdb::FlatIndex& index,size_t count,size_t dim) {
  std::vector<float> row(dim);
  for(size_t i=0;i<count;++i) {
    for(size_t j=0;j<dim;++j) {const uint32_t b=float_bits(i,j);std::memcpy(row.data()+j,&b,sizeof(b));}
    index.add(key(i),slot(i),row.data(),dim);
  }
  index.finish();
}
size_t scan(const mvdb::FlatIndex& index,size_t count,size_t dim,bool filtered,int selection=0,size_t stop=SIZE_MAX) {
  size_t predicates=0,visitors=0,expected_row=0;
  auto accepted=[&](size_t row) {return selection==0 || (selection==1 && row%3==1);};
  auto visitor=[&](int64_t pk,uint64_t where,const float* v,size_t d) {
    if(filtered)while(expected_row<count && !accepted(expected_row))++expected_row;
    assert(expected_row<count);check_row(expected_row,pk,where,v,d,dim);++expected_row;++visitors;
    return visitors<stop;
  };
  if(filtered)index.scan_filtered([&](size_t row,int64_t pk,uint64_t where) {
    assert(row==predicates++ && pk==key(row) && where==slot(row));return accepted(row);
  },visitor);
  else index.scan(visitor);
  if(stop==SIZE_MAX) {
    size_t expected=0;for(size_t row=0;row<count;++row)if(!filtered || accepted(row))++expected;
    assert(visitors==expected);if(filtered)assert(predicates==count);
  } else assert(visitors==std::min(stop,count));
  return visitors;
}
void boundary_cases(const std::string& path) {
  for(size_t dim:{size_t(1),size_t(128),size_t(768),size_t(262145)}) {
    const size_t stride=(dim+4)*sizeof(float),per=std::max<size_t>(1,(1u<<20)/stride);
    std::vector<size_t> counts={0,1,per-1,per,per+1,2*per+3};
    std::sort(counts.begin(),counts.end());counts.erase(std::unique(counts.begin(),counts.end()),counts.end());
    for(size_t count:counts) {
      mvdb::FlatIndex index(path,dim,count,0);populate(index,count,dim);
      assert(index.disk_backed()==(count!=0));
      const size_t capacity=std::min(count,per)*stride;
      for(bool filtered:{false,true}) {
        io={};io.active=true;allocation_target=capacity;matching_allocations=0;
        scan(index,count,dim,filtered);
        io.active=false;allocation_target=0;
        assert(io.bytes==count*stride && io.calls==(count+per-1)/per);
        assert(matching_allocations==(count ? 1u : 0u));
        if(filtered) {scan(index,count,dim,true,1);scan(index,count,dim,true,2);}
        if(count)scan(index,count,dim,filtered,0,1);
        ++completed_cases;
      }
    }
  }
}
void failure_cases(const std::string& path) {
  constexpr size_t dim=128,stride=(dim+4)*sizeof(float),per=(1u<<20)/stride,count=2*per+7;
  mvdb::FlatIndex index(path,dim,count,0);populate(index,count,dim);
  for(bool filtered:{false,true}) {
    io={};io.active=true;io.interrupt=true;io.cap=4093;
    scan(index,count,dim,filtered);io.active=false;
    assert(io.interrupts==1 && io.bytes==count*stride && io.calls>3);
    ++completed_cases;
    for(size_t fail_at:{size_t(17),per*stride+17})for(int failure:{0,EIO}) {
      size_t predicates=0,visitors=0;
      io={};io.active=true;io.cap=4093;io.fail_at=fail_at;io.failure=failure;
      bool caught=false;
      try {
        auto visitor=[&](int64_t pk,uint64_t where,const float* v,size_t d) {
          check_row(visitors,pk,where,v,d,dim);++visitors;return true;
        };
        if(filtered)index.scan_filtered([&](size_t row,int64_t pk,uint64_t where) {
          assert(row==predicates++ && pk==key(row) && where==slot(row));return true;
        },visitor);
        else index.scan(visitor);
      } catch(const std::runtime_error& e) {caught=true;assert(std::string(e.what())=="flat vector index I/O failed");}
      io.active=false;
      assert(caught && io.bytes==fail_at && visitors==(fail_at<per*stride ? 0 : per));
      if(filtered)assert(predicates==visitors);
      scan(index,count,dim,filtered);++completed_cases;
    }
    // Allocation failure must precede every read and every predicate/visitor.
    io={};io.active=true;allocation_target=per*stride;allocation_fail=true;matching_allocations=0;
    size_t callbacks=0;bool caught=false;
    try {
      auto visitor=[&](int64_t,uint64_t,const float*,size_t) {++callbacks;return true;};
      if(filtered)index.scan_filtered([&](size_t,int64_t,uint64_t) {++callbacks;return true;},visitor);
      else index.scan(visitor);
    } catch(const std::bad_alloc&) {caught=true;}
    allocation_fail=false;allocation_target=0;io.active=false;
    assert(caught && matching_allocations==1 && callbacks==0 && io.calls==0);
    scan(index,count,dim,filtered);++completed_cases;
    // User exceptions may exit partway through a valid block. Next call is clean.
    size_t callbacks2=0;bool callback_caught=false;
    try {
      auto visitor=[&](int64_t,uint64_t,const float*,size_t) {if(++callbacks2==3)throw 27;return true;};
      if(filtered)index.scan_filtered([](size_t,int64_t,uint64_t) {return true;},visitor);
      else index.scan(visitor);
    } catch(int value) {assert(value==27);callback_caught=true;}
    assert(callback_caught && callbacks2==3);scan(index,count,dim,filtered);++completed_cases;
  }
  // A throwing predicate likewise cannot contaminate the next call.
  bool caught=false;
  try {index.scan_filtered([](size_t,int64_t,uint64_t)->bool {throw 31;},[](int64_t,uint64_t,const float*,size_t) {assert(false);return true;});}
  catch(int value) {assert(value==31);caught=true;}
  assert(caught);scan(index,count,dim,true);++completed_cases;
}
void independent_buffers(const std::string& path) {
  constexpr size_t dim=128,count=4013;
  mvdb::FlatIndex index(path,dim,count,0);populate(index,count,dim);
  // Re-entry while the outer call still borrows its row must leave it unchanged.
  for(bool filtered:{false,true}) {
    size_t ordinal=0;bool nested=false;
    auto visitor=[&](int64_t pk,uint64_t where,const float* v,size_t d) {
      check_row(ordinal,pk,where,v,d,dim);
      if(!nested) {
        nested=true;
        index.scan([&](int64_t p,uint64_t s,const float* inner,size_t dd) {
          assert(inner!=v);check_row(0,p,s,inner,dd,dim);return false;
        });
        scan(index,count,dim,true,1);
        check_row(ordinal,pk,where,v,d,dim);
      }
      ++ordinal;return true;
    };
    if(filtered)index.scan_filtered([](size_t,int64_t,uint64_t) {return true;},visitor);
    else index.scan(visitor);
    assert(nested && ordinal==count);++completed_cases;
  }
  // Two simultaneous scans of this const index both hold their first row alive.
  std::atomic<const float*> first[2];first[0]=nullptr;first[1]=nullptr;
  std::atomic<unsigned> arrived{0};
  auto worker=[&](unsigned which) {
    size_t ordinal=0;
    auto visitor=[&](int64_t pk,uint64_t where,const float* v,size_t d) {
      check_row(ordinal,pk,where,v,d,dim);
      if(!ordinal) {
        first[which].store(v);arrived.fetch_add(1);
        while(arrived.load()!=2)std::this_thread::yield();
        assert(first[0].load()!=first[1].load());
        check_row(ordinal,pk,where,v,d,dim);
      }
      ++ordinal;return true;
    };
    if(which)index.scan_filtered([](size_t,int64_t,uint64_t) {return true;},visitor);
    else index.scan(visitor);
    assert(ordinal==count);
  };
  std::thread a(worker,0),b(worker,1);a.join();b.join();++completed_cases;
}
}
void* operator new(size_t n) {return allocate(n);}
void* operator new[](size_t n) {return allocate(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t bytes,off_t offset) {
  if(!io.active)return __real_pread(fd,data,bytes,offset);
  ++io.calls;if(!io.first_destination)io.first_destination=data;
  if(io.interrupt) {io.interrupt=false;++io.interrupts;errno=EINTR;return -1;}
  if(io.bytes>=io.fail_at) {errno=io.failure;return io.failure ? -1 : 0;}
  bytes=std::min(bytes,std::min(io.cap,io.fail_at-io.bytes));
  const ssize_t result=__real_pread(fd,data,bytes,offset);
  if(result>0)io.bytes+=size_t(result);
  return result;
}
int main() {
  Directory directory;
  boundary_cases(directory.path);failure_cases(directory.path);independent_buffers(directory.path);
  std::cout<<"{\"status\":\"passed\",\"cases\":"<<completed_cases
           <<",\"scope\":\"disk scan safety, poisoned allocation, pread faults and independent buffers; no timing\"}\n";
}

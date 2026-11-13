#pragma once
// One test translation unit only. Count requested bytes/calls through every
// C++ new/delete form; these are measured allocations, not token estimates.
// libc/internal malloc calls and allocator metadata are outside this meter.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace parser_meter {
inline std::atomic<size_t> calls{0},bytes{0},live{0},peak{0};
inline thread_local int fail_after=-1;
inline thread_local void (*allocation_hook)()=nullptr;
struct Header {void* raw;size_t size;};
inline void* allocate(size_t n,size_t alignment) {
  if(fail_after==0) {fail_after=-1;throw std::bad_alloc();}
  if(fail_after>0)--fail_after;
  if(auto hook=allocation_hook) {allocation_hook=nullptr;hook();}
  if(n>std::numeric_limits<size_t>::max()-sizeof(Header)-alignment)throw std::bad_alloc();
  void* raw=std::malloc(n+sizeof(Header)+alignment);
  if(!raw)throw std::bad_alloc();
  const uintptr_t start=reinterpret_cast<uintptr_t>(raw)+sizeof(Header);
  const uintptr_t address=(start+alignment-1)&~uintptr_t(alignment-1);
  new(reinterpret_cast<void*>(address-sizeof(Header))) Header{raw,n};
  calls.fetch_add(1,std::memory_order_relaxed);bytes.fetch_add(n,std::memory_order_relaxed);
  const size_t retained=live.fetch_add(n,std::memory_order_relaxed)+n;
  size_t old=peak.load(std::memory_order_relaxed);
  while(old<retained && !peak.compare_exchange_weak(old,retained,std::memory_order_relaxed)) {}
  return reinterpret_cast<void*>(address);
}
inline void release(void* p) noexcept {
  if(!p)return;
  auto* header=static_cast<Header*>(p)-1;
  live.fetch_sub(header->size,std::memory_order_relaxed);std::free(header->raw);
}
}
void* operator new(size_t n) {return parser_meter::allocate(n,alignof(std::max_align_t));}
void* operator new[](size_t n) {return ::operator new(n);}
void operator delete(void* p) noexcept {parser_meter::release(p);}
void operator delete[](void* p) noexcept {parser_meter::release(p);}
void operator delete(void* p,size_t) noexcept {parser_meter::release(p);}
void operator delete[](void* p,size_t) noexcept {parser_meter::release(p);}
void* operator new(size_t n,const std::nothrow_t&) noexcept {try{return ::operator new(n);}catch(...){return nullptr;}}
void* operator new[](size_t n,const std::nothrow_t&) noexcept {try{return ::operator new[](n);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept {parser_meter::release(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {parser_meter::release(p);}
void* operator new(size_t n,std::align_val_t a) {return parser_meter::allocate(n,static_cast<size_t>(a));}
void* operator new[](size_t n,std::align_val_t a) {return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept {parser_meter::release(p);}
void operator delete[](void* p,std::align_val_t) noexcept {parser_meter::release(p);}
void operator delete(void* p,size_t,std::align_val_t) noexcept {parser_meter::release(p);}
void operator delete[](void* p,size_t,std::align_val_t) noexcept {parser_meter::release(p);}
void* operator new(size_t n,std::align_val_t a,const std::nothrow_t&) noexcept {try{return ::operator new(n,a);}catch(...){return nullptr;}}
void* operator new[](size_t n,std::align_val_t a,const std::nothrow_t&) noexcept {try{return ::operator new[](n,a);}catch(...){return nullptr;}}
void operator delete(void* p,std::align_val_t,const std::nothrow_t&) noexcept {parser_meter::release(p);}
void operator delete[](void* p,std::align_val_t,const std::nothrow_t&) noexcept {parser_meter::release(p);}

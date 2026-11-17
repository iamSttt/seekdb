#pragma once
// Batched certified screen for the exact TopK scan.
//
// The shipped per-row loop performs one integer screen, one indirect call and
// one horizontal reduction for every row, so one row's dependent
// multiply-add/fold chain is the critical path and only one row's code bytes
// are ever in flight. This scanner changes neither the certificate nor the
// order of the exact evaluations; it changes how many rows are in flight:
//
//  * eight rows per block, evaluated by a kernel forced inline into one
//    AVX-512 translation unit, with the 128-coordinate form unrolled into four
//    independent chains so the eight rows interleave in the out-of-order
//    window instead of forming one serial chain per row;
//  * one certificate per block, taken from the block's first threshold. A
//    stale k-th distance is >= the live one, so the derived cap is weaker and
//    a row rejected here is still provably outside the current top-k: a block
//    boundary can only cost rejections, never correctness;
//  * a survivor runs the existing float bound and the existing exact kernel
//    against the live threshold, exactly as the per-row path does.
//
// Every decision here is a certified rejection computed by the proof in
// quantized_bound.h, so the winner set, the distances and the tie order are
// those of the unbatched path.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include "distance.h"
#include "quantized_bound.h"

namespace mvdb {
namespace scan_topk {

// One borrowed resident payload and the metadata the scan walks in ordinal
// order: the quantized codes, the packed float copy (preferred) or the
// per-row pointers, and the primary keys.
struct View {
  const int8_t* codes=nullptr;
  const float* packed=nullptr;
  const float* const* rows=nullptr;
  const int64_t* keys=nullptr;
  const int32_t* code_norms=nullptr;
  size_t dimension=0,stride=0,count=0;
};

// The kernels below are AVX-512 only, while MVDB_BATCHED_SCREEN is defined for
// every x86-64 GCC target, so the CPU has to be asked at run time -- the same
// dispatch distance.h and quantized_bound.h already use for their own kernels.
// Both the contiguous and the gathered screen run through scan(), so this one
// gate covers both. MVDB_SCREEN_FORCE_SCALAR=1 pins the per-row fallback, which
// keeps that path measurable on an AVX-512 host.
#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_AVX512_QUANTIZED) && !defined(MVDB_DISABLE_BATCHED_SCREEN) && !defined(__FAST_MATH__)
#define MVDB_BATCHED_SCREEN 1
#endif

inline bool batched_screen_available() {
#if defined(MVDB_BATCHED_SCREEN)
  static const bool ok=[]{
    const char* forced=std::getenv("MVDB_SCREEN_FORCE_SCALAR");
    if(forced && forced[0] && forced[0]!='0')return false;
    return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw");
  }();
  return ok;
#else
  return false;
#endif
}

// Rows per screen block, and the largest K the fixed worst-set holds.
constexpr size_t kBlock=8;
constexpr size_t kMaxK=32;

// One hit: the ranking key the executor compares.
struct Hit { double distance; int64_t pk; };

// The k smallest hits under (distance, primary key) ascending: the total order
// the executor's `better` produces for ORDER BY L2_DISTANCE(...) ASC.
inline bool hit_less(const Hit& a,const Hit& b) {
  return a.distance<b.distance || (a.distance==b.distance && a.pk<b.pk);
}

// Fixed-capacity worst set with one sorted insert. K <= kMaxK, so an insert is
// a binary search plus a short move; slot k-1 holds the live boundary.
class BestK {
 public:
  explicit BestK(size_t k):k_(k<kMaxK?k:kMaxK) {}
  bool full() const {return size_==k_;}
  size_t size() const {return size_;}
  const Hit& operator[](size_t i) const {return items_[i];}
  double worst() const {return items_[k_-1].distance;}
  void offer(double distance,int64_t pk) {
    if(size_==k_ && !hit_less(Hit{distance,pk},items_[k_-1]))return;
    const Hit c{distance,pk};
    const size_t end=size_<k_ ? size_ : k_-1;
    size_t lo=0,hi=end;
    while(lo<hi) {
      const size_t mid=(lo+hi)/2;
      if(hit_less(items_[mid],c))lo=mid+1;else hi=mid;
    }
    for(size_t i=end;i>lo;--i)items_[i]=items_[i-1];
    items_[lo]=c;
    if(size_<k_)++size_;
  }
 private:
  Hit items_[kMaxK];
  size_t size_=0,k_=0;
};

#if defined(MVDB_SIMD_BOUND_X86) && !defined(MVDB_DISABLE_AVX512_QUANTIZED) && \
    !defined(MVDB_DISABLE_BATCHED_SCREEN) && !defined(__FAST_MATH__)
// Guard for the call sites: everything below is compiled once with the AVX-512
// target so the kernels inline, and is never reached on another CPU.
namespace detail {
#define MVDB_SCAN_TARGET __attribute__((target("avx512f,avx512bw,avx2,no-fma")))
// The VNNI kernels are only ever called after vnni_available(), so the extra
// target feature cannot reach a CPU without it.
#define MVDB_SCAN_TARGET_VNNI __attribute__((target("avx512f,avx512bw,avx512vnni,avx2,no-fma")))
// Exact sum of squared code differences. The sum is integer and stays below
// 2^32 for n<=65536 (see quantized_bound.h), so reassociating the additions
// cannot change the value the certificate consumes.
MVDB_SCAN_TARGET __attribute__((always_inline))
inline uint32_t code_distance(const int8_t* a,const int8_t* b,size_t n) {
  __m512i s0=_mm512_setzero_si512(),s1=s0,s2=s0,s3=s0;
  size_t i=0;
  for(;i+128<=n;i+=128) {
    const __m512i x0=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+i)));
    const __m512i x1=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+i+32)));
    const __m512i x2=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+i+64)));
    const __m512i x3=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+i+96)));
    const __m512i y0=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+i)));
    const __m512i y1=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+i+32)));
    const __m512i y2=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+i+64)));
    const __m512i y3=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+i+96)));
    const __m512i d0=_mm512_sub_epi16(x0,y0),d1=_mm512_sub_epi16(x1,y1);
    const __m512i d2=_mm512_sub_epi16(x2,y2),d3=_mm512_sub_epi16(x3,y3);
    s0=_mm512_add_epi32(s0,_mm512_madd_epi16(d0,d0));
    s1=_mm512_add_epi32(s1,_mm512_madd_epi16(d1,d1));
    s2=_mm512_add_epi32(s2,_mm512_madd_epi16(d2,d2));
    s3=_mm512_add_epi32(s3,_mm512_madd_epi16(d3,d3));
  }
  for(;i+32<=n;i+=32) {
    const __m512i x=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+i)));
    const __m512i y=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+i)));
    const __m512i d=_mm512_sub_epi16(x,y);
    s0=_mm512_add_epi32(s0,_mm512_madd_epi16(d,d));
  }
  const __m512i c=_mm512_add_epi32(_mm512_add_epi32(s0,s1),_mm512_add_epi32(s2,s3));
  const __m256i halves=_mm256_add_epi32(_mm512_castsi512_si256(c),_mm512_extracti64x4_epi64(c,1));
  __m128i t=_mm_add_epi32(_mm256_castsi256_si128(halves),_mm256_extracti128_si256(halves,1));
  t=_mm_add_epi32(t,_mm_srli_si128(t,8));
  t=_mm_add_epi32(t,_mm_srli_si128(t,4));
  uint32_t result=static_cast<uint32_t>(_mm_cvtsi128_si32(t));
  for(;i<n;++i){const int d=int(a[i])-int(b[i]);result+=static_cast<uint32_t>(d*d);}
  return result;
}
// The 128-coordinate form, which is the compiled dimension of the resident
// index this path serves: no loop, no tail, four independent chains.
MVDB_SCAN_TARGET __attribute__((always_inline))
inline uint32_t code_distance_128(const int8_t* a,const int8_t* b) {
  const __m512i x0=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)a));
  const __m512i x1=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+32)));
  const __m512i x2=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+64)));
  const __m512i x3=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(a+96)));
  const __m512i y0=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)b));
  const __m512i y1=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+32)));
  const __m512i y2=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+64)));
  const __m512i y3=_mm512_cvtepi8_epi16(_mm256_loadu_si256((const __m256i*)(b+96)));
  const __m512i d0=_mm512_sub_epi16(x0,y0),d1=_mm512_sub_epi16(x1,y1);
  const __m512i d2=_mm512_sub_epi16(x2,y2),d3=_mm512_sub_epi16(x3,y3);
  const __m512i c=_mm512_add_epi32(
      _mm512_add_epi32(_mm512_madd_epi16(d0,d0),_mm512_madd_epi16(d1,d1)),
      _mm512_add_epi32(_mm512_madd_epi16(d2,d2),_mm512_madd_epi16(d3,d3)));
  const __m256i halves=_mm256_add_epi32(_mm512_castsi512_si256(c),_mm512_extracti64x4_epi64(c,1));
  __m128i t=_mm_add_epi32(_mm256_castsi256_si128(halves),_mm256_extracti128_si256(halves,1));
  t=_mm_add_epi32(t,_mm_srli_si128(t,8));
  t=_mm_add_epi32(t,_mm_srli_si128(t,4));
  return static_cast<uint32_t>(_mm_cvtsi128_si32(t));
}
// The same exact integer sum through one VNNI dot product per 64 coordinates:
// sum((x-y)^2) = ||x||^2 + ||y||^2 - 2 x.y, and sum(x.y) is
// sum((x+128) y) - 128 sum(y), so with the row's precomputed ||x||^2 and the
// query constant ||y||^2 + 256 sum(y) the whole screen is two multiply-adds and
// a lane fold -- no widening of 128 codes on every row. Every intermediate is an
// exact 32-bit integer (|terms| <= 255*127, |x.y| <= 128*255*127), so the value
// is bit-identical to code_distance and the certificate sees the same input.
MVDB_SCAN_TARGET_VNNI __attribute__((always_inline))
inline uint32_t code_distance_vnni(const int8_t* a,const int8_t* q,
                                   int32_t squared_norm,int32_t query_term,size_t n) {
  const __m512i flip=_mm512_set1_epi8(static_cast<char>(0x80));
  __m512i acc=_mm512_setzero_si512();
  for(size_t i=0;i+64<=n;i+=64) {
    const __m512i x=_mm512_xor_si512(_mm512_loadu_si512(a+i),flip);
    const __m512i y=_mm512_loadu_si512(q+i);
    acc=_mm512_dpbusd_epi32(acc,x,y);
  }
  const __m256i halves=_mm256_add_epi32(_mm512_castsi512_si256(acc),_mm512_extracti64x4_epi64(acc,1));
  __m128i t=_mm_add_epi32(_mm256_castsi256_si128(halves),_mm256_extracti128_si256(halves,1));
  t=_mm_add_epi32(t,_mm_srli_si128(t,8));
  t=_mm_add_epi32(t,_mm_srli_si128(t,4));
  return static_cast<uint32_t>(squared_norm+query_term-2*_mm_cvtsi128_si32(t));
}
// ||y||^2 + 256*sum(y) for the coded query: the part of the identity that does
// not depend on the row. Bounded by 128*127^2 + 256*128*127 < 2^23.
inline int32_t vnni_query_term(const int8_t* q,size_t n) {
  int32_t sum=0,squared=0;
  for(size_t i=0;i<n;++i) {
    const int32_t value=q[i];
    sum+=value;squared+=value*value;
  }
  return squared+256*sum;
}
inline bool vnni_available() {
  static const bool ok=__builtin_cpu_supports("avx512vnni") &&
                       __builtin_cpu_supports("avx512f") &&
                       __builtin_cpu_supports("avx512bw");
  return ok;
}
// Bit j set means row j survived the certificate, i.e. (double)code <= cap.
// The contiguous form takes a base pointer and a stride so the eight address
// computations stay inside the target function: handing over a pointer array
// makes every load address come out of a just-stored stack slot.
MVDB_SCAN_TARGET
inline unsigned screen_contiguous(const int8_t* base,size_t stride,
                                  const int8_t* q,size_t n,double cap) {
  uint32_t s[kBlock];
  if(n==128) {
#pragma GCC unroll 8
    for(size_t j=0;j<kBlock;++j)s[j]=code_distance_128(base+j*stride,q);
  } else {
#pragma GCC unroll 8
    for(size_t j=0;j<kBlock;++j)s[j]=code_distance(base+j*stride,q,n);
  }
  unsigned mask=0;
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)mask|=(static_cast<double>(s[j])>cap?0u:1u)<<j;
  return mask;
}
MVDB_SCAN_TARGET
inline unsigned screen_gathered(const int8_t* const* row,const int8_t* q,size_t n,double cap) {
  uint32_t s[kBlock];
  if(n==128) {
#pragma GCC unroll 8
    for(size_t j=0;j<kBlock;++j)s[j]=code_distance_128(row[j],q);
  } else {
#pragma GCC unroll 8
    for(size_t j=0;j<kBlock;++j)s[j]=code_distance(row[j],q,n);
  }
  unsigned mask=0;
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)mask|=(static_cast<double>(s[j])>cap?0u:1u)<<j;
  return mask;
}
MVDB_SCAN_TARGET_VNNI
inline unsigned screen_contiguous_norm(const int8_t* base,size_t stride,
                                       const int32_t* norms,const int8_t* q,
                                       int32_t term,size_t n,double cap) {
  uint32_t s[kBlock];
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)
    s[j]=code_distance_vnni(base+j*stride,q,norms[j],term,n);
  unsigned mask=0;
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)mask|=(static_cast<double>(s[j])>cap?0u:1u)<<j;
  return mask;
}
MVDB_SCAN_TARGET_VNNI
inline unsigned screen_gathered_norm(const int8_t* const* row,const int32_t* norms,
                                     const int8_t* q,int32_t term,size_t n,double cap) {
  uint32_t s[kBlock];
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)
    s[j]=code_distance_vnni(row[j],q,norms[j],term,n);
  unsigned mask=0;
#pragma GCC unroll 8
  for(size_t j=0;j<kBlock;++j)mask|=(static_cast<double>(s[j])>cap?0u:1u)<<j;
  return mask;
}
} // namespace detail
#endif

// Walks `n` rows (ordinals when non-null, else 0..count-1), screens eight at a
// time against one certificate per block, and hands every survivor to `visit`,
// which applies the live float bound and the exact kernel. `threshold` points
// at the live k-th distance (infinity until the worst-set is full) and is read,
// never written, here. `visit` returns false to stop the scan.
template <class Visit>
inline void scan(const View& view,const uint32_t* ordinals,size_t n,
                 const int8_t* qcodes,const QuantizedQueryBound& certificate,
                 const double* threshold,Visit&& visit) {
  if(!qcodes || !view.codes || !threshold || !view.dimension) {
    for(size_t i=0;i<n;++i)
      if(!visit(i))return;
    return;
  }
#ifdef MVDB_BATCHED_SCREEN
  // No AVX-512: the per-row path below is the shipped scan, unchanged.
  if(!batched_screen_available()) {
    for(size_t i=0;i<n;++i)
      if(!visit(i))return;
    return;
  }
  // The VNNI form needs the per-row norms, a 64-coordinate multiple and the
  // instruction itself; everything else keeps the widening kernel.
  const bool use_norms=view.code_norms && view.dimension%64==0 && detail::vnni_available();
  const int32_t term=use_norms ? detail::vnni_query_term(qcodes,view.dimension) : 0;
  size_t i=0;
  for(;i+kBlock<=n;) {
    double cap=0;
    if(!certificate.certified_cap(*threshold,&cap)) {
      if(!visit(i))return;
      ++i;
      continue;
    }
    unsigned mask=0;
    if(ordinals) {
      const int8_t* row[kBlock];
      for(size_t j=0;j<kBlock;++j)
        row[j]=view.codes+static_cast<size_t>(ordinals[i+j])*view.dimension;
      if(use_norms)
        mask=detail::screen_gathered_norm(row,view.code_norms+ordinals[i],qcodes,term,
                                          view.dimension,cap);
      else
        mask=detail::screen_gathered(row,qcodes,view.dimension,cap);
    } else if(use_norms) {
      mask=detail::screen_contiguous_norm(view.codes+i*view.dimension,view.dimension,
                                          view.code_norms+i,qcodes,term,view.dimension,cap);
    } else {
      mask=detail::screen_contiguous(view.codes+i*view.dimension,view.dimension,
                                     qcodes,view.dimension,cap);
    }
    while(mask) {
      const unsigned j=static_cast<unsigned>(__builtin_ctz(mask));
      mask&=mask-1;
      if(!visit(i+j))return;
    }
    i+=kBlock;
  }
  for(;i<n;++i)
    if(!visit(i))return;
#else
  for(size_t i=0;i<n;++i)
    if(!visit(i))return;
#endif
}
} // namespace scan_topk
} // namespace mvdb

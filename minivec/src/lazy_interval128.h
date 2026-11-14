#pragma once
// Query-local exact Top-K with certified lazy distance materialization.
#include "distance.h"
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mvdb {
namespace lazy128 {

constexpr size_t kDimension=128;
constexpr size_t kMaximumK=1024;

// Query lifetime precondition: FP controls do not change after construction.
// The database supplies finite float32 coordinates and immutable resident rows.
inline bool supported() {
#if defined(__x86_64__) && defined(MVDB_SIMD_BOUND_X86) && FLT_EVAL_METHOD==0 && !defined(__FAST_MATH__) && !defined(MVDB_DISABLE_SIMD_BOUND)
  const unsigned csr=_mm_getcsr();
  return std::fegetround()==FE_TONEAREST &&
      (csr & ((1u<<6)|(1u<<15)|(3u<<13)))==0 &&
      (csr & 0x1f80u)==0x1f80u &&
      distance_detail::choose_bound_sum()!=nullptr;
#else
  return false;
#endif
}

struct Interval { double lower=0,upper=std::numeric_limits<double>::infinity(); };
enum class SampleKind { Reject, Interval, ExactFallback };

class EnvelopeContext128 {
 public:
  EnvelopeContext128() {
#if defined(MVDB_SIMD_BOUND_X86) && !defined(__FAST_MATH__) && !defined(MVDB_DISABLE_SIMD_BOUND)
    if(supported())kernel_=distance_detail::choose_bound_sum();
#endif
  }
#if defined(MVDB_LAZY128_TEST_KERNEL) && defined(MVDB_SIMD_BOUND_X86)
  // Test caller chooses only an ISA actually supported by this CPU.
  explicit EnvelopeContext128(distance_detail::BoundSum test_kernel) {
    if(supported())kernel_=test_kernel;
  }
#endif
  bool enabled() const {
#if defined(MVDB_SIMD_BOUND_X86)
    return kernel_!=nullptr;
#else
    return false;
#endif
  }

  // Exactly one float sum. The old cached cutoff is tested before paying for
  // the survivor's interval. ExactFallback means no certified interval/reject.
  SampleKind sample(const float* a,const float* b,double threshold,Interval* out) const {
    if(!out || !a || !b || !enabled())return SampleKind::ExactFallback;
#if defined(MVDB_SIMD_BOUND_X86)
    const double sum=kernel_(a,b,kDimension);
    if(!std::isfinite(sum) || sum<0)return SampleKind::ExactFallback;
    if(std::isfinite(threshold) && threshold>=0) {
      if(!cutoff_valid_ || threshold!=cached_threshold_)update_cutoff(threshold);
      if(sum>cutoff_)return SampleKind::Reject;
    }
    // Both endpoints enclose RNE(exact real squared distance), not merely D.
    // See docs/LAZY128_CERTIFICATE.md: S*(1-r)-a <= RNE(D) <= S*(1+r)+a.
    const double product=up(sum*relative_);
    const double error=up(product+absolute_);
    const double lower=sum-error;
    out->lower=lower>0 ? distance_detail::positive_neighbors(lower).lower : 0;
    out->upper=up(sum+error);
    if(!std::isfinite(out->upper) || !(out->lower<=out->upper))
      return SampleKind::ExactFallback;
    return SampleKind::Interval;
#else
    (void)threshold;
    return SampleKind::ExactFallback;
#endif
  }

 private:
  static double up(double value) {
    return std::isfinite(value) ? distance_detail::positive_neighbors(value).upper : value;
  }
  void update_cutoff(double threshold) const {
    // Token-for-token arithmetic matches DistanceBoundContext::update_cutoff.
    double numerator=threshold+absolute_;
    cutoff_=std::numeric_limits<double>::infinity();
    if(std::isfinite(numerator)) {
      numerator=distance_detail::positive_neighbors(numerator).upper;
      const double quotient=numerator/denominator_;
      if(std::isfinite(quotient))
        cutoff_=distance_detail::positive_neighbors(quotient).upper;
    }
    cached_threshold_=threshold;
    cutoff_valid_=true;
  }
  const double relative_=(static_cast<double>(kDimension)+12)*0x1p-22;
  const double absolute_=static_cast<double>(kDimension)*0x1p-148;
  const double denominator_=distance_detail::positive_neighbors(1-relative_).lower;
  mutable double cutoff_=std::numeric_limits<double>::infinity(),cached_threshold_=0;
  mutable bool cutoff_valid_=false;
#if defined(MVDB_SIMD_BOUND_X86)
  distance_detail::BoundSum kernel_=nullptr;
#endif
};

struct Statistics {
  uint64_t offered=0,float_sums=0,float_rejects=0,exact_fallbacks=0;
  uint64_t interval_comparisons=0,overlap_comparisons=0,exact_calls=0;
  uint64_t accepted=0,final_exact_calls=0;
};

// Heap elements are slot indices. Only the external, stable slot cache is
// memoized by comparisons; the compared indices are never modified.
// Instrument=false compiles out counters and certificate debug assertions.
template<bool Instrument=false>
class TopK128 {
 public:
  TopK128(const float* query,size_t k):query_(query),k_(k) {
    if(!query || !k || k>kMaximumK)throw std::invalid_argument("lazy128 invalid query/K");
    slots_.resize(k+1);
    heap_.reserve(k);
  }
  TopK128(const TopK128&)=delete;
  TopK128& operator=(const TopK128&)=delete;

  bool enabled() const {return envelope_.enabled();}
  size_t size() const {return heap_.size();}
  double local_upper() const {return local_upper_;}
  double pruning_upper() const {return effective_upper_;}
  const Statistics& statistics() const {return statistics_;}

  // Caller certifies that a finite peer value has K distinct real witnesses.
  // Never publish peer_upper_ as this half's own K-row witness.
  void set_peer_upper(double value) {
    if(std::isfinite(value) && value>=0 && value<peer_upper_) {
      peer_upper_=value;
      if(value<effective_upper_)effective_upper_=value;
    }
  }

  bool offer(int64_t pk,const float* row) {
    if(!row)throw std::invalid_argument("lazy128 null row");
    if constexpr(Instrument) {
      ++statistics_.offered;
      statistics_.float_sums+=envelope_.enabled();
    }
    Interval interval;
    const auto kind=envelope_.sample(row,query_,effective_upper_,&interval);
    if(kind==SampleKind::Reject) {
      if constexpr(Instrument)++statistics_.float_rejects;
      return false;
    }
    const size_t slot=free_slot_;
    slots_[slot]=Slot{pk,row,interval.lower,interval.upper,0,false};
    if(kind==SampleKind::ExactFallback) {
      if constexpr(Instrument)++statistics_.exact_fallbacks;
      materialize(slot);
    }
    auto compare=[this](size_t a,size_t b){return better(a,b);};
    if(heap_.size()<k_) {
      heap_.push_back(slot);
      std::push_heap(heap_.begin(),heap_.end(),compare);
      free_slot_=heap_.size();
    } else {
      if(!better(slot,heap_.front())) {
        // The comparison may have tightened the root's cached interval.
        refresh_local_upper();
        return false;
      }
      std::pop_heap(heap_.begin(),heap_.end(),compare);
      const size_t evicted=heap_.back();
      heap_.back()=slot;
      std::push_heap(heap_.begin(),heap_.end(),compare);
      // Reuse only after the algorithm has finished all comparisons/moves.
      free_slot_=evicted;
    }
    if constexpr(Instrument)++statistics_.accepted;
    refresh_local_upper();
    return true;
  }

  // At most K local survivors. Call only after scanning this half. The caller
  // converts each to its unchanged Candidate before the usual global merge.
  template<class Visitor>
  void for_each_materialized(Visitor&& visitor) {
    [[maybe_unused]] const uint64_t before=statistics_.exact_calls;
    for(size_t id:heap_) {
      const double value=materialize(id);
      visitor(slots_[id].pk,value);
    }
    if constexpr(Instrument)statistics_.final_exact_calls+=statistics_.exact_calls-before;
    refresh_local_upper();
  }

 private:
  struct Slot {
    int64_t pk;
    const float* row;
    double lower,upper,exact;
    bool ready;
  };
  double materialize(size_t id) {
    auto& slot=slots_[id];
    if(!slot.ready) {
      // Integer ExactSum avoids depending on the x87 precision-control word.
      // This has a different cost from the old certified long-double shortcut.
      const double value=distance_exact(slot.row,query_,kDimension);
      if(!std::isfinite(value) || value<0)
        throw std::runtime_error("lazy128 requires finite canonical distances");
      if constexpr(Instrument) {
        if(!(slot.lower<=value && value<=slot.upper))
          throw std::runtime_error("lazy128 interval missed canonical distance");
        ++statistics_.exact_calls;
      }
      slot.exact=value;
      slot.lower=slot.upper=value;
      slot.ready=true;
    }
    return slot.exact;
  }
  bool better(size_t a,size_t b) {
    if(a==b)return false;
    if(slots_[a].upper<slots_[b].lower) {
      if constexpr(Instrument)++statistics_.interval_comparisons;
      return true;
    }
    if(slots_[b].upper<slots_[a].lower) {
      if constexpr(Instrument)++statistics_.interval_comparisons;
      return false;
    }
    if constexpr(Instrument)++statistics_.overlap_comparisons;
    const double da=materialize(a),db=materialize(b);
    return da<db || (da==db && slots_[a].pk<slots_[b].pk);
  }
  void refresh_local_upper() {
    if(heap_.size()!=k_)return;
    const double value=slots_[heap_.front()].upper;
    // Root is maximal in the canonical total order, so this root U witnesses
    // K rows even if the other slots' intervals are wider. A new root's U need
    // not decrease: retain the minimum of all historically valid witnesses.
    if(std::isfinite(value) && value>=0 && value<local_upper_) {
      local_upper_=value;
      if(value<effective_upper_)effective_upper_=value;
    }
  }
  const float* query_;
  const size_t k_;
  EnvelopeContext128 envelope_;
  std::vector<Slot> slots_;
  std::vector<size_t> heap_;
  size_t free_slot_=0;
  double local_upper_=std::numeric_limits<double>::infinity();
  double peer_upper_=std::numeric_limits<double>::infinity();
  double effective_upper_=std::numeric_limits<double>::infinity();
  Statistics statistics_;
};

} // namespace lazy128
} // namespace mvdb

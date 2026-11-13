#pragma once
#include <array>
#include "quantized_scale.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mvdb {
// An exact flat index, rebuilt from committed/transaction-visible table rows.
// Small indexes hold only per-row metadata (PK, slot) plus a non-owning pointer
// to each row's float32 payload; large ones use a rebuildable fixed-record file
// and a bounded sequential read buffer.
//
// Lifetime: a resident index never copies vector payloads. It borrows the row
// storage owned by the table, so every borrowed payload must outlive the index.
// The table enforces that by invalidating on mutation, or by retaining original
// UPDATE payloads in the opt-in delta until invalidation. Resident indexes are
// only built for rows held in memory -- a table whose
// payloads live in the rebuildable payload file gets a disk-backed index, which
// copies each record into its own file exactly as before.
//
// A resident index also owns an optional coarse partition (a k-means tree of
// one level) whose only consumer is the approximate search mode. The partition
// never influences exact results: it can only narrow the candidate set of a
// query the caller has explicitly marked approximate.
class FlatIndex {
 public:
  using Visitor=std::function<bool(int64_t,uint64_t,const float*,size_t)>;
  struct ResidentView {
    // count borrowed payload pointers, each dimension floats, owned by the table.
    const float* const* rows=nullptr;
    const int64_t* keys=nullptr;
    const uint64_t* slots=nullptr;
    size_t dimension=0,count=0;
    const int8_t* quantized=nullptr;
    double quantization_step=0;
    QuantizedScaleView quantization_scales;
  };
  FlatIndex(const std::string& dir,size_t dimension,size_t count,size_t memory_budget);
  ~FlatIndex();
  FlatIndex(const FlatIndex&)=delete;
  FlatIndex& operator=(const FlatIndex&)=delete;
  // `values` is borrowed, not copied, when this index is resident: it must stay
  // alive and unmoved until the index is destroyed. A disk-backed index copies it.
  void add(int64_t pk,uint64_t slot,const float* values,size_t dimension
#ifdef MVDB_ENABLE_SPILL_FILTER
      ,const int64_t* integer_values=nullptr,size_t integer_count=0
#endif
      );
  void finish();
  void scan(const Visitor& visitor) const;
#ifdef MVDB_ENABLE_SPILL_FILTER
  // Configure before add(), only for a disk index. Takes up to two columns in
  // the supplied order and returns the admitted prefix length. Arrays and
  // compression scratch share this budget, capped at 2 MiB. No PK copy needed.
  size_t configure_spill_integers(const size_t* columns,size_t count,size_t budget);
  bool spill_integer_value(size_t ordinal,size_t column,int64_t* value) const;
  bool has_spill_integer(size_t column) const;
  // Predicate sees the physical ordinal and existing key/slot metadata before
  // any row payload is materialised. It must not mutate this physical index.
  using MetadataPredicate=std::function<bool(size_t,int64_t,uint64_t)>;
  void scan_filtered(const MetadataPredicate& predicate,const Visitor& visitor) const;
#endif
  // Read-only, non-owning view; available only for a finished resident index.
  // Storage remains owned by this index and must not outlive it.
  bool resident_view(ResidentView* view) const;
  // Bounded lazy BIGINT postings. Low-cardinality columns share the original
  // single-column construction allowance; no additional budget is reserved.
  bool scan_integer_equal(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,const Visitor& visitor);
#if defined(MVDB_MULTI_POSTING_REUSE_ONLY) && !defined(MVDB_DISABLE_MULTI_POSTING)
  // Only successful, nonempty compound scans admit later compound reuse.
  // Later SQL projection/encoding failures are outside this scan's lifetime.
  // This belongs to the physical index: invalidation resets the data version.
  bool compound_filter_seen() const {return compound_filter_seen_;}
  void note_compound_filter() {compound_filter_seen_=true;}
  // First-query single-column candidate algorithm. It uses a dense posting in
  // the existing cache slot/budget; no second legacy allocation set is kept.
  bool scan_first_integer_equal(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,const Visitor& visitor);
#endif
  // Same posting list, returned as row ordinals into a ResidentView instead of
  // through a visitor: the caller walks the borrowed payload pointers itself
  // and therefore never materialises a row just to test an equality term.
  // The returned span is valid until the next posting query or index build.
  bool equal_ordinals(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,
      const uint32_t** ordinals,size_t* count);
  struct IntegerRange {
    int64_t lower=0,upper=0;
    bool has_lower=false,has_upper=false;
    bool lower_inclusive=true,upper_inclusive=true;
    bool contains(int64_t value) const {
      return (!has_lower || value>lower || (lower_inclusive && value==lower)) &&
             (!has_upper || value<upper || (upper_inclusive && value==upper));
    }
  };
  // Ranges never adjust an endpoint by +/-1, including INT64_MIN/MAX.
  // Any posting lookup may evict a previous span. Consume it before the next
  // lookup; cached_integer_count only inspects the cache and cannot evict.
  bool integer_ordinals(size_t column,const IntegerRange& range,
      const std::function<int64_t(uint64_t)>& read_value,
      const uint32_t** ordinals,size_t* count);
  bool cached_integer_count(size_t column,const IntegerRange& range,size_t* count) const;
  // Coarse partition used by approximate search. `probes` is the number of
  // nearest partitions to scan; 0 asks for the index's own adaptive width.
  // Returns false when this index has no usable partition (small, spilled or
  // built without one) and the caller must fall back to the exact scan.
  bool ann_probe(const float* query,size_t probes,
      std::vector<uint32_t>* buffered,const uint32_t** ordinals,size_t* count) const;
#ifdef MVDB_ENABLE_INDEX_DELTA
  // Use the same cached policy as partition probing: default 100% or ANN_OFF.
  static bool ann_uses_exact_scan();
#endif
  size_t ann_clusters() const {return ann_offsets_.empty() ? 0 : ann_offsets_.size()-1;}
  size_t resident_bytes() const;
  bool disk_backed() const {return fd_>=0;}
  size_t size() const {return count_;}
 private:
  const float* row_at(size_t i) const {return rows_[i];}
  void build_partition();
  void flush_build_block();
#ifdef MVDB_ENABLE_SPILL_FILTER
  void compress_spill_integers();
  struct SpillInteger {
    size_t column=0,width=8,bytes=0;
    int64_t minimum=0,maximum=0;
    std::unique_ptr<uint8_t[]> values;
  };
  std::array<SpillInteger,2> spill_integers_;
  size_t spill_integer_count_=0,spill_integer_budget_=0;
#endif
  size_t dimension_=0,expected_=0,count_=0;
  int fd_=-1;
  bool ready_=false;
  bool build_failed_=false;
  // Disk construction scratch, bounded by 256 KiB or one oversized record.
  // Released before the index becomes ready; excluded from resident retention.
  std::vector<float> build_block_;
  size_t build_used_=0;
  uint64_t build_offset_=0;
  // Borrowed row payload pointers (resident path); never owns the floats.
  std::vector<const float*> rows_;
  std::vector<int64_t> keys_;
  std::vector<uint64_t> slots_;
  std::unique_ptr<int8_t[]> quantized_;
  size_t quantized_budget_=0;
  double quantization_step_=0;
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
  std::unique_ptr<uint8_t[]> quantization_block_ids_;
  std::array<double,QuantizedScaleView::max_steps> quantization_steps_{};
  size_t quantization_step_count_=0,quantization_scale_budget_=0;
  size_t quantization_query_cache_bytes_=0;
#endif
  size_t posting_budget_=0;
#ifdef MVDB_DISABLE_MULTI_POSTING
  size_t posting_column_=0;
  bool postings_ready_=false;
  std::vector<int64_t> posting_values_;
  std::vector<uint32_t> posting_ordinals_;
#else
  struct IntegerPosting {
    size_t column=0,value_count=0,bytes=0;
    uint64_t last_used=0;
    bool ready=false;
    std::unique_ptr<int64_t[]> values;
    std::unique_ptr<uint32_t[]> ordinals;
    // Null for dense sorted values; otherwise starts of distinct-value runs.
    std::unique_ptr<uint32_t[]> starts;
  };
  std::array<IntegerPosting,2> postings_;
  uint64_t posting_clock_=0;
#ifdef MVDB_MULTI_POSTING_REUSE_ONLY
  bool compound_filter_seen_=false;
#endif
  IntegerPosting* integer_posting(size_t column,
      const std::function<int64_t(uint64_t)>& read_value);
  std::pair<size_t,size_t> posting_span(const IntegerPosting& posting,
      const IntegerRange& range) const;
#endif
  // One-level coarse partition: centroid table, member ordinals grouped by
  // partition, and the partition boundaries into that array.
  size_t ann_budget_=0;
  std::vector<float> ann_centroids_;
  std::vector<uint32_t> ann_members_;
  std::vector<uint32_t> ann_offsets_;
};
}

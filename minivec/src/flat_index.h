#pragma once
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
// The table enforces that by rebuilding (resetting) the index on any mutation,
// and by only building a resident index for rows held in memory -- a table whose
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
    // Maximum encoded-row residual norm, in code units; -1 means unknown.
    double quantization_error_radius=-1;
  };
  FlatIndex(const std::string& dir,size_t dimension,size_t count,size_t memory_budget);
  ~FlatIndex();
  FlatIndex(const FlatIndex&)=delete;
  FlatIndex& operator=(const FlatIndex&)=delete;
  // `values` is borrowed, not copied, when this index is resident: it must stay
  // alive and unmoved until the index is destroyed. A disk-backed index copies it.
  void add(int64_t pk,uint64_t slot,const float* values,size_t dimension);
  void finish();
  void scan(const Visitor& visitor) const;
  // Read-only, non-owning view; available only for a finished resident index.
  // Storage remains owned by this index and must not outlive it.
  bool resident_view(ResidentView* view) const;
  // One bounded, lazy BIGINT equality posting list. Returns false when this
  // index cannot cache it; callers then retain the sequential scan path.
  bool scan_integer_equal(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,const Visitor& visitor);
  // Same posting list, returned as row ordinals into a ResidentView instead of
  // through a visitor: the caller walks the borrowed payload pointers itself
  // and therefore never materialises a row just to test an equality term.
  // The returned span is valid until the next posting query or index build.
  bool equal_ordinals(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,
      const uint32_t** ordinals,size_t* count);
  // Coarse partition used by approximate search. `probes` is the number of
  // nearest partitions to scan; 0 asks for the index's own adaptive width.
  // Returns false when this index has no usable partition (small, spilled or
  // built without one) and the caller must fall back to the exact scan.
  bool ann_probe(const float* query,size_t probes,
      std::vector<uint32_t>* buffered,const uint32_t** ordinals,size_t* count) const;
  size_t ann_clusters() const {return ann_offsets_.empty() ? 0 : ann_offsets_.size()-1;}
  size_t resident_bytes() const;
  bool disk_backed() const {return fd_>=0;}
  size_t size() const {return count_;}
 private:
  const float* row_at(size_t i) const {return rows_[i];}
  void build_partition();
  void flush_build_block();
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
  double quantization_error_radius_=-1;
  size_t posting_budget_=0,posting_column_=0;
  bool postings_ready_=false;
  std::vector<int64_t> posting_values_;
  std::vector<uint32_t> posting_ordinals_;
  // One-level coarse partition: centroid table, member ordinals grouped by
  // partition, and the partition boundaries into that array.
  size_t ann_budget_=0;
  std::vector<float> ann_centroids_;
  std::vector<uint32_t> ann_members_;
  std::vector<uint32_t> ann_offsets_;
};
}

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
// Small indexes hold contiguous float32 vectors and PK/slot arrays. Large ones
// use a rebuildable fixed-record file and a bounded sequential read buffer.
//
// A resident index also owns an optional coarse partition (a k-means tree of
// one level) whose only consumer is the approximate search mode. The partition
// never influences exact results: it can only narrow the candidate set of a
// query the caller has explicitly marked approximate.
class FlatIndex {
 public:
  using Visitor=std::function<bool(int64_t,uint64_t,const float*,size_t)>;
  struct ResidentView {
    const float* vectors=nullptr;
    const int64_t* keys=nullptr;
    const uint64_t* slots=nullptr;
    size_t dimension=0,count=0;
    const int8_t* quantized=nullptr;
    double quantization_step=0;
  };
  FlatIndex(const std::string& dir,size_t dimension,size_t count,size_t memory_budget);
  ~FlatIndex();
  FlatIndex(const FlatIndex&)=delete;
  FlatIndex& operator=(const FlatIndex&)=delete;
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
  // through a visitor: the caller walks the contiguous float32 storage itself
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
  void build_partition();
  size_t dimension_=0,expected_=0,count_=0;
  int fd_=-1;
  bool ready_=false;
  std::vector<float> vectors_;
  std::vector<int64_t> keys_;
  std::vector<uint64_t> slots_;
  std::unique_ptr<int8_t[]> quantized_;
  size_t quantized_budget_=0;
  double quantization_step_=0;
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

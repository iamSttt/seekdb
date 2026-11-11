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
class FlatIndex {
 public:
  using Visitor=std::function<bool(int64_t,uint64_t,const float*,size_t)>;
  struct ResidentView {
    const float* vectors=nullptr;
    const int64_t* keys=nullptr;
    size_t dimension=0,count=0;
    const int8_t* quantized=nullptr;
    double quantization_step=0;
  };
  struct ResidentSelection {
    ResidentView index;
    const uint64_t* slots=nullptr;
    const std::pair<int64_t,size_t>* matches=nullptr;
    size_t count=0;
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
  // Borrowed equality range, sorted by physical index position within value.
  // A new posting-column cache, index mutation or destruction invalidates it.
  bool resident_integer_equal(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,ResidentSelection* selection);
  // One bounded, lazy BIGINT equality posting list. Returns false when this
  // index cannot cache it; callers then retain the sequential scan path.
  bool scan_integer_equal(size_t column,int64_t value,
      const std::function<int64_t(uint64_t)>& read_value,const Visitor& visitor);
  size_t resident_bytes() const;
  bool disk_backed() const {return fd_>=0;}
  size_t size() const {return count_;}
 private:
  bool prepare_integer_postings(size_t column,
      const std::function<int64_t(uint64_t)>& read_value);
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
  std::vector<std::pair<int64_t,size_t>> postings_;
};
}

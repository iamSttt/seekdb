#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mvdb {
// An exact flat index, rebuilt from committed/transaction-visible table rows.
// Small indexes hold contiguous float32 vectors and PK/slot arrays. Large ones
// use a rebuildable fixed-record file and a bounded sequential read buffer.
class FlatIndex {
 public:
  using Visitor=std::function<bool(int64_t,uint64_t,const float*,size_t)>;
  FlatIndex(const std::string& dir,size_t dimension,size_t count,size_t memory_budget);
  ~FlatIndex();
  FlatIndex(const FlatIndex&)=delete;
  FlatIndex& operator=(const FlatIndex&)=delete;
  void add(int64_t pk,uint64_t slot,const float* values,size_t dimension);
  void finish();
  void scan(const Visitor& visitor) const;
  size_t resident_bytes() const;
  bool disk_backed() const {return fd_>=0;}
  size_t size() const {return count_;}
 private:
  size_t dimension_=0,expected_=0,count_=0;
  int fd_=-1;
  bool ready_=false;
  std::vector<float> vectors_;
  std::vector<int64_t> keys_;
  std::vector<uint64_t> slots_;
};
}

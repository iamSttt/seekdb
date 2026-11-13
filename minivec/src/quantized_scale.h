#pragma once
#include <cstddef>
#include <cstdint>

namespace mvdb {
// Non-owning metadata for codes in index ordinal order. An absent view means
// the original global scale. Never interpret local codes with that old scale.
struct QuantizedScaleView {
  static constexpr size_t max_steps=8;
  static constexpr size_t block_rows=256;
  static constexpr size_t query_cache_limit=32u<<10;
  const uint8_t* block_ids=nullptr;
  const double* steps=nullptr;
  size_t block_count=0,row_count=0,step_count=0;
  size_t query_cache_bytes=0; // reserved separately for each of two workers
  bool present() const {return block_ids!=nullptr;}
};
}

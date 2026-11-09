// vector.h — vector kernels, exact top-k, and an IVF-Flat index.
//
// Design notes (see vector.cpp for the measurements):
//   * l2_distance returns SQUARED L2 (no sqrt) accumulated in float32 with four
//     independent lane accumulators. Deterministic: identical inputs produce a
//     bit-identical result, which is what makes the (dist asc, pk asc) tie-break
//     below exact for duplicate vectors.
//   * exact_topk is O(n log k) with a bounded max-heap written directly into the
//     caller's output vector (one reserve, zero per-row allocation).
//   * IvfIndex is IVF-Flat with k-means++ seeding, replication (a vector may
//     live in several inverted lists), and an optional *sound* radius bound that
//     turns probing into an exact accelerator (see use_radius_prune_).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <unordered_map>
#include <vector>

#include "value.h"

namespace mvdb {

// Squared Euclidean distance. Returns 0 when a/b is null or dim == 0.
float l2_distance(const float* a, const float* b, size_t dim);

struct Hit {
  int64_t pk;
  float dist;  // squared L2
};

// Exact top-k over `rows`, skipping dead[i] != 0.
// vec_col: column holding the VECTOR value; pk_col: BIGINT pk column.
// pk_col < 0 (or a non-BIGINT value there) falls back to Row::rid.
// Rows whose vector type/size does not match `dim` are ignored.
// Result is sorted by (dist asc, pk asc) and holds at most min(k, live) hits.
void exact_topk(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
                int vec_col, int pk_col, const float* q, size_t dim, size_t k,
                std::vector<Hit>* out);

class IvfIndex {
 public:
  IvfIndex() = default;

  // Builds from the current table state. `dim` == 0 means "infer from the first
  // live VECTOR value". `nlist` is clamped to the number of live rows.
  void build(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
             int vec_col, int pk_col, size_t dim, size_t nlist = 64);

  // insert() has replace semantics for an already-present pk.
  // Requires a prior build() (which fixes the dimension); if no list exists yet
  // a single-list index is bootstrapped and re-clustered as it grows.
  void insert(int64_t pk, const float* v);
  void erase(int64_t pk);

  void search(const float* q, size_t k, std::vector<Hit>* out) const;
  void search_filtered(const float* q, size_t k,
                       const std::function<bool(int64_t pk)>& pred,
                       std::vector<Hit>* out) const;

  bool empty() const { return live_ == 0; }

  // ---- tuning knobs (public so the executor can set them per query) -------
  // Lists probed per query before the (optional) radius bound takes over.
  size_t nprobe_ = 8;
  // Number of nearest inverted lists a single vector is filed under (1..8).
  // Replication buys recall at fixed nprobe for 4 bytes per duplicate entry.
  size_t replication_ = 2;
  // When true, probing continues past nprobe_ until the triangle-inequality
  // bound proves that no unprobed list can beat the current k-th hit. Results
  // are then bit-identical to exact_topk; cost adapts to how clustered the data
  // is. When false, exactly nprobe_ lists are scanned (pure approximate).
  bool use_radius_prune_ = true;
  // k-means trains on at most this many sampled vectors.
  size_t sample_cap_ = 8192;
  size_t kmeans_iters_ = 10;
  // search_filtered keeps probing while fewer than k*this predicate-passing
  // candidates were seen (bounded by nlist).
  size_t filtered_oversample_ = 8;

  // ---- introspection / persistence ---------------------------------------
  size_t size() const { return live_; }
  size_t dead_count() const { return dead_slots_; }
  size_t nlist() const { return nlist_; }
  size_t dim() const { return dim_; }
  size_t memory_bytes() const;
  // Little-endian, host float layout; versioned magic header.
  bool write_to(FILE* f) const;
  bool read_from(FILE* f);
  void clear();

 private:
  static constexpr size_t kMaxReplication = 8;

  void build_core_(std::vector<int64_t>* pks, std::vector<float>* vecs, size_t nl);
  void rebuild_from_storage_(size_t nl);
  void build_lists_();
  void kmeans_(const std::vector<uint32_t>& sample_slots);
  size_t nearest_lists_(const float* v, size_t r, uint32_t* ids, float* ds) const;
  void maybe_grow_();
  void ensure_query_buffers_() const;
  void search_impl_(const float* q, size_t k,
                    const std::function<bool(int64_t)>* pred,
                    std::vector<Hit>* out) const;

  size_t dim_ = 0;    // vector dimension
  size_t nlist_ = 0;  // number of inverted lists / centroids
  size_t live_ = 0;   // live slots
  size_t dead_slots_ = 0;
  size_t target_nlist_ = 64;  // nlist requested by build(); used for auto-grow

  std::vector<float> data_;         // live vectors, slot-major: slot*dim_
  std::vector<int64_t> slot_pk_;    // slot -> pk
  std::vector<uint8_t> slot_dead_;  // slot -> tombstone
  std::vector<std::vector<uint32_t>> lists_;  // inverted lists: slot ids
  std::vector<float> cents_;        // nlist_ * dim_
  std::vector<float> radius_;       // per list: max ||x - centroid|| (Euclidean)
  float max_rad_ = 0.f;             // max over radius_
  std::unordered_map<int64_t, uint32_t> pk2slot_;

  // query scratch (mutable: search() is const)
  mutable std::vector<float> cdist_;
  mutable std::vector<uint32_t> order_;
  mutable std::vector<uint32_t> stamp_;
  mutable uint32_t gen_ = 0;
};

}  // namespace mvdb

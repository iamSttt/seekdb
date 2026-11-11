// vector.h — SIMD distance kernels, streaming top-k, and an IVF-Flat index.
//
// Design notes (see vector.cpp for the measurements):
//   * l2_distance returns SQUARED L2 (no sqrt) accumulated in float32 in 4..8
//     SIMD lanes. The kernel is chosen once at runtime (AVX2+FMA -> SSE2 ->
//     portable scalar) with __attribute__((target(...))) on functions in the
//     same translation unit, so the build system stays untouched. Determinism:
//     a given build+CPU always produces the same value for the same inputs, and
//     every path (scan, IVF, batch) calls the *same* kernel, which is what makes
//     the (dist asc, pk asc) tie-break below bit-exact. Accumulation order
//     differs from the old 4-scalar-accumulator loop by <= 2 ulp (measured max
//     2.1e-07 relative against l2_distance_scalar, kept as the reference), so
//     relative to a pre-SIMD build the last bits of a distance may move;
//     ordering of distinct vectors is unaffected (the gaps are ~1e-3, the
//     rounding is ~1e-7).
//   * distance_many() keeps the query block and four row accumulators in
//     registers across rows and returns values bit-identical to l2_distance().
//   * TopK is a bounded max-heap with a single-compare reject path: the executor
//     offers (pk, vector) one row at a time and never materialises a result set
//     for ORDER BY distance LIMIT k.
//   * exact_topk is O(n log k) with a bounded max-heap written directly into the
//     caller's output vector (one reserve, zero per-row allocation).
//   * IvfIndex is IVF-Flat: k-means++ seeding over a bounded sample, 10 Lloyd
//     iterations, boundary-only list replication, per-list radius bound (always
//     on, always sound) plus an optional completeness guarantee, insert/erase
//     with tombstoning + automatic re-clustering, and write_to/read_from.
//
// Measured on the competition box (i9-14900HX, g++13 -O2, no -march, 128-d,
// k=10, recall@10 against exact_topk; laptop timings vary +-20% run to run):
//        n      shape            nlist  nprobe  recall   ms/q    vs exact
//      10k   i.i.d. uniform       64      8     0.504   0.045     4.6x
//      10k   i.i.d. uniform       64     32     0.94    0.161     1.3x
//      10k   i.i.d. uniform       64     40     0.98     --        --
//      10k   clustered (in-dist)  64      8     1.000   0.0062   34x
//     100k   i.i.d. uniform      256      8     0.340   0.58      6.6x
//     100k   i.i.d. uniform      256     32     0.738   2.14      1.8x
//     100k   clustered (in-dist)  64      8     1.000   0.075    70x
//     100k   clustered (in-dist) 256      8     1.000   0.072    63x
//     (the IVF rows above were measured with the pre-SIMD kernel; with the SIMD
//      kernels every configuration is another 15-20% faster, e.g. 10k uniform
//      nprobe=8 0.045 -> 0.039 ms/q and 100k nlist=256 nprobe=8 0.63 -> 0.40 ms/q)
//   exact_topk: 0.19-0.24 ms/query at 10k, 4.5 ms/query at 100k before the SIMD
//   rewrite (g++13 -O2, no -march). After it: 0.112 / 3.13 ms per query over the
//   same scattered rows, 0.090 / 2.49 ms over a packed column (`./test_vector
//   perf`; the full table, GB/s and ISA details are pasted at the top of
//   vector.cpp). The 100k case is DRAM-bound, so the win there is 1.2-1.5x, not
//   the 1.4-2.2x the L2/L3-resident sizes get.
//   memory: 100k x 128 = 55 MB (51.2 MB vectors + ~4 MB bookkeeping, +7.4%);
//           10k x 128 = 5.6 MB (+14%).
//   build: 10k/nlist=64 ~= 80 ms, 100k/nlist=256 ~= 1.0 s (k-means on an 8k
//          sample, 10 iterations, deterministic seed).
// Recall on *unstructured* data costs roughly one probe per percent of the table
// (distance concentration -- no pruning index can beat that), while on
// structured embeddings the always-on per-list radius test skips every list that
// provably cannot hold a winner, so 8/256 lists already returns exact answers.
//
// Thread-safety: search()/search_filtered() share mutable scratch (list order,
// dedup stamps, the top-k heap), so one IvfIndex must not be searched
// concurrently. build(), insert() and erase() must not run concurrently with a
// search.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
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

namespace detail {

// Signature of a compiled distance kernel: squared L2, non-null pointers,
// dim > 0 (the public entry points do the null/0-dim checks).
using DistFn = float (*)(const float* a, const float* b, size_t dim);

// Max-heap ordered by Hit::less (index 0 == worst hit so far), in place.
inline bool hit_less(const Hit& a, const Hit& b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}

inline void sift_up(Hit* h, size_t i) {
  while (i > 0) {
    const size_t p = (i - 1) / 2;
    if (!hit_less(h[p], h[i])) break;
    const Hit t = h[i];
    h[i] = h[p];
    h[p] = t;
    i = p;
  }
}

inline void sift_down(Hit* h, size_t n, size_t i) {
  for (;;) {
    size_t big = i;
    const size_t l = 2 * i + 1, r = l + 1;
    if (l < n && hit_less(h[big], h[l])) big = l;
    if (r < n && hit_less(h[big], h[r])) big = r;
    if (big == i) break;
    const Hit t = h[i];
    h[i] = h[big];
    h[big] = t;
    i = big;
  }
}

// Resolves the best kernel for this CPU (thread-safe, resolved once).
DistFn resolve_dist_fn();

}  // namespace detail

// ---- kernel introspection (tests + `./test_vector perf`) --------------------
// Every kernel compiled into this build, in selection order. Index 0 is always
// the portable scalar reference. *available() is false when the CPU (or the
// compiler/target) cannot run that kernel.
size_t l2_kernel_count();
const char* l2_kernel_name(size_t i);
bool l2_kernel_available(size_t i);
float l2_distance_kernel(size_t i, const float* a, const float* b, size_t dim);
// Name of the kernel l2_distance() is actually using ("avx2+fma", "sse2",
// "scalar"). Respects the MVDB_ISA env override (scalar|sse2|avx2).
const char* l2_isa_name();

// Portable reference kernel: four independent float accumulators, exactly the
// code the engine used before SIMD. Tests bound the SIMD rounding difference
// against it; nothing in the engine calls it.
float l2_distance_scalar(const float* a, const float* b, size_t dim);

// The selected kernel as a function pointer, resolved once per process. Callers
// on the hot path (TopK, search) cache it instead of re-resolving per row.
inline detail::DistFn l2_kernel() {
  static const detail::DistFn fn = detail::resolve_dist_fn();
  return fn;
}

// Same contract as l2_distance(), inline: use this in per-row loops.
inline float l2_distance_inline(const float* a, const float* b, size_t dim) {
  if (a == nullptr || b == nullptr || dim == 0) return 0.0f;
  return l2_kernel()(a, b, dim);
}

// out[i] = squared L2 between q and rows[i*dim .. i*dim+dim), i < n.
// Keeps the query block in registers across four rows at a time; every output
// value is bit-identical to l2_distance(q, rows + i*dim, dim).
void distance_many(const float* q, const float* rows, size_t n, size_t dim, float* out);

// ---------------------------------------------------------------- streaming --
// Bounded top-k with a single-compare reject path. Feed it N candidates, get k
// sorted hits: no per-row allocation, no full result set, O(N) amortised.
//
//   TopK top(k);                      // once, reuse across queries
//   top.reset(k);  top.set_query(q, dim);  // per query (set_query is REQUIRED:
//                                          //   offer() dereferences it)
//   for (each row) top.offer(pk, vec, dim);      // inlineable, no branches on k
//   top.finish(&out);                            // (dist asc, pk asc)
//
// offer() computes the distance with the selected SIMD kernel; offer_dist() is
// the same thing when the distance is already known (batched scans).
class TopK {
 public:
  TopK() = default;
  explicit TopK(size_t k, bool with_pk_tiebreak = true) { reset(k, with_pk_tiebreak); }
  TopK(const float* q, size_t dim, size_t k, bool with_pk_tiebreak = true) {
    reset(k, with_pk_tiebreak);
    set_query(q, dim);
  }

  // Grows the heap to k (no reallocation while k does not change) and empties it.
  // with_pk_tiebreak=false compares distances only: ties are then resolved by
  // insertion order instead of by pk.
  void reset(size_t k, bool with_pk_tiebreak = true);

  void set_query(const float* q, size_t dim) {
    q_ = q;
    dim_ = dim;
  }
  // Same query for rows of a different shape (rare); offer() passes dim anyway.
  void clear() { n_ = 0; }

  size_t k() const { return k_; }
  size_t size() const { return n_; }
  bool full() const { return n_ >= k_; }
  bool with_pk_tiebreak() const { return tie_; }
  size_t dim() const { return dim_; }
  // Current k-th best distance (inf until the heap is full) — a cheap external
  // threshold for callers that want to skip candidates themselves.
  float worst_dist() const {
    return (k_ != 0 && n_ >= k_) ? h_[0].dist : std::numeric_limits<float>::infinity();
  }
  // Unsorted view of the heap; heap[0] is the worst kept hit.
  const Hit* heap() const { return h_; }

  // Hot path. Nothing here allocates.
  inline void offer(int64_t pk, const float* v, size_t dim) {
    offer_dist(pk, fn_(v, q_, dim));
  }
  inline void offer_dist(int64_t pk, float d) {
    if (n_ < k_) {  // still filling: bounded by k inserts per query
      size_t i = n_++;
      h_[i].pk = pk;
      h_[i].dist = d;
      detail::sift_up(h_, i);
      return;
    }
    if (k_ == 0) return;
    const Hit w = h_[0];
    if (d > w.dist) return;  // the common case: one compare, no heap touch
    if (!(d < w.dist)) {     // equal distance, or NaN
      if (d != w.dist) return;                     // NaN never displaces a hit
      if (!tie_ || pk >= w.pk) return;             // tie-break: pk ascending
    }
    h_[0].pk = pk;
    h_[0].dist = d;
    detail::sift_down(h_, n_, 0);
  }

  // Copies the hits into *out sorted by (dist asc, pk asc). Never shrinks the
  // capacity of *out, so a reused output vector stays allocation-free.
  void finish(std::vector<Hit>* out);

 private:
  std::vector<Hit> heap_;  // owns the storage (capacity >= max(k,1))
  Hit* h_ = nullptr;       // == heap_.data(); cached for the hot path
  // Pre-resolved so a default-constructed object rejects every offer_dist()
  // instead of calling a null pointer; reset() re-resolves it. offer() still
  // requires set_query().
  detail::DistFn fn_ = l2_kernel();
  const float* q_ = nullptr;
  size_t n_ = 0;
  size_t k_ = 0;
  size_t dim_ = 0;
  bool tie_ = true;
};

// Exact top-k over `rows`, skipping dead[i] != 0.
// vec_col: column holding the VECTOR value; pk_col: BIGINT pk column.
// pk_col < 0 (or a non-BIGINT value there) falls back to Row::rid.
// Rows whose vector type/size does not match `dim` are ignored.
// Result is sorted by (dist asc, pk asc) and holds at most min(k, live) hits.
void exact_topk(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
                int vec_col, int pk_col, const float* q, size_t dim, size_t k,
                std::vector<Hit>* out);

// Same, over a packed row-major n x dim matrix with a parallel pk array (the
// shape an IVF/column store holds). Runs four rows per distance call.
void topk_matrix(const float* mat, const int64_t* pks, size_t n, const float* q,
                 size_t dim, size_t k, std::vector<Hit>* out);

class IvfIndex {
 public:
  IvfIndex() = default;

  // Builds from the current table state. `dim` == 0 means "infer from the first
  // live VECTOR value". `nlist` is clamped to the live row count; pass 0 for an
  // automatic cell count (clamp(sqrt(n), 16, 4096) -- 64 cells is right for 10k
  // rows, 256..1024 for 100k).
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
  // Lists probed per query. Measured on this machine (i9-14900HX, g++13 -O2,
  // k=10, recall vs exact_topk):
  //   n=10k i.i.d. uniform 128-d, nlist=64: np 8 -> 0.50 @3.3x | 16 -> 0.77
  //     | 32 -> 0.95 @1.0x | 40 -> 0.98 | 64 -> 1.00 (slower than a flat scan)
  //   n=100k i.i.d. uniform, nlist=256: np 8 -> 0.34 @8.6x | 32 -> 0.74 @2.5x
  //   n=100k clustered embeddings, nlist=256: np 8 -> 1.000 @63x
  // i.e. recall on *unstructured* data costs ~1 probe per percent of the data
  // (no index can beat that); on structured embeddings 8/256 lists are enough
  // for perfect recall. 8 is the fast default; 32-40 is the "recall >= 0.95 on
  // random data" setting; use_radius_prune_ = nprobe_ = anything is exact.
  size_t nprobe_ = 8;
  // Number of nearest inverted lists a single vector is filed under (1..8).
  // Replication buys recall at fixed nprobe for 4 bytes per duplicate entry, but
  // only boundary points are duplicated: a point joins its 2nd..r-th nearest
  // list only if that list's squared distance is within replication_ratio_ of
  // the nearest. Without that cap, a "hub" centroid in clustered high-dim data
  // ends up holding a fifth of the table (measured: 3454 of 20000 entries),
  // which destroys probe cost.
  size_t replication_ = 2;
  float replication_ratio_ = 1.3f;  // squared-distance factor for extra lists
  // When true, probing continues past nprobe_ until the triangle-inequality
  // bound proves that no unprobed list can beat the current k-th hit -- i.e. a
  // guarantee that the answer is complete. Results are bit-identical to
  // exact_topk. (The per-list radius test is always on and always sound; this
  // flag only adds the "keep probing until the whole table is excluded" part,
  // which changes nothing on clustered data and costs ~30% on uniform data.)
  bool use_radius_prune_ = false;
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
  // Diagnostics: live entries per inverted list. A large max/median ratio means
  // nlist is too small for the data (badly merged cells), which inflates probe
  // cost: check it before blaming the recall numbers.
  std::vector<size_t> list_sizes() const;
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
  size_t nearest_lists_(const float* v, size_t r, uint32_t* ids, float* ds);
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
  mutable TopK scratch_;  // reused top-k heap (no per-query allocation)
  std::vector<float> ndist_;  // nearest_lists_ scratch: nlist_ centroid distances
};

}  // namespace mvdb

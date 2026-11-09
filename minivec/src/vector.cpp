// vector.cpp — see vector.h for the contract and the tuning knobs.
//
// ============================================================================
// BENCH — `./test_vector perf`, i9-14900HX, g++ 13.3 -O2 (no -march), Linux
//         under WSL2, k=10, i.i.d. uniform queries, single thread. Median of 3
//         runs (each column already best-of-3); the box is shared, so absolute
//         ms move +-15% run to run while the ratios stay put.
//
//   ISA selected: avx2 (compiled: scalar, sse2, avx2+fma; chosen at runtime via
//   __builtin_cpu_supports, forceable with MVDB_ISA=scalar|sse2|avx2).
//
//      n   dim | legacy ms/q | scan+TopK  (vs legacy) | packed topk (vs legacy) | GB/s
//     1k   128 |    0.018    |  0.0120      1.5x       |  0.0080      2.2x       | 66
//    10k   128 |    0.154    |  0.1120      1.38x      |  0.0900      1.71x      | 57
//   100k   128 |    3.63     |  3.130       1.16x      |  2.490       1.46x      | 21
//     1k   512 |    0.055    |  0.0330      1.67x      |  0.0250      2.15x      | 81
//    10k   512 |    0.820    |  0.590       1.38x      |  0.420       1.95x      | 49
//   100k   512 |   12.89     | 10.55        1.22x      |  7.750       1.66x      | 27
//
//   "legacy" is the pre-SIMD kernel + old heap scan (the 0.19/4.5 ms baseline;
//   0.154 vs 0.19 here is just this box being quiet), "packed" is distance_many
//   + fused TopK over a contiguous n x dim column.
//   ms for 1000 queries, 10k x 128 :  154 (legacy) ->  112 ->  90 (packed)
//   ms for 1000 queries, 100k x 128 : 3626 (legacy) -> 3126 -> 2488 (packed)
//
//   GB/s = n*dim*4 / ms on the packed column. 100k rows are past L3 (51 MB /
//   205 MB), so the scan sits on the DRAM roof and the kernel stops mattering:
//   21-27 GB/s is this box's single-thread streaming limit. At 10k x 128 (5 MB,
//   L3-resident) the ALU is the limit and AVX2+FMA buys 1.38x on scattered rows,
//   1.71x on a packed column.
//   Materialising every row and sorting (the buffering executor shape) costs
//   0.59 ms/q at 10k x 128 and 9.08 ms/q at 100k x 128, i.e. the fused TopK is
//   6.5x / 3.6x cheaper than that before any kernel work is counted.
// ============================================================================
#include "vector.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>

#if defined(__x86_64__) || defined(__i386__)
#define MVDB_X86 1
#include <immintrin.h>
#else
#define MVDB_X86 0
#endif

namespace mvdb {

// ================================================================ kernels ====
// Three implementations of squared L2, selected once at runtime:
//   * scalar       — the pre-SIMD code (four float accumulators), portable.
//   * sse2         — 8 floats per iteration, 2 xmm accumulators. Baseline on
//                    every x86-64 CPU, so it is the 32-bit fallback.
//   * avx2+fma     — 16 floats per iteration, 2 ymm accumulators, fused
//                    multiply-add (one rounding instead of two).
// Compiling several ISAs in one TU is done with __attribute__((target(...))) on
// plain functions: no build-system change, no separate object files, and the
// generic build (-O2, no -march) still emits SSE2-only code everywhere else.
//
// Accumulation order is fixed per kernel and identical between the single-row
// and the four-row batched variant (they share the `finish` helper and the
// horizontal reduction), so distance_many() is bit-identical to l2_distance()
// and a given build+CPU always answers the same query the same way.
namespace {

// ---- portable reference (also the non-x86 kernel) ---------------------------
float l2_scalar(const float* __restrict a, const float* __restrict b, size_t dim) {
  float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
  size_t i = 0;
  for (; i + 4 <= dim; i += 4) {
    const float d0 = a[i] - b[i];
    const float d1 = a[i + 1] - b[i + 1];
    const float d2 = a[i + 2] - b[i + 2];
    const float d3 = a[i + 3] - b[i + 3];
    s0 += d0 * d0;
    s1 += d1 * d1;
    s2 += d2 * d2;
    s3 += d3 * d3;
  }
  for (; i < dim; ++i) {
    const float d = a[i] - b[i];
    s0 += d * d;
  }
  return (s0 + s1) + (s2 + s3);
}

void l2_scalar_many(const float* __restrict q, const float* __restrict rows, size_t n,
                    size_t dim, float* __restrict out) {
  for (size_t r = 0; r < n; ++r) out[r] = l2_scalar(q, rows + r * dim, dim);
}

#if MVDB_X86

// ---- SSE2 -------------------------------------------------------------------
__attribute__((target("sse2"))) inline float sse2_hsum(__m128 x) {
  x = _mm_add_ps(x, _mm_movehl_ps(x, x));
  x = _mm_add_ss(x, _mm_shuffle_ps(x, x, 0x55));
  return _mm_cvtss_f32(x);
}

// Completes one row: 4-float blocks into acc0, then the scalar tail, then the
// horizontal reduction. Shared by the single-row and the 4-row kernels so their
// results are bit-identical.
__attribute__((target("sse2"))) inline float sse2_finish(const float* a, const float* b,
                                                         size_t i, size_t dim, __m128 acc0,
                                                         __m128 acc1) {
  for (; i + 4 <= dim; i += 4) {
    const __m128 d = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
    acc0 = _mm_add_ps(acc0, _mm_mul_ps(d, d));
  }
  float tail = 0.f;
  for (; i < dim; ++i) {
    const float d = a[i] - b[i];
    tail += d * d;
  }
  return sse2_hsum(_mm_add_ps(acc0, acc1)) + tail;
}

__attribute__((target("sse2"))) float l2_sse2(const float* __restrict a,
                                              const float* __restrict b, size_t dim) {
  __m128 acc0 = _mm_setzero_ps(), acc1 = _mm_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= dim; i += 8) {
    const __m128 d0 = _mm_sub_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i));
    const __m128 d1 = _mm_sub_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4));
    acc0 = _mm_add_ps(acc0, _mm_mul_ps(d0, d0));
    acc1 = _mm_add_ps(acc1, _mm_mul_ps(d1, d1));
  }
  return sse2_finish(a, b, i, dim, acc0, acc1);
}

__attribute__((target("sse2"))) void l2_sse2_many(const float* __restrict q,
                                                  const float* __restrict rows, size_t n,
                                                  size_t dim, float* __restrict out) {
  size_t r = 0;
  for (; r + 4 <= n; r += 4) {  // 4 rows: the query blocks load once per step
    const float* p0 = rows + (r + 0) * dim;
    const float* p1 = rows + (r + 1) * dim;
    const float* p2 = rows + (r + 2) * dim;
    const float* p3 = rows + (r + 3) * dim;
    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
    __m128 b0 = _mm_setzero_ps(), b1 = _mm_setzero_ps();
    __m128 c0 = _mm_setzero_ps(), c1 = _mm_setzero_ps();
    __m128 d0_ = _mm_setzero_ps(), d1_ = _mm_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
      const __m128 q0 = _mm_loadu_ps(q + i), q1 = _mm_loadu_ps(q + i + 4);
      __m128 d;
      d = _mm_sub_ps(_mm_loadu_ps(p0 + i), q0);
      a0 = _mm_add_ps(a0, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p0 + i + 4), q1);
      a1 = _mm_add_ps(a1, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p1 + i), q0);
      b0 = _mm_add_ps(b0, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p1 + i + 4), q1);
      b1 = _mm_add_ps(b1, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p2 + i), q0);
      c0 = _mm_add_ps(c0, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p2 + i + 4), q1);
      c1 = _mm_add_ps(c1, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p3 + i), q0);
      d0_ = _mm_add_ps(d0_, _mm_mul_ps(d, d));
      d = _mm_sub_ps(_mm_loadu_ps(p3 + i + 4), q1);
      d1_ = _mm_add_ps(d1_, _mm_mul_ps(d, d));
    }
    out[r + 0] = sse2_finish(q, p0, i, dim, a0, a1);
    out[r + 1] = sse2_finish(q, p1, i, dim, b0, b1);
    out[r + 2] = sse2_finish(q, p2, i, dim, c0, c1);
    out[r + 3] = sse2_finish(q, p3, i, dim, d0_, d1_);
  }
  for (; r < n; ++r) out[r] = l2_sse2(q, rows + r * dim, dim);
}

// ---- AVX2 + FMA -------------------------------------------------------------
__attribute__((target("avx2,fma"))) inline float avx2_hsum(__m256 x) {
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(x), _mm256_extractf128_ps(x, 1));
  s = _mm_add_ps(s, _mm_movehl_ps(s, s));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 0x55));
  return _mm_cvtss_f32(s);
}

__attribute__((target("avx2,fma"))) inline float avx2_finish(const float* a, const float* b,
                                                             size_t i, size_t dim, __m256 acc0,
                                                             __m256 acc1) {
  for (; i + 8 <= dim; i += 8) {
    const __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }
  float tail = 0.f;
  for (; i < dim; ++i) {
    const float d = a[i] - b[i];
    tail += d * d;
  }
  return avx2_hsum(_mm256_add_ps(acc0, acc1)) + tail;
}

__attribute__((target("avx2,fma"))) float l2_avx2(const float* __restrict a,
                                                  const float* __restrict b, size_t dim) {
  __m256 acc0 = _mm256_setzero_ps(), acc1 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= dim; i += 16) {
    const __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    const __m256 d1 =
        _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
  }
  return avx2_finish(a, b, i, dim, acc0, acc1);
}

__attribute__((target("avx2,fma"))) void l2_avx2_many(const float* __restrict q,
                                                      const float* __restrict rows, size_t n,
                                                      size_t dim, float* __restrict out) {
  size_t r = 0;
  for (; r + 4 <= n; r += 4) {  // 4 rows in flight: one query load per 4 rows
    const float* p0 = rows + (r + 0) * dim;
    const float* p1 = rows + (r + 1) * dim;
    const float* p2 = rows + (r + 2) * dim;
    const float* p3 = rows + (r + 3) * dim;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 b0 = _mm256_setzero_ps(), b1 = _mm256_setzero_ps();
    __m256 c0 = _mm256_setzero_ps(), c1 = _mm256_setzero_ps();
    __m256 c2 = _mm256_setzero_ps(), c3 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 16 <= dim; i += 16) {
      const __m256 q0 = _mm256_loadu_ps(q + i), q1 = _mm256_loadu_ps(q + i + 8);
      __m256 d;
      d = _mm256_sub_ps(_mm256_loadu_ps(p0 + i), q0);
      a0 = _mm256_fmadd_ps(d, d, a0);
      d = _mm256_sub_ps(_mm256_loadu_ps(p0 + i + 8), q1);
      a1 = _mm256_fmadd_ps(d, d, a1);
      d = _mm256_sub_ps(_mm256_loadu_ps(p1 + i), q0);
      b0 = _mm256_fmadd_ps(d, d, b0);
      d = _mm256_sub_ps(_mm256_loadu_ps(p1 + i + 8), q1);
      b1 = _mm256_fmadd_ps(d, d, b1);
      d = _mm256_sub_ps(_mm256_loadu_ps(p2 + i), q0);
      c0 = _mm256_fmadd_ps(d, d, c0);
      d = _mm256_sub_ps(_mm256_loadu_ps(p2 + i + 8), q1);
      c1 = _mm256_fmadd_ps(d, d, c1);
      d = _mm256_sub_ps(_mm256_loadu_ps(p3 + i), q0);
      c2 = _mm256_fmadd_ps(d, d, c2);
      d = _mm256_sub_ps(_mm256_loadu_ps(p3 + i + 8), q1);
      c3 = _mm256_fmadd_ps(d, d, c3);
    }
    out[r + 0] = avx2_finish(q, p0, i, dim, a0, a1);
    out[r + 1] = avx2_finish(q, p1, i, dim, b0, b1);
    out[r + 2] = avx2_finish(q, p2, i, dim, c0, c1);
    out[r + 3] = avx2_finish(q, p3, i, dim, c2, c3);
  }
  for (; r < n; ++r) out[r] = l2_avx2(q, rows + r * dim, dim);
}

bool cpu_has_avx2() {
  static const bool has = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  return has;
}

#endif  // MVDB_X86

// ---- dispatch table ---------------------------------------------------------
struct Kernel {
  const char* name;
  detail::DistFn fn;
  void (*many)(const float* q, const float* rows, size_t n, size_t dim, float* out);
};

const Kernel kTable[] = {
    {"scalar", l2_scalar, l2_scalar_many},
#if MVDB_X86
    {"sse2", l2_sse2, l2_sse2_many},
    {"avx2", l2_avx2, l2_avx2_many},
#endif
};
const size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);

bool kernel_available(size_t i) {
  if (i >= kTableSize) return false;
#if MVDB_X86
  if (i == 2) return cpu_has_avx2();
#endif
  return true;
}

struct Active {
  size_t idx;
  detail::DistFn fn;
  void (*many)(const float*, const float*, size_t, size_t, float*);
  const char* name;
};

// Resolved exactly once. MVDB_ISA=scalar|sse2|avx2 (prefix match) forces a
// kernel — that is how the test suite proves every fallback is correct.
Active make_active() {
  size_t idx = 0;
  const char* env = std::getenv("MVDB_ISA");
  if (env != nullptr && *env != '\0') {
    const size_t n = std::strlen(env);
    for (size_t i = 0; i < kTableSize; ++i) {
      if (std::strncmp(kTable[i].name, env, n) == 0 && kernel_available(i)) {
        idx = i;
        break;
      }
    }
  } else {
    for (size_t i = kTableSize; i-- > 0;) {
      if (kernel_available(i)) {
        idx = i;
        break;
      }
    }
  }
  return Active{idx, kTable[idx].fn, kTable[idx].many, kTable[idx].name};
}

const Active& active() {
  static const Active a = make_active();
  return a;
}

}  // namespace

// ---------------------------------------------------------------- distance --
// Squared L2 (see vector.h). Same contract as before the SIMD rewrite: 0 for
// null operands or dim 0, and identical inputs always give identical output.
float l2_distance(const float* a, const float* b, size_t dim) {
  if (a == nullptr || b == nullptr || dim == 0) return 0.0f;
  return detail::resolve_dist_fn()(a, b, dim);
}

detail::DistFn detail::resolve_dist_fn() { return active().fn; }

float l2_distance_scalar(const float* a, const float* b, size_t dim) {
  if (a == nullptr || b == nullptr || dim == 0) return 0.0f;
  return l2_scalar(a, b, dim);
}

size_t l2_kernel_count() { return kTableSize; }
const char* l2_kernel_name(size_t i) { return i < kTableSize ? kTable[i].name : ""; }
bool l2_kernel_available(size_t i) { return kernel_available(i); }
float l2_distance_kernel(size_t i, const float* a, const float* b, size_t dim) {
  if (i >= kTableSize || a == nullptr || b == nullptr || dim == 0) return 0.0f;
  return kTable[i].fn(a, b, dim);
}
const char* l2_isa_name() { return active().name; }

void distance_many(const float* q, const float* rows, size_t n, size_t dim, float* out) {
  if (q == nullptr || rows == nullptr || out == nullptr || n == 0 || dim == 0) return;
  active().many(q, rows, n, dim, out);
}

// ------------------------------------------------------------- streaming -----
void TopK::reset(size_t k, bool with_pk_tiebreak) {
  k_ = k;
  tie_ = with_pk_tiebreak;
  n_ = 0;
  fn_ = detail::resolve_dist_fn();
  if (heap_.size() < (k < 1 ? 1 : k)) heap_.resize(k < 1 ? 1 : k);
  h_ = heap_.data();
}

void TopK::finish(std::vector<Hit>* out) {
  if (out == nullptr) return;
  out->clear();
  if (n_ == 0) return;
  if (out->capacity() < n_) out->reserve(n_);  // grows, never shrinks
  out->assign(h_, h_ + n_);
  std::sort(out->begin(), out->end(), detail::hit_less);
}

namespace {

// Extracts pk for a row: pk column when it is a BIGINT, Row::rid otherwise.
inline int64_t row_pk(const Row& r, int pk_col) {
  if (pk_col >= 0 && static_cast<size_t>(pk_col) < r.vals.size()) {
    const Value& v = r.vals[pk_col];
    if (v.type == Type::BIGINT) return v.i;
  }
  return r.rid;
}

bool wr_bytes(FILE* f, const void* p, size_t n) {
  return n == 0 || std::fwrite(p, 1, n, f) == n;
}
bool rd_bytes(FILE* f, void* p, size_t n) {
  return n == 0 || std::fread(p, 1, n, f) == n;
}

constexpr uint32_t kMagic = 0x564D4449u;  // 'IDMV'
constexpr uint32_t kVersion = 1u;

// Automatic cell count: ~sqrt(n) cells keeps ~sqrt(n) points per list, which is
// the sweet spot in the measurements (64 for 10k, 316 for 100k).
size_t auto_nlist(size_t n) {
  size_t nl = static_cast<size_t>(std::sqrt(static_cast<double>(n)));
  if (nl < 16) nl = 16;
  if (nl > 4096) nl = 4096;
  if (nl > n) nl = n ? n : 1;
  return nl ? nl : 1;
}

}  // namespace

// -------------------------------------------------------------- exact top-k --
void exact_topk(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
                int vec_col, int pk_col, const float* q, size_t dim, size_t k,
                std::vector<Hit>* out) {
  if (out == nullptr) return;
  out->clear();
  if (k == 0 || q == nullptr || dim == 0) return;
  TopK top(k);  // one allocation, never grows
  top.set_query(q, dim);
  const detail::DistFn fn = detail::resolve_dist_fn();
  const size_t nd = dead.size();
  // (Prefetching the row -> vals[] -> vector<float> chain a few rows ahead was
  // measured and rejected: +6% at 100k x 512 but -10% at 128-d, where the extra
  // metadata load serialises in front of the two cache lines that actually matter.)
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i < nd && dead[i]) continue;
    const Row& r = rows[i];
    if (vec_col < 0 || static_cast<size_t>(vec_col) >= r.vals.size()) continue;
    const Value& v = r.vals[vec_col];
    if (v.type != Type::VECTOR || v.v.size() != dim) continue;
    top.offer_dist(row_pk(r, pk_col), fn(q, v.v.data(), dim));  // no per-row alloc
  }
  top.finish(out);
}

void topk_matrix(const float* mat, const int64_t* pks, size_t n, const float* q,
                 size_t dim, size_t k, std::vector<Hit>* out) {
  if (out == nullptr) return;
  out->clear();
  if (k == 0 || q == nullptr || mat == nullptr || pks == nullptr || dim == 0) return;
  TopK top(k);
  top.set_query(q, dim);
  float buf[256];
  size_t i = 0;
  for (; i + 256 <= n; i += 256) {  // batch the distance, fuse the top-k insert
    distance_many(q, mat + i * dim, 256, dim, buf);
    const int64_t* p = pks + i;
    for (size_t j = 0; j < 256; ++j) top.offer_dist(p[j], buf[j]);
  }
  if (i < n) {
    const size_t rest = n - i;
    distance_many(q, mat + i * dim, rest, dim, buf);
    const int64_t* p = pks + i;
    for (size_t j = 0; j < rest; ++j) top.offer_dist(p[j], buf[j]);
  }
  top.finish(out);
}

// ----------------------------------------------------------------- IvfIndex --
void IvfIndex::clear() {
  dim_ = 0;
  nlist_ = 0;
  live_ = 0;
  dead_slots_ = 0;
  target_nlist_ = 64;
  data_.clear();
  slot_pk_.clear();
  slot_dead_.clear();
  lists_.clear();
  cents_.clear();
  radius_.clear();
  max_rad_ = 0.f;
  pk2slot_.clear();
  cdist_.clear();
  order_.clear();
  stamp_.clear();
  gen_ = 0;
}

size_t IvfIndex::memory_bytes() const {
  size_t b = data_.capacity() * sizeof(float);
  b += slot_pk_.capacity() * sizeof(int64_t);
  b += slot_dead_.capacity();
  b += sizeof(lists_);
  for (const auto& l : lists_) b += sizeof(l) + l.capacity() * sizeof(uint32_t);
  b += cents_.capacity() * sizeof(float);
  b += radius_.capacity() * sizeof(float);
  b += cdist_.capacity() * sizeof(float);
  b += order_.capacity() * sizeof(uint32_t);
  b += stamp_.capacity() * sizeof(uint32_t);
  // Conservative per-node cost of libstdc++'s unordered_map (node + bucket array).
  b += pk2slot_.size() * (sizeof(int64_t) + sizeof(uint32_t) + 2 * sizeof(void*) + 16);
  return b;
}

// k-means++ seeding + Lloyd iterations over the sampled slots. Deterministic:
// fixed seed, so a rebuild reproduces the same clustering.
void IvfIndex::kmeans_(const std::vector<uint32_t>& sidx) {
  const size_t d = dim_;
  const size_t nl = nlist_;
  const size_t ns = sidx.size();
  std::mt19937 rng(0x5EED1234u);

  std::uniform_int_distribution<size_t> pick(0, ns - 1);
  std::memcpy(cents_.data(), data_.data() + static_cast<size_t>(sidx[pick(rng)]) * d,
              d * sizeof(float));

  std::vector<float> d2(ns, std::numeric_limits<float>::max());
  for (size_t c = 1; c < nl; ++c) {
    const float* last = cents_.data() + (c - 1) * d;
    double total = 0.0;
    for (size_t i = 0; i < ns; ++i) {
      const float dd = l2_distance(data_.data() + static_cast<size_t>(sidx[i]) * d, last, d);
      if (dd < d2[i]) d2[i] = dd;
      total += static_cast<double>(d2[i]);
    }
    size_t chosen = sidx[0];
    if (total > 0.0) {
      std::uniform_real_distribution<double> u(0.0, total);
      const double t = u(rng);
      double acc = 0.0;
      for (size_t i = 0; i < ns; ++i) {
        acc += static_cast<double>(d2[i]);
        if (acc >= t) {
          chosen = sidx[i];
          break;
        }
      }
    } else {
      chosen = sidx[pick(rng)];
    }
    std::memcpy(cents_.data() + c * d, data_.data() + static_cast<size_t>(chosen) * d,
                d * sizeof(float));
  }

  std::vector<float> sums(nl * d, 0.f);
  std::vector<float> cdist(nl, 0.f);
  std::vector<size_t> cnt(nl, 0);
  for (size_t it = 0; it < kmeans_iters_; ++it) {
    std::fill(sums.begin(), sums.end(), 0.f);
    std::fill(cnt.begin(), cnt.end(), 0);
    for (size_t i = 0; i < ns; ++i) {
      const float* p = data_.data() + static_cast<size_t>(sidx[i]) * d;
      // One call scores this point against every centroid: the point stays in
      // registers across the whole centroid array (see distance_many()).
      distance_many(p, cents_.data(), nl, d, cdist.data());
      size_t best = 0;
      float bd = cdist[0];
      for (size_t c = 1; c < nl; ++c) {
        if (cdist[c] < bd) {
          bd = cdist[c];
          best = c;
        }
      }
      ++cnt[best];
      float* s = sums.data() + best * d;
      for (size_t j = 0; j < d; ++j) s[j] += p[j];
    }
    for (size_t c = 0; c < nl; ++c) {
      float* cc = cents_.data() + c * d;
      if (cnt[c] == 0) {  // re-seed an empty cluster on a random sample point
        const size_t r = sidx[pick(rng)];
        std::memcpy(cc, data_.data() + static_cast<size_t>(r) * d, d * sizeof(float));
      } else {
        const float inv = 1.0f / static_cast<float>(cnt[c]);
        const float* s = sums.data() + c * d;
        for (size_t j = 0; j < d; ++j) cc[j] = s[j] * inv;
      }
    }
  }
}

// r best centroids for v, ascending by squared distance.
size_t IvfIndex::nearest_lists_(const float* v, size_t r, uint32_t* ids, float* ds) {
  if (nlist_ == 0 || dim_ == 0) return 0;
  if (r == 0) return 0;
  if (r > kMaxReplication) r = kMaxReplication;
  // All nlist_ centroid distances in one batched pass (~1 KB of scratch), then
  // the r-best selection over that buffer — the sort no longer interleaves with
  // the distance evaluation.
  ndist_.resize(nlist_);
  distance_many(v, cents_.data(), nlist_, dim_, ndist_.data());
  const float* nd = ndist_.data();
  size_t cnt = 0;
  for (size_t c = 0; c < nlist_; ++c) {
    const float d = nd[c];
    if (cnt == r && d >= ds[cnt - 1]) continue;
    size_t pos = (cnt < r) ? cnt : (r - 1);
    while (pos > 0 && ds[pos - 1] > d) {
      ds[pos] = ds[pos - 1];
      ids[pos] = ids[pos - 1];
      --pos;
    }
    ds[pos] = d;
    ids[pos] = static_cast<uint32_t>(c);
    if (cnt < r) ++cnt;
  }
  return cnt;
}

// Assigns every live slot to its r nearest centroids (extra lists only when the
// point sits near a cell boundary -- see replication_ratio_), then reorders the
// master vector array so that each list's members are *contiguous and ascending*:
// a probe then walks memory forward instead of chasing scattered slots (measured
// ~2.5x cheaper per candidate on 128-d). Radius per list = furthest member.
void IvfIndex::build_lists_() {
  const size_t n = slot_pk_.size();
  const size_t r = std::max<size_t>(1, std::min(replication_, nlist_));
  std::vector<uint32_t> assign(n * r);
  std::vector<float> adist(n * r);
  std::vector<uint8_t> keep(n, 1);
  for (size_t s = 0; s < n; ++s) {
    const size_t got = nearest_lists_(data_.data() + s * dim_, r, assign.data() + s * r,
                                     adist.data() + s * r);
    const float lim = adist[s * r] * replication_ratio_;
    size_t k = 1;
    while (k < got && adist[s * r + k] <= lim) ++k;
    keep[s] = static_cast<uint8_t>(k);
  }

  // Stable counting sort of slots by primary list id.
  std::vector<uint32_t> cnt(nlist_ + 1, 0);
  for (size_t s = 0; s < n; ++s) ++cnt[assign[s * r] + 1];
  for (size_t c = 0; c < nlist_; ++c) cnt[c + 1] += cnt[c];
  std::vector<uint32_t> newpos(n), old_of_new(n);
  for (size_t s = 0; s < n; ++s) newpos[s] = cnt[assign[s * r]]++;
  for (size_t s = 0; s < n; ++s) old_of_new[newpos[s]] = static_cast<uint32_t>(s);
  {
    std::vector<float> nd(data_.size());
    std::vector<int64_t> npk(n);
    for (size_t s = 0; s < n; ++s) {
      const uint32_t p = newpos[s];
      std::memcpy(nd.data() + static_cast<size_t>(p) * dim_, data_.data() + s * dim_,
                  dim_ * sizeof(float));
      npk[p] = slot_pk_[s];
    }
    data_.swap(nd);
    slot_pk_.swap(npk);
  }

  // Lists, appended in ascending slot order (i.e. ascending memory in data_).
  lists_.assign(nlist_, std::vector<uint32_t>());
  radius_.assign(nlist_, 0.f);
  max_rad_ = 0.f;
  for (size_t p = 0; p < n; ++p) {
    const size_t s = old_of_new[p];
    const uint32_t* a = assign.data() + s * r;
    const float* ad = adist.data() + s * r;
    for (size_t j = 0; j < keep[s]; ++j) {
      lists_[a[j]].push_back(static_cast<uint32_t>(p));
      const float rd = std::sqrt(ad[j]);  // Euclidean radius, used by the bound
      if (rd > radius_[a[j]]) radius_[a[j]] = rd;
    }
  }
  for (float v : radius_)
    if (v > max_rad_) max_rad_ = v;

  pk2slot_.clear();
  pk2slot_.reserve(n * 2);
  for (size_t p = 0; p < n; ++p) pk2slot_[slot_pk_[p]] = static_cast<uint32_t>(p);
}

// Adopts (pks, vecs) — both sized for the same number of rows — and clusters them.
void IvfIndex::build_core_(std::vector<int64_t>* pks, std::vector<float>* vecs, size_t nl) {
  const size_t n = pks->size();
  if (n == 0 || dim_ == 0) {
    clear();
    return;
  }
  nlist_ = std::max<size_t>(1, std::min(nl, n));
  live_ = n;
  dead_slots_ = 0;

  data_.swap(*vecs);
  slot_pk_.swap(*pks);
  slot_dead_.assign(n, 0);
  lists_.assign(nlist_, std::vector<uint32_t>());
  cents_.assign(nlist_ * dim_, 0.f);
  radius_.assign(nlist_, 0.f);

  // Train on a bounded sample when n is large.
  const size_t sample = std::min(n, std::max<size_t>(sample_cap_, nlist_ * 4));
  std::vector<uint32_t> sidx(n);
  for (size_t i = 0; i < n; ++i) sidx[i] = static_cast<uint32_t>(i);
  if (sample < n) {
    std::mt19937 rng(0xC0FFEEu);  // partial Fisher-Yates, deterministic
    for (size_t i = 0; i < sample; ++i) std::swap(sidx[i], sidx[i + rng() % (n - i)]);
    sidx.resize(sample);
    sidx.shrink_to_fit();
  }
  kmeans_(sidx);
  build_lists_();

  cdist_.assign(nlist_, 0.f);
  order_.resize(nlist_);
  stamp_.assign(n, 0);
  gen_ = 0;
}

void IvfIndex::build(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
                     int vec_col, int pk_col, size_t dim, size_t nlist) {
  clear();
  if (dim == 0) {  // infer from the first live VECTOR value
    const size_t nd = dead.size();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (i < nd && dead[i]) continue;
      if (vec_col < 0 || static_cast<size_t>(vec_col) >= rows[i].vals.size()) continue;
      const Value& v = rows[i].vals[vec_col];
      if (v.type == Type::VECTOR && !v.v.empty()) {
        dim = v.v.size();
        break;
      }
    }
  }
  dim_ = dim;
  if (dim_ == 0) return;

  std::vector<int64_t> pks;
  std::vector<float> vecs;
  pks.reserve(rows.size());
  vecs.reserve(rows.size() * dim_);
  const size_t nd = dead.size();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i < nd && dead[i]) continue;
    if (vec_col < 0 || static_cast<size_t>(vec_col) >= rows[i].vals.size()) continue;
    const Value& v = rows[i].vals[vec_col];
    if (v.type != Type::VECTOR || v.v.size() != dim_) continue;
    pks.push_back(row_pk(rows[i], pk_col));
    vecs.insert(vecs.end(), v.v.begin(), v.v.end());
  }
  if (pks.empty()) return;  // keep dim_ so inserts can bootstrap the index
  const size_t want = nlist == 0 ? auto_nlist(pks.size()) : nlist;
  target_nlist_ = want;  // insert() grows the index towards this cell count
  build_core_(&pks, &vecs, want);
}

void IvfIndex::rebuild_from_storage_(size_t nl) {
  std::vector<int64_t> pks;
  std::vector<float> vecs;
  pks.reserve(live_);
  vecs.reserve(live_ * dim_);
  const size_t slots = slot_pk_.size();
  for (size_t s = 0; s < slots; ++s) {
    if (slot_dead_[s]) continue;
    pks.push_back(slot_pk_[s]);
    vecs.insert(vecs.end(), data_.data() + s * dim_, data_.data() + (s + 1) * dim_);
  }
  if (pks.empty()) {
    const size_t d = dim_;
    const size_t tn = target_nlist_;
    clear();
    dim_ = d;
    target_nlist_ = tn;
    return;
  }
  build_core_(&pks, &vecs, std::max<size_t>(1, std::min(nl, pks.size())));
}

void IvfIndex::maybe_grow_() {
  if (target_nlist_ > nlist_ && live_ >= 8 * nlist_) {
    rebuild_from_storage_(std::min(target_nlist_, std::max<size_t>(nlist_ * 2, live_)));
    return;
  }
  const size_t total = slot_pk_.size();
  if (dead_slots_ > 256 && dead_slots_ * 2 > total) rebuild_from_storage_(nlist_);
}

void IvfIndex::insert(int64_t pk, const float* v) {
  if (v == nullptr || dim_ == 0) return;
  if (pk2slot_.find(pk) != pk2slot_.end()) erase(pk);  // replace semantics
  if (nlist_ == 0) {  // bootstrap: one list seeded on the first vector
    nlist_ = 1;
    cents_.assign(dim_, 0.f);
    std::memcpy(cents_.data(), v, dim_ * sizeof(float));
    lists_.assign(1, std::vector<uint32_t>());
    radius_.assign(1, 0.f);
    max_rad_ = 0.f;
    cdist_.assign(1, 0.f);
    order_.resize(1);
    target_nlist_ = std::max<size_t>(1, target_nlist_);
  }
  const uint32_t slot = static_cast<uint32_t>(slot_pk_.size());
  data_.insert(data_.end(), v, v + dim_);
  slot_pk_.push_back(pk);
  slot_dead_.push_back(0);
  pk2slot_[pk] = slot;
  ++live_;
  if (stamp_.size() < slot_pk_.size()) stamp_.resize(slot_pk_.size(), 0);

  const size_t r = std::max<size_t>(1, std::min(replication_, nlist_));
  uint32_t ids[kMaxReplication];
  float ds[kMaxReplication];
  const size_t got = nearest_lists_(v, r, ids, ds);
  const float lim = ds[0] * replication_ratio_;  // boundary points only
  size_t keep = 1;
  while (keep < got && ds[keep] <= lim) ++keep;
  for (size_t j = 0; j < keep; ++j) {
    lists_[ids[j]].push_back(slot);
    const float rd = std::sqrt(ds[j]);
    if (rd > radius_[ids[j]]) radius_[ids[j]] = rd;
    if (rd > max_rad_) max_rad_ = rd;
  }
  maybe_grow_();
}

void IvfIndex::erase(int64_t pk) {
  const auto it = pk2slot_.find(pk);
  if (it == pk2slot_.end()) return;
  const uint32_t slot = it->second;
  pk2slot_.erase(it);
  if (slot_dead_[slot]) return;
  slot_dead_[slot] = 1;
  --live_;
  ++dead_slots_;
  // radius_ and max_rad_ stay as over-estimates: still sound, only looser.
  maybe_grow_();
}

void IvfIndex::ensure_query_buffers_() const {
  if (cdist_.size() != nlist_) cdist_.assign(nlist_, 0.f);
  if (order_.size() != nlist_) order_.resize(nlist_);
  if (stamp_.size() != slot_pk_.size()) stamp_.assign(slot_pk_.size(), 0);
}

namespace {
using Pred = std::function<bool(int64_t)>;
}

void IvfIndex::search(const float* q, size_t k, std::vector<Hit>* out) const {
  const Pred* none = nullptr;
  search_impl_(q, k, none, out);
}

void IvfIndex::search_filtered(const float* q, size_t k, const Pred& pred,
                               std::vector<Hit>* out) const {
  search_impl_(q, k, &pred, out);
}

void IvfIndex::search_impl_(const float* q, size_t k, const Pred* pred,
                            std::vector<Hit>* out) const {
  if (out == nullptr) return;
  out->clear();
  if (q == nullptr || k == 0 || live_ == 0 || nlist_ == 0 || dim_ == 0) return;
  ensure_query_buffers_();
  if (++gen_ == 0) {  // wrap: clear stamps and restart the generation
    std::fill(stamp_.begin(), stamp_.end(), 0u);
    gen_ = 1;
  }
  const detail::DistFn fn = detail::resolve_dist_fn();
  // Reused bounded heap: no allocation once k has been seen before.
  if (scratch_.k() != k) scratch_.reset(k);
  scratch_.clear();
  scratch_.set_query(q, dim_);

  // 1) Centroid ranking: full ascending order, needed by the pruning walk.
  //    One batched pass keeps q in registers across all centroids.
  distance_many(q, cents_.data(), nlist_, dim_, cdist_.data());
  for (size_t c = 0; c < nlist_; ++c) order_[c] = static_cast<uint32_t>(c);
  std::sort(order_.begin(), order_.end(), [this](uint32_t a, uint32_t b) {
    if (cdist_[a] != cdist_[b]) return cdist_[a] < cdist_[b];
    return a < b;
  });

  // 2) Probe.
  const size_t min_probe = std::max<size_t>(1, std::min(nprobe_, nlist_));
  const size_t want_pass =
      pred != nullptr ? k * std::max<size_t>(1, filtered_oversample_) : 0;
  const float* base = data_.data();
  size_t probed = 0, passed = 0;

  for (size_t oi = 0; oi < nlist_; ++oi) {
    const uint32_t ci = order_[oi];
    const bool full = scratch_.full();
    const float worst = full ? scratch_.worst_dist() : std::numeric_limits<float>::infinity();
    // Tolerance for float rounding in sqrt-based bounds (percent-level slack,
    // far below any real gap, so pruning never drops a true winner).
    const float tol = 1e-3f * (1.0f + worst);
    if (probed >= min_probe) {
      bool more = false;
      if (pred != nullptr && passed < want_pass) more = true;
      if (!more) {
        if (!use_radius_prune_) break;
        if (full) {
          const float sc = std::sqrt(cdist_[ci]);
          const float gap = sc - max_rad_;
          const float lb = gap > 0.f ? gap * gap : 0.f;  // <= d(q,x) for all later lists
          if (lb > worst + tol) break;
        }
      }
    }
    ++probed;
    // This list alone cannot contribute (sound: d(q,x) >= (sqrt(d(q,c))-r_c)^2).
    if (full) {
      const float sc = std::sqrt(cdist_[ci]);
      const float gap = sc - radius_[ci];
      const float lb = gap > 0.f ? gap * gap : 0.f;
      if (lb > worst + tol) continue;
    }
    const std::vector<uint32_t>& list = lists_[ci];
    const size_t ln = list.size();
    for (size_t li = 0; li < ln; ++li) {
      const uint32_t slot = list[li];
#if defined(__GNUC__)
      if (li + 4 < ln) __builtin_prefetch(base + static_cast<size_t>(list[li + 4]) * dim_, 0, 1);
#endif
      if (slot_dead_[slot]) continue;
      if (stamp_[slot] == gen_) continue;  // replication / multi-probe duplicate
      stamp_[slot] = gen_;
      const int64_t pk = slot_pk_[slot];
      if (pred != nullptr && !(*pred)(pk)) continue;  // filter BEFORE top-k
      ++passed;
      scratch_.offer_dist(pk, fn(q, base + static_cast<size_t>(slot) * dim_, dim_));
    }
  }
  scratch_.finish(out);
}

// --------------------------------------------------------------- diagnostics --
std::vector<size_t> IvfIndex::list_sizes() const {
  std::vector<size_t> out(nlist_, 0);
  for (size_t c = 0; c < nlist_; ++c) {
    size_t live = 0;
    for (uint32_t slot : lists_[c]) {
      if (!slot_dead_[slot]) ++live;
    }
    out[c] = live;
  }
  return out;
}

// ------------------------------------------------------------------ persist --
bool IvfIndex::write_to(FILE* f) const {
  if (f == nullptr) return false;
  const uint32_t magic = kMagic, ver = kVersion;
  const uint64_t hdr[6] = {static_cast<uint64_t>(dim_),
                           static_cast<uint64_t>(nlist_),
                           static_cast<uint64_t>(live_),
                           static_cast<uint64_t>(slot_pk_.size()),
                           static_cast<uint64_t>(dead_slots_),
                           static_cast<uint64_t>(target_nlist_)};
  const float mr = max_rad_;
  if (!wr_bytes(f, &magic, 4) || !wr_bytes(f, &ver, 4) || !wr_bytes(f, hdr, sizeof hdr) ||
      !wr_bytes(f, &mr, 4))
    return false;
  if (!wr_bytes(f, cents_.data(), cents_.size() * 4)) return false;
  if (!wr_bytes(f, radius_.data(), radius_.size() * 4)) return false;
  if (!wr_bytes(f, data_.data(), slot_pk_.size() * dim_ * 4)) return false;
  if (!wr_bytes(f, slot_pk_.data(), slot_pk_.size() * 8)) return false;
  if (!wr_bytes(f, slot_dead_.data(), slot_dead_.size())) return false;
  for (const auto& l : lists_) {
    const uint64_t c = l.size();
    if (!wr_bytes(f, &c, 8)) return false;
    if (!wr_bytes(f, l.data(), l.size() * 4)) return false;
  }
  return true;
}

bool IvfIndex::read_from(FILE* f) {
  if (f == nullptr) return false;
  uint32_t magic = 0, ver = 0;
  uint64_t hdr[6] = {0, 0, 0, 0, 0, 0};
  float mr = 0.f;
  const auto bad = [this]() {
    clear();
    return false;
  };
  if (!rd_bytes(f, &magic, 4) || magic != kMagic) return bad();
  if (!rd_bytes(f, &ver, 4) || ver != kVersion) return bad();
  if (!rd_bytes(f, hdr, sizeof hdr) || !rd_bytes(f, &mr, 4)) return bad();
  const size_t dim = hdr[0], nl = hdr[1], live = hdr[2], slots = hdr[3];
  const size_t deads = hdr[4], target = hdr[5];
  if (dim > (1u << 16) || nl > (1u << 22) || slots > (1ull << 33)) return bad();
  if (dim != 0 && slots > (1ull << 33) / dim) return bad();
  if (live > slots || deads > slots) return bad();

  clear();
  dim_ = dim;
  nlist_ = nl;
  live_ = live;
  dead_slots_ = deads;
  target_nlist_ = target ? target : 64;
  cents_.resize(nl * dim);
  radius_.resize(nl);
  data_.resize(slots * dim);
  slot_pk_.resize(slots);
  slot_dead_.resize(slots);
  lists_.assign(nl, std::vector<uint32_t>());
  if (!rd_bytes(f, cents_.data(), cents_.size() * 4)) return bad();
  if (!rd_bytes(f, radius_.data(), radius_.size() * 4)) return bad();
  if (!rd_bytes(f, data_.data(), slots * dim * 4)) return bad();
  if (!rd_bytes(f, slot_pk_.data(), slots * 8)) return bad();
  if (!rd_bytes(f, slot_dead_.data(), slots)) return bad();
  for (size_t c = 0; c < nl; ++c) {
    uint64_t cnt = 0;
    if (!rd_bytes(f, &cnt, 8) || cnt > slots) return bad();
    lists_[c].resize(cnt);
    if (!rd_bytes(f, lists_[c].data(), cnt * 4)) return bad();
  }
  pk2slot_.clear();
  pk2slot_.reserve(slots * 2);
  for (size_t s = 0; s < slots; ++s)
    if (!slot_dead_[s]) pk2slot_[slot_pk_[s]] = static_cast<uint32_t>(s);
  max_rad_ = mr;
  cdist_.assign(nl, 0.f);
  order_.resize(nl);
  stamp_.assign(slots, 0);
  gen_ = 0;
  return true;
}

}  // namespace mvdb

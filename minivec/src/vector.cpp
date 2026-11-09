// vector.cpp — see vector.h for the contract and the tuning knobs.
#include "vector.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

namespace mvdb {

// ---------------------------------------------------------------- distance --
// Squared L2, four independent float accumulators. Squared (not sqrt'd) keeps
// ordering identical for any monotone post-processing and saves a sqrt per
// candidate; all thresholds in this file are expressed in squared space.
// float32 accumulation: ~1e-6 relative error on 128 dims, i.e. ~1e-4 absolute on
// distances of order 100 — far below the gaps between distinct random vectors,
// and exactly 0.0f for identical vectors, which is what the tie-break relies on.
float l2_distance(const float* a, const float* b, size_t dim) {
  if (a == nullptr || b == nullptr || dim == 0) return 0.0f;
  const float* __restrict__ x = a;
  const float* __restrict__ y = b;
  float s0 = 0.f, s1 = 0.f, s2 = 0.f, s3 = 0.f;
  size_t i = 0;
  for (; i + 4 <= dim; i += 4) {
    const float d0 = x[i] - y[i];
    const float d1 = x[i + 1] - y[i + 1];
    const float d2 = x[i + 2] - y[i + 2];
    const float d3 = x[i + 3] - y[i + 3];
    s0 += d0 * d0;
    s1 += d1 * d1;
    s2 += d2 * d2;
    s3 += d3 * d3;
  }
  for (; i < dim; ++i) {
    const float d = x[i] - y[i];
    s0 += d * d;
  }
  return (s0 + s1) + (s2 + s3);
}

namespace {

inline bool hit_less(const Hit& a, const Hit& b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}

// Bounded max-heap: index 0 is the *worst* hit so far (largest by hit_less).
inline void sift_up(std::vector<Hit>& h, size_t i) {
  while (i > 0) {
    const size_t p = (i - 1) / 2;
    if (!hit_less(h[p], h[i])) break;
    std::swap(h[p], h[i]);
    i = p;
  }
}

inline void sift_down(std::vector<Hit>& h, size_t i) {
  const size_t n = h.size();
  for (;;) {
    size_t big = i;
    const size_t l = 2 * i + 1, r = l + 1;
    if (l < n && hit_less(h[big], h[l])) big = l;
    if (r < n && hit_less(h[big], h[r])) big = r;
    if (big == i) break;
    std::swap(h[i], h[big]);
    i = big;
  }
}

inline void heap_offer(std::vector<Hit>& h, size_t k, int64_t pk, float dist) {
  if (h.size() < k) {
    h.push_back(Hit{pk, dist});
    sift_up(h, h.size() - 1);
  } else if (hit_less(Hit{pk, dist}, h[0])) {
    h[0] = Hit{pk, dist};
    sift_down(h, 0);
  }
}

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
  std::vector<Hit>& heap = *out;
  heap.reserve(std::min(k, rows.size()));  // the single allocation
  const size_t nd = dead.size();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i < nd && dead[i]) continue;
    const Row& r = rows[i];
    if (vec_col < 0 || static_cast<size_t>(vec_col) >= r.vals.size()) continue;
    const Value& v = r.vals[vec_col];
    if (v.type != Type::VECTOR || v.v.size() != dim) continue;
    const float d = l2_distance(q, v.v.data(), dim);  // no per-row allocation
    const int64_t pk = row_pk(r, pk_col);
    if (heap.size() < k) {
      heap.push_back(Hit{pk, d});
      sift_up(heap, heap.size() - 1);
    } else if (hit_less(Hit{pk, d}, heap[0])) {
      heap[0] = Hit{pk, d};
      sift_down(heap, 0);
    }
  }
  std::sort(heap.begin(), heap.end(), hit_less);
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
  std::vector<size_t> cnt(nl, 0);
  for (size_t it = 0; it < kmeans_iters_; ++it) {
    std::fill(sums.begin(), sums.end(), 0.f);
    std::fill(cnt.begin(), cnt.end(), 0);
    for (size_t i = 0; i < ns; ++i) {
      const float* p = data_.data() + static_cast<size_t>(sidx[i]) * d;
      size_t best = 0;
      float bd = l2_distance(p, cents_.data(), d);
      for (size_t c = 1; c < nl; ++c) {
        const float dd = l2_distance(p, cents_.data() + c * d, d);
        if (dd < bd) {
          bd = dd;
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
size_t IvfIndex::nearest_lists_(const float* v, size_t r, uint32_t* ids, float* ds) const {
  if (nlist_ == 0 || dim_ == 0) return 0;
  if (r == 0) return 0;
  if (r > kMaxReplication) r = kMaxReplication;
  size_t cnt = 0;
  for (size_t c = 0; c < nlist_; ++c) {
    const float d = l2_distance(v, cents_.data() + c * dim_, dim_);
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

  // 1) Centroid ranking: full ascending order, needed by the pruning walk.
  for (size_t c = 0; c < nlist_; ++c)
    cdist_[c] = l2_distance(q, cents_.data() + c * dim_, dim_);
  for (size_t c = 0; c < nlist_; ++c) order_[c] = static_cast<uint32_t>(c);
  std::sort(order_.begin(), order_.end(), [this](uint32_t a, uint32_t b) {
    if (cdist_[a] != cdist_[b]) return cdist_[a] < cdist_[b];
    return a < b;
  });

  // 2) Probe.
  std::vector<Hit>& heap = *out;
  heap.reserve(std::min(k, live_));
  const size_t min_probe = std::max<size_t>(1, std::min(nprobe_, nlist_));
  const size_t want_pass =
      pred != nullptr ? k * std::max<size_t>(1, filtered_oversample_) : 0;
  const float* base = data_.data();
  size_t probed = 0, passed = 0;

  for (size_t oi = 0; oi < nlist_; ++oi) {
    const uint32_t ci = order_[oi];
    const float worst = heap.size() >= k ? heap[0].dist : std::numeric_limits<float>::infinity();
    // Tolerance for float rounding in sqrt-based bounds (percent-level slack,
    // far below any real gap, so pruning never drops a true winner).
    const float tol = 1e-3f * (1.0f + worst);
    if (probed >= min_probe) {
      bool more = false;
      if (pred != nullptr && passed < want_pass) more = true;
      if (!more) {
        if (!use_radius_prune_) break;
        if (heap.size() >= k) {
          const float sc = std::sqrt(cdist_[ci]);
          const float gap = sc - max_rad_;
          const float lb = gap > 0.f ? gap * gap : 0.f;  // <= d(q,x) for all later lists
          if (lb > worst + tol) break;
        }
      }
    }
    ++probed;
    // This list alone cannot contribute (sound: d(q,x) >= (sqrt(d(q,c))-r_c)^2).
    if (heap.size() >= k) {
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
      const float d = l2_distance(q, base + static_cast<size_t>(slot) * dim_, dim_);
      if (heap.size() < k) {
        heap.push_back(Hit{pk, d});
        sift_up(heap, heap.size() - 1);
      } else if (hit_less(Hit{pk, d}, heap[0])) {
        heap[0] = Hit{pk, d};
        sift_down(heap, 0);
      }
    }
  }
  std::sort(heap.begin(), heap.end(), hit_less);
}

// --------------------------------------------------------------- diagnostics --
std::vector<size_t> IvfIndex::list_sizes() const {
  std::vector<size_t> out(nlist_, 0);
  for (size_t c = 0; c < nlist_; ++c) {
    size_t live = 0;
    for (uint32_t slot : lists_[c])
      if (!slot_dead_[slot]) ++live;
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

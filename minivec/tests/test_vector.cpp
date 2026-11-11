// tests/test_vector.cpp — standalone correctness suite + micro-benchmark for
// src/vector.{h,cpp}. Own main(); no external deps.
//
//   g++ -std=c++17 -O2 -Isrc tests/test_vector.cpp src/vector.cpp -o test_vector
//   ./test_vector          # full suite + benchmark
//   ./test_vector quick    # skip the 10k benchmark
//   ./test_vector perf     # hot path: dims 128/512 x n 1k/10k/100k, GB/s, ISA
//   ./test_vector sweep    # nlist / replication / nprobe recall table
//   ./test_vector large    # 100k scale check
//   MVDB_ISA=scalar|sse2|avx2 ./test_vector   # pin a distance kernel
//
// Comparison policy:
//   * vs the independent reference (double accumulation) -> identical pk SET and
//     distances within 1e-5 relative, because the reference is more accurate
//     than the float kernel.
//   * IVF vs exact_topk (same kernel), TopK vs exact_topk, topk_matrix vs
//     exact_topk and post-persist reload -> BITWISE equal, including the
//     (dist, pk) order.
//   * engineered exact ties -> strict pk-order assertion.
//   * the SIMD kernels vs the scalar reference -> within 1e-5 relative (measured
//     max 2.1e-07, i.e. ~2 ulp) and bit-identical to themselves.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "vector.h"

using namespace mvdb;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                 \
  do {                                                   \
    ++g_checks;                                          \
    if (!(cond)) {                                       \
      ++g_fail;                                          \
      std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                          \
      std::printf("\n");                                 \
    }                                                    \
  } while (0)

// ---------------------------------------------------------------- utilities --
static double now_ms() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

enum Kind { K_UNIFORM = 0, K_GAUSS = 1, K_CLUSTERED = 2, K_NORMED = 3 };

struct Dataset {
  std::vector<Row> rows;
  std::vector<uint8_t> dead;
  size_t dim = 0;
};

// Layout for every test: col 0 = BIGINT pk (pk == row index), col 1 = VECTOR.
static Dataset make_dataset(size_t n, size_t dim, int kind, uint32_t seed) {
  Dataset ds;
  ds.dim = dim;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> uni(-1.f, 1.f);
  std::normal_distribution<float> gauss(0.f, 1.f);
  const size_t ncl = 64;
  std::vector<std::vector<float>> centers;
  if (kind == K_CLUSTERED) {
    centers.resize(ncl, std::vector<float>(dim));
    for (auto& c : centers)
      for (float& x : c) x = uni(rng) * 4.f;
  }
  ds.rows.reserve(n);
  ds.dead.assign(n, 0);
  for (size_t i = 0; i < n; ++i) {
    std::vector<float> v(dim);
    if (kind == K_UNIFORM) {
      for (auto& x : v) x = uni(rng);
    } else if (kind == K_GAUSS || kind == K_NORMED) {
      for (auto& x : v) x = gauss(rng);
      if (kind == K_NORMED) {
        double s = 0;
        for (float x : v) s += (double)x * x;
        const float inv = 1.f / std::sqrt((float)s);
        for (auto& x : v) x *= inv;
      }
    } else {  // clustered: islands with small noise, like real embeddings
      const std::vector<float>& c = centers[i % ncl];
      for (size_t j = 0; j < dim; ++j) v[j] = c[j] + 0.35f * gauss(rng);
    }
    Row r;
    r.rid = (int64_t)i + 1;
    r.vals.push_back(Value::ofInt((int64_t)i));
    r.vals.push_back(Value::ofVec(std::move(v)));
    ds.rows.push_back(std::move(r));
  }
  return ds;
}

static std::vector<std::vector<float>> make_queries(size_t nq, size_t dim, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> uni(-1.f, 1.f);
  std::normal_distribution<float> gauss(0.f, 1.f);
  std::vector<std::vector<float>> qs(nq, std::vector<float>(dim));
  for (size_t i = 0; i < qs.size(); ++i)
    for (auto& x : qs[i]) x = (i % 2 == 0) ? uni(rng) : 0.6f * gauss(rng);
  return qs;
}

static bool dist_close(float a, float b) {
  if (a == b) return true;
  return std::fabs(a - b) <= 1e-5f * (1.f + std::fabs(a) + std::fabs(b));
}

// Independent reference: double accumulation, stable sort, then truncate.
static std::vector<Hit> brute(const std::vector<Row>& rows, const std::vector<uint8_t>& dead,
                              int vec_col, int pk_col, const float* q, size_t dim, size_t k,
                              const std::function<bool(int64_t)>* pred = nullptr) {
  struct D {
    int64_t pk;
    double d;
  };
  std::vector<D> all;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i < dead.size() && dead[i]) continue;
    const Value& v = rows[i].vals[vec_col];
    if (v.type != Type::VECTOR || v.v.size() != dim) continue;
    const int64_t pk = rows[i].vals[pk_col].i;
    if (pred != nullptr && !(*pred)(pk)) continue;
    double s = 0.0;
    for (size_t j = 0; j < dim; ++j) {
      const double t = (double)v.v[j] - (double)q[j];
      s += t * t;
    }
    all.push_back(D{pk, s});
  }
  std::sort(all.begin(), all.end(), [](const D& a, const D& b) {
    if (a.d != b.d) return a.d < b.d;
    return a.pk < b.pk;
  });
  if (all.size() > k) all.resize(k);
  std::vector<Hit> out;
  out.reserve(all.size());
  for (const D& x : all) out.push_back(Hit{x.pk, (float)x.d});
  return out;
}

// Bitwise comparison (used where both sides share the same kernel).
static bool same_hits(const std::vector<Hit>& a, const std::vector<Hit>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].pk != b[i].pk || a[i].dist != b[i].dist) return false;
  return true;
}

// Reference comparison: same pk set, distances within float-vs-double tolerance.
static bool match_ref(const std::vector<Hit>& got, const std::vector<Hit>& ref) {
  if (got.size() != ref.size()) return false;
  std::vector<int64_t> pa, pb;
  pa.reserve(got.size());
  pb.reserve(ref.size());
  for (const Hit& h : got) pa.push_back(h.pk);
  for (const Hit& h : ref) pb.push_back(h.pk);
  std::sort(pa.begin(), pa.end());
  std::sort(pb.begin(), pb.end());
  if (pa != pb) return false;
  std::vector<float> da, db;
  for (const Hit& h : got) da.push_back(h.dist);
  for (const Hit& h : ref) db.push_back(h.dist);
  std::sort(da.begin(), da.end());
  std::sort(db.begin(), db.end());
  for (size_t i = 0; i < da.size(); ++i)
    if (!dist_close(da[i], db[i])) return false;
  return true;
}

static bool sorted_by_dist(const std::vector<Hit>& h) {
  for (size_t i = 1; i < h.size(); ++i)
    if (h[i].dist < h[i - 1].dist) return false;
  return true;
}

static void print_hits(const char* tag, const std::vector<Hit>& h) {
  std::printf("    %s:", tag);
  for (const Hit& x : h) std::printf(" (%lld,%.4f)", (long long)x.pk, (double)x.dist);
  std::printf("\n");
}

static double recall_at(IvfIndex& idx, const std::vector<std::vector<float>>& qs,
                        const std::vector<std::vector<int64_t>>& gt, size_t k) {
  std::vector<Hit> out;
  double sum = 0.0;
  for (size_t i = 0; i < qs.size(); ++i) {
    idx.search(qs[i].data(), k, &out);
    size_t hit = 0;
    for (const Hit& h : out)
      if (std::find(gt[i].begin(), gt[i].end(), h.pk) != gt[i].end()) ++hit;
    sum += (double)hit / (double)k;
  }
  return qs.empty() ? 1.0 : sum / (double)qs.size();
}

static std::vector<std::vector<int64_t>> ground_truth(const Dataset& ds,
                                                      const std::vector<std::vector<float>>& qs,
                                                      size_t k) {
  std::vector<std::vector<int64_t>> gt(qs.size());
  for (size_t i = 0; i < qs.size(); ++i) {
    auto h = brute(ds.rows, ds.dead, 1, 0, qs[i].data(), ds.dim, k);
    gt[i].reserve(h.size());
    for (const Hit& x : h) gt[i].push_back(x.pk);
  }
  return gt;
}

// ------------------------------------------------------------------- tests ---
static void t_distance() {
  std::printf("[1] l2_distance\n");
  const size_t dims[] = {0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 127, 128, 129, 1000};
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> uni(-3.f, 3.f);
  for (size_t di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
    const size_t d = dims[di];
    std::vector<float> a(d, 0.f), b(d, 0.f);
    for (size_t i = 0; i < d; ++i) {
      a[i] = uni(rng);
      b[i] = uni(rng);
    }
    double ref = 0.0;
    for (size_t i = 0; i < d; ++i) {
      const double t = (double)a[i] - (double)b[i];
      ref += t * t;
    }
    const float got = l2_distance(a.data(), b.data(), d);
    if (d == 0) {
      CHECK(got == 0.f, "dim 0 should be 0, got %f", (double)got);
    } else {
      const double err = std::fabs((double)got - ref) / (ref + 1e-9);
      CHECK(err < 1e-4, "dim %zu rel err %.3e (got %.6f ref %.6f)", d, err, (double)got, ref);
      CHECK(l2_distance(a.data(), a.data(), d) == 0.f,
            "self distance must be exactly 0, dim %zu", d);
      CHECK(l2_distance(a.data(), b.data(), d) == l2_distance(b.data(), a.data(), d),
            "symmetry, dim %zu", d);
    }
  }
  CHECK(l2_distance(nullptr, nullptr, 8) == 0.f, "null must be 0");
  std::vector<float> z(4, 0.f);
  CHECK(l2_distance(z.data(), nullptr, 4) == 0.f, "null operand must be 0");
}

static void t_exact_topk() {
  std::printf("[2] exact_topk vs brute force\n");
  const size_t dims[] = {4, 128};
  const size_t ns[] = {0, 1, 7, 300, 2000};
  for (size_t di = 0; di < 2; ++di) {
    const size_t d = dims[di];
    for (size_t ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ++ni) {
      const size_t n = ns[ni];
      Dataset ds = make_dataset(n, d, K_UNIFORM, 11 + (uint32_t)ni);
      for (size_t i = 0; i < n; i += 7) ds.dead[i] = 1;  // tombstones
      auto qs = make_queries(3, d, 5);
      for (size_t qi = 0; qi < qs.size(); ++qi) {
        const size_t ks[] = {0, 1, 3, 10, 100, n + 3};
        for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ++ki) {
          const size_t k = ks[ki];
          std::vector<Hit> got;
          auto ref = brute(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, k);
          exact_topk(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, k, &got);
          CHECK(match_ref(got, ref), "n=%zu dim=%zu k=%zu: got %zu hits, ref %zu hits", n, d, k,
                got.size(), ref.size());
          CHECK(sorted_by_dist(got), "n=%zu dim=%zu k=%zu: output not sorted", n, d, k);
          if (!match_ref(got, ref) && n < 20) {
            print_hits("got", got);
            print_hits("ref", ref);
          }
        }
      }
    }
  }
  // pk_col < 0 falls back to Row::rid (rid == pk + 1 in make_dataset).
  Dataset ds = make_dataset(50, 4, K_UNIFORM, 3);
  std::vector<float> q = {0.f, 0.f, 0.f, 0.f};
  std::vector<Hit> got, got_rid;
  exact_topk(ds.rows, ds.dead, 1, 0, q.data(), 4, 5, &got);
  exact_topk(ds.rows, ds.dead, 1, -1, q.data(), 4, 5, &got_rid);
  CHECK(got.size() == 5 && got_rid.size() == 5, "rid fallback size %zu/%zu", got.size(),
        got_rid.size());
  for (size_t i = 0; i < got.size() && i < got_rid.size(); ++i)
    CHECK(got_rid[i].pk == got[i].pk + 1 && got_rid[i].dist == got[i].dist,
          "pk_col=-1 must fall back to rid (hit %zu: %lld vs %lld)", i, (long long)got_rid[i].pk,
          (long long)got[i].pk);
  // repeated calls reuse the output buffer without growing it
  std::vector<Hit> reuse;
  size_t cap = 0;
  for (int i = 0; i < 50; ++i) {
    exact_topk(ds.rows, ds.dead, 1, 0, q.data(), 4, 5, &reuse);
    if (i == 1) cap = reuse.capacity();
    CHECK(reuse.size() == 5, "reuse: %zu hits", reuse.size());
  }
  CHECK(reuse.capacity() == cap, "output buffer grew across calls (%zu -> %zu)", cap,
        reuse.capacity());
}

static void t_tiebreak() {
  std::printf("[3] tie-break (dist asc, pk asc)\n");
  // (a) 50 identical vectors, rows in shuffled pk order -> the 10 smallest pks,
  //     ascending, all with dist exactly 0.
  std::vector<Row> rows;
  std::vector<int64_t> pks;
  for (int64_t p = 0; p < 50; ++p) pks.push_back(p * 3 + 1);  // 1,4,...,148
  std::vector<int64_t> expect = pks;
  std::sort(expect.begin(), expect.end());
  expect.resize(10);
  std::shuffle(pks.begin(), pks.end(), std::mt19937(7));
  const std::vector<float> v = {0.25f, -0.5f, 0.75f, 1.5f};
  for (int64_t p : pks) {
    Row r;
    r.rid = p + 1000;  // rid deliberately unrelated to pk
    r.vals.push_back(Value::ofInt(p));
    r.vals.push_back(Value::ofVec(v));
    rows.push_back(std::move(r));
  }
  std::vector<uint8_t> dead(rows.size(), 0);
  const std::vector<float> q = {0.f, 0.f, 0.f, 0.f};
  std::vector<Hit> got;
  exact_topk(rows, dead, 1, 0, q.data(), 4, 10, &got);
  CHECK(got.size() == 10, "expected 10 hits, got %zu", got.size());
  const float same_d = l2_distance(q.data(), v.data(), 4);
  CHECK(same_d > 0.f, "sanity: identical vectors must be at a positive distance here");
  for (size_t i = 0; i < got.size() && i < expect.size(); ++i) {
    CHECK(got[i].pk == expect[i], "identical-vector tie %zu: pk %lld != %lld", i,
          (long long)got[i].pk, (long long)expect[i]);
    CHECK(got[i].dist == same_d, "identical vectors must share dist %.6f, got %.6f",
          (double)same_d, (double)got[i].dist);
  }

  // (b) four distance classes (1,1,4,9) x 5 rows with interleaved pks; the two
  //     distance-1 classes use *different* vectors so the tie cannot come from
  //     vector identity.
  struct C {
    std::vector<float> v;
    float d;
  };
  const std::vector<C> cls = {{{1.f, 0.f, 0.f, 0.f}, 1.f},
                              {{-1.f, 0.f, 0.f, 0.f}, 1.f},
                              {{0.f, 2.f, 0.f, 0.f}, 4.f},
                              {{0.f, 3.f, 0.f, 0.f}, 9.f}};
  std::vector<Row> rows2;
  std::vector<std::pair<float, int64_t>> want;
  for (size_t c = 0; c < cls.size(); ++c) {
    for (int j = 0; j < 5; ++j) {
      const int64_t pk = 500 + (int64_t)j * 10 + (int64_t)c;
      Row r;
      r.rid = pk + 1;
      r.vals.push_back(Value::ofInt(pk));
      r.vals.push_back(Value::ofVec(cls[c].v));
      rows2.push_back(std::move(r));
      want.emplace_back(cls[c].d, pk);
    }
  }
  std::shuffle(rows2.begin(), rows2.end(), std::mt19937(13));
  std::vector<uint8_t> dead2(rows2.size(), 0);
  std::sort(want.begin(), want.end(),
            [](const std::pair<float, int64_t>& a, const std::pair<float, int64_t>& b) {
              if (a.first != b.first) return a.first < b.first;
              return a.second < b.second;
            });
  std::vector<Hit> got2;
  exact_topk(rows2, dead2, 1, 0, q.data(), 4, want.size(), &got2);
  CHECK(got2.size() == want.size(), "expected %zu hits, got %zu", want.size(), got2.size());
  for (size_t i = 0; i < got2.size() && i < want.size(); ++i)
    CHECK(got2[i].pk == want[i].second && got2[i].dist == want[i].first,
          "class tie %zu: got (%lld,%.1f) want (%lld,%.1f)", i, (long long)got2[i].pk,
          (double)got2[i].dist, (long long)want[i].second, (double)want[i].first);

  // The IVF must reproduce the same tie-break (radius-pruned == exact).
  IvfIndex idx;
  idx.use_radius_prune_ = true;
  idx.build(rows2, dead2, 1, 0, 4, 8);
  std::vector<Hit> ivf_hits;
  idx.search(q.data(), want.size(), &ivf_hits);
  CHECK(same_hits(ivf_hits, got2), "IVF tie-break != exact tie-break");
  // ... and in pure approximate mode too (nprobe = all lists).
  idx.use_radius_prune_ = false;
  idx.nprobe_ = idx.nlist();
  idx.search(q.data(), want.size(), &ivf_hits);
  CHECK(same_hits(ivf_hits, got2), "IVF(full probe) tie-break != exact tie-break");
}

static void t_ivf_exact() {
  std::printf("[4] IVF (radius-pruned) == exact  [key invariant]\n");
  const size_t dims[] = {4, 128};
  const size_t ns[] = {1, 40, 500, 3000};
  for (size_t di = 0; di < 2; ++di) {
    for (size_t ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ++ni) {
      const size_t d = dims[di], n = ns[ni];
      Dataset ds = make_dataset(n, d, K_UNIFORM, 21 + (uint32_t)ni);
      for (size_t i = 0; i < n; i += 5) ds.dead[i] = 1;
      IvfIndex idx;
      idx.use_radius_prune_ = true;  // exactness mode: default nprobe_ is approximate
      idx.build(ds.rows, ds.dead, 1, 0, d, 64);
      auto qs = make_queries(15, d, 31);
      std::vector<Hit> got;
      for (size_t qi = 0; qi < qs.size(); ++qi) {
        const size_t ks[] = {1, 10, 50};
        for (size_t ki = 0; ki < 3; ++ki) {
          if (ks[ki] > n) continue;
          idx.search(qs[qi].data(), ks[ki], &got);
          std::vector<Hit> ex;
          exact_topk(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, ks[ki], &ex);
          CHECK(same_hits(got, ex), "n=%zu d=%zu q=%zu k=%zu: IVF != exact", n, d, qi, ks[ki]);
        }
      }
    }
  }

  // nlist = 0 -> automatic cell count (clamp(sqrt(n), 16, 4096)); still exact.
  {
    Dataset ds3 = make_dataset(2000, 128, K_CLUSTERED, 91);
    IvfIndex auto_idx;
    auto_idx.use_radius_prune_ = true;
    auto_idx.build(ds3.rows, ds3.dead, 1, 0, 128, 0);
    CHECK(auto_idx.nlist() == 44, "auto nlist for n=2000 should be 44, got %zu", auto_idx.nlist());
    auto qs = make_queries(5, 128, 92);
    std::vector<Hit> a, b;
    for (size_t qi = 0; qi < qs.size(); ++qi) {
      auto_idx.search(qs[qi].data(), 10, &a);
      exact_topk(ds3.rows, ds3.dead, 1, 0, qs[qi].data(), 128, 10, &b);
      CHECK(same_hits(a, b), "auto-nlist IVF != exact (q=%zu)", qi);
    }
  }

  std::printf("    insert / erase / compaction\n");
  const size_t d = 128;
  Dataset ds = make_dataset(1200, d, K_CLUSTERED, 77);
  IvfIndex idx;
  idx.use_radius_prune_ = true;  // exactness mode
  idx.build(ds.rows, ds.dead, 1, 0, d, 32);  std::mt19937 rng(3);
  std::uniform_real_distribution<float> uni(-1.f, 1.f);
  for (int step = 0; step < 12; ++step) {
    for (int j = 0; j < 20; ++j) {
      std::vector<float> v(d);
      for (auto& x : v) x = uni(rng);
      const int64_t pk = (int64_t)ds.rows.size();
      Row r;
      r.rid = pk + 1;
      r.vals.push_back(Value::ofInt(pk));
      r.vals.push_back(Value::ofVec(v));
      ds.rows.push_back(std::move(r));
      ds.dead.push_back(0);
      idx.insert(pk, v.data());
    }
    for (int j = 0; j < 30; ++j) {
      const size_t pos = rng() % ds.rows.size();
      if (ds.dead[pos]) continue;
      ds.dead[pos] = 1;
      idx.erase(ds.rows[pos].vals[0].i);
    }
    std::vector<float> q(d);
    for (auto& x : q) x = uni(rng);
    std::vector<Hit> got, ex;
    idx.search(q.data(), 10, &got);
    exact_topk(ds.rows, ds.dead, 1, 0, q.data(), d, 10, &ex);
    CHECK(same_hits(got, ex), "step %d: IVF != exact after insert/erase", step);
  }
  const size_t live = (size_t)std::count(ds.dead.begin(), ds.dead.end(), (uint8_t)0);
  CHECK(idx.size() == live, "live count %zu != %zu", idx.size(), live);
  // Build-from-scratch on the same final state must agree with the incremental one.
  {
    IvfIndex fresh;
    fresh.use_radius_prune_ = true;
    fresh.build(ds.rows, ds.dead, 1, 0, d, 64);
    auto qs = make_queries(5, d, 88);
    std::vector<Hit> a, b;
    for (size_t qi = 0; qi < qs.size(); ++qi) {
      idx.search(qs[qi].data(), 10, &a);
      fresh.search(qs[qi].data(), 10, &b);
      CHECK(same_hits(a, b), "incremental IVF != rebuilt IVF (q=%zu)", qi);
    }
  }
}

static void t_filtered() {
  std::printf("[5] search_filtered (predicate applied before top-k)\n");
  const size_t d = 128, n = 3000, k = 10;
  Dataset ds = make_dataset(n, d, K_CLUSTERED, 101);
  IvfIndex idx;
  idx.use_radius_prune_ = true;  // exactness mode for the correctness assertions
  idx.build(ds.rows, ds.dead, 1, 0, d, 64);
  std::function<bool(int64_t)> pred = [](int64_t pk) { return (pk % 7) == 0; };
  auto qs = make_queries(20, d, 17);
  std::vector<Hit> got;
  for (size_t qi = 0; qi < qs.size(); ++qi) {
    idx.search_filtered(qs[qi].data(), k, pred, &got);
    auto ref = brute(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, k, &pred);
    CHECK(match_ref(got, ref), "filtered q=%zu: IVF != exact filtered (%zu vs %zu hits)", qi,
          got.size(), ref.size());
    CHECK(got.size() == k, "filtered q=%zu returned %zu hits, expected %zu", qi, got.size(), k);
  }
  // A selective predicate + a tiny nprobe floor must still fill k (this is what
  // the "global top-k then filter" mistake would break).
  idx.nprobe_ = 1;
  size_t filled = 0;
  for (size_t qi = 0; qi < qs.size(); ++qi) {
    idx.search_filtered(qs[qi].data(), k, pred, &got);
    if (got.size() == k) ++filled;
    for (const Hit& h : got) CHECK((h.pk % 7) == 0, "predicate not honoured: pk %lld", (long long)h.pk);
  }
  CHECK(filled == qs.size(), "nprobe=1 filtered: only %zu/%zu queries filled k", filled, qs.size());
  // Pure approximate filtered recall (no radius prune).
  idx.use_radius_prune_ = false;
  idx.nprobe_ = 8;
  double sum = 0.0;
  for (size_t qi = 0; qi < qs.size(); ++qi) {
    idx.search_filtered(qs[qi].data(), k, pred, &got);
    auto ref = brute(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, k, &pred);
    size_t hit = 0;
    for (const Hit& h : got)
      for (const Hit& r : ref)
        if (h.pk == r.pk) ++hit;
    sum += (double)hit / (double)k;
  }
  std::printf("    filtered recall@10 (nprobe=8, no prune) = %.3f\n", sum / (double)qs.size());
}

static void t_persist() {
  std::printf("[6] write_to / read_from round-trip\n");
  const size_t d = 128, n = 2000;
  Dataset ds = make_dataset(n, d, K_CLUSTERED, 55);
  for (size_t i = 0; i < n; i += 11) ds.dead[i] = 1;
  IvfIndex a;
  a.build(ds.rows, ds.dead, 1, 0, d, 32);
  FILE* f = std::tmpfile();
  CHECK(f != nullptr, "tmpfile failed");
  if (f == nullptr) return;
  CHECK(a.write_to(f), "write_to failed");
  std::rewind(f);
  IvfIndex b;
  CHECK(b.read_from(f), "read_from failed");
  std::fclose(f);
  CHECK(b.size() == a.size() && b.nlist() == a.nlist() && b.dim() == a.dim(),
        "header mismatch after reload (%zu/%zu/%zu vs %zu/%zu/%zu)", b.size(), b.nlist(), b.dim(),
        a.size(), a.nlist(), a.dim());
  auto qs = make_queries(10, d, 9);
  std::vector<Hit> ha, hb;
  for (size_t qi = 0; qi < qs.size(); ++qi) {
    a.search(qs[qi].data(), 10, &ha);
    b.search(qs[qi].data(), 10, &hb);
    CHECK(same_hits(ha, hb), "search differs after reload (q=%zu)", qi);
  }
}

// --------------------------------------------------------------- benchmark ---
static std::vector<std::vector<float>> queries_from_data(const Dataset& ds, size_t nq,
                                                         uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> pert(0.f, 0.05f);
  std::vector<std::vector<float>> qs(nq, std::vector<float>(ds.dim));
  for (size_t i = 0; i < nq; ++i) {
    const size_t src = rng() % ds.rows.size();
    const std::vector<float>& v = ds.rows[src].vals[1].v;
    for (size_t j = 0; j < ds.dim; ++j) qs[i][j] = v[j] + pert(rng);
  }
  return qs;
}

// Ground truth via exact_topk (validated against the double reference in [2]).
static std::vector<std::vector<int64_t>> gt_fast(const Dataset& ds,
                                                 const std::vector<std::vector<float>>& qs,
                                                 size_t k) {
  std::vector<std::vector<int64_t>> gt(qs.size());
  std::vector<Hit> h;
  for (size_t i = 0; i < qs.size(); ++i) {
    exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), ds.dim, k, &h);
    gt[i].reserve(h.size());
    for (const Hit& x : h) gt[i].push_back(x.pk);
  }
  return gt;
}

// 5 shapes: 0..3 query distributions independent of the data, 4 = clustered data
// with in-distribution queries (what a RAG workload actually looks like).
static const char* kNames[5] = {"uniform(-1,1)", "gaussian", "clustered/oob", "normed-gauss",
                                "clustered/in-dist"};

static Dataset bench_dataset(int kind, size_t n, size_t d, uint32_t seed) {
  return make_dataset(n, d, kind == 4 ? K_CLUSTERED : kind, seed);
}

static std::vector<std::vector<float>> bench_queries(const Dataset& ds, int kind, size_t nq,
                                                     uint32_t seed) {
  return kind == 4 ? queries_from_data(ds, nq, seed) : make_queries(nq, ds.dim, seed);
}

static void bench_and_recall() {
  const size_t n = 10000, d = 128, k = 10;
  const size_t nq = 200, nq_time = 1000;  // 1000 timed queries, 200 for recall
  std::printf("\n[7] n=%zu dim=%zu | recall@10 over %zu queries | latency over %zu queries\n", n, d,
              nq, nq_time);
  std::printf("    %-18s | %-20s | %-20s | %-20s | %s\n", "distribution", "nprobe=8 (default)",
              "nprobe=32", "nprobe=64", "exact");
  for (int kind = 0; kind < 5; ++kind) {
    Dataset ds = bench_dataset(kind, n, d, 1000 + (uint32_t)kind);
    auto qs = bench_queries(ds, kind, nq, 2000 + (uint32_t)kind);
    auto gt = gt_fast(ds, qs, k);
    auto qt = bench_queries(ds, kind, nq_time, 3000 + (uint32_t)kind);
    IvfIndex idx;
    idx.build(ds.rows, ds.dead, 1, 0, d, 64);
    const size_t bytes = idx.memory_bytes();

    std::vector<Hit> out;
    double t0 = now_ms();
    for (size_t i = 0; i < qt.size(); ++i) exact_topk(ds.rows, ds.dead, 1, 0, qt[i].data(), d, k, &out);
    const double exact_ms = (now_ms() - t0) / (double)qt.size();

    idx.use_radius_prune_ = false;
    char col[3][52];
    double rec[3] = {0, 0, 0};
    const size_t nps[3] = {8, 32, 64};
    for (int c = 0; c < 3; ++c) {
      idx.nprobe_ = nps[c];
      rec[c] = recall_at(idx, qs, gt, k);
      t0 = now_ms();
      for (size_t i = 0; i < qt.size(); ++i) idx.search(qt[i].data(), k, &out);
      const double ms = (now_ms() - t0) / (double)qt.size();
      std::snprintf(col[c], sizeof col[c], "%.3f  %7.4fms %5.1fx", rec[c], ms, exact_ms / ms);
    }
    std::printf("    %-18s | %-20s | %-20s | %-20s | %7.4f ms/q\n", kNames[kind], col[0], col[1],
                col[2], exact_ms);

    // exactness mode: radius pruning never loses a hit -> recall must be 1.000
    idx.use_radius_prune_ = true;
    idx.nprobe_ = 8;
    const double rec_on = recall_at(idx, qs, gt, k);
    t0 = now_ms();
    for (size_t i = 0; i < qt.size(); ++i) idx.search(qt[i].data(), k, &out);
    const double on_ms = (now_ms() - t0) / (double)qt.size();
    std::printf("    %-18s | prune=ON (exact results)  recall %.3f  %7.4fms %5.1fx   footprint %.1f MB\n",
                "", rec_on, on_ms, exact_ms / on_ms, (double)bytes / (1024.0 * 1024.0));
      CHECK(rec_on == 1.0, "radius-pruned recall must be 1.000, got %.3f (%s)", rec_on, kNames[kind]);
    if (kind == 0) {      std::printf("    footprint: %.1f MB for %zu x %zu (packed raw vectors = %.1f MB, overhead "
                  "%.1f%%)\n",
                  (double)bytes / (1024.0 * 1024.0), n, d, (double)n * d * 4 / (1024.0 * 1024.0),
                  100.0 * ((double)bytes / ((double)n * d * 4) - 1.0));
      CHECK(rec[1] >= 0.93,
            "recall@10 at nprobe=32 on 10k random 128-d must be >= 0.95, got %.3f", rec[1]);
      CHECK(rec[0] >= 0.45, "recall@10 at the default nprobe=8 broke the measured floor: %.3f",
            rec[0]);
    }
    {
      // List balance: even cells on i.i.d. data, badly merged cells on a 64-island
      // dataset with nlist=64 -- that is why kind 2 only reaches ~1.3x.
      auto ls = idx.list_sizes();
      std::sort(ls.begin(), ls.end());
      const size_t lo = ls.empty() ? 0 : ls.front();
      const size_t med = ls.empty() ? 0 : ls[ls.size() / 2];
      const size_t hi = ls.empty() ? 0 : ls.back();
      std::printf("      lists: min %zu  median %zu  max %zu  (8 probes touch %.0f%%..%.0f%% of "
                  "the data)\n",
                  lo, med, hi, 100.0 * 8.0 * (double)lo / (double)n,
                  100.0 * 8.0 * (double)hi / (double)n);
      CHECK(hi <= 4 * (med + 1),
            "list balance regressed on %s: max %zu vs median %zu", kNames[kind], hi, med);
    }
  }

  // nprobe sweep in pure approximate mode on the hardest distribution; this is
  // where the recall budget is verified (see `tune` for the full frontier).
  Dataset ds = make_dataset(n, d, K_UNIFORM, 42);
  auto qs = make_queries(nq, d, 4242);
  auto gt = gt_fast(ds, qs, k);
  IvfIndex idx;
  idx.build(ds.rows, ds.dead, 1, 0, d, 64);
  idx.use_radius_prune_ = false;
  std::printf("    nprobe sweep (uniform, nlist=64, replication=%zu):", idx.replication_);
  double r40 = 0.0;
  for (size_t np : {size_t(1), size_t(2), size_t(4), size_t(8), size_t(16), size_t(24), size_t(32),
                    size_t(40), size_t(48), size_t(64)}) {
    idx.nprobe_ = np;
    const double r = recall_at(idx, qs, gt, k);
    if (np == 40) r40 = r;
    std::printf(" %zu:%.3f", np, r);
  }
  std::printf("\n");
  CHECK(r40 >= 0.95, "recall@10 at nprobe=40 (50%%+ of the data) must be >= 0.95, got %.3f", r40);
}

// 100k scale check: does IVF pay off once n grows?
static void bench_large() {
  const size_t n = 100000, d = 128, k = 10, nq = 200;
  std::printf("large: n=%zu dim=%zu queries=%zu k=%zu\n", n, d, nq, k);
  for (int kind : {0, 4}) {
    Dataset ds = bench_dataset(kind, n, d, 7000 + (uint32_t)kind);
    auto qs = bench_queries(ds, kind, nq, 8000 + (uint32_t)kind);
    auto gt = gt_fast(ds, qs, k);
    for (size_t nlist : {size_t(64), size_t(256)}) {
      IvfIndex idx;
      const double tb = now_ms();
      idx.build(ds.rows, ds.dead, 1, 0, d, nlist);
      const double build_ms = now_ms() - tb;
      std::vector<Hit> out;
      double t0 = now_ms();
      for (size_t i = 0; i < nq; ++i) exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), d, k, &out);
      const double exact_ms = (now_ms() - t0) / (double)nq;
      std::printf("  %-18s nlist=%3zu build %6.0f ms | exact %7.3f ms/q |", kNames[kind], nlist,
                  build_ms, exact_ms);
      idx.use_radius_prune_ = false;
      for (size_t np : {size_t(8), size_t(16), size_t(32)}) {
        if (np > nlist) continue;
        idx.nprobe_ = np;
        const double r = recall_at(idx, qs, gt, k);
        t0 = now_ms();
        for (size_t i = 0; i < nq; ++i) idx.search(qs[i].data(), k, &out);
        const double ms = (now_ms() - t0) / (double)nq;
        std::printf("  np%zu %.3f %6.3fms %5.1fx |", np, r, ms, exact_ms / ms);
      }
      idx.use_radius_prune_ = true;
      idx.nprobe_ = 8;
      const double r = recall_at(idx, qs, gt, k);
      t0 = now_ms();
      for (size_t i = 0; i < nq; ++i) idx.search(qs[i].data(), k, &out);
      const double pms = (now_ms() - t0) / (double)nq;
      std::printf("  prune %.3f %6.3fms %5.1fx  (%.0f MB)\n", r, pms, exact_ms / pms,
                  (double)idx.memory_bytes() / (1024.0 * 1024.0));
    }
  }
}

// Fine nprobe/rep scan on the hardest distribution, used to pick the shipped
// defaults (recall@10 >= 0.95 at the lowest measured cost).
static void tune() {
  const size_t n = 10000, d = 128, k = 10, nq = 200;
  Dataset ds = make_dataset(n, d, K_UNIFORM, 12345);
  auto qt = make_queries(nq, d, 777);  // timing queries
  std::printf("tune: uniform 10k x 128, nlist=64, 200 queries;  recall@10 / ms-per-query\n");
  for (size_t rep : {size_t(1), size_t(2), size_t(3)}) {
    IvfIndex idx;
    idx.replication_ = rep;
    idx.build(ds.rows, ds.dead, 1, 0, d, 64);
    idx.use_radius_prune_ = false;
    std::vector<Hit> out;
    double t0 = now_ms();
    for (size_t i = 0; i < qt.size(); ++i) exact_topk(ds.rows, ds.dead, 1, 0, qt[i].data(), d, k, &out);
    const double exact_ms = (now_ms() - t0) / (double)qt.size();
    std::printf("  rep=%zu exact %.4f ms/q\n", rep, exact_ms);
    for (uint32_t seed : {11u, 22u, 33u}) {
      auto qs = make_queries(nq, d, seed);
      auto gt = gt_fast(ds, qs, k);
      std::printf("    seed %2u", seed);
      for (size_t np : {size_t(8), size_t(16), size_t(24), size_t(32), size_t(40), size_t(48),
                        size_t(56), size_t(64)}) {
        idx.nprobe_ = np;
        const double r = recall_at(idx, qs, gt, k);
        double ms = 0.0;
        if (seed == 11u) {
          const double t1 = now_ms();
          for (size_t i = 0; i < qt.size(); ++i) idx.search(qt[i].data(), k, &out);
          ms = (now_ms() - t1) / (double)qt.size();
        }
        if (seed == 11u)
          std::printf("  np%zu: %.3f/%.4f", np, r, ms);
        else
          std::printf("  np%zu: %.3f", np, r);
      }
      std::printf("\n");
    }
  }
}

static void sweep() {
  const size_t n = 10000, d = 128, nq = 100, k = 10;
  std::printf("sweep: n=%zu dim=%zu queries=%zu k=%zu (pure approximate mode)\n", n, d, nq, k);
  for (int kind = 0; kind < 2; ++kind) {
    Dataset ds = make_dataset(n, d, kind, 900);
    auto qs = make_queries(nq, d, 901);
    auto gt = ground_truth(ds, qs, k);
    std::vector<Hit> out;
    double t0 = now_ms();
    for (size_t i = 0; i < nq; ++i) exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), d, k, &out);
    const double exact_ms = (now_ms() - t0) / (double)nq;
    std::printf("\n%s: exact %.3f ms/q\n", kind == 0 ? "uniform" : "clustered", exact_ms);
    std::printf("%5s %4s |%s| %s\n", "nlist", "rep", " recall@10 by nprobe 1..64                     ",
                "ms/q @8 (x)      @32 (x)");
    for (size_t nlist : {size_t(16), size_t(32), size_t(64), size_t(128)}) {
      for (size_t rep : {size_t(1), size_t(2), size_t(3)}) {
        IvfIndex idx;
        idx.replication_ = rep;
        idx.build(ds.rows, ds.dead, 1, 0, d, nlist);
        idx.use_radius_prune_ = false;
        std::printf("%5zu %4zu |", nlist, rep);
        for (size_t np :
             {size_t(1), size_t(2), size_t(4), size_t(8), size_t(16), size_t(32), size_t(64)}) {
          idx.nprobe_ = np;
          std::printf(" %.3f", recall_at(idx, qs, gt, k));
        }
        std::printf(" |");
        for (size_t np : {size_t(8), size_t(32)}) {
          idx.nprobe_ = np;
          const double t1 = now_ms();
          for (size_t i = 0; i < nq; ++i) idx.search(qs[i].data(), k, &out);
          std::printf(" %6.3f (%4.1fx)", (now_ms() - t1) / (double)nq, 1.0);
        }
        std::printf("\n");
      }
    }
  }
}

// ------------------------------------------------------ [8] SIMD kernels ----
static void t_simd() {
  std::printf("[8] SIMD kernels vs the scalar reference\n");
  const size_t nk = l2_kernel_count();
  std::printf("    kernels:");
  for (size_t i = 0; i < nk; ++i)
    std::printf(" %s%s", l2_kernel_name(i), l2_kernel_available(i) ? "" : "(n/a)");
  std::printf("   selected: %s\n", l2_isa_name());
  CHECK(nk >= 1, "no kernel compiled in");
  CHECK(std::strcmp(l2_kernel_name(0), "scalar") == 0, "kernel 0 must be the scalar reference");
  CHECK(std::strcmp(l2_kernel_name(l2_kernel_count()), "") == 0, "out-of-range kernel name");

  const size_t dims[] = {1,  2,   3,   4,   5,   7,   8,   15,  16, 17,
                         31, 32,  33,  63,  64,  65,  127, 128, 129, 255,
                         256, 257, 512, 1000};
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> uni(-3.f, 3.f);
  double max_rel = 0.0;
  for (size_t di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
    const size_t d = dims[di];
    std::vector<float> a(d), b(d);
    for (size_t i = 0; i < d; ++i) {
      a[i] = uni(rng);
      b[i] = uni(rng);
    }
    const float ref = l2_distance_scalar(a.data(), b.data(), d);
    for (size_t ki = 0; ki < nk; ++ki) {
      if (!l2_kernel_available(ki)) continue;
      const float got = l2_distance_kernel(ki, a.data(), b.data(), d);
      const double rel = std::fabs((double)got - (double)ref) / (1.0 + std::fabs((double)ref));
      if (rel > max_rel) max_rel = rel;
      // Lane count and reduction order differ from the scalar kernel; the
      // spread is a few ulp and never reorders vectors that differ by >1e-6.
      CHECK(rel < 1e-5, "%s dim %zu: rel %.3e vs scalar (%.9g vs %.9g)", l2_kernel_name(ki), d,
            rel, (double)got, (double)ref);
      CHECK(l2_distance_kernel(ki, a.data(), a.data(), d) == 0.f,
            "%s: self distance must be exactly 0, dim %zu", l2_kernel_name(ki), d);
      CHECK(l2_distance_kernel(ki, a.data(), b.data(), d) ==
                l2_distance_kernel(ki, b.data(), a.data(), d),
            "%s: symmetry, dim %zu", l2_kernel_name(ki), d);
      CHECK(got == l2_distance_kernel(ki, a.data(), b.data(), d),
            "%s: not deterministic, dim %zu", l2_kernel_name(ki), d);
    }
    // The dispatched entry point must agree with the named kernel it selected.
    size_t sel = 0;
    for (size_t ki = 0; ki < nk; ++ki)
      if (std::strcmp(l2_kernel_name(ki), l2_isa_name()) == 0) sel = ki;
    CHECK(l2_distance(a.data(), b.data(), d) == l2_distance_kernel(sel, a.data(), b.data(), d),
          "l2_distance() diverges from the selected kernel (%s) at dim %zu", l2_isa_name(), d);
  }
  std::printf("    max relative deviation from the scalar kernel: %.2e\n", max_rel);
  // dimension 0 and null operands keep returning exactly 0
  CHECK(l2_distance(nullptr, nullptr, 8) == 0.f, "null must be 0");
  CHECK(l2_distance_scalar(nullptr, nullptr, 8) == 0.f, "scalar null must be 0");

  // distance_many is bit-identical to l2_distance (that is what lets the IVF
  // trainer batch it without moving a single centroid).
  const size_t mns[] = {0, 1, 3, 4, 5, 7, 8, 16, 33, 64};
  const size_t mds[] = {4, 8, 15, 16, 17, 128, 129};
  for (size_t mi = 0; mi < sizeof(mns) / sizeof(mns[0]); ++mi) {
    for (size_t dj = 0; dj < sizeof(mds) / sizeof(mds[0]); ++dj) {
      const size_t n = mns[mi], d = mds[dj];
      std::vector<float> mat(n * d + 1), q(d), out(n + 1), ref(n + 1);
      for (auto& x : mat) x = uni(rng);
      for (auto& x : q) x = uni(rng);
      distance_many(q.data(), mat.data(), n, d, out.data());
      for (size_t i = 0; i < n; ++i) ref[i] = l2_distance(q.data(), mat.data() + i * d, d);
      for (size_t i = 0; i < n; ++i)
        CHECK(out[i] == ref[i], "distance_many n=%zu dim=%zu row %zu: %.9g != %.9g", n, d, i,
              (double)out[i], (double)ref[i]);
    }
  }
}

// --------------------------------------------------- [9] streaming top-k -----
static void t_topk() {
  std::printf("[9] TopK streaming vs exact_topk\n");
  const size_t dims[] = {4, 128};
  const size_t ns[] = {0, 1, 7, 300, 2000};
  std::vector<Hit> a, b;
  for (size_t di = 0; di < 2; ++di) {
    const size_t d = dims[di];
    for (size_t ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ++ni) {
      const size_t n = ns[ni];
      Dataset ds = make_dataset(n, d, K_UNIFORM, 61 + (uint32_t)ni);
      for (size_t i = 0; i < n; i += 6) ds.dead[i] = 1;
      auto qs = make_queries(3, d, 62);
      for (size_t qi = 0; qi < qs.size(); ++qi) {
        const size_t ks[] = {0, 1, 3, 10, 100, n + 3};
        for (size_t ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ++ki) {
          const size_t k = ks[ki];
          exact_topk(ds.rows, ds.dead, 1, 0, qs[qi].data(), d, k, &a);
          TopK top(qs[qi].data(), d, k);
          for (size_t i = 0; i < ds.rows.size(); ++i) {
            if (ds.dead[i]) continue;
            top.offer(ds.rows[i].vals[0].i, ds.rows[i].vals[1].v.data(), d);
          }
          top.finish(&b);
          CHECK(same_hits(a, b), "TopK n=%zu dim=%zu k=%zu q=%zu: %zu vs %zu hits", n, d, k, qi,
                b.size(), a.size());
          CHECK(sorted_by_dist(b), "TopK output not sorted (n=%zu k=%zu)", n, k);
        }
      }
    }
  }

  // k larger than the input, and the reuse path (reset/clear on one object).
  Dataset ds = make_dataset(100, 4, K_UNIFORM, 63);
  TopK reuse;
  std::vector<float> qv = {0.f, 0.f, 0.f, 0.f};
  for (int round = 0; round < 20; ++round) {
    const size_t k = 1 + (size_t)round % 12;  // k changes between rounds: heap is regrown
    reuse.reset(k);
    reuse.set_query(qv.data(), 4);
    std::vector<Hit> want;
    exact_topk(ds.rows, ds.dead, 1, 0, qv.data(), 4, k, &want);
    for (size_t i = 0; i < ds.rows.size(); ++i)
      reuse.offer_dist(ds.rows[i].vals[0].i, l2_distance(qv.data(), ds.rows[i].vals[1].v.data(), 4));
    std::vector<Hit> got;
    reuse.finish(&got);
    CHECK(same_hits(got, want), "reused TopK round %d (k=%zu): %zu vs %zu hits", round, k, got.size(),
          want.size());
    CHECK(reuse.k() == k && reuse.size() == want.size(), "reuse bookkeeping round %d", round);
  }
  // default-constructed object with k set later
  TopK late;
  CHECK(late.k() == 0 && late.full(), "default TopK must be an empty full heap of k=0");
  CHECK(late.worst_dist() == std::numeric_limits<float>::infinity(), "empty heap worst = inf");
  late.reset(5);
  late.set_query(qv.data(), 4);
  for (size_t i = 0; i < ds.rows.size(); ++i)
    late.offer(ds.rows[i].vals[0].i, ds.rows[i].vals[1].v.data(), 4);
  std::vector<Hit> want5;
  exact_topk(ds.rows, ds.dead, 1, 0, qv.data(), 4, 5, &want5);
  std::vector<Hit> got5;
  late.finish(&got5);
  CHECK(same_hits(got5, want5), "late-init TopK != exact_topk");
  CHECK(got5.size() == 5 && late.full(), "late-init TopK must be full");
  // finish() never shrinks the caller's buffer (the executor keeps one vector)
  std::vector<Hit> reused_out;
  std::vector<Hit> g2;
  for (int i = 0; i < 30; ++i) {
    late.finish(&reused_out);
    if (i == 1) g2 = reused_out;
    CHECK(reused_out.size() == 5, "finish: %zu hits", reused_out.size());
  }
  CHECK(same_hits(reused_out, g2), "finish() is not repeatable");
  // An empty offer set yields an empty result.
  TopK empty_topk(3);
  empty_topk.set_query(qv.data(), 4);
  std::vector<Hit> none;
  empty_topk.finish(&none);
  CHECK(none.empty(), "no offers must give no hits");

  // Ties: identical vectors must keep pk-ascending order no matter the order in
  // which they are offered (heap-based selection is order independent).
  {
    std::vector<int64_t> pks;
    for (int64_t p = 0; p < 40; ++p) pks.push_back(p * 2 + 3);
    std::vector<int64_t> expect = pks;
    std::sort(expect.begin(), expect.end());
    expect.resize(7);
    const std::vector<float> v = {1.5f, -0.25f, 0.5f, 2.f};
    std::vector<float> q4 = {0.f, 0.f, 0.f, 0.f};
    for (int order = 0; order < 2; ++order) {
      if (order) std::reverse(pks.begin(), pks.end());
      TopK tie_topk(7);
      tie_topk.set_query(q4.data(), 4);
      for (int64_t p : pks) tie_topk.offer(p, v.data(), 4);
      std::vector<Hit> hits;
      tie_topk.finish(&hits);
      CHECK(hits.size() == 7, "tie: %zu hits", hits.size());
      for (size_t i = 0; i < hits.size(); ++i)
        CHECK(hits[i].pk == expect[i], "tie order %d hit %zu: pk %lld != %lld", order, i,
              (long long)hits[i].pk, (long long)expect[i]);
      // without the pk tie-break the distances are still identical
      TopK notb(7, false);
      notb.set_query(q4.data(), 4);
      for (int64_t p : pks) notb.offer(p, v.data(), 4);
      std::vector<Hit> nh;
      notb.finish(&nh);
      CHECK(nh.size() == 7 && sorted_by_dist(nh), "no-tiebreak TopK shape");
      for (size_t i = 0; i < nh.size(); ++i)
        CHECK(nh[i].dist == hits[i].dist, "no-tiebreak distances must match");
    }
  }

  // topk_matrix over a packed matrix == exact_topk (same kernel, same heap).
  {
    const size_t d = 128, n = 777;
    Dataset pds = make_dataset(n, d, K_UNIFORM, 65);
    std::vector<float> mat(n * d);
    std::vector<int64_t> pks(n);
    for (size_t i = 0; i < n; ++i) {
      std::memcpy(mat.data() + i * d, pds.rows[i].vals[1].v.data(), d * sizeof(float));
      pks[i] = pds.rows[i].vals[0].i;
    }
    auto qs = make_queries(5, d, 66);
    for (size_t qi = 0; qi < qs.size(); ++qi) {
      for (size_t k : {size_t(1), size_t(10), size_t(n + 1)}) {
        std::vector<Hit> ex, gm;
        exact_topk(pds.rows, pds.dead, 1, 0, qs[qi].data(), d, k, &ex);
        topk_matrix(mat.data(), pks.data(), n, qs[qi].data(), d, k, &gm);
        CHECK(same_hits(gm, ex), "topk_matrix k=%zu q=%zu: %zu vs %zu hits", k, qi, gm.size(),
              ex.size());
      }
    }
  }
}

// ------------------------------------------------------- [perf] benchmark ----
// The pre-SIMD implementation, copied verbatim from the build this one replaces
// (scalar 4-lane accumulation + the same bounded heap). It is the "before"
// column: same compiler, same flags, same TU, same data.
namespace {

bool lg_hit_less(const Hit& a, const Hit& b) {
  if (a.dist != b.dist) return a.dist < b.dist;
  return a.pk < b.pk;
}

void lg_sift_up(std::vector<Hit>& h, size_t i) {
  while (i > 0) {
    const size_t p = (i - 1) / 2;
    if (!lg_hit_less(h[p], h[i])) break;
    std::swap(h[p], h[i]);
    i = p;
  }
}

void lg_sift_down(std::vector<Hit>& h, size_t i) {
  const size_t n = h.size();
  for (;;) {
    size_t big = i;
    const size_t l = 2 * i + 1, r = l + 1;
    if (l < n && lg_hit_less(h[big], h[l])) big = l;
    if (r < n && lg_hit_less(h[big], h[r])) big = r;
    if (big == i) break;
    std::swap(h[i], h[big]);
    i = big;
  }
}

float lg_l2(const float* a, const float* b, size_t dim) {
  if (a == nullptr || b == nullptr || dim == 0) return 0.0f;
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

void lg_exact_topk(const std::vector<Row>& rows, const std::vector<uint8_t>& dead, int vec_col,
                   int pk_col, const float* q, size_t dim, size_t k, std::vector<Hit>* out)
    __attribute__((noinline));  // the original lived in another TU: keep the call boundary

void lg_exact_topk(const std::vector<Row>& rows, const std::vector<uint8_t>& dead, int vec_col,
                   int pk_col, const float* q, size_t dim, size_t k, std::vector<Hit>* out) {
  out->clear();
  if (k == 0 || q == nullptr || dim == 0) return;
  std::vector<Hit>& heap = *out;
  heap.reserve(std::min(k, rows.size()));
  const size_t nd = dead.size();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i < nd && dead[i]) continue;
    const Row& r = rows[i];
    if (vec_col < 0 || static_cast<size_t>(vec_col) >= r.vals.size()) continue;
    const Value& v = r.vals[vec_col];
    if (v.type != Type::VECTOR || v.v.size() != dim) continue;
    const float d = lg_l2(q, v.v.data(), dim);
    const int64_t pk = r.vals[pk_col].i;
    if (heap.size() < k) {
      heap.push_back(Hit{pk, d});
      lg_sift_up(heap, heap.size() - 1);
    } else if (lg_hit_less(Hit{pk, d}, heap[0])) {
      heap[0] = Hit{pk, d};
      lg_sift_down(heap, 0);
    }
  }
  std::sort(heap.begin(), heap.end(), lg_hit_less);
}

}  // namespace

// Milliseconds per query for one scan variant over `nq` queries.
static double time_legacy(const Dataset& ds, const std::vector<std::vector<float>>& qs, size_t k,
                          std::vector<Hit>& out) {
  const double t0 = now_ms();
  for (size_t i = 0; i < qs.size(); ++i)
    lg_exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), ds.dim, k, &out);
  return (now_ms() - t0) / (double)qs.size();
}

static double time_exact(const Dataset& ds, const std::vector<std::vector<float>>& qs, size_t k,
                         std::vector<Hit>& out) {
  const double t0 = now_ms();
  for (size_t i = 0; i < qs.size(); ++i)
    exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), ds.dim, k, &out);
  return (now_ms() - t0) / (double)qs.size();
}

static double time_topk_matrix(const float* mat, const int64_t* pks, size_t n,
                               const std::vector<std::vector<float>>& qs, size_t dim, size_t k,
                               std::vector<Hit>& out) {
  const double t0 = now_ms();
  for (size_t i = 0; i < qs.size(); ++i) topk_matrix(mat, pks, n, qs[i].data(), dim, k, &out);
  return (now_ms() - t0) / (double)qs.size();
}

// What an executor that materialises every row and then sorts pays (lower bound:
// no string formatting, no OutRow struct).
static double time_fullsort(const float* mat, const int64_t* pks, size_t n,
                            const std::vector<std::vector<float>>& qs, size_t dim, size_t k,
                            std::vector<Hit>& out) {
  std::vector<float> dist(n);
  std::vector<Hit> buf(n);
  const double t0 = now_ms();
  for (size_t qi = 0; qi < qs.size(); ++qi) {
    distance_many(qs[qi].data(), mat, n, dim, dist.data());
    for (size_t i = 0; i < n; ++i) buf[i] = Hit{pks[i], dist[i]};
    std::stable_sort(buf.begin(), buf.end(), detail::hit_less);
    const size_t kk = std::min(k, n);
    out.assign(buf.begin(), buf.begin() + (long)kk);
    for (size_t i = 0; i < n; ++i) buf[i] = Hit{0, 0};  // keep the next sort honest
  }
  return (now_ms() - t0) / (double)qs.size();
}

static void perf() {
  const size_t k = 10;
  const size_t dims[] = {128, 512};
  const size_t ns[] = {1000, 10000, 100000};
  std::printf("MiniVectorDB hot-path benchmark (ORDER BY distance LIMIT %zu)\n", k);
  std::printf("  ISA selected : %s\n", l2_isa_name());
  std::printf("  kernels      :");
  for (size_t i = 0; i < l2_kernel_count(); ++i)
    std::printf(" %s%s", l2_kernel_name(i), l2_kernel_available(i) ? "" : "(n/a)");
  std::printf("\n");
  std::printf("  build        : g++ -O2 -std=c++17, no -march\n");
  std::printf("  queries      : i.i.d. uniform(-1,1); nq picked for ~2.5e9 element-scans/column\n");
  std::printf("  legacy       = pre-SIMD scalar kernel + the old heap scan (the baseline)\n");
  std::printf("  topk-mat     = distance_many + fused TopK over a packed n x dim matrix\n");
  std::printf("  fullsort     = materialise all n distances, stable_sort, take k (executor shape)\n");
  std::printf("  GB/s         = n*dim*4 bytes / topk-mat ms (vector payload only, no pks)\n");
  std::printf("  timing       = best of 3 runs per column\n\n");
  std::printf("%7s %4s %5s | %9s | %9s %5s | %9s %5s | %9s %5s | %6s | %9s %9s %11s\n", "n", "dim",
              "nq", "legacy", "exact", "x", "topk-mat", "x", "fullsort", "x", "GB/s",
              "1000q leg", "1000q topk", "1000q GB/s");
  double s10_exact = 0, s10_mat = 0, s100_exact = 0, s100_mat = 0;
  for (size_t di = 0; di < sizeof(dims) / sizeof(dims[0]); ++di) {
    const size_t d = dims[di];
    for (size_t ni = 0; ni < sizeof(ns) / sizeof(ns[0]); ++ni) {
      const size_t n = ns[ni];
      Dataset ds = make_dataset(n, d, K_UNIFORM, 99 + (uint32_t)ni);
      size_t nq = (size_t)(2.5e9 / (double)(n * d));
      if (nq < 20) nq = 20;
      if (nq > 1000) nq = 1000;
      auto qs = make_queries(nq, d, 77);

      std::vector<float> mat(n * d);
      std::vector<int64_t> pks(n);
      for (size_t i = 0; i < n; ++i) {
        std::memcpy(mat.data() + i * d, ds.rows[i].vals[1].v.data(), d * sizeof(float));
        pks[i] = ds.rows[i].vals[0].i;
      }
      std::vector<Hit> out;
      // Best of 3: a laptop under WSL has enough interference that the minimum
      // is the only number worth publishing (medians move +-15% run to run).
      double ms_leg = 1e30, ms_ex = 1e30, ms_mat = 1e30, ms_fs = 1e30;
      for (int rep = 0; rep < 3; ++rep) {
        ms_leg = std::min(ms_leg, time_legacy(ds, qs, k, out));
        ms_ex = std::min(ms_ex, time_exact(ds, qs, k, out));
        ms_mat = std::min(ms_mat, time_topk_matrix(mat.data(), pks.data(), n, qs, d, k, out));
        ms_fs = std::min(ms_fs, time_fullsort(mat.data(), pks.data(), n, qs, d, k, out));
      }
      const double gbs_mat = (double)n * (double)d * 4.0 / (ms_mat * 1e6);
      const double gbs_leg = (double)n * (double)d * 4.0 / (ms_leg * 1e6);
      std::printf("%7zu %4zu %5zu | %9.4f | %9.4f %5.2f | %9.4f %5.2f | %9.3f %5.1f | %6.1f | %8.1fms %8.1fms %9.1f\n",
                  n, d, nq, ms_leg, ms_ex, ms_leg / ms_ex, ms_mat, ms_leg / ms_mat, ms_fs,
                  ms_leg / ms_fs, gbs_mat, ms_leg * 1000.0, ms_mat * 1000.0, gbs_leg);
      if (d == 128 && n == 10000) { s10_exact = ms_leg / ms_ex; s10_mat = ms_leg / ms_mat; }
      if (d == 128 && n == 100000) { s100_exact = ms_leg / ms_ex; s100_mat = ms_leg / ms_mat; }
    }
  }
  std::printf("\n  speedup vs the pre-SIMD baseline (128-d):\n");
  std::printf("    10k  x 128: scan+TopK %.2fx   packed matrix %.2fx\n", s10_exact, s10_mat);
  std::printf("   100k  x 128: scan+TopK %.2fx   packed matrix %.2fx\n", s100_exact, s100_mat);
  std::printf("  (fullsort is what the executor pays today: n distances + a full sort per query;\n");
  std::printf("   TopK keeps 10 hits and drops the sort, which is the bigger half of the win.)\n");
  std::printf("\n  hint: MVDB_ISA=scalar|sse2|avx2 re-runs with a fixed kernel.\n");
}

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "sweep") {
    sweep();
    return 0;
  }
  if (mode == "tune") {
    tune();
    return 0;
  }
  if (mode == "large") {
    bench_large();
    return 0;
  }
  if (mode == "perf") {
    perf();
    return 0;
  }
  std::printf("MiniVectorDB vector engine tests (%s, ISA %s)\n",
              mode == "quick" ? "quick" : "full", l2_isa_name());
  t_distance();
  t_exact_topk();
  t_tiebreak();
  t_ivf_exact();
  t_filtered();
  t_persist();
  t_simd();
  t_topk();
  if (mode != "quick") bench_and_recall();
  std::printf("\n%s: %d checks, %d failures\n", g_fail == 0 ? "PASS" : "FAIL", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}

// tests/test_vector.cpp — standalone correctness suite + micro-benchmark for
// src/vector.{h,cpp}. Own main(); no external deps.
//
//   g++ -std=c++17 -O2 -Isrc tests/test_vector.cpp src/vector.cpp -o test_vector
//   ./test_vector          # full suite + benchmark
//   ./test_vector quick    # skip the 10k benchmark
//   ./test_vector sweep    # nlist / replication / nprobe recall table
//
// Comparison policy:
//   * vs the independent reference (double accumulation) -> identical pk SET and
//     distances within 1e-5 relative, because the reference is more accurate
//     than the float kernel.
//   * IVF vs exact_topk (same kernel) and post-persist reload -> BITWISE equal,
//     including the (dist, pk) order.
//   * engineered exact ties -> strict pk-order assertion.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
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

  std::printf("    insert / erase / compaction\n");
  const size_t d = 128;
  Dataset ds = make_dataset(1200, d, K_CLUSTERED, 77);
  IvfIndex idx;
  idx.build(ds.rows, ds.dead, 1, 0, d, 32);
  std::mt19937 rng(3);
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
static void bench_and_recall() {
  const size_t n = 10000, d = 128, nq = 200, k = 10;
  const char* names[] = {"uniform(-1,1)", "gaussian", "clustered(64)", "normed-gauss"};
  std::printf("\n[7] recall@10 + latency   n=%zu dim=%zu queries=%zu k=%zu\n", n, d, nq, k);
  std::printf("    %-15s | %-21s | %-21s | %-21s | %s\n", "distribution", "nprobe=8  recall",
              "nprobe=32 recall", "nprobe=64 recall", "exact");
  for (int kind = 0; kind < 4; ++kind) {
    Dataset ds = make_dataset(n, d, kind, 1000 + (uint32_t)kind);
    auto qs = make_queries(nq, d, 2000 + (uint32_t)kind);
    auto gt = ground_truth(ds, qs, k);
    IvfIndex idx;
    idx.build(ds.rows, ds.dead, 1, 0, d, 64);
    const size_t bytes = idx.memory_bytes();

    std::vector<Hit> out;
    double t0 = now_ms();
    for (size_t i = 0; i < nq; ++i) exact_topk(ds.rows, ds.dead, 1, 0, qs[i].data(), d, k, &out);
    const double exact_ms = (now_ms() - t0) / (double)nq;

    idx.use_radius_prune_ = false;
    char col[3][48];
    double rec[3] = {0, 0, 0};
    const size_t nps[3] = {8, 32, 64};
    for (int c = 0; c < 3; ++c) {
      idx.nprobe_ = nps[c];
      rec[c] = recall_at(idx, qs, gt, k);
      t0 = now_ms();
      for (size_t i = 0; i < nq; ++i) idx.search(qs[i].data(), k, &out);
      const double ms = (now_ms() - t0) / (double)nq;
      std::snprintf(col[c], sizeof col[c], "%.3f  %6.3fms %4.1fx", rec[c], ms, exact_ms / ms);
    }
    std::printf("    %-15s | %-21s | %-21s | %-21s | %6.3f ms/q\n", names[kind], col[0], col[1],
                col[2], exact_ms);

    // exactness mode: radius pruning never loses a hit -> recall must be 1.000
    idx.use_radius_prune_ = true;
    idx.nprobe_ = 8;
    const double rec_on = recall_at(idx, qs, gt, k);
    t0 = now_ms();
    for (size_t i = 0; i < nq; ++i) idx.search(qs[i].data(), k, &out);
    const double on_ms = (now_ms() - t0) / (double)nq;
    std::printf("    %-15s | prune=ON(exact) recall %.3f  %6.3fms %4.1fx   footprint %.1f MB\n",
                "", rec_on, on_ms, exact_ms / on_ms, (double)bytes / (1024.0 * 1024.0));
    CHECK(rec_on == 1.0, "radius-pruned recall must be 1.000, got %.3f (%s)", rec_on, names[kind]);
    if (kind == 0) {
      std::printf("    (10k x 128 raw vectors = %.1f MB; index overhead = %.1f%%)\n",
                  (double)n * d * 4 / (1024.0 * 1024.0),
                  100.0 * ((double)bytes / ((double)n * d * 4) - 1.0));
      CHECK(rec[1] >= 0.95, "recall@10 at nprobe=32 on 10k random 128-d must be >= 0.95, got %.3f",
            rec[1]);
    }
  }

  // nprobe sweep in pure approximate mode on the hardest distribution.
  Dataset ds = make_dataset(n, d, K_UNIFORM, 42);
  auto qs = make_queries(nq, d, 4242);
  auto gt = ground_truth(ds, qs, k);
  IvfIndex idx;
  idx.build(ds.rows, ds.dead, 1, 0, d, 64);
  idx.use_radius_prune_ = false;
  std::printf("    nprobe sweep (uniform, nlist=64, replication=%zu):", idx.replication_);
  for (size_t np : {size_t(1), size_t(2), size_t(4), size_t(8), size_t(16), size_t(32), size_t(64)}) {
    idx.nprobe_ = np;
    std::printf(" %zu:%.3f", np, recall_at(idx, qs, gt, k));
  }
  std::printf("\n");
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

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "sweep") {
    sweep();
    return 0;
  }
  std::printf("MiniVectorDB vector engine tests (%s)\n", mode == "quick" ? "quick" : "full");
  t_distance();
  t_exact_topk();
  t_tiebreak();
  t_ivf_exact();
  t_filtered();
  t_persist();
  if (mode != "quick") bench_and_recall();
  std::printf("\n%s: %d checks, %d failures\n", g_fail == 0 ? "PASS" : "FAIL", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}

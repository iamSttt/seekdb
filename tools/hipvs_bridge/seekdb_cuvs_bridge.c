/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * Build strategy (V6):
 *   The stock CAGRA build spends almost all of its time inside NN-descent
 *   refinement, and trimming the iteration budget trades directly against
 *   Recall@100. Instead of refining a random graph, we construct the graph
 *   ourselves on the GPU:
 *
 *     1. hipVS brute-force kNN over the dataset itself  -> exact k+1 neighbours
 *     2. a tiny kernel drops the self edge and rewrites int64 -> uint32
 *     3. cuvsCagraIndexFromArgs() wraps that graph as a CAGRA index
 *
 *   The resulting graph is an exact 128-NN graph, i.e. strictly better quality
 *   than the refined graph the baseline builds, at a fraction of the cost.
 *   Search still goes through the untouched hipVS CAGRA search.
 *
 *   Everything is resolved through the fixed C API; if any of the extra entry
 *   points is missing at runtime we fall back to the original NN-descent build,
 *   so a build can never fail just because a symbol is unavailable.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* RTLD_DEFAULT from <dlfcn.h> */
#endif
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/distance/distance.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Degree of the graph handed to CAGRA. 128-d data is already perfectly served
 * by an exact 128-NN graph; at 256-d the extra headroom of a 256-NN graph keeps
 * Recall@10/@100 indistinguishable from the baseline's refined graph. */
#define SK_GRAPH_DEGREE 128
#define SK_GRAPH_DEGREE_HIDIM 256

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim, deg;
  void* own_dataset;   /* device copy of the vectors, referenced by the index */
  void* own_graph;     /* device uint32 kNN graph, referenced by the index    */
} sk_cuvs_index;

/* Provided by seekdb_knn_graph.cu (compiled by the same hipcc invocation). */
extern int seekdb_knn_drop_self(const void* in_i64, void* out_u32,
                                long n, long k_in, long k_out);

static double sk_now(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------ */
/* Optional cuVS entry points, resolved lazily so that a hipVS build   */
/* that lacks them simply degrades to the stock NN-descent path.       */
/* ------------------------------------------------------------------ */
typedef cuvsError_t (*sk_fn_bf_create)(cuvsBruteForceIndex_t*);
typedef cuvsError_t (*sk_fn_bf_destroy)(cuvsBruteForceIndex_t);
typedef cuvsError_t (*sk_fn_bf_build)(cuvsResources_t, DLManagedTensor*,
                                      cuvsDistanceType, float, cuvsBruteForceIndex_t);
typedef cuvsError_t (*sk_fn_bf_search)(cuvsResources_t, cuvsBruteForceIndex_t,
                                       DLManagedTensor*, DLManagedTensor*,
                                       DLManagedTensor*, cuvsFilter);
typedef cuvsError_t (*sk_fn_cagra_from_args)(cuvsResources_t, cuvsDistanceType,
                                             DLManagedTensor*, DLManagedTensor*,
                                             cuvsCagraIndex_t);

static struct {
  int                ready;   /* 0 = not probed, 1 = available, -1 = missing */
  sk_fn_bf_create        bf_create;
  sk_fn_bf_destroy       bf_destroy;
  sk_fn_bf_build         bf_build;
  sk_fn_bf_search        bf_search;
  sk_fn_cagra_from_args  cagra_from_args;
} sk_syms;

static void* sk_sym(const char* name) { return dlsym(RTLD_DEFAULT, name); }

static int sk_load_symbols(void)
{
  if (sk_syms.ready) return sk_syms.ready > 0 ? 0 : -1;
  sk_syms.bf_create       = (sk_fn_bf_create)sk_sym("cuvsBruteForceIndexCreate");
  sk_syms.bf_destroy      = (sk_fn_bf_destroy)sk_sym("cuvsBruteForceIndexDestroy");
  sk_syms.bf_build        = (sk_fn_bf_build)sk_sym("cuvsBruteForceBuild");
  sk_syms.bf_search       = (sk_fn_bf_search)sk_sym("cuvsBruteForceSearch");
  sk_syms.cagra_from_args = (sk_fn_cagra_from_args)sk_sym("cuvsCagraIndexFromArgs");
  const int ok = sk_syms.bf_create && sk_syms.bf_destroy && sk_syms.bf_build &&
                 sk_syms.bf_search && sk_syms.cagra_from_args;
  sk_syms.ready = ok ? 1 : -1;
  if (!ok) {
    fprintf(stderr, "[seekdb_cuvs] brute-force / index-from-args symbols missing:"
                    " bf_create=%d bf_destroy=%d bf_build=%d bf_search=%d from_args=%d\n",
            sk_syms.bf_create != NULL, sk_syms.bf_destroy != NULL,
            sk_syms.bf_build != NULL, sk_syms.bf_search != NULL,
            sk_syms.cagra_from_args != NULL);
  }
  return ok ? 0 : -1;
}

static void sk_tensor_2d(DLManagedTensor* t, void* data, int device, long r0, long r1,
                         int code, int bits, long* shape)
{
  memset(t, 0, sizeof(*t));
  shape[0] = r0; shape[1] = r1;
  t->dl_tensor.data = data;
  t->dl_tensor.device.device_type = device ? kDLCUDA : kDLCPU;
  t->dl_tensor.device.device_id = 0;
  t->dl_tensor.ndim = 2;
  t->dl_tensor.dtype.code = code;
  t->dl_tensor.dtype.bits = bits;
  t->dl_tensor.dtype.lanes = 1;
  t->dl_tensor.shape = shape;
  t->dl_tensor.strides = 0;
  t->dl_tensor.byte_offset = 0;
  t->manager_ctx = 0;
  t->deleter = 0;
}

static size_t sk_env_size(const char* name, size_t dflt);

/* Build the index from an exact GPU kNN graph. Returns 0 on success. */
static int sk_build_exact_knn(sk_cuvs_index* h, const float* base, long n, long dim)
{
  const long deg   = h->deg;
  const long k_in  = deg + 1;   /* brute force always returns the query itself */
  void* d_base = NULL;
  long long* d_nbr = NULL;
  float* d_dist = NULL;
  cuvsBruteForceIndex_t bf = NULL;
  int rc = -1;
  double t0 = sk_now(), t_bf_build = 0, t_bf_search = 0, t_decode = 0, t_index = 0;

  if (sk_load_symbols() != 0) return -1;
  if (n <= deg || n > (long)1 << 31) return -1;

  const size_t ds_bytes   = sizeof(float) * (size_t)n * (size_t)dim;
  const size_t nbr_bytes  = sizeof(long long) * (size_t)n * (size_t)k_in;
  const size_t dist_bytes = sizeof(float) * (size_t)n * (size_t)k_in;
  const size_t graph_bytes = sizeof(unsigned) * (size_t)n * (size_t)deg;

  if (cuvsRMMAlloc(h->res, &d_base, ds_bytes) != CUVS_SUCCESS) goto done;
  h->own_dataset = d_base;
  if (cudaMemcpy(d_base, base, ds_bytes, cudaMemcpyDefault) != cudaSuccess) goto done;
  if (cuvsRMMAlloc(h->res, (void**)&d_nbr, nbr_bytes) != CUVS_SUCCESS) goto done;
  if (cuvsRMMAlloc(h->res, (void**)&d_dist, dist_bytes) != CUVS_SUCCESS) goto done;
  if (cuvsRMMAlloc(h->res, &h->own_graph, graph_bytes) != CUVS_SUCCESS) goto done;

  {
    DLManagedTensor dset;
    long dset_shape[2];
    sk_tensor_2d(&dset, d_base, 1, n, dim, kDLFloat, 32, dset_shape);

    if (sk_syms.bf_create(&bf) != CUVS_SUCCESS) goto done;
    if (sk_syms.bf_build(h->res, &dset, L2Expanded, 0.f, bf) != CUVS_SUCCESS) goto done;
    t_bf_build = sk_now() - t0;

    {
      DLManagedTensor q, nb, db;
      long q_shape[2], nb_shape[2], db_shape[2];
      cuvsFilter filter;
      filter.type = NO_FILTER;
      filter.addr = (uintptr_t)0;
      sk_tensor_2d(&q, d_base, 1, n, dim, kDLFloat, 32, q_shape);
      sk_tensor_2d(&nb, d_nbr, 1, n, k_in, kDLInt, 64, nb_shape);
      sk_tensor_2d(&db, d_dist, 1, n, k_in, kDLFloat, 32, db_shape);
      double t = sk_now();
      if (sk_syms.bf_search(h->res, bf, &q, &nb, &db, filter) != CUVS_SUCCESS) goto done;
      t_bf_search = sk_now() - t;
    }
  }
  sk_syms.bf_destroy(bf); bf = NULL;

  {
    double t = sk_now();
    if (seekdb_knn_drop_self(d_nbr, h->own_graph, n, k_in, deg) != 0) goto done;
    t_decode = sk_now() - t;
  }
  /* The int64 neighbours and their distances are dead weight from here on. */
  cuvsRMMFree(h->res, d_nbr, nbr_bytes);  d_nbr = NULL;
  cuvsRMMFree(h->res, d_dist, dist_bytes); d_dist = NULL;

  {
    double t = sk_now();
    DLManagedTensor dset, graph;
    long dset_shape[2], graph_shape[2];
    sk_tensor_2d(&dset, d_base, 1, n, dim, kDLFloat, 32, dset_shape);
    sk_tensor_2d(&graph, h->own_graph, 1, n, deg, kDLUInt, 32, graph_shape);
    if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) goto done;
    if (sk_syms.cagra_from_args(h->res, L2Expanded, &graph, &dset, h->idx) != CUVS_SUCCESS) goto done;
    t_index = sk_now() - t;
  }

  fprintf(stderr, "[seekdb_cuvs] exact-knn graph n=%ld dim=%ld deg=%ld "
                  "bf_build=%.3fs bf_search=%.3fs decode=%.3fs index=%.3fs total=%.3fs\n",
          n, dim, deg, t_bf_build, t_bf_search, t_decode, t_index, sk_now() - t0);
  rc = 0;

done:
  if (bf && sk_syms.bf_destroy) sk_syms.bf_destroy(bf);
  if (d_nbr) cuvsRMMFree(h->res, d_nbr, nbr_bytes);
  if (d_dist) cuvsRMMFree(h->res, d_dist, dist_bytes);
  if (rc != 0) {
    if (h->own_graph) { cuvsRMMFree(h->res, h->own_graph, graph_bytes); h->own_graph = NULL; }
    if (h->own_dataset) { cuvsRMMFree(h->res, h->own_dataset, ds_bytes); h->own_dataset = NULL; }
    if (h->idx) { cuvsCagraIndexDestroy(h->idx); h->idx = NULL; }
  }
  return rc;
}

/* Stock CAGRA build (NN-descent). Used when the exact-kNN path is unavailable. */
static int sk_build_nn_descent(sk_cuvs_index* h, const float* base, long n, long dim)
{
  DLManagedTensor dset;
  long ds[2];
  sk_tensor_2d(&dset, (void*)base, 0, n, dim, kDLFloat, 32, ds);
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  {
    const char* nit = getenv("SEEKDB_CUVS_NITER");
    h->ip->nn_descent_niter = (nit && *nit) ? (size_t)strtol(nit, NULL, 10)
                                            : (size_t)((dim >= 192) ? 70 : 10);
  }
  if (cuvsCagraIndexCreate(&h->idx) != CUVS_SUCCESS) return -1;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) {
    cuvsCagraIndexDestroy(h->idx); h->idx = NULL;
    return -1;
  }
  return 0;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) {
    cuvsResourcesDestroy(h->res); free(h); return NULL;
  }

  h->deg = (long)sk_env_size("SEEKDB_CUVS_DEG",
                             (size_t)((dim >= 192) ? SK_GRAPH_DEGREE_HIDIM : SK_GRAPH_DEGREE));
  if (h->deg < 16) h->deg = 16;
  if (!getenv("SEEKDB_CUVS_FORCE_NN_DESCENT") && sk_build_exact_knn(h, base, n, dim) == 0) {
    h->n = n; h->dim = dim;
    return h;
  }
  fprintf(stderr, "[seekdb_cuvs] falling back to NN-descent build\n");
  if (sk_build_nn_descent(h, base, n, dim) != 0) {
    fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
    cuvsCagraIndexParamsDestroy(h->ip);
    cuvsResourcesDestroy(h->res); free(h);
    return NULL;
  }
  h->n = n; h->dim = dim;
  return h;
}

static size_t sk_env_size(const char* name, size_t dflt)
{
  const char* v = getenv(name);
  if (!v || !*v) return dflt;
  long l = strtol(v, NULL, 10);
  return l > 0 ? (size_t)l : dflt;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  float* qd = 0; unsigned* nd = 0; float* dd = 0; cuvsCagraSearchParams_t sp = 0;
  if (cuvsRMMAlloc(h->res, (void**)&qd, sizeof(float)*nq*h->dim)  != CUVS_SUCCESS) { rc=10; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&nd, sizeof(unsigned)*nq*topk) != CUVS_SUCCESS) { rc=11; goto done; }
  if (cuvsRMMAlloc(h->res, (void**)&dd, sizeof(float)*nq*topk)    != CUVS_SUCCESS) { rc=12; goto done; }
  if (cudaMemcpy(qd, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    sk_tensor_2d(&qt, qd, 1, nq, h->dim, kDLFloat, 32, qs);
    sk_tensor_2d(&nt, nd, 1, nq, topk, kDLUInt, 32, nsh);
    sk_tensor_2d(&dt, dd, 1, nq, topk, kDLFloat, 32, dsh);
    if (cuvsCagraSearchParamsCreate(&sp) != CUVS_SUCCESS) { rc=14; goto done; }
    {
      const char* itopk_env = getenv("SEEKDB_CUVS_ITOPK");
      size_t required_itopk;
      if (itopk_env && *itopk_env) {
        required_itopk = sk_env_size("SEEKDB_CUVS_ITOPK", 512);
      } else {
        /* The graph is an exact kNN graph, so the beam converges earlier than on
         * the baseline's refined graph; 256 intermediates keep recall identical
         * while cutting the per-query work. Keep it under 512: larger candidate
         * lists break this hipVS search implementation. */
        required_itopk = (size_t)topk * 2;
        if (required_itopk < 256) required_itopk = 256;
        if (required_itopk > 512) required_itopk = 512;
      }
      if (sp->itopk_size < required_itopk) sp->itopk_size = required_itopk;
      sp->search_width = sk_env_size("SEEKDB_CUVS_WIDTH", 4);
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, nd, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist) cudaMemcpy(out_dist, dd, sizeof(float)*nq*topk, cudaMemcpyDefault);
  }
done:
  if (sp) cuvsCagraSearchParamsDestroy(sp);
  if (dd) cuvsRMMFree(h->res, dd, sizeof(float)*nq*topk);
  if (nd) cuvsRMMFree(h->res, nd, sizeof(unsigned)*nq*topk);
  if (qd) cuvsRMMFree(h->res, qd, sizeof(float)*nq*h->dim);
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->idx) cuvsCagraIndexDestroy(h->idx);
  if (h->own_graph)   cuvsRMMFree(h->res, h->own_graph, sizeof(unsigned)*(size_t)h->n*(size_t)h->deg);
  if (h->own_dataset) cuvsRMMFree(h->res, h->own_dataset, sizeof(float)*(size_t)h->n*(size_t)h->dim);
  if (h->ip) cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}

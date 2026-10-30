/* GPU graph post-processing for the seekdb <-> hipVS bridge.
 *
 * The exact-kNN graph that replaces CAGRA's NN-descent build is produced by
 * hipVS' brute-force kNN, which is forced by the fixed C API to emit int64
 * neighbour ids. CAGRA wants a uint32 row-major graph, and the self edge is
 * useless for a graph index, so this kernel rewrites
 *   in[row, 0..k_in)  (int64, nearest first, includes the point itself)
 * into
 *   out[row, 0..k_out) (uint32, self removed)
 * entirely on the GPU.
 */
#include <hip/hip_runtime.h>
#include <stdint.h>

__global__ void sk_drop_self_kernel(const long long* __restrict__ in,
                                    unsigned* __restrict__ out,
                                    long n, long k_in, long k_out)
{
  long row = (long)blockIdx.x * blockDim.x + threadIdx.x;
  if (row >= n) return;

  const long long* src = in + row * k_in;
  unsigned* dst        = out + row * k_out;
  long w               = 0;
  for (long i = 0; i < k_in && w < k_out; ++i) {
    long long id = src[i];
    if (id == row) continue;
    if (id < 0 || id >= n) continue;
    dst[w++] = (unsigned)id;
  }
  /* Duplicate/tie collapse can leave the row short; pad with a neighbour that
   * is guaranteed to be legal so every row has exactly k_out entries. */
  for (long i = 0; w < k_out; ++i) {
    dst[w++] = (unsigned)((row + 1 + i) % n);
  }
}

extern "C" int seekdb_knn_drop_self(const void* in_i64, void* out_u32,
                                    long n, long k_in, long k_out)
{
  if (!in_i64 || !out_u32 || n <= 0 || k_in <= 0 || k_out <= 0 || k_out >= k_in) return 1;
  long threads = 256;
  long blocks  = (n + threads - 1) / threads;
  sk_drop_self_kernel<<<(unsigned)blocks, (unsigned)threads>>>(
    (const long long*)in_i64, (unsigned*)out_u32, n, k_in, k_out);
  return hipDeviceSynchronize() == hipSuccess ? 0 : 1;
}

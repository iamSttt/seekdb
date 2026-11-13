// Environment probe only: no DB, profile generation, SQL, or timing.
#include "quantized_bound.h"
#include <cstdio>
int main() {
  const char* native="scalar";
  bool admitted=false,avx2=false,avx512=false;
#if defined(MVDB_SIMD_BOUND_X86)
  avx2=__builtin_cpu_supports("avx2");
  avx512=__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && avx2;
  const auto selected=mvdb::quantized_detail::choose_distance();
  if(selected==mvdb::quantized_detail::distance_avx2){native="avx2";admitted=true;}
#if !defined(MVDB_DISABLE_AVX512_QUANTIZED)
  else if(selected==mvdb::quantized_detail::distance_avx512)native="avx512";
#endif
  else if(selected==mvdb::quantized_detail::distance_sse2)native="sse2";
#endif
  std::printf("{\"schema_version\":1,\"status\":\"%s\",\"native_row_isa\":\"%s\","
      "\"selected_batch_isa\":\"avx2\",\"avx2\":%s,\"avx512\":%s,"
      "\"database_runs\":0,\"pgo_builds\":0,\"performance_pairs\":0}\n",
      admitted?"supported_native_avx2":"unsupported_native_isa",native,
      avx2?"true":"false",avx512?"true":"false");
  return admitted?0:78;
}

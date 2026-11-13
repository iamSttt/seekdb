#pragma once
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sched.h>

namespace mvdb {

// Call once in main, before database recovery or any worker creation. This
// changes only the adaptive query threshold: the existing startup calibration
// and explicit thread/threshold overrides keep their original behavior.
inline void configure_query_startup_affinity() {
  // Even an explicitly empty value belongs to the caller. Do not silently
  // reinterpret it or alter the meaning of SINGLE_THREAD=0 / QUERY_THREADS=2.
  if (std::getenv("MVDB_QUERY_THREADS") ||
      std::getenv("MVDB_QUERY_FANOUT_ROWS") ||
      std::getenv("MVDB_SINGLE_THREAD")) return;

  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (::sched_getaffinity(0, sizeof(allowed), &allowed) != 0 ||
      CPU_COUNT(&allowed) != 1) return;

  // This is deliberately an affinity-only rule. A quota of one CPU across
  // multiple allowed CPUs has different scheduling behavior and is untested.
  char threshold[3 * sizeof(long) + 2];
  std::snprintf(threshold, sizeof(threshold), "%ld", LONG_MAX);
  if (::setenv("MVDB_QUERY_FANOUT_ROWS", threshold, 0) == 0)
    std::fprintf(stderr, "mvdb: single-CPU affinity; adaptive query fanout disabled, startup calibration retained\n");
}

}  // namespace mvdb

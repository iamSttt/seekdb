#include "../src/query_startup_policy.h"
#include "../src/query_pool.h"
#include <cerrno>
#include <dirent.h>
#include <cstring>

// A separate failure binary uses --wrap=sched_getaffinity. No test hooks or
// alternate decision function are compiled into the production helper.
extern "C" int __wrap_sched_getaffinity(pid_t, size_t, cpu_set_t*) {
  errno = EINVAL;
  return -1;
}

static int threads() {
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return -1;
  int count = 0;
  while (dirent* entry = readdir(dir))
    if (entry->d_name[0] != '.') ++count;
  closedir(dir);
  return count;
}

int main() {
  mvdb::configure_query_startup_affinity();
  const char* threshold = std::getenv("MVDB_QUERY_FANOUT_ROWS");
  const char* parse = std::getenv("MVDB_PARSE_THREADS");
  std::printf("threshold=%s\nparse=%s\n", threshold ? threshold : "<unset>",
              parse ? parse : "<unset>");
  std::printf("mode=%d\nmin_rows=%zu\nthreads_before=%d\n",
              static_cast<int>(mvdb::query_fanout_mode()),
              mvdb::query_fanout_min_rows(), threads());
  mvdb::query_pool_startup_probe();
  std::printf("threads_after=%d\nwanted=%d\n", threads(),
              int(mvdb::query_fanout_wanted(10000)));
  return 0;
}

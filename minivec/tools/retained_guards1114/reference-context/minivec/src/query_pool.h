// query_pool.h — calibrated intra-query scan fan-out
//
// A fan-out has to pay a sleep/wake round trip on both sides before it can
// split any work, so whether a second thread helps is a property of the host,
// not of the program: on a bare-metal kernel the handshake is single-digit
// microseconds and the split wins, while under a hypervisor it can exceed the
// half-scan it is trying to save. The decision is therefore measured at
// startup and taken per query, never baked in at compile time.
#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <signal.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace mvdb {
namespace query_pool_detail {

// The only spin in the fan-out is the join, and it is armed only for the query
// that decided to fan out: the calling thread is on its own critical path, so
// it spins briefly before sleeping. Bounded well under 2000 pause iterations,
// measured at ~9 us, and zero for every query that stays serial.
constexpr int kJoinSpin = 200;
constexpr size_t kWorkerStack = 256u << 10;
constexpr int kProbeRounds = 200;
constexpr int kProbeWarmup = 8;

// Measured on the reference host: a 10,000-row unfiltered scan costs ~100 us,
// i.e. ~10 ns per candidate row. Used only to predict whether the split pays;
// the decision is a threshold, not a measurement of this query.
constexpr double kScanNsPerRow = 10.0;
// Predicted saving must clear this before a second thread is worth using.
constexpr double kMinGainUs = 15.0;

inline void spin_hint() {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

inline void futex_wait(std::atomic<uint32_t>* word, uint32_t expected) {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAIT_PRIVATE, expected,
            nullptr, nullptr, 0);
}
inline void futex_wake(std::atomic<uint32_t>* word) {
  ::syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), FUTEX_WAKE_PRIVATE, 1,
            nullptr, nullptr, 0);
}

struct Pool {
  std::atomic<uint32_t> seq{0};   // the caller publishes a job by bumping this
  std::atomic<uint32_t> ready{0}; // Worker inherited its blocked termination mask.
  std::exception_ptr error; // Published by done release, consumed after acquire.
  std::atomic<uint32_t> done{0};  // the worker echoes the generation it finished
  // Zero between queries: the worker sleeps instead of holding a core warm.
  std::atomic<int> idle_spin{0};
  void* ctx = nullptr;
  void (*body)(void*, unsigned) = nullptr;
};

inline Pool& pool() {
  static Pool instance;
  return instance;
}

inline void* worker_main(void* arg) {
  Pool& p = *static_cast<Pool*>(arg);
  p.ready.store(1, std::memory_order_release);
  futex_wake(&p.ready);
  uint32_t seen = 0;
  for (;;) {
    const int budget = p.idle_spin.load(std::memory_order_relaxed);
    uint32_t now = p.seq.load(std::memory_order_acquire);
    for (int spin = 0; now == seen && spin < budget; ++spin) {
      spin_hint();
      now = p.seq.load(std::memory_order_acquire);
    }
    if (now == seen) {
      futex_wait(&p.seq, seen);
      continue;
    }
    seen = now;
    try { p.body(p.ctx, 1); }
    catch (...) { p.error = std::current_exception(); }
    p.done.store(seen, std::memory_order_release);
    futex_wake(&p.done);
  }
  return nullptr;
}

inline bool start_worker() {
  static const bool started = [] {
    Pool& p = pool();
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    if (pthread_attr_setstacksize(&attr, kWorkerStack) != 0 ||
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    // Block before creation so there is no startup window where the worker can
    // consume a process termination signal instead of interrupting main's read.
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, &worker_main, &p);
    const int restore = pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    pthread_attr_destroy(&attr);
    if (restore != 0) std::terminate(); // Cannot safely continue with main masked.
    if (rc != 0) return false;
    while (!p.ready.load(std::memory_order_acquire)) futex_wait(&p.ready, 0);
    return true;
  }();
  return started;
}

}  // namespace query_pool_detail

enum class FanoutMode { Never, Force, Adaptive };

// One core, or an explicit request for a serial query, rules the fan-out out
// entirely; an explicit MVDB_QUERY_THREADS=2 selects it regardless of the
// handshake, which is what makes the decision testable on a slow host.
inline FanoutMode query_fanout_mode() {
  static const FanoutMode mode = [] {
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    const bool two_cores = online >= 2;
    const char* single = std::getenv("MVDB_SINGLE_THREAD");
    if (single && *single && *single != '0') return FanoutMode::Never;
    const char* want = std::getenv("MVDB_QUERY_THREADS");
    if (want && *want) {
      const long value = std::strtol(want, nullptr, 10);
      // Never above two threads: the platform runs on two cores.
      if (value >= 2 && two_cores) return FanoutMode::Force;
      return FanoutMode::Never;
    }
    return two_cores ? FanoutMode::Adaptive : FanoutMode::Never;
  }();
  return mode;
}

// Candidate rows below which an extra thread costs more than it saves.
inline size_t query_fanout_min_rows() {
  static const size_t rows = [] {
    const char* text = std::getenv("MVDB_QUERY_FANOUT_ROWS");
    if (!text || !*text) return size_t(1024);
    const long value = std::strtol(text, nullptr, 10);
    return value < 0 ? size_t(1024) : static_cast<size_t>(value);
  }();
  return rows;
}

namespace query_pool_detail {
inline void empty_body(void*, unsigned) {}
inline double micros(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double, std::micro>(d).count();
}
}  // namespace query_pool_detail

// Runs body(ctx,0) on the calling thread and body(ctx,1) on the shared worker,
// then returns after both finished. With allow_spin the worker may spin for the
// duration of this one query; it always returns to its sleep afterwards.
inline void query_fanout_run(void* ctx, void (*body)(void*, unsigned), bool allow_spin) {
  using namespace query_pool_detail;
  static const bool started = start_worker();
  if (!started) {
    std::exception_ptr errors[2];
    for (unsigned half = 0; half < 2; ++half) {
      try { body(ctx, half); } catch (...) { errors[half] = std::current_exception(); }
    }
    if (errors[0]) std::rethrow_exception(errors[0]);
    if (errors[1]) std::rethrow_exception(errors[1]);
    return;
  }
  Pool& p = pool();
  // The worker never spins for work, not even inside a fan-out: by the time it
  // is idle its half is done, so a spin there can only burn CPU, never shorten
  // this query. The only spin worth arming is the join below.
  p.idle_spin.store(0, std::memory_order_relaxed);
  p.error = nullptr;
  p.ctx = ctx;
  p.body = body;
  const uint32_t generation = p.seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  futex_wake(&p.seq);
  std::exception_ptr caller_error;
  try { body(ctx, 0); } catch (...) { caller_error = std::current_exception(); }
  uint32_t now = p.done.load(std::memory_order_acquire);
  for (int spin = 0; now != generation && spin < (allow_spin ? kJoinSpin : 0); ++spin) {
    spin_hint();
    now = p.done.load(std::memory_order_acquire);
  }
  while (now != generation) {
    futex_wait(&p.done, now);
    now = p.done.load(std::memory_order_acquire);
  }
  const std::exception_ptr worker_error = p.error;
  p.error = nullptr;
  p.ctx = nullptr;
  p.body = nullptr;
  // Back to sleep: between two queries this thread must hold no core.
  p.idle_spin.store(0, std::memory_order_relaxed);
  if (caller_error) std::rethrow_exception(caller_error);
  if (worker_error) std::rethrow_exception(worker_error);
}

// Median cost of one sleep/wake handshake, in microseconds. The probe spends
// kProbeWarmup rounds on a cold worker and thread start, which are discarded.
inline double measure_wake_us() {
  using namespace query_pool_detail;
  if (!start_worker()) return 1e9;
  Pool& p = pool();
  p.idle_spin.store(0, std::memory_order_relaxed);
  double samples[kProbeRounds];
  int collected = 0;
  for (int round = 0; round < kProbeWarmup + kProbeRounds; ++round) {
    const auto mark = std::chrono::steady_clock::now();
    query_fanout_run(nullptr, &empty_body, false);
    const double elapsed = micros(std::chrono::steady_clock::now() - mark);
    if (round >= kProbeWarmup) samples[collected++] = elapsed;
  }
  if (collected == 0) return 1e9;
  for (int i = 1; i < collected; ++i) {  // insertion sort: only the median matters
    const double value = samples[i];
    int j = i - 1;
    while (j >= 0 && samples[j] > value) { samples[j + 1] = samples[j]; --j; }
    samples[j + 1] = value;
  }
  return samples[collected / 2];
}

// The handshake cost of this host: injected value if given, measured otherwise.
inline double query_wake_us() {
  static const double wake = [] {
    const char* text = std::getenv("MVDB_WAKE_US_OVERRIDE");
    if (text && *text) {
      const double injected = std::strtod(text, nullptr);
      if (injected >= 0) {
        std::fprintf(stderr, "mvdb: fanout wake_us=%.2f (injected)\n", injected);
        return injected;
      }
    }
    const double measured = measure_wake_us();
    std::fprintf(stderr, "mvdb: fanout wake_us=%.2f (measured, median of %d handshakes)\n",
                 measured, query_pool_detail::kProbeRounds);
    return measured;
  }();
  return wake;
}

// Startup probe: calibrate before the first query so no query pays for it.
inline void query_pool_startup_probe() {
  if (query_fanout_mode() != FanoutMode::Never) (void)query_wake_us();
}

// Whether this query's candidate count repays a handshake.
inline bool query_fanout_wanted(size_t rows) {
  using namespace query_pool_detail;
  const FanoutMode mode = query_fanout_mode();
  if (mode == FanoutMode::Never) return false;
  if (mode == FanoutMode::Force) {
    static int logged = 0;
    if (logged++ < 4)
      std::fprintf(stderr, "mvdb: fanout rows=%zu -> 2 threads (forced)\n", rows);
    return true;
  }
  const size_t min_rows = query_fanout_min_rows();
  const double wake = query_wake_us();
  const double scan_us = static_cast<double>(rows) * kScanNsPerRow * 1e-3;
  const double gain_us = scan_us * 0.5 - 2.0 * wake;
  const bool wanted = rows >= min_rows && gain_us >= kMinGainUs;
  static int accepted = 0, rejected = 0;
  if ((wanted ? accepted++ : rejected++) < 4)
    std::fprintf(stderr,
                 "mvdb: fanout rows=%zu scan=%.1fus wake=%.2fus predicted_gain=%.1fus -> %s\n",
                 rows, scan_us, wake, gain_us, wanted ? "2 threads" : "1 thread");
  return wanted;
}

}  // namespace mvdb

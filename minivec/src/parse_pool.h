// parse_pool.h — a fixed, tiny worker pool for splitting ONE statement's row
// literal parsing across cores.
//
// Scope rules (they are the reason this lives on its own):
//   * a worker only runs pure conversion code (literal text -> Value); it never
//     touches storage, the WAL, or shared parser state;
//   * the calling thread is one of the parts, so a 2-core box uses the caller
//     plus exactly one worker and never parks a core;
//   * the pool is created once, on first use, and deliberately leaked, so a
//     statement can never be scheduled onto a half-destroyed singleton;
//   * exceptions are captured per part and rethrown on the caller, earliest part
//     first, so the error reported is the one a sequential parse would hit.
#pragma once

#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <pthread.h>

namespace mvdb {

// min(2, hardware_concurrency), overridable with MVDB_PARSE_THREADS.
// MVDB_PARSE_THREADS=1 restores single-threaded behaviour exactly.
inline int parse_thread_count() {
  const char* env = std::getenv("MVDB_PARSE_THREADS");
  if (env != nullptr && *env != '\0') {
    const int requested = std::atoi(env);
    if (requested >= 1 && requested <= 8) return requested;
  }
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw < 2u) return 1;
  return 2;
}

class ParsePool {
 public:
  static ParsePool& instance() {
    static ParsePool* pool = new ParsePool(parse_thread_count());
    return *pool;
  }

  // Total parts, caller included.
  int parts() const { return parts_; }

  // Runs fn(0 .. n-1) split into contiguous slices. With a single part, or
  // fewer than two items, it runs in the caller and lets an exception
  // propagate exactly as it always did.
  void run(std::size_t n, const std::function<void(std::size_t)>& fn) {
    if (parts_ < 2 || n < 2) {
      for (std::size_t i = 0; i < n; ++i) fn(i);
      return;
    }
    std::vector<std::exception_ptr> errors(static_cast<std::size_t>(parts_), nullptr);
    Job job{n, parts_, &fn, errors.data()};
    {
      std::lock_guard<std::mutex> lock(mu_);
      job_ = &job;
      busy_ = parts_ - 1;
      ++generation_;
      cv_worker_.notify_all();
    }
    errors[0] = run_part(job, 0);
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_main_.wait(lock, [this] { return busy_ == 0; });
      job_ = nullptr;
    }
    for (const std::exception_ptr& error : errors)
      if (error != nullptr) std::rethrow_exception(error);
  }

 private:
  struct Job {
    std::size_t n;
    int parts;
    const std::function<void(std::size_t)>* fn;
    std::exception_ptr* errors;
  };

  // Contiguous slice of [0,n): part p owns [n*p/parts, n*(p+1)/parts).
  static std::exception_ptr run_part(const Job& job, int part) {
    const std::size_t begin =
        job.n * static_cast<std::size_t>(part) / static_cast<std::size_t>(job.parts);
    const std::size_t end =
        job.n * static_cast<std::size_t>(part + 1) / static_cast<std::size_t>(job.parts);
    try {
      for (std::size_t i = begin; i < end; ++i) (*job.fn)(i);
    } catch (...) {
      return std::current_exception();
    }
    return nullptr;
  }

  explicit ParsePool(int parts) : parts_(parts < 1 ? 1 : parts) {
    if (parts_ < 2) return;
    // A worker must never receive the process's shutdown signal while the
    // foreground thread is blocked in read(2). Block before creation so even a
    // newly scheduled worker inherits the mask without a startup race.
    sigset_t blocked, saved;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    if (pthread_sigmask(SIG_BLOCK, &blocked, &saved) != 0) {
      parts_ = 1;  // Safe serial fallback if signal ownership cannot be assured.
      return;
    }
    try {
      workers_.reserve(static_cast<std::size_t>(parts_ - 1));
      for (int w = 1; w < parts_; ++w)
        workers_.emplace_back([this, w] { worker_loop(w); });
      std::unique_lock<std::mutex> lock(mu_);
      cv_main_.wait(lock, [this] { return ready_ == parts_ - 1; });
    } catch (...) {
      // Partial construction can already own joinable threads. Stop and join
      // every one before the vector or synchronization objects are destroyed.
      {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        cv_worker_.notify_all();
      }
      for (std::thread& worker : workers_)
        if (worker.joinable()) worker.join();
      pthread_sigmask(SIG_SETMASK, &saved, nullptr);
      throw;
    }
    pthread_sigmask(SIG_SETMASK, &saved, nullptr);
  }

  void worker_loop(int part) {
    std::unique_lock<std::mutex> lock(mu_);
    ++ready_;
    cv_main_.notify_one();
    std::uint64_t seen = 0;
    for (;;) {
      cv_worker_.wait(lock, [this, seen] { return stop_ || generation_ != seen; });
      if (stop_) return;
      seen = generation_;
      const Job* job = job_;
      lock.unlock();
      job->errors[static_cast<std::size_t>(part)] = run_part(*job, part);
      lock.lock();
      --busy_;
      cv_main_.notify_one();
    }
  }

  std::mutex mu_;
  std::condition_variable cv_worker_;
  std::condition_variable cv_main_;
  Job* job_ = nullptr;
  std::uint64_t generation_ = 0;
  int busy_ = 0;
  int ready_ = 0;
  int parts_;
  bool stop_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace mvdb

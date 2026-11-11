#pragma once
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#if defined(__unix__) || defined(__APPLE__)
#include <pthread.h>
#include <signal.h>
#endif

namespace mvdb {

// One optional worker, reused across statements. The submission lock covers
// both jobs and their completion, so queued reference captures never outlive
// run_pair(), including when either job throws. Jobs must not change FP controls.
class ResidentTopKWorker {
 public:
  using ThreadFactory=std::thread(*)(void(*)(void*),void*);
  explicit ResidentTopKWorker(ThreadFactory factory=&start_thread):factory_(factory) {}
  ResidentTopKWorker(const ResidentTopKWorker&)=delete;
  ResidentTopKWorker& operator=(const ResidentTopKWorker&)=delete;
  ~ResidentTopKWorker() {
    std::unique_lock<std::mutex> submission(submission_);
    {
      std::lock_guard<std::mutex> state(state_);stopping_=true;
    }
    ready_.notify_one();
    if(thread_.joinable())thread_.join();
  }

  // false means neither job ran: the caller may use its ordinary serial path.
  // Otherwise both jobs finish before this returns or propagates an exception.
  template<class Background,class Foreground>
  bool run_pair(Background&& background,Foreground&& foreground) {
    std::unique_lock<std::mutex> submission(submission_);
    if(unavailable_)return false;
    std::function<void()> queued;
    try {queued=std::forward<Background>(background);}
    catch(const std::bad_alloc&) {return false;}
    if(!thread_.joinable()) {
      try {thread_=factory_(&entry,this);}
      catch(...) {unavailable_=true;return false;}
      if(!thread_.joinable()){unavailable_=true;return false;}
      std::unique_lock<std::mutex> state(state_);
      done_.wait(state,[this]{return startup_ready_;});
      if(!startup_ok_) {
        state.unlock();thread_.join();unavailable_=true;return false;
      }
    }
    {
      std::lock_guard<std::mutex> state(state_);
      task_=std::move(queued);error_=nullptr;finished_=false;pending_=true;
    }
    ready_.notify_one();
    std::exception_ptr foreground_error;
    try {std::forward<Foreground>(foreground)();}
    catch(...) {foreground_error=std::current_exception();}
    std::exception_ptr background_error;
    {
      std::unique_lock<std::mutex> state(state_);
      done_.wait(state,[this]{return finished_;});
      background_error=std::move(error_);
    }
    if(foreground_error)std::rethrow_exception(foreground_error);
    if(background_error)std::rethrow_exception(background_error);
    return true;
  }

 private:
  static std::thread start_thread(void(*function)(void*),void* argument) {
    return std::thread(function,argument);
  }
  static void entry(void* argument) {
    auto* self=static_cast<ResidentTopKWorker*>(argument);
    bool ok=true;
#if defined(__unix__) || defined(__APPLE__)
    // Leave termination delivery to the main thread: it may be blocked in
    // read(2), whose EINTR wakes the process's existing stop-signal handling.
    sigset_t signals;
    ok=::sigemptyset(&signals)==0 && ::sigaddset(&signals,SIGTERM)==0 &&
       ::sigaddset(&signals,SIGINT)==0 && ::pthread_sigmask(SIG_BLOCK,&signals,nullptr)==0;
#endif
    {
      std::lock_guard<std::mutex> state(self->state_);
      self->startup_ok_=ok;self->startup_ready_=true;
    }
    self->done_.notify_one();
    if(ok)self->loop();
  }
  void loop() {
    for(;;) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> state(state_);
        ready_.wait(state,[this]{return stopping_ || pending_;});
        if(stopping_)return;
        task=std::move(task_);pending_=false;
      }
      std::exception_ptr error;
      try {task();}catch(...) {error=std::current_exception();}
      task=nullptr; // Destroy captures before publishing completion.
      {
        std::lock_guard<std::mutex> state(state_);
        error_=std::move(error);finished_=true;
      }
      done_.notify_one();
    }
  }
  ThreadFactory factory_;
  std::mutex submission_,state_;
  std::condition_variable ready_,done_;
  std::thread thread_;
  std::function<void()> task_;
  std::exception_ptr error_;
  bool pending_=false,finished_=true,stopping_=false,unavailable_=false;
  bool startup_ready_=false,startup_ok_=false;
};

inline ResidentTopKWorker* resident_topk_worker() {
  // Even mutex/condition-variable allocation failures leave acceleration off.
  // The static owner destroys the worker and joins its thread at normal exit.
  static const std::unique_ptr<ResidentTopKWorker> worker=[]() noexcept {
    try {return std::unique_ptr<ResidentTopKWorker>(new ResidentTopKWorker);}
    catch(...) {return std::unique_ptr<ResidentTopKWorker>();}
  }();
  return worker.get();
}
} // namespace mvdb

// g++ -O2 -std=c++17 -pthread -Isrc tests/test_resident_topk_worker.cpp -o /tmp/test-topk-worker
#include "resident_topk_worker.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
using namespace mvdb;
static std::atomic<int> starts{0},alive{0},failed_starts{0};
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static std::thread counted_thread(void(*function)(void*),void* argument) {
  ++starts;
  return std::thread([=]{++alive;function(argument);--alive;});
}
static std::thread failed_thread(void(*)(void*),void*) {
  ++failed_starts;
  throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}
int main() {
  {
    ResidentTopKWorker unavailable(failed_thread);int calls=0;
    for(int i=0;i<2;++i)
      require(!unavailable.run_pair([&]{++calls;},[&]{++calls;}),"failed creation must fall back");
    require(calls==0 && failed_starts==1,"fallback must run neither job or repeatedly create threads");
  }
  {
    ResidentTopKWorker worker(counted_thread);std::thread::id first_worker;
    for(int i=0;i<128;++i) {
      int left=0,right=0;std::thread::id observed;
      require(worker.run_pair([&]{
#if defined(__unix__) || defined(__APPLE__)
                                sigset_t mask;require(::pthread_sigmask(SIG_SETMASK,nullptr,&mask)==0,"read helper signal mask");
                                require(::sigismember(&mask,SIGTERM)==1 && ::sigismember(&mask,SIGINT)==1,
                                        "worker must not intercept the main thread's termination signals");
#endif
                                observed=std::this_thread::get_id();right=i+2;},
                              [&]{left=i+1;}),"worker available");
      require(left==i+1 && right==i+2,"both results must be published before returning");
      require(observed!=std::this_thread::get_id(),"background runs on another thread");
      if(i==0)first_worker=observed;
      require(first_worker==observed && starts==1,"one persistent thread across queries");
    }
    bool foreground_ran=false;
    try {
      worker.run_pair([]{throw std::runtime_error("background");},[&]{foreground_ran=true;});
      require(false,"background exception lost");
    } catch(const std::runtime_error& e) {
      require(std::string(e.what())=="background" && foreground_ran,"background exception propagation");
    }
    bool background_finished=false;
    try {
      worker.run_pair([&]{std::this_thread::sleep_for(std::chrono::milliseconds(5));background_finished=true;},
                      []{throw std::runtime_error("foreground");});
      require(false,"foreground exception lost");
    } catch(const std::runtime_error& e) {
      require(std::string(e.what())=="foreground" && background_finished,
              "foreground failure must still wait for captured references to finish");
    }
    try {
      worker.run_pair([]{throw std::runtime_error("second");},[]{throw std::runtime_error("first");});
      require(false,"both exceptions lost");
    } catch(const std::runtime_error& e) {require(std::string(e.what())=="first","deterministic exception priority");}
    int reused=0;
    require(worker.run_pair([&]{reused=1;},[]{}) && reused==1,"reuse after failure");

    std::atomic<int> active{0},maximum{0},completed{0},failures{0};
    auto job=[&] {
      const int now=++active;int previous=maximum.load();
      while(now>previous && !maximum.compare_exchange_weak(previous,now)){}
      std::this_thread::yield();--active;++completed;
    };
    std::vector<std::thread> callers;
    for(int i=0;i<4;++i)callers.emplace_back([&] {
      try {for(int j=0;j<32;++j)require(worker.run_pair(job,job),"concurrent submit");}
      catch(...) {++failures;}
    });
    for(auto& caller:callers)caller.join();
    require(!failures && completed==256 && maximum<=2 && active==0,
            "a submission remains exclusive through completion of both jobs");
    require(starts==1,"concurrent callers must reuse the same helper");
  }
  require(alive==0,"destructor joins the waiting worker");
  std::cout<<"PASS persistent TopK worker, serialized submissions, exception wait/propagation, creation fallback and join\n";
}

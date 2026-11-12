// g++ -std=c++17 -O2 -pthread -Isrc tests/test_query_pool_safety.cpp
//     -Wl,--wrap=pthread_create -o /tmp/test-query-pool
// Run normally and with --create-fails (separate processes).
#include "query_pool.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <iostream>
#include <signal.h>
#include <unistd.h>

static bool fail_create=false;
static pthread_t worker;
extern "C" int __real_pthread_create(pthread_t*,const pthread_attr_t*,void*(*)(void*),void*);
extern "C" int __wrap_pthread_create(pthread_t* t,const pthread_attr_t* a,void*(*f)(void*),void* p) {
  if(fail_create)return EAGAIN;
  const int result=__real_pthread_create(t,a,f,p);
  if(!result)worker=*t;
  return result;
}
struct Job { unsigned throws; bool completed[2]={false,false}; };
static void run(void* raw,unsigned half) {
  Job& job=*static_cast<Job*>(raw);
  if(half==1)usleep(1000); // Main must wait even when its own callback throws.
  job.completed[half]=true;
  if(job.throws&(1u<<half))throw std::runtime_error(half ? "worker" : "caller");
}
static volatile sig_atomic_t signals=0;
static void term(int) { ++signals; }
int main(int argc,char** argv) {
  alarm(15);
  fail_create=argc>1 && std::strcmp(argv[1],"--create-fails")==0;
  struct sigaction action{};action.sa_handler=term;sigemptyset(&action.sa_mask);
  assert(sigaction(SIGTERM,&action,nullptr)==0);
  assert(sigaction(SIGINT,&action,nullptr)==0);
  for(unsigned repeat=0;repeat<3;++repeat)for(unsigned throws:{1u,2u,3u,0u}) {
    Job job{throws};bool caught=false;
    try {mvdb::query_fanout_run(&job,run,true);}
    catch(const std::runtime_error& error) {
      caught=true;
      assert(std::string(error.what())==((throws&1)?"caller":"worker"));
    }
    assert(caught==(throws!=0));assert(job.completed[0] && job.completed[1]);
  }
  if(!fail_create) {
    // A worker-targeted termination signal must remain blocked while idle.
    assert(pthread_kill(worker,SIGTERM)==0);assert(pthread_kill(worker,SIGINT)==0);
    usleep(20000);assert(signals==0);
    Job job{0};mvdb::query_fanout_run(&job,run,false);
    assert(job.completed[0] && job.completed[1]);assert(signals==0);
  }
  // Main's original mask was restored and process termination reaches it.
  assert(kill(getpid(),SIGTERM)==0);
  assert(signals==1);
  std::cout<<"PASS query pool exceptions, next job, idle signals, create fallback="<<fail_create<<"\n";
}

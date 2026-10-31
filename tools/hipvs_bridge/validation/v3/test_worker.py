"""Exercise the actual worker class with concurrent callers and failed startup."""
from pathlib import Path
import subprocess,sys,tempfile
source=Path(sys.argv[1]).read_text()
worker=source[source.index('class CuvsSearchWorker {'):source.index('// [BATCH] Feed nq')]
prefix=r'''
#include <pthread.h>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <cerrno>
static bool fail_start = false;
static int create_test(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *p) {
  return fail_start ? EAGAIN : pthread_create(t,a,fn,p);
}
struct ObCuvsJob { int input; int output = -1; pthread_t executor = {}; };
static void *ob_cuvs_job(void *arg) {
  auto *j=static_cast<ObCuvsJob *>(arg);
  pthread_attr_t attr;assert(pthread_getattr_np(pthread_self(),&attr)==0);
  size_t size=0;assert(pthread_attr_getstacksize(&attr,&size)==0);
  pthread_attr_destroy(&attr);assert(size>=32UL*1024*1024);
  j->output=j->input*7+3;j->executor=pthread_self();return nullptr;
}
#define pthread_create create_test
'''
suffix=r'''
#undef pthread_create
int main() {
  for(int cycle=0;cycle<10;++cycle) {
    CuvsSearchWorker worker;ObCuvsJob first{0};assert(worker.run(first));
    assert(!pthread_equal(first.executor,pthread_self()));
    std::vector<std::thread> callers;
    for(int c=0;c<8;++c) callers.emplace_back([&,c]{
      for(int i=0;i<200;++i) {ObCuvsJob j{c*200+i};assert(worker.run(j));
        assert(j.output==j.input*7+3);assert(pthread_equal(j.executor,first.executor));}
    });
    for(auto &thread:callers)thread.join();
  }
  fail_start=true;
  {CuvsSearchWorker unavailable;ObCuvsJob j{8};assert(!unavailable.run(j));assert(j.output==-1);}
}
'''
with tempfile.TemporaryDirectory(prefix='cagra-worker-') as directory:
    d=Path(directory);(d/'test.cpp').write_text(prefix+worker+suffix)
    subprocess.run(['g++','-std=c++17','-O1','-g','-fsanitize=address,undefined','-pthread',str(d/'test.cpp'),'-o',str(d/'test')],check=True)
    subprocess.run([str(d/'test')],check=True,timeout=90)
print('PASS: 16000 concurrent handoffs, one worker per lifetime, 32MB stack, shutdown/join, failed startup; ASan/UBSan')

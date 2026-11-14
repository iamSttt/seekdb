// Test-only allocation-failure sweep. No production fault hooks or timed work.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <execinfo.h>
#include <filesystem>
#include <iostream>
#include <new>
#include <unistd.h>

namespace {
thread_local bool tracking=false, injected=false;
thread_local size_t allocations=0, fail_at=0;
thread_local const char* current_query="setup";
thread_local size_t current_ordinal=0, failed_bytes=0;
void diagnostic_terminate() noexcept {
  tracking=false;
  std::fprintf(stderr,"OOM_TERMINATE query=%s ordinal=%zu failed_bytes=%zu allocations=%zu injected=%d\n",
               current_query,current_ordinal,failed_bytes,allocations,int(injected));
  void* frames[64];const int count=::backtrace(frames,64);
  ::backtrace_symbols_fd(frames,count,STDERR_FILENO);
  std::_Exit(86);
}
void* allocate(size_t bytes) {
  if(tracking && ++allocations==fail_at) {
    fail_at=0;injected=true;failed_bytes=bytes;throw std::bad_alloc();
  }
  void* p=std::malloc(bytes ? bytes : 1);
  if(!p)throw std::bad_alloc();
  return p;
}
}
void* operator new(size_t n) {return allocate(n);}
void* operator new[](size_t n) {return allocate(n);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,size_t) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}

int main() {
  using namespace mvdb;
  std::set_terminate(diagnostic_terminate);
  alarm(120);
  char path[]="/tmp/mvdb-typed-oom-XXXXXX";assert(mkdtemp(path));
  size_t failed_allocations=0,queries=0;
  {
    Database db;std::string error,out;int code=0;assert(db.open(path,&error));
    auto call=[&](const std::string& sql) {
      Statement st;assert(parse_sql(sql,&st,&error,&code));
      assert(execute_json(db,st,&out,&error,&code,false));
    };
    call("CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(100),v VECTOR(128));");
    std::string vector="[";
    for(int i=0;i<128;++i) {if(i)vector+=',';vector+="0.125";}
    vector+=']';
    for(int i=0;i<5;++i)
      call("INSERT INTO t VALUES("+std::to_string(i)+",'valid UTF8 "+std::string("\xe4\xb8\xad")+"',"+vector+");");
    for(const char* sql:{"SELECT id,s FROM t ORDER BY id LIMIT 10;",
                        "SELECT id,s,v FROM t ORDER BY id LIMIT 10;",
                        "SELECT id,s,v FROM t;",
                        "SELECT id,s,v FROM t ORDER BY id DESC LIMIT 4096;",
                        "SELECT id,s,v FROM t WHERE id=100;"}) {
      Statement st;assert(parse_sql(sql,&st,&error,&code));
      std::string expected;
      assert(execute_json(db,st,&expected,&error,&code,false)); // warm shared state
      size_t count=0;
      {
        std::string result,err;int ec=0;
        allocations=0;fail_at=0;injected=false;tracking=true;
        const bool ok=execute_json(db,st,&result,&err,&ec,false);
        tracking=false;count=allocations;
        assert(ok && ec==0 && err.empty() && result==expected && !injected && count>0);
      }
      for(size_t ordinal=1;ordinal<=count;++ordinal) {
        std::string result,err;int ec=0;
        current_query=sql;current_ordinal=ordinal;failed_bytes=0;
        allocations=0;fail_at=ordinal;injected=false;tracking=true;
        const bool ok=execute_json(db,st,&result,&err,&ec,false);
        tracking=false;fail_at=0;
        assert(injected && !ok && ec!=0 && result.empty());
        ++failed_allocations;
        // No stale borrowed string, leaked partial result, or poisoned query state.
        assert(execute_json(db,st,&result,&err,&ec,false));
        assert(result==expected && err.empty() && ec==0);
      }
      ++queries;
    }
  }
  std::filesystem::remove_all(path);
  std::cout<<"{\"status\":\"passed\",\"queries\":"<<queries
           <<",\"injected_allocations\":"<<failed_allocations
           <<",\"scope\":\"main-thread one-shot allocation failures; no path or performance assertion\"}\n";
}

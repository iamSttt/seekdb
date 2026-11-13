// Opt-in compound-reuse gate: old single-column candidate selection first,
// multi-column planning only after a successful nonempty compound scan.
// g++ -std=c++17 -pthread -Isrc -DMVDB_MULTI_POSTING_REUSE_ONLY
// -DMVDB_FILTER_DIAGNOSTICS tests/test_filter_reuse.cpp
// src/{db,exec,sql,value,flat_index}.cpp -o test_filter_reuse
#include "db.h"
#include "filter_diagnostics.h"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#if !defined(MVDB_MULTI_POSTING_REUSE_ONLY) || defined(MVDB_DISABLE_MULTI_POSTING) || !defined(MVDB_FILTER_DIAGNOSTICS)
#error "Requires reuse-only multi-posting with diagnostics"
#endif

namespace {
struct Allocation {void* pointer=nullptr;size_t bytes=0;};
Allocation allocations[32];
bool meter=false;
size_t live_bytes=0,peak_bytes=0;
int fail_after=-1;
void release(void* pointer) {
  for(auto& item:allocations)if(item.pointer==pointer && pointer) {
    live_bytes-=item.bytes;item=Allocation{};break;
  }
  std::free(pointer);
}
}
void* operator new[](size_t bytes) {
  if(void* p=std::malloc(bytes?bytes:1))return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept {release(p);}
void operator delete[](void* p,size_t) noexcept {release(p);}
void* operator new[](size_t bytes,const std::nothrow_t&) noexcept {
  if(fail_after>=0 && fail_after--==0)return nullptr;
  void* p=nullptr;try {p=::operator new[](bytes);} catch(...) {return nullptr;}
  if(meter) {
    bool saved=false;
    for(auto& item:allocations)if(!item.pointer) {item={p,bytes};saved=true;break;}
    assert(saved);live_bytes+=bytes;peak_bytes=std::max(peak_bytes,live_bytes);
  }
  return p;
}
void operator delete[](void* p,const std::nothrow_t&) noexcept {release(p);}

using namespace mvdb;
namespace {
std::string error;int code=0;
struct Directory {
  std::string path;
  Directory() {char p[]="/tmp/mvdb-filter-reuse-XXXXXX";assert(mkdtemp(p));path=p;}
  ~Directory() {std::filesystem::remove_all(path);}
};
struct Diagnostic {
  FilterDiagnostics data;
  Diagnostic() {assert(!filter_diag_current);filter_diag_current=&data;}
  ~Diagnostic() {filter_diag_current=nullptr;}
};
Cond eq(const char* name,int64_t value) {
  Cond c;c.column=name;c.op=Cond::EQ;c.value=Value::ofInt(value);return c;
}
void create(Database& db,int64_t rows=1000) {
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;
  Column b;b.name="b";b.type=Type::BIGINT;
  Column v;v.name="v";v.type=Type::VECTOR;v.len=5;
  assert(db.create_table("t",{id,a,b,v},&error,&code));
  assert(db.begin(&error,&code));
  for(int64_t i=0;i<rows;++i)
    assert(db.insert("t",{Value::ofInt(i),Value::ofInt(i<500),Value::ofInt(i%100==0?7:0),
        Value::ofVec({float(i),0,0,0,0})},&error,&code));
  assert(db.commit(&error,&code));assert(db.create_vector_index("t","v","vi",&error,&code));
}
FilterDiagnostics check(Database& db,const std::vector<Cond>& conds) {
  std::map<int64_t,std::vector<float>> expected,actual;
  // Independent row scan supplies exact values/counts, not index candidates.
  db.scan("t",conds,[&](const Row& row) {expected.emplace(row.vals[0].i,row.vals[3].v);return true;});
  Diagnostic diag;
  db.scan_vectors("t",conds,[&](int64_t pk,const float* v,size_t n) {
    assert(actual.emplace(pk,std::vector<float>(v,v+n)).second);return true;
  });
  assert(expected==actual);return diag.data;
}
void reset(Database& db) {
  db.table("t")->invalidate_vector_index();
  FlatIndex::ResidentView view;assert(db.resident_vector_view("t",&view));
  assert(!db.table("t")->vector_index->compound_filter_seen());
}
void database_cases(const std::string& path) {
  Database db;assert(db.open(path,&error));create(db);
  const std::vector<Cond> compound={eq("a",1),eq("b",7)};
  auto first=check(db,compound);
  assert(first.sample_rows==0 && first.plan_choices[1]==0 && first.plan_choices[2]==0);
  assert(first.posting_builds[1]==1 && first.vector_visits==500);
  assert(db.table("t")->vector_index->compound_filter_seen());
  auto second=check(db,compound);
  assert(second.plan_choices[2]==1 && second.vector_visits==10 && second.posting_builds[2]==1);
  auto third=check(db,compound);assert(third.posting_builds[2]==0 && third.posting_hits[2]==1);
  // Single-equality path keeps its existing direct plan, without warming the
  // compound gate or adding sampling. The next compound query remains first.
  reset(db);auto single=check(db,{eq("b",7)});
  assert(single.plan_choices[2]==1 && single.sample_rows==0);
  assert(!db.table("t")->vector_index->compound_filter_seen());
  first=check(db,compound);assert(first.plan_choices[2]==0 && first.sample_rows==0);
  // Empty results and thrown consumers must never admit compound reuse.
  reset(db);auto empty=check(db,{eq("a",1),eq("b",999)});
  assert(!db.table("t")->vector_index->compound_filter_seen() && empty.sample_rows==0);
  empty=check(db,{eq("a",1),eq("b",999)});
  assert(!db.table("t")->vector_index->compound_filter_seen() && empty.posting_hits[1]==1 && empty.posting_builds[1]==0);
  bool threw=false;
  try {db.scan_vectors("t",compound,[](int64_t,const float*,size_t)->bool {throw std::runtime_error("consumer failure");});}
  catch(const std::runtime_error&) {threw=true;}
  assert(threw && !db.table("t")->vector_index->compound_filter_seen());
  first=check(db,compound);assert(first.sample_rows==0 && first.plan_choices[2]==0);
  // Optional allocation failure falls back exactly but does not warm the gate.
  for(int failure=0;failure<3;++failure) {
    reset(db);fail_after=failure;first=check(db,compound);fail_after=-1;
    assert(!db.table("t")->vector_index->compound_filter_seen());
    assert(first.sample_rows==0 && first.vector_visits==1000);
    first=check(db,compound);assert(first.sample_rows==0 && db.table("t")->vector_index->compound_filter_seen());
  }
  // Every new write version, statement rollback, full rollback and reopen
  // resets the physical-index state. A first query is counted only once.
  for(int i=0;i<3;++i) {
    assert(db.update_rows("t",{eq("id",0)},{{"a",Value::ofInt(i%2)}},&error,&code)==1);
    assert(!db.table("t")->vector_index);
    first=check(db,compound);assert(first.sample_rows==0 && first.plan_choices[2]==0);
    second=check(db,compound);assert(second.plan_choices[2]==1);
  }
  assert(db.begin(&error,&code));assert(db.begin_statement(&error,&code));
  assert(db.update_rows("t",{eq("id",0)},{{"a",Value::ofInt(99)}},&error,&code)==1);
  check(db,compound);assert(!db.end_statement(false,&error,&code));
  assert(!db.table("t")->vector_index);first=check(db,compound);assert(first.sample_rows==0);
  assert(db.update_rows("t",{eq("id",0)},{{"a",Value::ofInt(98)}},&error,&code)==1);
  check(db,compound);assert(db.rollback(&error,&code));assert(!db.table("t")->vector_index);
  first=check(db,compound);assert(first.sample_rows==0);
  assert(db.checkpoint(&error));db.close();assert(db.open(path,&error));
  assert(!db.table("t")->vector_index->compound_filter_seen());
  first=check(db,compound);assert(first.sample_rows==0 && first.plan_choices[2]==0);db.close();
}
void budgets_and_exceptions(const std::string& dir) {
  constexpr size_t n=512,base=n*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  constexpr size_t allowance=n*(2*sizeof(int64_t)+sizeof(uint32_t));
  const float vector[5]={0,1,2,3,4};
  auto fill=[&](FlatIndex& index) {for(size_t i=0;i<n;++i)index.add(i,i,vector,5);index.finish();};
  assert(!live_bytes);meter=true;peak_bytes=0;
  {
    FlatIndex index(dir,5,n,base+allowance);fill(index);
    auto reader=[](uint64_t slot) {return int64_t(slot%2);};
    auto visitor=[](int64_t pk,uint64_t,const float*,size_t) {assert(pk%2==1);return true;};
    assert(index.scan_first_integer_equal(0,1,reader,visitor));
    assert(live_bytes==n*(sizeof(int64_t)+sizeof(uint32_t)) && peak_bytes<=allowance);
    FlatIndex::IntegerRange range;range.has_lower=range.has_upper=true;range.lower=range.upper=1;
    const uint32_t* ordinals=nullptr;size_t count=0;
    assert(index.integer_ordinals(1,range,[](uint64_t slot){return int64_t(slot%17);},&ordinals,&count));
    assert(peak_bytes<=allowance); // Dense first cache is evicted before new scratch.
    assert(index.scan_first_integer_equal(0,1,reader,visitor));
    assert(peak_bytes<=allowance && index.resident_bytes()==base+allowance);
    bool threw=false;
    try {index.scan_first_integer_equal(2,1,[](uint64_t)->int64_t {throw std::runtime_error("read failure");},visitor);}
    catch(const std::runtime_error&) {threw=true;}
    assert(threw && !live_bytes && !index.compound_filter_seen());
    assert(index.scan_first_integer_equal(0,1,reader,visitor) && peak_bytes<=allowance);
  }
  assert(!live_bytes);
  {
    FlatIndex index(dir,5,n,base);fill(index);
    assert(!index.scan_first_integer_equal(0,1,[](uint64_t slot){return int64_t(slot);},
        [](int64_t,uint64_t,const float*,size_t){assert(false);return false;}));
    assert(!live_bytes && !index.compound_filter_seen());
  }
  assert(!live_bytes);meter=false;
}
void small_and_early_stop(const std::string& empty_path,const std::string& one_path) {
  const std::vector<Cond> compound={eq("a",1),eq("b",7)};
  Database empty;assert(empty.open(empty_path,&error));create(empty,0);
  check(empty,compound);check(empty,compound);
  assert(!empty.table("t")->vector_index->compound_filter_seen());empty.close();
  Database one;assert(one.open(one_path,&error));create(one,1);
  size_t visited=0;
  {
    Diagnostic first;
    one.scan_vectors("t",compound,[&](int64_t pk,const float*,size_t) {assert(pk==0);++visited;return false;});
    assert(visited==1 && first.data.sample_rows==0 && first.data.plan_choices[1]==0);
    assert(one.table("t")->vector_index->compound_filter_seen());
  }
  {
    Diagnostic second;
    one.scan_vectors("t",compound,[&](int64_t pk,const float*,size_t) {assert(pk==0);++visited;return false;});
    assert(visited==2 && second.data.plan_choices[1]==1 && second.data.posting_hits[1]==1);
    assert(one.table("t")->vector_index->compound_filter_seen());
  }
  one.close();
}
}
int main() {
  alarm(120);unsetenv("MVDB_ANN_PROBE_PERCENT");unsetenv("MVDB_ANN_OFF");
  Directory database,budget,empty,one;
  database_cases(database.path);budgets_and_exceptions(budget.path);small_and_early_stop(empty.path,one.path);
  std::cout<<"PASS compound first-query gate, repeated reuse, no-hit/error/reset, single equality, 0/1 rows, early stop, shared budget and allocation failures\n";
}

// Compile with -DMVDB_FILTER_DIAGNOSTICS, with and without
// -DMVDB_ENABLE_EQUALITY_FASTPATH. Link src/{db,exec,sql,value,flat_index}.cpp.
// Existing test_filter_paths and test_filter_reuse additionally gate unchanged
// range/compound selection, the shared 20N peak and reuse-only admission.
#include "db.h"
#include "exec.h"
#include "filter_diagnostics.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <unistd.h>

#if !defined(MVDB_FILTER_DIAGNOSTICS) || defined(MVDB_DISABLE_MULTI_POSTING)
#error "This gate requires multi-posting and diagnostic counters"
#endif

namespace { int fail_array_after=-1; }
void* operator new[](size_t bytes) {
  if(void* p=std::malloc(bytes?bytes:1))return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete[](void* p,size_t) noexcept {std::free(p);}
void* operator new[](size_t bytes,const std::nothrow_t&) noexcept {
  if(fail_array_after>=0 && fail_array_after--==0)return nullptr;
  try {return ::operator new[](bytes);} catch(...) {return nullptr;}
}
void operator delete[](void* p,const std::nothrow_t&) noexcept {std::free(p);}

using namespace mvdb;
namespace {
std::string error;int code=0;
struct Directory {
  std::string path;
  Directory() {char p[]="/tmp/mvdb-equality-fastpath-XXXXXX";assert(mkdtemp(p));path=p;}
  ~Directory() {std::filesystem::remove_all(path);}
};
struct Diagnostic {
  FilterDiagnostics data;
  Diagnostic() {assert(!filter_diag_current);filter_diag_current=&data;}
  ~Diagnostic() {filter_diag_current=nullptr;}
};
int64_t low_value(size_t slot) {
  const int64_t values[]={INT64_MIN,-7,0,9,INT64_MAX};return values[slot%5];
}
void direct_cases(const std::string& dir) {
  constexpr size_t n=512;
  constexpr size_t base=n*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  constexpr size_t allowance=n*(2*sizeof(int64_t)+sizeof(uint32_t));
  const float vector[5]={0,1,2,3,4};
  const auto fill=[&](FlatIndex& index,size_t rows) {
    for(size_t i=0;i<rows;++i)index.add(int64_t(i),i,vector,5);
    index.finish();
  };
  FlatIndex index(dir,5,n,base+allowance);fill(index,n);
  size_t reads[3]={};
  const auto value=[](size_t column,size_t slot) -> int64_t {
    if(column==0)return low_value(slot);
    if(column==1)return int64_t(slot%17);
    // Dense, but with a real duplicate run (not all unique values).
    return slot<2 ? INT64_MIN : slot==n-1 ? INT64_MAX : int64_t(slot);
  };
  const auto check=[&](size_t column,int64_t target) {
    const uint32_t* ordinals=nullptr;size_t count=0;
    assert(index.equal_ordinals(column,target,[&](uint64_t slot) {
      ++reads[column];return value(column,size_t(slot));
    },&ordinals,&count));
    std::vector<uint32_t> expected;
    for(uint32_t i=0;i<n;++i)if(value(column,i)==target)expected.push_back(i);
    assert(count==expected.size());
    for(size_t i=0;i<count;++i)assert(ordinals[i]==expected[i]);
    assert(index.resident_bytes()==base+allowance);
  };
  const int64_t targets[]={INT64_MIN,INT64_MIN+1,-8,-7,-1,0,1,9,16,17,510,511,INT64_MAX-1,INT64_MAX};
  for(size_t column:{0u,1u,0u,1u})for(int64_t target:targets)check(column,target);
  assert(reads[0]==n && reads[1]==n); // Both compressed entries remain reusable.
  for(int64_t target:targets)check(2,target);
  assert(reads[2]==n); // Dense duplicate runs still require an upper bound.
  for(int64_t target:targets)check(0,target);
  assert(reads[0]==2*n); // Dense construction evicted old compressed entries.
  for(size_t rows:{size_t(0),size_t(1)}) {
    FlatIndex small(dir,5,rows,rows*44);fill(small,rows);
    for(int64_t target:{INT64_MIN,INT64_MAX}) {
      const uint32_t* ordinals=nullptr;size_t count=0;
      assert(small.equal_ordinals(0,target,[](uint64_t){return INT64_MAX;},&ordinals,&count));
      assert(count==(rows && target==INT64_MAX ? 1u:0u));
      if(count)assert(ordinals[0]==0);
    }
  }
  FlatIndex below(dir,5,n,base+allowance-1);fill(below,n);
  const uint32_t* ordinals=nullptr;size_t count=0;
  assert(!below.equal_ordinals(0,0,[](uint64_t)->int64_t {assert(false);return 0;},&ordinals,&count));
  assert(!ordinals && !count);
}
Cond eq(const char* column,int64_t value) {
  Cond c;c.column=column;c.op=Cond::EQ;c.value=Value::ofInt(value);return c;
}
struct ModelRow {int64_t scalar;int coordinate;};
using Model=std::map<int64_t,ModelRow>;
FilterDiagnostics check_scan(Database& db,const Model& model,int64_t value,bool stop=false) {
  size_t expected=0,visited=0;
  for(const auto& item:model)if(item.second.scalar==value)++expected;
  Diagnostic diag;
  db.scan_vectors("t",{eq("a",value)},[&](int64_t pk,const float* vector,size_t n) {
    assert(model.count(pk) && model.at(pk).scalar==value && n==16);
    assert(vector[0]==model.at(pk).coordinate);
    for(size_t i=1;i<n;++i)assert(vector[i]==0);
    ++visited;return !stop;
  });
  assert(visited==(stop ? std::min<size_t>(1,expected):expected));
  return diag.data;
}
void check_topk(Database& db,const Model& model,int64_t value) {
  for(bool desc:{false,true}) {
    std::vector<std::pair<int,int64_t>> expected;
    for(const auto& item:model)if(item.second.scalar==value)
      expected.emplace_back(item.second.coordinate*item.second.coordinate,item.first);
    std::sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
      return a.first==b.first ? a.second<b.second : desc ? a.first>b.first : a.first<b.first;
    });
    if(expected.size()>7)expected.resize(7);
    Statement statement;
    const std::string sql="SELECT id,L2_DISTANCE(v,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') AS d FROM t WHERE a="+
        std::to_string(value)+" ORDER BY d"+(desc ? " DESC":"")+" LIMIT 7;";
    assert(parse_sql(sql,&statement,&error,&code));
    for(bool ann:{false,true}) {
      std::string output;assert(execute_json(db,statement,&output,&error,&code,ann));
      const auto rows=nlohmann::json::parse(output).at("rows");assert(rows.size()==expected.size());
      for(size_t i=0;i<expected.size();++i) {
        assert(rows[i][0].get<int64_t>()==expected[i].second);
        assert(rows[i][1].get<double>()==std::abs(model.at(expected[i].second).coordinate));
      }
    }
  }
}
void database_cases(const std::string& path) {
  Database db;assert(db.open(path,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;
  Column v;v.name="v";v.type=Type::VECTOR;v.len=16;
  assert(db.create_table("t",{id,a,v},&error,&code));
  Model model;
  assert(db.begin(&error,&code));
  for(int key=255;key>=0;--key) {
    std::vector<float> vector(16);vector[0]=float(key%23-11);
    const int64_t scalar=low_value(size_t(key));
    assert(db.insert("t",{Value::ofInt(key),Value::ofInt(scalar),Value::ofVec(vector)},&error,&code));
    model.emplace(key,ModelRow{scalar,key%23-11});
  }
  assert(db.commit(&error,&code));assert(db.create_vector_index("t","v","vi",&error,&code));
  for(int64_t value:{INT64_MIN,int64_t(-7),int64_t(1),INT64_MAX}) {
    check_scan(db,model,value);check_scan(db,model,value,true);check_topk(db,model,value);
  }
#ifdef MVDB_MULTI_POSTING_REUSE_ONLY
  assert(!db.table("t")->vector_index->compound_filter_seen());
#endif
  // Fail each optional compressed posting allocation. A single query makes
  // exactly one build attempt, then enumerates the full table without retry.
  for(int fault=0;fault<4;++fault) {
    db.table("t")->invalidate_vector_index();
    FlatIndex::ResidentView view;assert(db.resident_vector_view("t",&view));
    fail_array_after=fault;
    const auto failed=check_scan(db,model,-7);fail_array_after=-1;
    assert(failed.posting_build_attempts[1]==1 && failed.posting_builds[1]==0);
    assert(failed.vector_visits==model.size());
    const auto rebuilt=check_scan(db,model,-7);
    assert(rebuilt.posting_builds[1]==1);
    const auto reused=check_scan(db,model,-7);
    assert(reused.posting_hits[1]==1 && reused.posting_builds[1]==0);
  }
  bool threw=false;
  try {db.scan_vectors("t",{eq("a",-7)},[](int64_t,const float*,size_t)->bool {throw std::runtime_error("visitor fault");});}
  catch(const std::runtime_error&) {threw=true;}
  assert(threw);check_scan(db,model,-7);
  assert(db.begin(&error,&code));
  assert(db.update_rows("t",{eq("id",1)},{{"a",Value::ofInt(INT64_MAX)}},&error,&code)==1);
  Model changed=model;changed.at(1).scalar=INT64_MAX;
  check_scan(db,changed,INT64_MAX);check_topk(db,changed,INT64_MAX);
  assert(db.rollback(&error,&code));check_scan(db,model,INT64_MAX);
  assert(db.begin(&error,&code));
  assert(db.delete_rows("t",{eq("id",6)},&error,&code)==1);model.erase(6);
  assert(db.insert("t",{Value::ofInt(6),Value::ofInt(INT64_MAX),Value::ofVec(std::vector<float>(16))},&error,&code));
  model.emplace(6,ModelRow{INT64_MAX,0});assert(db.commit(&error,&code));
  check_scan(db,model,-7);check_topk(db,model,INT64_MAX);
  assert(db.checkpoint(&error));db.close();assert(db.open(path,&error));
  check_scan(db,model,INT64_MAX);check_topk(db,model,INT64_MAX);db.close();
}
}
int main() {
  Directory direct,database;direct_cases(direct.path);database_cases(database.path);
  std::cout<<"PASS equality posting compressed/dense/extreme/empty runs, exact TopK, one-attempt allocation fallback and mutation/reopen\n";
}

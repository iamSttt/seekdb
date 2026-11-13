// Link src/{db,exec,sql,value,flat_index}.cpp with -std=c++17 -pthread -Isrc.
// Also run with MVDB_DISABLE_MULTI_POSTING and with both
// MVDB_FLAT_INDEX_MEMORY_BYTES=1 / MVDB_SPILL_BYTES=1.
#include "db.h"
#include "distance.h"
#include "exec.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <random>
#include <unistd.h>

// Fault and peak accounting for optional exact-length posting arrays only.
// The replacements remain malloc/free paired; vector/SQL allocations are not
// counted. Every tracked allocation must be freed before resetting the meter.
namespace {
struct Allocation {void* pointer=nullptr;size_t bytes=0;};
Allocation allocations[32];
bool meter=false;
size_t live_bytes=0,peak_bytes=0;
int fail_after=-1;
void release_array(void* pointer) {
  for(auto& allocation:allocations)if(allocation.pointer==pointer && pointer) {
    live_bytes-=allocation.bytes;allocation=Allocation{};break;
  }
  std::free(pointer);
}
}
void* operator new[](size_t bytes) {
  if(void* pointer=std::malloc(bytes ? bytes : 1))return pointer;
  throw std::bad_alloc();
}
void operator delete[](void* pointer) noexcept {release_array(pointer);}
void operator delete[](void* pointer,size_t) noexcept {release_array(pointer);}
void* operator new[](size_t bytes,const std::nothrow_t&) noexcept {
  if(fail_after>=0 && fail_after--==0)return nullptr;
  void* pointer=nullptr;
  try {pointer=::operator new[](bytes);} catch(...) {return nullptr;}
  if(meter) {
    bool saved=false;
    for(auto& allocation:allocations)if(!allocation.pointer) {
      allocation={pointer,bytes};saved=true;break;
    }
    assert(saved);live_bytes+=bytes;peak_bytes=std::max(peak_bytes,live_bytes);
  }
  return pointer;
}
void operator delete[](void* pointer,const std::nothrow_t&) noexcept {release_array(pointer);}

using namespace mvdb;
using Range=FlatIndex::IntegerRange;
static std::string err;
static int code=0;
struct Directory {
  std::string path;
  Directory() {char text[]="/tmp/mvdb-filter-paths-XXXXXX";assert(::mkdtemp(text));path=text;}
  ~Directory() {std::filesystem::remove_all(path);}
};
static Range equal(int64_t value) {
  Range range;range.has_lower=range.has_upper=true;range.lower=range.upper=value;return range;
}

#ifndef MVDB_DISABLE_MULTI_POSTING
static void posting_tests(const std::string& dir) {
  constexpr size_t n=512,base=n*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  constexpr size_t allowance=n*(2*sizeof(int64_t)+sizeof(uint32_t));
  const float vector[5]={0,1,2,3,4};
  auto fill=[&](FlatIndex& index) {for(size_t i=0;i<n;++i)index.add(int64_t(i),i,vector,5);index.finish();};
  assert(!live_bytes);meter=true;peak_bytes=0;
  {
    FlatIndex index(dir,5,n,base+allowance);fill(index);
    size_t reads[4]={},count=0;
    const uint32_t* ordinals=nullptr;
    auto read=[&](size_t column,uint64_t slot) {
      ++reads[column];return int64_t(slot%(column==0 ? 2 : column==1 ? 17 : 5));
    };
    for(size_t column:{0u,1u,0u,1u}) {
      assert(index.integer_ordinals(column,equal(1),[&](uint64_t slot){return read(column,slot);},&ordinals,&count));
      for(size_t j=0;j<count;++j)assert(ordinals[j]%(column==0 ? 2 : 17)==1);
      assert(index.resident_bytes()==base+allowance && peak_bytes<=allowance);
    }
    assert(reads[0]==n && reads[1]==n); // Alternating columns must not rebuild.
    assert(index.cached_integer_count(0,equal(1),&count) && count==n/2);
    assert(index.integer_ordinals(2,equal(2),[&](uint64_t slot){return read(2,slot);},&ordinals,&count));
    assert(!index.cached_integer_count(0,equal(1),&count)); // Least-recently used.
    assert(index.cached_integer_count(1,equal(1),&count));
    // A unique-valued column uses the dense fallback and evicts compressed
    // columns when its 20N construction peak requires the full allowance.
    assert(index.integer_ordinals(3,Range{},[](uint64_t slot){return int64_t(slot);},&ordinals,&count));
    assert(count==n && peak_bytes<=allowance);
    assert(!index.cached_integer_count(1,equal(1),&count));
    // Every open/closed endpoint, including contradictory and extreme ranges,
    // is compared with a direct independent enumeration of input values.
    const int64_t endpoints[]={INT64_MIN,-1,0,1,255,511,512,INT64_MAX};
    for(int64_t lower:endpoints)for(int64_t upper:endpoints)
      for(unsigned mask=0;mask<16;++mask) {
        Range range;range.lower=lower;range.upper=upper;
        range.has_lower=(mask&1)!=0;range.has_upper=(mask&2)!=0;
        range.lower_inclusive=(mask&4)!=0;range.upper_inclusive=(mask&8)!=0;
        assert(index.integer_ordinals(3,range,[](uint64_t)->int64_t {assert(false);return 0;},&ordinals,&count));
        std::vector<uint32_t> expected;
        for(uint32_t i=0;i<n;++i)if(range.contains(i))expected.push_back(i);
        assert(count==expected.size() && std::equal(expected.begin(),expected.end(),ordinals));
      }
    const auto extremes=[](uint64_t slot) {
      return slot==0 ? INT64_MIN : slot==511 ? INT64_MAX : int64_t(slot%17);
    };
    for(int64_t value:{INT64_MIN,INT64_MAX}) {
      assert(index.integer_ordinals(4,equal(value),extremes,&ordinals,&count));
      assert(count==1 && ordinals[0]==(value==INT64_MIN ? 0u : 511u));
    }
    assert(peak_bytes<=allowance);
  }
  assert(!live_bytes);
  // Each of the four optional array allocations may fail. Failure publishes
  // no partial cache, releases scratch, and permits a later successful build.
  for(int fault=0;fault<4;++fault) {
    peak_bytes=0;
    FlatIndex index(dir,5,n,base+allowance);fill(index);
    const uint32_t* ordinals=nullptr;size_t count=0;
    fail_after=fault;
    assert(!index.integer_ordinals(0,equal(1),[](uint64_t slot){return int64_t(slot%2);},&ordinals,&count));
    fail_after=-1;
    assert(!ordinals && !count && !live_bytes && peak_bytes<=allowance);
    assert(index.integer_ordinals(0,equal(1),[](uint64_t slot){return int64_t(slot%2);},&ordinals,&count));
    assert(count==n/2);
    bool threw=false;
    try {index.integer_ordinals(1,equal(0),[](uint64_t)->int64_t {throw std::runtime_error("read fault");},&ordinals,&count);}
    catch(const std::runtime_error&) {threw=true;}
    assert(threw && !index.cached_integer_count(1,equal(0),&count));
    assert(index.cached_integer_count(0,equal(1),&count) && count==n/2);
    assert(peak_bytes<=allowance);
  }
  assert(!live_bytes);meter=false;
  FlatIndex below(dir,5,n,base+allowance-1);fill(below);
  const uint32_t* ordinals=nullptr;size_t count=0;
  assert(!below.integer_ordinals(0,Range{},[](uint64_t){return int64_t(0);},&ordinals,&count));
  FlatIndex empty(dir,5,0,0);empty.finish();
  assert(empty.integer_ordinals(0,Range{},[](uint64_t)->int64_t {assert(false);return 0;},&ordinals,&count));
  assert(!ordinals && !count);
}
#endif

static Cond condition(const std::string& column,Cond::Op op,int64_t value) {
  Cond cond;cond.column=column;cond.op=op;cond.value=Value::ofInt(value);return cond;
}
static void check(Database& db,const std::vector<Cond>& conds) {
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan("t",conds,[&](const Row& row){expected.emplace(row.vals[0].i,row.vals[4].v);return true;});
  db.scan_vectors("t",conds,[&](int64_t key,const float* vector,size_t dimension) {
    assert(actual.emplace(key,std::vector<float>(vector,vector+dimension)).second);return true;
  });
  assert(actual==expected);
  size_t visited=0;
  db.scan_vectors("t",conds,[&](int64_t,const float*,size_t){++visited;return false;});
  assert(visited==(expected.empty() ? 0u : 1u));
}
static void topk(Database& db,const std::string& where) {
  Statement statement;
  const std::string sql="SELECT id,L2_DISTANCE(v,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') AS d FROM t WHERE "+where+" ORDER BY d LIMIT 7;";
  assert(parse_sql(sql,&statement,&err,&code));
  std::vector<std::pair<double,int64_t>> expected;
  const float zero[16]={};
  db.scan("t",statement.conds,[&](const Row& row) {
    expected.emplace_back(distance_squared(row.vals[4].v.data(),zero,16),row.vals[0].i);return true;
  });
  std::sort(expected.begin(),expected.end());if(expected.size()>7)expected.resize(7);
  for(bool ann:{false,true}) {
    std::string output;assert(execute_json(db,statement,&output,&err,&code,ann));
    const auto rows=nlohmann::json::parse(output).at("rows");assert(rows.size()==expected.size());
    for(size_t i=0;i<expected.size();++i)assert(rows[i][0].get<int64_t>()==expected[i].second);
  }
}
static void database_tests(const std::string& dir) {
  Database db;assert(db.open(dir,&err));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;
  Column b=a;b.name="b";
  Column text;text.name="s";text.type=Type::VARCHAR;text.len=16;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=16;
  assert(db.create_table("t",{id,a,b,text,vector},&err,&code));
  auto add=[&](int64_t key) {
    std::vector<float> values(16);
    for(size_t j=0;j<values.size();++j)values[j]=float((key*7+int64_t(j)*3)%31-15)/8;
    assert(db.insert("t",{Value::ofInt(key),Value::ofInt(key%2),Value::ofInt(key%17),
        Value::ofStr(key%3 ? "keep" : "drop"),Value::ofVec(values)},&err,&code));
  };
  assert(db.begin(&err,&code));for(int i=511;i>=0;--i)add(i);assert(db.commit(&err,&code));
  assert(db.create_vector_index("t","v","vi",&err,&code));
#ifndef MVDB_DISABLE_MULTI_POSTING
  FlatIndex::ResidentView view;
  if(db.resident_vector_view("t",&view)) {
    // The 64-point sample visits ordinals 0,8,...,504. Alter only an unseen
    // scalar, preserving vector/norm order: zero sampled hits is not emptiness.
    const int64_t rare_key=view.keys[1];
    assert(db.update_rows("t",{condition("id",Cond::EQ,rare_key)},
                          {{"b",Value::ofInt(99)}},&err,&code)==1);
    check(db,{condition("a",Cond::GE,0),condition("b",Cond::EQ,99)});
    size_t count=0;
    assert(db.table("t")->vector_index->cached_integer_count(2,equal(99),&count) && count==1);
  }
#endif
  const std::vector<Cond> compound={condition("a",Cond::GE,0),condition("b",Cond::EQ,7)};
  check(db,compound);
#ifndef MVDB_DISABLE_MULTI_POSTING
  if(db.resident_vector_view("t",&view)) {
    size_t count=0;
    assert(db.table("t")->vector_index->cached_integer_count(2,equal(7),&count));
    assert(!db.table("t")->vector_index->cached_integer_count(1,Range{},&count));
    // A fresh optional allocation failure must retain the exact full-scan result.
    fail_after=0;check(db,{condition("id",Cond::LT,9)});fail_after=-1;
  }
#endif
  check(db,{condition("a",Cond::EQ,1)});check(db,{condition("b",Cond::EQ,1)});
  for(Cond::Op op:{Cond::EQ,Cond::NE,Cond::LT,Cond::LE,Cond::GT,Cond::GE}) {
    for(int64_t value:{INT64_MIN,int64_t(-1),int64_t(0),int64_t(8),INT64_MAX})
      check(db,{condition("b",op,value)});
  }
  std::mt19937 random(7319);
  for(size_t i=0;i<160;++i) {
    std::vector<Cond> conds={condition("a",static_cast<Cond::Op>(random()%6),int64_t(random()%4)-1),
                           condition("b",static_cast<Cond::Op>(random()%6),int64_t(random()%20)-1),
                           condition("b",static_cast<Cond::Op>(random()%6),int64_t(random()%20)-1)};
    if(i%2)std::reverse(conds.begin(),conds.end());
    check(db,conds);
  }
  for(const char* where:{"a>=0 AND b=7","b=7 AND a>=0","b>=3 AND b<8 AND a=1",
      "b>=3 AND b<=3","b>3 AND b<=3","b='007' AND s='keep'","b='invalid'",
      "b>=-9223372036854775808 AND b<=9223372036854775807","b<>2 AND a=1"})topk(db,where);
  assert(db.begin(&err,&code));
  assert(db.update_rows("t",{condition("id",Cond::LT,40)},{{"b",Value::ofInt(7)}},&err,&code)==40);
  assert(!db.table("t")->vector_index);check(db,compound);
  assert(db.begin_statement(&err,&code));add(700);
  assert(db.update_rows("t",{},{{"a",Value::ofInt(9)}},&err,&code)==513);
  assert(!db.end_statement(false,&err,&code));check(db,compound);
  assert(db.delete_rows("t",{condition("b",Cond::EQ,7)},&err,&code)>0);check(db,compound);
  assert(db.rollback(&err,&code));assert(!db.table("t")->vector_index);check(db,compound);
  topk(db,"a=1 AND b>=2 AND b<10");
  assert(db.begin(&err,&code));assert(db.drop_table("t",&err,&code));
  assert(db.rollback(&err,&code));check(db,compound);
  assert(db.checkpoint(&err));db.close();assert(db.open(dir,&err));check(db,compound);
  topk(db,"s='keep' AND b<=8 AND a=1");db.close();
}
int main() {
  Directory direct,database;
#ifndef MVDB_DISABLE_MULTI_POSTING
  posting_tests(direct.path);
#endif
  database_tests(database.path);
  std::cout<<"PASS bounded multi-column/range postings, plan selection, faults, rollback and exact TopK\n";
}

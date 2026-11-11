// Link db.cpp, flat_index.cpp and value.cpp. Repeat with
// -DMVDB_FLAT_INDEX_MEMORY_BYTES=1 -DMVDB_SPILL_BYTES=1 to exercise disk paths.
#include "db.h"
#include "flat_index.h"
#include "distance.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
using namespace mvdb;
static std::string err;
static int code=0;
static std::vector<Column> schema() {
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column group;group.name="g";group.type=Type::BIGINT;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=5;
  return {id,group,vector};
}
static void insert(Database& db,int64_t id) {
  assert(db.insert("t",{Value::ofInt(id),Value::ofInt(id%3),
      Value::ofVec({float(id),1,2,3,4})},&err,&code));
}
static void check(Database& db,const std::vector<Cond>& conds={}) {
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan("t",conds,[&](const Row& row){expected[row.vals[0].i]=row.vals[2].v;return true;});
  const std::vector<float> query{0,1,2,0,0};
  std::vector<std::pair<double,int64_t>> a,b;
  db.scan_vectors("t",conds,[&](int64_t pk,const float* values,size_t size) {
    assert(actual.emplace(pk,std::vector<float>(values,values+size)).second);
    a.emplace_back(distance_squared(values,query.data(),size),pk);return true;
  });
  assert(actual==expected);
  for(const auto& kv:expected)b.emplace_back(distance_squared(kv.second.data(),query.data(),query.size()),kv.first);
  std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());assert(a==b);
  size_t visited=0;
  db.scan_vectors("t",conds,[&](int64_t,const float*,size_t){++visited;return false;});
  assert(visited==(expected.empty()?0:1));
}
int main() {
  char path[]="/tmp/mvdb-flat-index-XXXXXX";assert(::mkdtemp(path));
  Database db;assert(db.open(path,&err));assert(db.create_table("t",schema(),&err,&code));
  check(db); // non-indexed fallback, including empty tables
  assert(db.begin_statement(&err,&code));
  assert(db.create_vector_index("t","v","idx",&err,&code));
  assert(db.end_statement(true,&err,&code));
  assert(db.table("t")->vector_index && db.table("t")->vector_index->size()==0);
  assert(db.begin(&err,&code));for(int i=99;i>=0;--i)insert(db,i);assert(db.commit(&err,&code));
  assert(!db.table("t")->vector_index);check(db);assert(db.table("t")->vector_index->size()==100);
  Cond group;group.column="g";group.value=Value::ofInt(1);
  check(db,{group});
  // Coercion, contradictory conjunctions, and column switching must use the
  // exact same predicate semantics as the ordinary table scan.
  Cond text_group=group;text_group.value=Value::ofStr("+01");check(db,{text_group});
  Cond other_group=group;other_group.value=Value::ofInt(2);check(db,{group,other_group});
  Cond range;range.column="id";range.op=Cond::GE;range.value=Value::ofInt(40);
  check(db,{range,group});
  Cond id;id.column="id";id.value=Value::ofInt(52);check(db,{id});check(db,{group});
  Cond missing;missing.column="id";missing.value=Value::ofInt(-100);check(db,{missing});
  assert(db.begin(&err,&code));
  assert(db.begin_statement(&err,&code));
  assert(db.update_rows("t",{group},{{"v",Value::ofVec({7,6,5,4,3})}},&err,&code)==33);
  assert(db.end_statement(true,&err,&code));check(db,{group});
  // Changing the indexed scalar itself must discard cached postings, even if
  // the vector values are untouched. Rollback below must restore both.
  assert(db.update_rows("t",{id},{{"g",Value::ofInt(2)}},&err,&code)==1);
  check(db,{group});check(db,{other_group});
  assert(db.update_rows("t",{id},{{"g",Value::ofInt(1)}},&err,&code)==1);
  check(db,{group});
  assert(db.begin_statement(&err,&code));
  insert(db,1000);
  assert(!db.insert("t",{Value::ofInt(1),Value::ofInt(0),Value::ofVec({0,0,0,0,0})},&err,&code));
  assert(!db.end_statement(false,&err,&code));check(db);
  assert(db.delete_rows("t",{group},&err,&code)==33);check(db);
  assert(db.rollback(&err,&code));check(db);
  db.scan("t",{},[](const Row& row){assert(row.vals[2].v[0]==float(row.vals[0].i));return true;});
  // A dropped indexed table must return with valid contents after rollback.
  assert(db.begin(&err,&code));assert(db.drop_table("t",&err,&code));
  assert(db.create_table("t",schema(),&err,&code));insert(db,9999);
  assert(db.rollback(&err,&code));check(db);
  assert(db.table("t")->vector_index->size()==100);
  // Metadata and a real physical structure are restored before open returns.
  db.close();assert(db.open(path,&err));
  assert(db.has_vector_index("t") && db.table("t")->vector_index);check(db);
  assert(db.checkpoint(&err));db.close();assert(db.open(path,&err));
  assert(db.table("t")->vector_index);check(db,{group});
  // Creating an index inside an aborted transaction must leave no index.
  assert(db.create_table("plain",schema(),&err,&code));
  assert(db.begin(&err,&code));assert(db.create_vector_index("plain","v","pidx",&err,&code));
  assert(db.table("plain")->vector_index);assert(db.rollback(&err,&code));
  assert(!db.has_vector_index("plain") && !db.table("plain")->vector_index);
  // Direct physical disk index test, independent of the build-budget macro.
  FlatIndex disk(path,5,3,0);
  for(int64_t pk:{9,-2,4}) {float v[5]={float(pk),1,2,3,4};disk.add(pk,static_cast<uint64_t>(pk+2),v,5);}
  disk.finish();assert(disk.disk_backed() && disk.resident_bytes()==0);
  std::vector<int64_t> seen;
  disk.scan([&](int64_t pk,uint64_t slot,const float* values,size_t dim){
    assert(slot==static_cast<uint64_t>(pk+2) && dim==5 && values[0]==float(pk));seen.push_back(pk);return true;
  });
  assert((seen==std::vector<int64_t>{9,-2,4}));
  assert(!disk.scan_integer_equal(0,9,[](uint64_t){return 9;},
      [](int64_t,uint64_t,const float*,size_t){assert(false);return true;}));
  // A direct resident index proves postings cache one column, retain ordinal
  // order for equal keys, and honor early termination without reading again.
  FlatIndex resident(path,5,3,4096);
  for(int64_t pk:{9,-2,4}){float v[5]={float(pk),1,2,3,4};resident.add(pk,uint64_t(pk+2),v,5);}
  resident.finish();size_t reads=0;seen.clear();
  auto scalar=[&](uint64_t slot){++reads;return slot==0?int64_t(-1):int64_t(7);};
  auto collect=[&](int64_t pk,uint64_t,const float*,size_t){seen.push_back(pk);return true;};
  assert(resident.scan_integer_equal(1,7,scalar,collect));
  assert(reads==3 && (seen==std::vector<int64_t>{9,4}));seen.clear();
  assert(resident.scan_integer_equal(1,-1,scalar,collect));
  assert(reads==3 && (seen==std::vector<int64_t>{-2}));
  assert(resident.scan_integer_equal(0,7,scalar,collect) && reads==6);
  std::cout<<"PASS physical flat index, filters, mutation/statement/transaction rollback, eager restart rebuild, disk scan\n";
}

// New scope regression for the isolated 9ed + spill-only draft; not executed.
// Build DB and this file with MVDB_FLAT_INDEX_MEMORY_BYTES=8192, default spill
// threshold. A disk index over resident payload must not admit scalar arrays.
#include "db.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <unistd.h>

using namespace mvdb;
static std::string error;
static int code=0;
static Cond condition(const char* column,Cond::Op op,int64_t value) {
  Cond c;c.column=column;c.op=op;c.value=Value::ofInt(value);return c;
}
static void compare(Database& db) {
  // Includes cached and uncached columns, range/equality, PK and contradiction.
  const std::vector<std::vector<Cond>> filters={
    {},{condition("id",Cond::GT,490)},
    {condition("a",Cond::EQ,3),condition("b",Cond::GE,7)},
    {condition("a",Cond::GE,0),condition("c",Cond::EQ,9)},
    {condition("b",Cond::GT,8),condition("b",Cond::LE,8)}};
  for(const auto& filter:filters) {
    std::map<int64_t,std::vector<float>> expected,actual;
    db.scan("t",filter,[&](const Row& row) {
      assert(expected.emplace(row.vals[0].i,row.vals[4].v).second);return true;
    });
    db.scan_vectors("t",filter,[&](int64_t id,const float* v,size_t dimension) {
      assert(actual.emplace(id,std::vector<float>(v,v+dimension)).second);return true;
    });
    assert(expected==actual);
  }
}
int main() {
  alarm(60);
  char path[]="/tmp/mvdb-spill-scope-XXXXXX";assert(mkdtemp(path));
  Database db;assert(db.open(path,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;Column b=a;b.name="b";Column c=a;c.name="c";
  Column v;v.name="v";v.type=Type::VECTOR;v.len=128;
  assert(db.create_table("t",{id,a,b,c,v},&error,&code));
  assert(db.begin(&error,&code));
  for(int64_t i=0;i<500;++i)
    assert(db.insert("t",{Value::ofInt(i),Value::ofInt(i%13),Value::ofInt(i%17),
      Value::ofInt(i%19),Value::ofVec(std::vector<float>(128,float(i)))},&error,&code));
  assert(db.commit(&error,&code));
  assert(!db.table("t")->payload_store);
  assert(db.create_vector_index("t","v","vi",&error,&code));
  assert(db.table("t")->vector_index->disk_backed());
  for(size_t column:{size_t(1),size_t(2),size_t(3)})
    assert(!db.table("t")->vector_index->has_spill_integer(column));
  assert(db.table("t")->vector_index->resident_bytes()==0);
  compare(db);
  // Preserve EE's existing transactional UPDATE migration, then roll back.
  assert(db.begin(&error,&code));
  assert(db.update_rows("t",{condition("id",Cond::EQ,0)},{{"a",Value::ofInt(123)}},&error,&code)==1);
  assert(db.table("t")->payload_store);
  assert(db.rollback(&error,&code));
  assert(!db.table("t")->vector_index);
  compare(db);
  assert(db.table("t")->vector_index->disk_backed());
  assert(db.table("t")->vector_index->has_spill_integer(1));
  assert(db.table("t")->vector_index->has_spill_integer(2));
  assert(!db.table("t")->vector_index->has_spill_integer(3));
  assert(db.table("t")->vector_index->resident_bytes()<=8192);
  assert(db.checkpoint(&error));db.close();assert(db.open(path,&error));
  // Restart may restore a resident payload table: the current storage state,
  // rather than historical spill membership, must determine cache admission.
  compare(db);
  if(!db.table("t")->payload_store)
    for(size_t column:{size_t(1),size_t(2),size_t(3)})
      assert(!db.table("t")->vector_index->has_spill_integer(column));
  db.close();std::filesystem::remove_all(path);
  std::cout<<"PASS payload-resident disk-index exclusion, two-column spill admission and rollback/reopen equivalence\n";
}

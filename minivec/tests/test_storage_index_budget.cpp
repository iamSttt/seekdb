// Shared production index budget, including opt-in delta reservations and
// spilled scalar arrays. Also run with MVDB_FLAT_INDEX_MEMORY_BYTES=8192 / 1.
// g++ -std=c++17 -O2 -pthread -Isrc tests/test_storage_index_budget.cpp
//     src/{db,flat_index,value}.cpp -o /tmp/test_storage_index_budget
#include "db.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

#ifndef MVDB_FLAT_INDEX_MEMORY_BYTES
#define MVDB_FLAT_INDEX_MEMORY_BYTES (16ull << 20)
#endif

using namespace mvdb;
static std::string error;
static int code=0;
static Cond eq(const char* column,int64_t value) {
  Cond c;c.column=column;c.value=Value::ofInt(value);c.op=Cond::EQ;return c;
}
static void budget(Database& db) {
  size_t used=0;
  for(const auto& name:db.table_names()) {
    const Table& table=*db.table(name);
    if(table.vector_index)used+=table.vector_index->resident_bytes();
#ifdef MVDB_ENABLE_INDEX_DELTA
    used+=table.index_delta_budget;
    if(table.index_delta)assert(table.vector_index && table.index_delta_budget);
    if(table.payload_store)assert(!table.index_delta && !table.index_delta_budget);
#endif
    assert(used<=MVDB_FLAT_INDEX_MEMORY_BYTES);
  }
}
static void compare(Database& db,const std::string& name) {
  const std::vector<Cond> filter={eq("a",3),eq("b",7)};
  size_t expected=0,actual=0;
  db.scan(name,filter,[&](const Row&){++expected;return true;});
  db.scan_vectors(name,filter,[&](int64_t id,const float* v,size_t dim) {
    assert(id%13==3 && id%17==7 && dim==16 && v[0]==float(id));
    ++actual;return true;
  });
  assert(actual==expected);budget(db);
}
int main() {
  alarm(180);
  char directory[]="/tmp/mvdb-storage-budget-XXXXXX";assert(mkdtemp(directory));
  Database db;assert(db.open(directory,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;Column b=a;b.name="b";
  Column v;v.name="v";v.type=Type::VECTOR;v.len=16;
  Column s;s.name="s";s.type=Type::VARCHAR;s.len=20000;
  const std::string wide(12000,'x');
  assert(db.create_table("disk",{id,a,b,v,s},&error,&code));
  assert(db.begin(&error,&code));
  for(int i=0;i<256;++i)assert(db.insert("disk",{Value::ofInt(i),Value::ofInt(i%13),
      Value::ofInt(i%17),Value::ofVec(std::vector<float>(16,float(i))),Value::ofStr(wide)},&error,&code));
  assert(db.commit(&error,&code));
  assert(db.create_vector_index("disk","v","disk_v",&error,&code));
  // Cross the unchanged 1 MiB Undo-image allowance with real retained images.
  // The table, any live delta, and its original owners migrate/invalidate.
  assert(db.begin(&error,&code));
  for(int i=0;i<100 && !db.table("disk")->payload_store;++i)
    assert(db.update_rows("disk",{eq("id",0)},{{"s",Value::ofStr(wide+std::to_string(i))}},&error,&code)==1);
  assert(db.table("disk")->payload_store);
  assert(db.rollback(&error,&code));
  compare(db,"disk");
#ifdef MVDB_ENABLE_SPILL_FILTER
  if(MVDB_FLAT_INDEX_MEMORY_BYTES>=4096)
    assert(db.table("disk")->vector_index->has_spill_integer(1));
#endif
  bool saw_resident=false,saw_disk=false;
  // Delta reservations make the production budget fill with modest row data;
  // later tables must fall back without exceeding that same global allowance.
  for(int table_number=0;table_number<80;++table_number) {
    const std::string name="t"+std::to_string(table_number);
    assert(db.create_table(name,{id,a,b,v},&error,&code));
    assert(db.begin(&error,&code));
    for(int i=0;i<256;++i)assert(db.insert(name,{Value::ofInt(i),Value::ofInt(i%13),
        Value::ofInt(i%17),Value::ofVec(std::vector<float>(16,float(i)))},&error,&code));
    assert(db.commit(&error,&code));
    assert(db.create_vector_index(name,"v","v_"+name,&error,&code));
    saw_disk|=db.table(name)->vector_index->disk_backed();
    saw_resident|=!db.table(name)->vector_index->disk_backed();
    budget(db);compare(db,name);
    // A rollback discards both auxiliary representations and reclaims their
    // reserved allowance before a subsequent lazy build.
    assert(db.begin(&error,&code));
    assert(db.update_rows(name,{eq("id",0)},{{"a",Value::ofInt(3)}},&error,&code)==1);
    budget(db);
    assert(db.rollback(&error,&code));compare(db,name);
  }
#ifdef MVDB_ENABLE_INDEX_DELTA
  if(MVDB_FLAT_INDEX_MEMORY_BYTES==(16ull<<20))assert(saw_resident && saw_disk);
#endif
  (void)saw_resident;(void)saw_disk;
  compare(db,"disk");
  assert(db.checkpoint(&error));db.close();assert(db.open(directory,&error));
  compare(db,"disk");compare(db,"t79");db.close();
  std::filesystem::remove_all(directory);
  std::cout<<"PASS shared index budget, delta/spill coexistence, rollback and reopen\n";
}

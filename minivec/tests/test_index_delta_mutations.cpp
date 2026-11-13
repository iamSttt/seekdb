// Independent model for the opt-in INSERT/DELETE/UPDATE index delta.
// Link src/{db,exec,sql,value,flat_index}.cpp, -std=c++17 -pthread -Isrc.
// Run both without and with MVDB_ENABLE_INDEX_DELTA, plus ASan/UBSan.
// Forced spill / a tiny index budget must remain correct through rebuilds.
#include "db.h"
#include "exec.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

using namespace mvdb;
namespace {
std::string error;
int code=0;
struct Directory {
  std::string path;
  Directory() {char p[]="/tmp/mvdb-delta-mutations-XXXXXX";assert(mkdtemp(p));path=p;}
  ~Directory() {std::filesystem::remove_all(path);}
};
struct Expected {int64_t group;std::vector<float> vector;};
using Model=std::map<int64_t,Expected>;
Cond eq(int64_t id) {
  Cond c;c.column="id";c.op=Cond::EQ;c.value=Value::ofInt(id);return c;
}
Expected value(int64_t id) {
  // Dyadic inputs make the independent scalar squared-distance sum exact.
  // Many equal distances deliberately cross base/delta and insertion order.
  return {id%7,std::vector<float>(16,float(id%13-6)/4)};
}
void insert(Database& db,Model& model,int64_t id,Expected expected) {
  assert(!model.count(id));
  assert(db.insert("t",{Value::ofInt(id),Value::ofInt(expected.group),
      Value::ofVec(expected.vector)},&error,&code));
  model.emplace(id,std::move(expected));
}
void update(Database& db,Model& model,int64_t id,Expected expected) {
  assert(model.count(id));
  assert(db.update_rows("t",{eq(id)},{{"g",Value::ofInt(expected.group)},
      {"v",Value::ofVec(expected.vector)}},&error,&code)==1);
  model[id]=std::move(expected);
}
void erase(Database& db,Model& model,int64_t id) {
  assert(model.count(id));assert(db.delete_rows("t",{eq(id)},&error,&code)==1);
  model.erase(id);
}
bool eligible(const Table& table) {
#ifdef MVDB_ENABLE_INDEX_DELTA
  return table.vector_index && !table.vector_index->disk_backed() &&
      !table.payload_store && table.index_delta_budget>=65536;
#else
  (void)table;return false;
#endif
}
void expect_delta(const Table& table,const std::shared_ptr<FlatIndex>& base,bool enabled) {
#ifdef MVDB_ENABLE_INDEX_DELTA
  if(enabled)assert(table.vector_index==base && table.index_delta);
#else
  (void)table;(void)base;(void)enabled;
#endif
}
void expect_invalid(const Table& table) {
  assert(!table.vector_index);
#ifdef MVDB_ENABLE_INDEX_DELTA
  assert(!table.index_delta);
#endif
}
void check_query(Database& db,const Model& model,bool ann,bool descending,size_t k,
                 bool filtered=false) {
  std::vector<std::pair<double,int64_t>> expected;
  for(const auto& entry:model) {
    if(filtered && entry.second.group!=3)continue;
    double distance=0;
    for(float component:entry.second.vector)distance+=double(component)*double(component);
    expected.emplace_back(distance,entry.first);
  }
  std::sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
    return a.first!=b.first ? (descending ? a.first>b.first : a.first<b.first) : a.second<b.second;
  });
  expected.resize(std::min(k,expected.size()));
  const std::string sql="SELECT id,g,v FROM t"+std::string(filtered ? " WHERE g=3" : "")+
      " ORDER BY L2_DISTANCE(v,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') "+
      (descending ? "DESC" : "ASC")+" LIMIT "+std::to_string(k)+";";
  Statement statement;assert(parse_sql(sql,&statement,&error,&code));
  std::string response;assert(execute_json(db,statement,&response,&error,&code,ann));
  const auto output=nlohmann::json::parse(response);
  assert(output.at("columns")==nlohmann::json::array({"id","g","v"}));
  const auto& rows=output.at("rows");assert(rows.size()==expected.size());
  for(size_t i=0;i<expected.size();++i) {
    const auto id=expected[i].second;const auto& wanted=model.at(id);
    assert(rows[i][0].get<int64_t>()==id && rows[i][1].get<int64_t>()==wanted.group);
    assert(rows[i][2].get<std::vector<float>>()==wanted.vector);
  }
}
void check(Database& db,const Model& model,bool filtered=false) {
  std::map<int64_t,std::vector<float>> actual;
  db.scan_vectors("t",{},[&](int64_t id,const float* vector,size_t n) {
    assert(model.count(id) && n==16);
    assert(actual.emplace(id,std::vector<float>(vector,vector+n)).second);
    return true;
  });
  assert(actual.size()==model.size());
  for(const auto& entry:model)assert(actual.at(entry.first)==entry.second.vector);
  size_t visited=0;
  db.scan_vectors("t",{},[&](int64_t,const float*,size_t){++visited;return false;});
  assert(visited==(model.empty() ? 0u : 1u));
  // Both modes use the default 100% ANN width. In particular an ANN request
  // must not see stale base data after an exact request consumed the delta.
  for(bool ann:{false,true})for(bool descending:{false,true})
    for(size_t k:{size_t(0),size_t(1),size_t(17)})check_query(db,model,ann,descending,k);
  check_query(db,model,false,false,model.size()+3); // all IDs, duplicates and ties
  if(filtered)for(bool ann:{false,true})check_query(db,model,ann,false,17,true);
}
void create(Database& db,Model& model,size_t count=8192,bool fill_capacity=true) {
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column group;group.name="g";group.type=Type::BIGINT;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=16;
  assert(db.create_table("t",{id,group,vector},&error,&code));
  assert(db.begin(&error,&code));
  for(int64_t id=int64_t(count)-1;id>=0;--id)insert(db,model,id,value(id));
  // Fill any reserved capacity before constructing the borrowing base. The
  // first subsequent INSERT necessarily relocates the Row array.
  if(fill_capacity)for(size_t id=count;id<db.table("t")->rows.capacity();++id)
    insert(db,model,int64_t(id),value(int64_t(id)));
  assert(db.commit(&error,&code));
  assert(db.create_vector_index("t","v","vi",&error,&code));
}
void mutations(const std::string& path) {
  Database db;assert(db.open(path,&error));Model model;create(db,model);check(db,model);
  Model committed=model;Table& table=*db.table("t");
  const size_t tail=table.rows.size(),capacity=table.rows.capacity();
  auto base=table.vector_index;const bool enabled=eligible(table);
  assert(db.begin(&error,&code));
  erase(db,model,5);insert(db,model,100000,{3,std::vector<float>(16,0)});
  assert(table.rows.capacity()>capacity && table.pk_pos.at(100000)==tail);
  update(db,model,7,{3,std::vector<float>(16,0)});
  erase(db,model,17);insert(db,model,17,{3,std::vector<float>(16,0)});
  insert(db,model,200000,{3,std::vector<float>(16,0)});
  // Duplicate-PK rejection preserves prior successful transaction changes.
  const size_t before_duplicate=table.rows.size();
  assert(!db.insert("t",{Value::ofInt(17),Value::ofInt(-1),
      Value::ofVec(std::vector<float>(16,99))},&error,&code));
  assert(code==static_cast<int>(ErrCode::DUP_KEY) && table.rows.size()==before_duplicate);
  expect_delta(table,base,enabled);check(db,model,true);expect_delta(table,base,enabled);
  assert(db.rollback(&error,&code));model=committed;expect_invalid(table);
  assert(table.rows.size()==tail);check(db,model);

  // Rolled-back INSERT tail slots can be reused without resurrecting a delta.
  assert(db.begin(&error,&code));insert(db,model,300000,{4,std::vector<float>(16,-2)});
  assert(table.pk_pos.at(300000)==tail);assert(db.commit(&error,&code));
  check(db,model,true);committed=model;

  // A failed compound statement rolls back its deletes/inserts/updates while
  // leaving the surrounding transaction usable.
  assert(db.begin(&error,&code));assert(db.begin_statement(&error,&code));
  erase(db,model,6);insert(db,model,400000,{3,std::vector<float>(16,0)});
  update(db,model,8,{3,std::vector<float>(16,0)});check(db,model);
  assert(!db.insert("t",{Value::ofInt(300000),Value::ofInt(8),
      Value::ofVec(std::vector<float>(16,8))},&error,&code));
  assert(!db.end_statement(false,&error,&code));model=committed;
  expect_invalid(table);check(db,model);assert(db.commit(&error,&code));

  // Repeated deletion/reinsertion of one PK leaves tombstoned base and delta
  // slots. Update-then-delete and insert-then-delete must emit no ghost row.
  assert(db.begin(&error,&code));
  for(int i=0;i<4;++i) {
    erase(db,model,9);insert(db,model,9,{3,std::vector<float>(16,float(i)/4)});
    check(db,model);
  }
  update(db,model,10,{3,std::vector<float>(16,0)});erase(db,model,10);
  insert(db,model,500000,{3,std::vector<float>(16,0)});erase(db,model,500000);
  assert(db.delete_rows("t",{eq(500000)},&error,&code)==0);
  check(db,model,true);assert(db.commit(&error,&code));committed=model;

  // A clean 8K-row base admits exactly 64 distinct dirty slots. The 65th
  // mutation must discard the delta, and the ordinary rebuild remains exact.
  table.invalidate_vector_index();check(db,model,true);
  base=table.vector_index;const bool capacity_enabled=eligible(table);
  assert(db.begin(&error,&code));
  for(int i=0;i<64;++i) {
    if(i%3==0)update(db,model,100+i,{3,std::vector<float>(16,0)});
    else if(i%3==1)erase(db,model,100+i);
    else insert(db,model,600000+i,{3,std::vector<float>(16,0)});
  }
  expect_delta(table,base,capacity_enabled);
  insert(db,model,700000,{3,std::vector<float>(16,0)});
  expect_invalid(table);check(db,model);
  assert(db.rollback(&error,&code));model=committed;check(db,model,true);

  // WAL recovery and snapshot recovery are separate, both checked against the
  // independent committed model; the in-memory delta is never persistent.
  assert(db.begin(&error,&code));erase(db,model,11);
  insert(db,model,800000,{3,std::vector<float>(16,0)});
  update(db,model,12,{3,std::vector<float>(16,0)});
  assert(db.commit(&error,&code));check(db,model);db.close();
  assert(db.open(path,&error));check(db,model,true);
  assert(db.checkpoint(&error));db.close();assert(db.open(path,&error));check(db,model);
  committed=model;assert(db.begin(&error,&code));erase(db,model,12);
  insert(db,model,900000,{3,std::vector<float>(16,0)});check(db,model);
  db.close();model=committed;assert(db.open(path,&error));check(db,model,true);db.close();
}
void small_and_policy(const std::string& path,bool approximate) {
  Database db;assert(db.open(path,&error));Model model;create(db,model,3,false);
  check(db,model);Table& table=*db.table("t");
  auto base=table.vector_index;const bool enabled=eligible(table);
  update(db,model,0,{3,std::vector<float>(16,0)});
  update(db,model,0,{3,std::vector<float>(16,0.25f)});
  expect_delta(table,base,enabled); // one slot, even after two mutations
  if(approximate) {
    // Separate process mode: policy variables are cached by the production
    // code. Even this tiny table, whose probe later falls back, must rebuild
    // current partitions when the operator explicitly requests width 50%.
    assert(db.resident_vector_index("t"));
#ifdef MVDB_ENABLE_INDEX_DELTA
    assert(!table.index_delta);
#endif
    check(db,model);return;
  }
  check(db,model,true);expect_delta(table,base,enabled);
  insert(db,model,10,{3,std::vector<float>(16,0)});
  expect_invalid(table);check(db,model); // second distinct slot exceeds limit 1
  base=table.vector_index;const bool delete_enabled=eligible(table);
  erase(db,model,1);expect_delta(table,base,delete_enabled);check(db,model,true);
  insert(db,model,1,{3,std::vector<float>(16,0)});expect_invalid(table);check(db,model);
}
} // namespace
int main(int argc,char** argv) {
  alarm(180);
  unsetenv("MVDB_ANN_PROBE_PERCENT");unsetenv("MVDB_ANN_OFF");
  const std::string mode=argc==2 ? argv[1] : "default";
  assert(argc<=2 && (mode=="default" || mode=="--ann-half" || mode=="--ann-off"));
  if(mode!="default")setenv("MVDB_ANN_PROBE_PERCENT","50",1);
  if(mode=="--ann-off")setenv("MVDB_ANN_OFF","1",1);
  Directory directory,small;
  small_and_policy(small.path,mode=="--ann-half");
  if(mode=="default")mutations(directory.path);
  if(mode=="default")std::cout<<"PASS independent mixed-delta model, exact/default-ANN, ties/tombstones, rollback/commit/reopen, slots/capacity and 1/64-slot limits\n";
  else std::cout<<"PASS delta policy and small-table model "<<mode<<"\n";
}

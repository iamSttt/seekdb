// Opt-in UPDATE-only delta correctness and failure tests.
// Link src/{db,exec,sql,value,flat_index}.cpp with
// -DMVDB_ENABLE_INDEX_DELTA -std=c++17 -pthread. Also run under ASan/UBSan.
#include "db.h"
#include "exec.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <new>
#include <string>
#include <vector>
#include <unistd.h>

#ifndef MVDB_ENABLE_INDEX_DELTA
#error "This candidate test requires MVDB_ENABLE_INDEX_DELTA"
#endif

namespace {
int allocation_failure=-1;
}
void* operator new(size_t bytes) {
  if(allocation_failure>=0 && allocation_failure--==0)throw std::bad_alloc();
  if(void* pointer=std::malloc(bytes?bytes:1))return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {std::free(pointer);}
void operator delete(void* pointer,size_t) noexcept {std::free(pointer);}

using namespace mvdb;
static std::string error;
static int code=0;
struct Directory {
  std::string path;
  Directory() {char path[]="/tmp/mvdb-index-delta-XXXXXX";assert(mkdtemp(path));this->path=path;}
  ~Directory() {std::filesystem::remove_all(path);}
};
static Cond eq(int64_t id) {
  Cond condition;condition.column="id";condition.op=Cond::EQ;condition.value=Value::ofInt(id);return condition;
}
static std::vector<float> values(int64_t id) {
  std::vector<float> result(16);
  for(size_t j=0;j<result.size();++j)result[j]=float((id+int64_t(j)*3)%31-15)/8;
  return result;
}
static void create(Database& db,const std::string& name,size_t rows) {
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column group;group.name="g";group.type=Type::BIGINT;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=16;
  assert(db.create_table(name,{id,group,vector},&error,&code));
  assert(db.begin(&error,&code));
  for(size_t i=0;i<rows;++i)
    assert(db.insert(name,{Value::ofInt(i),Value::ofInt(i%7),Value::ofVec(values(i))},&error,&code));
  assert(db.commit(&error,&code));
  assert(db.create_vector_index(name,"v","vi_"+name,&error,&code));
}
static void check(Database& db,const std::string& name) {
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan(name,{},[&](const Row& row){expected.emplace(row.vals[0].i,row.vals[2].v);return true;});
  db.scan_vectors(name,{},[&](int64_t id,const float* v,size_t dimension) {
    assert(actual.emplace(id,std::vector<float>(v,v+dimension)).second);return true;
  });
  assert(actual==expected);
  size_t visited=0;
  db.scan_vectors(name,{},[&](int64_t,const float*,size_t){++visited;return false;});
  assert(visited==(expected.empty()?0:1));
}
static void topk(Database& db,bool descending,size_t k) {
  std::vector<std::pair<double,int64_t>> ranked;
  db.scan("t",{},[&](const Row& row) {
    double distance=0;
    for(float value:row.vals[2].v)distance+=double(value)*double(value);
    ranked.emplace_back(distance,row.vals[0].i);return true;
  });
  std::sort(ranked.begin(),ranked.end(),[&](const auto& a,const auto& b) {
    return a.first!=b.first?(descending?a.first>b.first:a.first<b.first):a.second<b.second;
  });
  if(ranked.size()>k)ranked.resize(k);
  Statement statement;
  const std::string sql="SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') "+
      std::string(descending?"DESC":"ASC")+" LIMIT "+std::to_string(k);
  assert(parse_sql(sql,&statement,&error,&code));
  std::string response;assert(execute_json(db,statement,&response,&error,&code));
  const auto rows=nlohmann::json::parse(response).at("rows");assert(rows.size()==ranked.size());
  for(size_t i=0;i<ranked.size();++i)assert(rows[i][0].get<int64_t>()==ranked[i].second);
}
static void update(Database& db,int64_t id,const std::vector<float>& vector) {
  assert(db.begin_statement(&error,&code));
  assert(db.update_rows("t",{eq(id)},{{"v",Value::ofVec(vector)}},&error,&code)==1);
  assert(db.end_statement(true,&error,&code));
}

static void mutation_tests(const std::string& path) {
  Database db;assert(db.open(path,&error));create(db,"t",1024);
  Table& table=*db.table("t");
  auto base=table.vector_index;
  FlatIndex::ResidentView original;assert(base->resident_view(&original));
  size_t old_ordinal=0;
  while(original.keys[old_ordinal]!=1)++old_ordinal;
  const float* old_pointer=original.rows[old_ordinal];
  const std::vector<float> old(old_pointer,old_pointer+16);
  update(db,1,std::vector<float>(16,0));
  assert(table.vector_index==base && table.index_delta && table.index_delta_budget<=256u<<10);
  assert(std::equal(old.begin(),old.end(),old_pointer));
  FlatIndex::ResidentView rejected;assert(!db.resident_vector_view("t",&rejected));
  check(db,"t");
  for(bool descending:{false,true})for(size_t k:{size_t(0),size_t(1),size_t(7),size_t(2048)})topk(db,descending,k);
  // Multiple changes of one slot retain its original allocation exactly once.
  for(int i=0;i<20;++i)update(db,1,std::vector<float>(16,float(i)/8));
  assert(table.vector_index==base && table.index_delta);
  assert(std::equal(old.begin(),old.end(),old_pointer));check(db,"t");

  assert(db.begin(&error,&code));
  assert(db.begin_statement(&error,&code));
  assert(db.update_rows("t",{eq(2)},{{"g",Value::ofInt(99)}},&error,&code)==1);
  assert(!db.end_statement(false,&error,&code));
  assert(!table.vector_index && !table.index_delta);check(db,"t");
  assert(db.rollback(&error,&code));
  // A predicate uses a freshly built current posting cache, never base values.
  update(db,2,std::vector<float>(16,1));assert(table.index_delta);
  size_t filtered=0;
  db.scan_vectors("t",{eq(2)},[&](int64_t id,const float* v,size_t dimension) {
    assert(id==2 && dimension==16 && v[0]==1);++filtered;return true;
  });
  assert(filtered==1 && !table.index_delta && table.vector_index);
  update(db,3,std::vector<float>(16,2));assert(table.index_delta);
  assert(db.resident_vector_index("t") && !table.index_delta); // ANN accessor rebuild.

  // PK UPDATE is explicitly unsupported by this database. A rejected attempt
  // must leave an already live delta, the original key and transaction intact.
  assert(db.begin(&error,&code));
  assert(db.update_rows("t",{eq(4)},{{"g",Value::ofInt(77)}},&error,&code)==1);
  const auto before_rejected_pk=table.vector_index;
  assert(table.index_delta);
  assert(db.update_rows("t",{eq(4)},{{"id",Value::ofInt(4000)}},&error,&code)==-1);
  assert(code==static_cast<int>(ErrCode::NOT_SUPPORTED));
  assert(error=="updating the primary key is not supported");
  assert(table.index_delta && table.vector_index==before_rejected_pk);
  size_t old_keys=0,new_keys=0;
  db.scan("t",{eq(4)},[&](const Row& row){assert(row.vals[1].i==77);++old_keys;return true;});
  db.scan("t",{eq(4000)},[&](const Row&){++new_keys;return true;});
  assert(old_keys==1 && new_keys==0);check(db,"t");
  assert(db.rollback(&error,&code));check(db,"t");

  // 1024 rows allow ten distinct dirty slots. The eleventh falls back.
  for(int i=0;i<10;++i)update(db,i,std::vector<float>(16,float(i)));
  assert(table.index_delta && table.vector_index);
  update(db,10,std::vector<float>(16,-1));
  assert(!table.index_delta && !table.vector_index);check(db,"t");

  update(db,1,std::vector<float>(16,3));assert(table.index_delta);
  assert(db.begin(&error,&code));
  assert(db.insert("t",{Value::ofInt(2000),Value::ofInt(0),Value::ofVec(values(2000))},&error,&code));
  assert(!table.index_delta && !table.vector_index);check(db,"t");
  assert(db.rollback(&error,&code));check(db,"t");
  update(db,1,std::vector<float>(16,4));assert(table.index_delta);
  assert(db.begin(&error,&code));
  assert(db.delete_rows("t",{eq(3)},&error,&code)==1);
  assert(!table.index_delta && !table.vector_index);check(db,"t");
  assert(db.rollback(&error,&code));check(db,"t");

  update(db,1,std::vector<float>(16,5));assert(table.index_delta);
  assert(db.begin(&error,&code));
  assert(db.drop_table("t",&error,&code));assert(db.rollback(&error,&code));check(db,"t");
  update(db,1,std::vector<float>(16,6));
  assert(db.begin(&error,&code));update(db,1,std::vector<float>(16,7));
  db.close();assert(db.open(path,&error));
  assert(!db.table("t")->index_delta);check(db,"t");
  db.scan("t",{eq(1)},[](const Row& row){assert(row.vals[2].v==std::vector<float>(16,6));return true;});
  db.close();
}

static void allocation_and_budget_tests(const std::string& path) {
  for(int failure=0;failure<3;++failure) {
    Table table;table.storage_dir=path;table.schema.vec_idx=2;
    for(int i=0;i<1000;++i) {
      Row row;row.rid=i;row.vals={Value::ofInt(i),Value::ofInt(0),Value::ofVec(values(i))};
      table.append_row(std::move(row));
    }
    table.vector_index=std::make_shared<FlatIndex>(path,16,1000,4u<<20);
    for(size_t i=0;i<table.rows.size();++i)table.vector_index->add(i,i,table.rows[i].vals[2].v.data(),16);
    table.vector_index->finish();table.index_delta_budget=256u<<10;
    Row replacement=table.rows[1];replacement.vals[2].v.assign(16,9);
    allocation_failure=failure;
    table.replace_row(1,std::move(replacement));
    allocation_failure=-1;
    assert(!table.vector_index && !table.index_delta);
    assert(table.rows[1].vals[0].i==1 && table.rows[1].vals[2].v==std::vector<float>(16,9));
  }
  Database db;assert(db.open(path,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column text;text.name="s";text.type=Type::VARCHAR;text.len=400000;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=16;
  assert(db.create_table("wide",{id,text,vector},&error,&code));
  assert(db.insert("wide",{Value::ofInt(1),Value::ofStr(std::string(300000,'x')),Value::ofVec(values(1))},&error,&code));
  assert(db.create_vector_index("wide","v","wide_index",&error,&code));
  assert(db.update_rows("wide",{eq(1)},{{"v",Value::ofVec(values(2))}},&error,&code)==1);
  assert(!db.table("wide")->index_delta && !db.table("wide")->vector_index);
  db.close();
}
int main() {
  alarm(120);
  Directory mutations,faults;
  mutation_tests(mutations.path);
  allocation_and_budget_tests(faults.path);
  std::cout<<"PASS experimental UPDATE-only delta owner lifetime, exact merge, invalidation, limits and allocation faults\n";
}

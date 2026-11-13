// Run against both the retained and candidate executor with the same features.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

using namespace mvdb;
static size_t checks=0;
static void check(bool ok,const char* message) {
  ++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(1);}
}
static bool same(const Value& a,const Value& b) {
  return a.type==b.type && a.i==b.i && a.s==b.s && a.v.size()==b.v.size() &&
         (a.v.empty() || std::memcmp(a.v.data(),b.v.data(),a.v.size()*sizeof(float))==0);
}
static void unchanged(const Statement& a,const Statement& b) {
  check(a.kind==b.kind && a.table==b.table && a.rows.size()==b.rows.size() && a.columns.size()==b.columns.size(),"AST shape unchanged");
  for(size_t i=0;i<a.rows.size();++i) {
    check(a.rows[i].size()==b.rows[i].size(),"AST row shape unchanged");
    for(size_t j=0;j<a.rows[i].size();++j)check(same(a.rows[i][j],b.rows[i][j]),"AST Value unchanged");
  }
  for(size_t i=0;i<a.columns.size();++i)check(a.columns[i].name==b.columns[i].name,"AST columns unchanged");
}
static Statement parse(const std::string& sql) {
  Statement st;std::string error;int code=0;
  check(parse_sql(sql,&st,&error,&code,false),"parse fixture");return st;
}
static void success(Database& db,const Statement& st) {
  std::string out,error;int code=0;check(execute_json(db,st,&out,&error,&code),"successful statement");
}
static void sql(Database& db,const std::string& statement) {success(db,parse(statement));}
static void failure(Database& db,const Statement& st,int expected) {
  std::string out="must clear",error;int code=0;
  check(!execute_json(db,st,&out,&error,&code) && code==expected && out.empty(),"failed statement code and empty output");
}
static const Row& read(Database& db,int64_t id) {
  Table* table=db.table("t");check(table!=nullptr && table->pk_pos.count(id)==1,"row exists");
  return table->rows.at(table->pk_pos.at(id));
}
static void verify_table(Database& db) {
  Table* table=db.table("t");check(table && table->live_count()==4,"only committed fixture rows");
  for(int64_t id:{1,2,3,4}) {
    const Row& row=read(db,id);
    check(row.vals[1].type==Type::VARCHAR && row.vals[2].type==Type::VECTOR,"stored destination types");
    check(row.vals[2].s.empty() && row.vals[2].v.size()==3,"stored vector normalized");
  }
  check(read(db,1).vals[1].s==" [1, 2, -0] ","VARCHAR bracket spelling retained");
  check(read(db,1).vals[2].v==std::vector<float>({1,2,0}) && std::signbit(read(db,1).vals[2].v[2]),"vector values and signed zero");
  check(read(db,3).vals[1].s==" [7, 8] " && read(db,3).vals[2].v==std::vector<float>({3,4,5}),"explicit column order retained");
}
int main() {
  char path[]="/tmp/mvdb-insert-owned-XXXXXX";check(mkdtemp(path)!=nullptr,"temporary directory");
  Database db;std::string error;check(db.open(path,&error),"open database");
  sql(db,"CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(128),v VECTOR(3));");

  Statement first=parse("INSERT INTO t VALUES (1,' [1, 2, -0] ','[1,2,-0]'),(2,'text','[2,3,4]');");
  // An over-reserved AST must not donate its oversized allocation to the table.
  first.rows[0][2].v.reserve(65536);const size_t capacity=first.rows[0][2].v.capacity();
  const Statement saved=first;
  sql(db,"BEGIN;");success(db,first);unchanged(first,saved);
  check(first.rows[0][2].v.capacity()==capacity,"AST reserve retained");
  check(read(db,1).vals[2].v.capacity()<capacity,"stored row does not inherit oversized AST reserve");
  sql(db,"ROLLBACK;");check(db.table("t")->live_count()==0,"rollback after success");
  success(db,first);unchanged(first,saved);
  failure(db,first,1062);unchanged(first,saved);

  Statement mapped=parse("INSERT INTO t(v,s,id) VALUES ('[3,4,5]',' [7, 8] ',3);");
  mapped.rows[0][0].v.reserve(65536);const size_t mapped_capacity=mapped.rows[0][0].v.capacity();
  const Statement mapped_saved=mapped;
  sql(db,"BEGIN;");success(db,mapped);unchanged(mapped,mapped_saved);
  check(mapped.rows[0][0].v.capacity()==mapped_capacity,"mapped AST reserve retained");
  check(read(db,3).vals[2].v.capacity()<mapped_capacity,"mapped row does not inherit oversized AST reserve");
  sql(db,"ROLLBACK;");check(db.table("t")->pk_pos.count(3)==0,"mapped success rolled back");
  success(db,mapped);unchanged(mapped,mapped_saved);
  failure(db,mapped,1062);unchanged(mapped,mapped_saved);
  sql(db,"INSERT INTO t VALUES (4,'before-failure','[0,0,0]');");
  verify_table(db);

  const Statement scalar=parse("INSERT INTO t VALUES ('9',42,'[0,0,0]');");
  const Statement scalar_saved=scalar;
  sql(db,"BEGIN;");success(db,scalar);unchanged(scalar,scalar_saved);
  check(read(db,9).vals[0].i==9 && read(db,9).vals[1].s=="42","scalar coercions retained");
  sql(db,"ROLLBACK;");verify_table(db);

  for(const auto& item:std::vector<std::pair<std::string,int>>{
      {"INSERT INTO t(id,v,id) VALUES (9,'[1,2,3]',9);",1110},
      {"INSERT INTO t(id,v) VALUES (9,'[1,2,3]');",1364},
      {"INSERT INTO t VALUES (9,'good','[1,2,3]'),(10,'bad','[1,2]');",1264},
      {"INSERT INTO t VALUES (9,'first','[1,2,3]'),(9,'duplicate','[4,5,6]');",1062},
      {"INSERT INTO t(v,id,s) VALUES ('[1,2,3]',9,'first'),('[1,2]',10,'bad');",1264},
      {"INSERT INTO t(v,id,s) VALUES ('[1,2,3]',9,'first'),('[4,5,6]',9,'duplicate');",1062},
      {"INSERT INTO t VALUES (9,[1,2,3],'[0,0,0]');",1264},
      {"INSERT INTO t VALUES (9,'valid','not vector');",1264}}) {
    Statement bad=parse(item.first);const Statement before=bad;
    for(bool explicit_txn:{false,true}) {
      if(explicit_txn){sql(db,"BEGIN;");sql(db,"INSERT INTO t VALUES (99,'previous statement','[0,0,0]');");}
      for(int repeat=0;repeat<2;++repeat) {
        failure(db,bad,item.second);unchanged(bad,before);
        check(db.table("t")->pk_pos.count(9)==0 && db.table("t")->pk_pos.count(10)==0,"failed statement earlier rows rolled back");
        check(db.table("t")->pk_pos.count(99)==size_t(explicit_txn),"previous explicit transaction statement retained");
      }
      if(explicit_txn)sql(db,"ROLLBACK;");
      verify_table(db);
    }
  }
  // Manually constructed nonfinite and wrong-size literals still use the
  // existing Database validation; dropping spelling cannot bypass it.
  Statement nonfinite=parse("INSERT INTO t VALUES (9,'first','[1,2,3]'),(10,'bad','[4,5,6]');");
  nonfinite.rows[1][2].v[1]=std::numeric_limits<float>::infinity();
  const Statement inf_saved=nonfinite;failure(db,nonfinite,1264);unchanged(nonfinite,inf_saved);verify_table(db);
  db.close();check(db.open(path,&error),"reopen persisted rows");verify_table(db);unchanged(first,saved);
  db.close();std::filesystem::remove_all(path);
  std::printf("PASS INSERT owned-copy %zu checks\n",checks);
}

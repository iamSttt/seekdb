// In-process diagnostic only: separate parser and engine cost from JSONL IPC.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>
using namespace mvdb;
using Clock=std::chrono::steady_clock;
int main() {
  char path[]="/tmp/mvdb-query-profile-XXXXXX";
  if(!::mkdtemp(path))return 1;
  Database db;std::string err,out;int code=0;
  if(!db.open(path,&err))throw std::runtime_error(err);
  Column pk;pk.name="id";pk.type=Type::BIGINT;pk.is_primary=true;
  Column grp;grp.name="grp";grp.type=Type::BIGINT;
  Column vec;vec.name="v";vec.type=Type::VECTOR;vec.len=128;
  if(!db.create_table("t",{pk,grp,vec},&err,&code))throw std::runtime_error(err);
  std::mt19937 rng(91417);std::normal_distribution<float> normal;
  if(!db.begin(&err,&code))throw std::runtime_error(err);
  for(int i=9999;i>=0;--i){
    std::vector<float> v(128);for(float& x:v)x=normal(rng);
    if(!db.insert("t",{Value::ofInt(i),Value::ofInt(i%10),Value::ofVec(v)},&err,&code))throw std::runtime_error(err);
  }
  if(!db.commit(&err,&code) || !db.create_vector_index("t","v","vi",&err,&code))throw std::runtime_error(err);
  std::vector<std::string> sql;
  for(int i=0;i<1000;++i){
    std::string q="'[";for(int j=0;j<128;++j){if(j)q+=',';q+=std::to_string(normal(rng));}q+="]'";
    sql.push_back("SELECT id,L2_DISTANCE(v,"+q+") AS d FROM t"+
      (i%2?" WHERE grp="+std::to_string((i/2)%10):"")+" ORDER BY d LIMIT 10;");
  }
  std::vector<Statement> statements;statements.reserve(sql.size());
  const auto start=Clock::now();
  for(const auto& query:sql){Statement st;if(!parse_sql(query,&st,&err,&code))throw std::runtime_error(err);statements.push_back(std::move(st));}
  const double parse_seconds=std::chrono::duration<double>(Clock::now()-start).count();
  double elapsed[2]={};size_t output_bytes=0;
  for(size_t i=0;i<statements.size();++i){
    const auto begin=Clock::now();
    if(!execute_json(db,statements[i],&out,&err,&code))throw std::runtime_error(err);
    elapsed[i%2]+=std::chrono::duration<double>(Clock::now()-begin).count();output_bytes+=out.size();
  }
  std::cout<<"query_profile parse_1000_seconds="<<parse_seconds
    <<" execute_500_unfiltered_seconds="<<elapsed[0]<<" execute_500_filtered_seconds="<<elapsed[1]
    <<" output_bytes="<<output_bytes<<"\n";
}

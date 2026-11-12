// In-process diagnostic only: separate parser and engine cost from JSONL IPC.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include "../third_party/json.hpp"
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
  using Json=nlohmann::json;
  std::vector<std::string> requests;requests.reserve(sql.size());
  for(size_t i=0;i<sql.size();++i)
    requests.push_back(Json{{"id",i},{"op","execute"},{"sql",sql[i]}}.dump());
  size_t consumed_bytes=0;
  const auto json_start=Clock::now();
  std::vector<Json> parsed_requests;parsed_requests.reserve(requests.size());
  for(const auto& request:requests) {
    auto parsed=Json::parse(request);
    consumed_bytes+=parsed.at("sql").get_ref<const std::string&>().size();
    parsed_requests.push_back(std::move(parsed));
  }
  const double json_parse_seconds=std::chrono::duration<double>(Clock::now()-json_start).count();
  const auto dump_start=Clock::now();
  for(const auto& parsed:parsed_requests)consumed_bytes+=parsed.dump().size();
  const double json_dump_seconds=std::chrono::duration<double>(Clock::now()-dump_start).count();
  double elapsed[2]={};size_t output_bytes=0;
  for(size_t i=0;i<statements.size();++i){
    const auto begin=Clock::now();
    if(!execute_json(db,statements[i],&out,&err,&code))throw std::runtime_error(err);
    elapsed[i%2]+=std::chrono::duration<double>(Clock::now()-begin).count();output_bytes+=out.size();
  }
  std::cout<<"query_profile parse_1000_seconds="<<parse_seconds
    <<" execute_500_unfiltered_seconds="<<elapsed[0]<<" execute_500_filtered_seconds="<<elapsed[1]
    <<" output_bytes="<<output_bytes<<"\n";
  std::cout<<Json{{"kind","native_query_profile"},{"queries",sql.size()},
    {"sql_parse_seconds",parse_seconds},{"json_parse_seconds",json_parse_seconds},
    {"json_dump_seconds",json_dump_seconds},{"execute_unfiltered_seconds",elapsed[0]},
    {"execute_filtered_seconds",elapsed[1]},{"output_bytes",output_bytes},
    {"consumed_bytes",consumed_bytes},
    {"scope","Separate in-process passes; JSON DOM parse/dump microbenchmark is not the strict production request parser. Do not subtract from different transport workloads."}}.dump()<<"\n";
}

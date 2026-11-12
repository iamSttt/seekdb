// Build with MVDB_METER_PARSER_ALLOCATIONS for real new/delete counters; run
// timing without it so instrumentation does not distort the timing comparison.
#include "sql.h"
#include <algorithm>
#ifdef MVDB_METER_PARSER_ALLOCATIONS
#include "../tests/parser_allocation_meter.h"
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

struct Case {const char* name;size_t iterations;std::vector<std::string> inputs;bool valid=true;};
static std::string vector_text(size_t dimensions,int seed) {
  std::string value="[";
  for(size_t i=0;i<dimensions;++i) {
    if(i)value+=',';
    value+=std::to_string((int(i)*13+seed)%97-48)+".125";
  }
  return value+"]";
}
static Case make_case(const char* name,size_t iterations,int mode) {
  Case result{name,iterations,{}};
  for(int seed=0;seed<8;++seed) {
    std::string sql;
    if(mode==0)sql="SELECT id FROM table"+std::to_string(seed)+" WHERE id="+std::to_string(seed)+" LIMIT 10";
    else if(mode==1)sql="SELECT id FROM t ORDER BY L2_DISTANCE(v,'"+vector_text(128,seed)+"') LIMIT 10";
    else if(mode==2)sql="SELECT `long``identifier` FROM t WHERE s='"+std::string(80,'a'+seed)+"\\nquoted''tail' LIMIT 3";
    else if(mode==3 || mode==4) {
      sql="INSERT INTO t VALUES ";
      const int rows=mode==3 ? 32 : 384;
      for(int row=0;row<rows;++row) {
        if(row)sql+=',';
        sql+='('+std::to_string(row)+",'"+vector_text(128,seed+row)+"')";
      }
    } else {sql="SELECT id FROM t WHERE s='"+std::string(200,'a'+seed);result.valid=false;}
    result.inputs.push_back(std::move(sql));
  }
  return result;
}
int main(int argc,char** argv) {
  const size_t divisor=argc>1 ? std::max(1,std::atoi(argv[1])) : 1;
  const std::vector<Case> cases={make_case("short_select",20000,0),make_case("vector128",5000,1),
      make_case("escaped",10000,2),make_case("insert32",400,3),make_case("oversized_insert",24,4),
      make_case("syntax_error",5000,5)};
  size_t checksum=0;
  for(const Case& item:cases) {
    // Initialize worker pool and token capacities outside the observed loop.
    for(const std::string& sql:item.inputs) {
      mvdb::Statement st;std::string error;int code=0;
      if(mvdb::parse_sql(sql,&st,&error,&code,false)!=item.valid)throw std::runtime_error("warmup status mismatch");
    }
    const size_t iterations=std::max<size_t>(1,item.iterations/divisor);
#ifdef MVDB_METER_PARSER_ALLOCATIONS
    const size_t before_calls=parser_meter::calls.load(),before_bytes=parser_meter::bytes.load();
    const size_t before_live=parser_meter::live.load();
#endif
    const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<iterations;++i) {
      mvdb::Statement st;std::string error;int code=0;
      const bool ok=mvdb::parse_sql(item.inputs[i%item.inputs.size()],&st,&error,&code,false);
      if(ok!=item.valid || !st.raw.empty())throw std::runtime_error("parse status/raw mismatch");
      if(!ok && code!=1064)throw std::runtime_error("error code mismatch");
      checksum+=st.items.size()+st.rows.size()+st.conds.size()+error.size();
    }
    const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    std::printf("{\"case\":\"%s\",\"iterations\":%zu,\"elapsed_ns\":%lld,\"checksum\":%zu,\"instrumented\":%s",
        item.name,iterations,static_cast<long long>(elapsed),checksum,
#ifdef MVDB_METER_PARSER_ALLOCATIONS
        "true");
    std::printf(",\"new_calls\":%zu,\"requested_bytes\":%zu,\"retained_delta_bytes\":%lld",
        parser_meter::calls.load()-before_calls,parser_meter::bytes.load()-before_bytes,
        static_cast<long long>(parser_meter::live.load())-static_cast<long long>(before_live));
#else
        "false");
#endif
    std::puts("}");
  }
}

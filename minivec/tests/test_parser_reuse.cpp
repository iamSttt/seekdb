#include "sql.h"
#include "parser_allocation_meter.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#ifndef MVDB_TOKEN_REUSE_BYTES
#define MVDB_TOKEN_REUSE_BYTES (256u << 10)
#endif
using namespace mvdb;
static constexpr size_t retained_limit=
#ifdef MVDB_ENABLE_TOKEN_REUSE
    MVDB_TOKEN_REUSE_BYTES;
#else
    0;
#endif
static std::string batch(bool invalid=false) {
  std::string sql="INSERT INTO t VALUES ";
  for(int i=0;i<32;++i) {
    if(i)sql+=',';
    sql+='('+std::string(invalid && i==17 ? "9223372036854775808" : "7")+
        ",'left\\nright''tail','[0,1,2,3]')";
  }
  return sql;
}
static void good() {
  Statement st;std::string error;int code=0;
  assert(parse_sql("SELECT id FROM t WHERE id=17 LIMIT 3",&st,&error,&code,false));
  assert(st.raw.empty() && st.table=="t" && st.limit==3 && st.conds[0].value.i==17);
  assert(error.empty() && !code);
}
static void lifetime_and_errors() {
  Statement kept;std::string error;int code=0;
  {
    std::string input=batch();
    assert(parse_sql(input,&kept,&error,&code,false));
    input.assign(input.size(),'z');std::string().swap(input);
  }
  for(int repeat=0;repeat<20;++repeat) {
    good();
    for(const std::string& bad:std::vector<std::string>{"SELECT 'broken", "SELECT `broken", "SELECT [1,", "SELECT /* broken", batch(true)}) {
      Statement failed;
      assert(!parse_sql(bad,&failed,&error,&code,false));
      assert(failed.kind==Statement::UNKNOWN && failed.raw.empty() && code==1064);
      good();
    }
    assert(kept.rows.size()==32 && kept.rows[17][1].s=="left\nright'tail");
    assert(kept.rows[17][2].v==std::vector<float>({0,1,2,3}));
  }
  // Vary values and token kinds at the same reused positions; no template or
  // stale literal/quoted-identifier state may influence the next statement.
  for(int i=0;i<200;++i) {
    Statement current;
    const std::string sql="SELECT `select` FROM `table"+std::to_string(i)+"` WHERE s='value"+std::to_string(i)+"'";
    assert(parse_sql(sql,&current,&error,&code,i%2));
    assert(current.table=="table"+std::to_string(i));
    assert(current.conds[0].value.s=="value"+std::to_string(i));
    assert(current.raw==(i%2 ? sql : std::string()));
  }
}
static void retained_memory() {
  const size_t before=parser_meter::live.load();
  {
    Statement statement;std::string error;int code=0;
    const std::string large="INSERT INTO t VALUES ('"+std::string(2u<<20,'x')+"')";
    assert(parse_sql(large,&statement,&error,&code,false));
    assert(statement.rows[0][0].s.size()==2u<<20);
  }
  assert(parser_meter::live.load()<=before+retained_limit);
  {
    Statement statement;std::string error;int code=0;
    std::string many="SELECT id";
    for(int i=0;i<12000;++i)many+=",id";
    many+=" FROM t";
    assert(parse_sql(many,&statement,&error,&code,false));
    assert(statement.items.size()==12001);
  }
  assert(parser_meter::live.load()<=before+retained_limit);
  good();
  assert(parser_meter::live.load()<=before+retained_limit);
}
static void allocation_failures() {
  const std::string input="SELECT long_column_name_that_allocates FROM long_table_name_that_allocates "
      "WHERE long_column_name_that_allocates='long literal that allocates memory' ORDER BY id LIMIT 3";
  size_t failures=0;
  for(int point=0;point<64;++point) {
    Statement statement;std::string error;int code=0;
    parser_meter::fail_after=point;
    const bool ok=parse_sql(input,&statement,&error,&code,false);
    parser_meter::fail_after=-1;
    if(!ok) {
      ++failures;assert(code==1064 && statement.kind==Statement::UNKNOWN);
      assert(error=="out of memory while parsing the statement");
    }
    assert(statement.raw.empty());good();
  }
  assert(failures>0);
}
static bool nested_ok=false;
static void nested_parse() {
  Statement statement;std::string error;int code=0;
  nested_ok=parse_sql("SELECT id FROM nested WHERE id=19",&statement,&error,&code,false);
  assert(nested_ok && statement.table=="nested" && statement.conds[0].value.i==19);
}
static void reentrant_and_threads() {
  const std::string outer="SELECT outer_column_name_that_allocates FROM outer_table_name_that_allocates WHERE id=23";
  {
    Statement statement;std::string error;int code=0;
    parser_meter::allocation_hook=nested_parse;
    assert(parse_sql(outer,&statement,&error,&code,false));
    parser_meter::allocation_hook=nullptr;
    assert(nested_ok && statement.table=="outer_table_name_that_allocates" && statement.conds[0].value.i==23);
  }
  const size_t before=parser_meter::live.load();
  {
    std::vector<std::thread> workers;
    for(int worker=0;worker<4;++worker)workers.emplace_back([worker] {
      for(int i=0;i<200;++i) {
        Statement st;std::string error;int code=0;
        const std::string table="worker"+std::to_string(worker);
        const std::string text="payload-"+std::to_string(i)+std::string(64,'a'+worker);
        std::string input="SELECT id FROM "+table+" WHERE s='"+text+"'";
        assert(parse_sql(input,&st,&error,&code,false));
        input.clear();assert(st.table==table && st.conds[0].value.s==text);
      }
    });
    for(auto& worker:workers)worker.join();
  }
  // Every caller thread's cache is destroyed at exit, including string buffers.
  assert(parser_meter::live.load()==before);
}
int main() {
  alarm(120);
  // Start the existing literal-materialization pool before measuring retained
  // parser memory. Its deliberately process-lived workers are not this cache.
  {Statement st;std::string error;int code=0;assert(parse_sql(batch(),&st,&error,&code,false));}
  lifetime_and_errors();retained_memory();allocation_failures();reentrant_and_threads();
  std::printf("PASS owned token reuse, input destruction, errors/OOM, reentrancy, threads and bounded retention; new_calls=%zu requested_bytes=%zu\n",
      parser_meter::calls.load(),parser_meter::bytes.load());
}

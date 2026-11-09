#include "db.h"
#include "exec.h"
#include "sql.h"
#include "distance.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <unistd.h>
using namespace mvdb;
struct Oracle {int64_t id,n;std::string text;double d;};
static std::string query(Database& db,const std::string& text,bool stream) {
  Statement st;std::string err,out,total;int code=0;
  assert(parse_sql(text,&st,&err,&code));
  OutputSink sink;
  if(stream)sink=[&](const std::string& s){total+=s;};
  assert(execute(db,st,&out,&err,&code,sink));
  return total+out;
}
static std::vector<int64_t> ids(const std::string& text) {
  std::istringstream in(text);std::string line;std::getline(in,line);
  std::vector<int64_t> result;
  while(std::getline(in,line))if(!line.empty())result.push_back(std::stoll(line));
  return result;
}
static int stress_topk() {
  struct rlimit cap{160ull<<20,160ull<<20};assert(::setrlimit(RLIMIT_AS,&cap)==0);
  char path[]="/tmp/mvdb-query-topk-XXXXXX";assert(::mkdtemp(path));
  try {
    Database db;std::string err;int code=0;assert(db.open(path,&err));
    Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
    Column v;v.name="v";v.type=Type::VECTOR;v.len=262144;
    assert(db.create_table("t",{id,v},&err,&code));
    for(int i=0;i<256;++i) {
      if(i%16==0)assert(db.begin(&err,&code));
      assert(db.insert("t",{Value::ofInt(i),Value::ofVec(std::vector<float>(262144,float(i)))},&err,&code));
      if(i%16==15)assert(db.commit(&err,&code));
    }
    const auto actual=ids(query(db,"SELECT id FROM t ORDER BY id DESC LIMIT 256",true));
    assert(actual.size()==256);
    for(size_t i=0;i<actual.size();++i)assert(actual[i]==255-static_cast<int64_t>(i));
    std::cout<<"PASS 256MiB unprojected vector payload TopK under 160MiB address cap\n";
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"FAIL TopK candidate memory gate: "<<e.what()<<'\n';return 1;
  }
}
int main(int argc,char** argv) {
  ::setenv("MVDB_OUT","tsv",1);
  if(argc>1 && std::string(argv[1])=="--stress-topk")return stress_topk();
  char path[]="/tmp/mvdb-query-test-XXXXXX";assert(::mkdtemp(path));
  Database db;std::string err;int code=0;assert(db.open(path,&err));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column n;n.name="n";n.type=Type::BIGINT;
  Column s;s.name="s";s.type=Type::VARCHAR;s.len=30000;
  Column v;v.name="v";v.type=Type::VECTOR;v.len=5;
  assert(db.create_table("t",{id,n,s,v},&err,&code));
  std::vector<Oracle> oracle;
  assert(db.begin(&err,&code));
  for(int64_t i=599;i>=0;--i) {
    const int64_t number=std::numeric_limits<int64_t>::max()-i%7;
    const std::string text=(i%3==0?"汉":i%3==1?"é":"z")+std::string(24000,'a'+i%7);
    const float a=static_cast<float>(i%11),b=static_cast<float>((i/11)%7);
    oracle.push_back({i,number,text,double(a)*a+double(b)*b+1});
    assert(db.insert("t",{Value::ofInt(i),Value::ofInt(number),Value::ofStr(text),Value::ofVec({a,b,0,1,0})},&err,&code));
  }
  assert(db.commit(&err,&code));
  for(bool desc:{false,true}) {
    auto sorted=oracle;
    std::sort(sorted.begin(),sorted.end(),[&](const Oracle&a,const Oracle&b){
      if(a.n==b.n)return a.id<b.id;return desc?a.n>b.n:a.n<b.n;
    });
    std::vector<int64_t> expected;for(size_t j=0;j<17;++j)expected.push_back(sorted[j].id);
    const std::string sql="SELECT id FROM t ORDER BY n "+std::string(desc?"DESC":"ASC")+" LIMIT 17";
    assert(ids(query(db,sql,false))==expected);assert(ids(query(db,sql,true))==expected);
  }
  auto sorted=oracle;
  std::sort(sorted.begin(),sorted.end(),[](const Oracle&a,const Oracle&b){
    if(a.d==b.d)return a.id<b.id;return a.d<b.d;
  });
  std::vector<int64_t> near;for(size_t j=0;j<23;++j)near.push_back(sorted[j].id);
  assert(ids(query(db,"SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0,0,0]') LIMIT 23",true))==near);
  std::vector<int64_t> filtered;
  for(const auto& r:sorted)if(r.n==std::numeric_limits<int64_t>::max() && filtered.size()<23)filtered.push_back(r.id);
  assert(ids(query(db,"SELECT id FROM t WHERE n=9223372036854775807 ORDER BY L2_DISTANCE(v,'[0,0,0,0,0]') LIMIT 23",true))==filtered);
  assert(ids(query(db,"SELECT id FROM t ORDER BY n LIMIT 0",true)).empty());
  std::sort(sorted.begin(),sorted.end(),[](const Oracle&a,const Oracle&b){
    const int c=utf8_compare(a.text,b.text);return c==0?a.id<b.id:c>0;
  });
  std::vector<int64_t> ordered;for(const auto& r:sorted)ordered.push_back(r.id);
  // Large nonprojected VARCHAR sort keys force multiple external runs.
  assert(ids(query(db,"SELECT id FROM t ORDER BY s DESC",true))==ordered);
  assert(ids(query(db,"SELECT id FROM t ORDER BY s DESC LIMIT 10000",true))==ordered);
  const std::string full=query(db,"SELECT s,id FROM t ORDER BY n DESC",true);
  const std::string buffered=query(db,"SELECT s,id FROM t ORDER BY n DESC",false);
  assert(full==buffered);
  const float hi=std::numeric_limits<float>::max();
  for(size_t length:{size_t(1),size_t(3),size_t(4),size_t(5),size_t(127),size_t(128),size_t(129)}) {
    std::vector<float>a(length,hi),b(length,-hi);
    const double actual=distance_squared(a.data(),b.data(),length);
    const long double diff=static_cast<long double>(hi)*2;
    const long double expected=diff*diff*length;
    assert(std::isfinite(actual));
    assert(std::fabs(static_cast<long double>(actual)-expected)<=expected*1e-14L);
    assert(distance_squared(a.data(),a.data(),length)==0);
  }
  std::cout<<"PASS TopK ties, BIGINT precision, nonprojected keys, spill-safe copies, streaming/external sorting, float32 extrema\n";
}

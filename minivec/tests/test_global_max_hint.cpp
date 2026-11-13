// Build with -DMVDB_TEST_GLOBAL_MAX_HINT and db/exec/sql/value/flat_index.cpp.
// This is a correctness/mechanism fixture, not a performance benchmark.
#include "db.h"
#include "exec.h"
#include "quantized_bound.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <vector>
#include <unistd.h>

#ifndef MVDB_TEST_GLOBAL_MAX_HINT
#error This fixture requires the test-only coordinate counters.
#endif
using namespace mvdb;
namespace {
size_t checks=0;
void check(bool value,const char* why) {
  ++checks;if(!value){std::fprintf(stderr,"FAIL global max hint: %s (%zu)\n",why,checks);std::exit(1);}
}
float fp(uint32_t bits){float v;std::memcpy(&v,&bits,4);return v;}
uint32_t bits(float v){uint32_t b;std::memcpy(&b,&v,4);return b;}
uint64_t bits(double v){uint64_t b;std::memcpy(&b,&v,8);return b;}
struct Directory {
  char path[64]="/tmp/mvdb-global-max-hint-XXXXXX";
  Directory(){check(mkdtemp(path)!=nullptr,"temporary directory");}
  ~Directory(){std::error_code e;std::filesystem::remove_all(path,e);}
};
struct Environment {
  std::fenv_t saved;
#ifdef MVDB_SIMD_BOUND_X86
  unsigned csr;
#endif
  Environment(){
    check(std::fegetenv(&saved)==0,"save FP environment");
#ifdef MVDB_SIMD_BOUND_X86
    csr=_mm_getcsr();_mm_setcsr((_mm_getcsr()|0x1f80u)&~0xe07fu);
#endif
    check(std::fesetround(FE_TONEAREST)==0,"nearest rounding");
  }
  ~Environment(){std::fesetenv(&saved);
#ifdef MVDB_SIMD_BOUND_X86
    _mm_setcsr(csr);
#endif
  }
};
void clear_flags(){std::feclearexcept(FE_ALL_EXCEPT);
#ifdef MVDB_SIMD_BOUND_X86
  _mm_setcsr(_mm_getcsr()&~0x3fu);
#endif
}
unsigned flags(){unsigned f=unsigned(std::fetestexcept(FE_ALL_EXCEPT));
#ifdef MVDB_SIMD_BOUND_X86
  f|=(_mm_getcsr()&0x3fu)<<16;
#endif
  return f;
}
// A legitimate producer has already performed the original full double norm
// pass. In particular, its subnormal conversions set sticky DE before finish.
// Do not clear flags between these operations when comparing FP state.
#if defined(__GNUC__)
__attribute__((noinline,noclone))
#endif
double original_norm(const float* p,size_t n){double norm=0;for(size_t d=0;d<n;++d){const double x=p[d];norm+=x*x;}return norm;}
volatile double norm_sink=0;
uint32_t integer_max(const std::vector<float>& values){uint32_t m=0;for(float v:values)m=std::max(m,bits(v)&0x7fffffffu);return m;}
void max_oracle(){
  std::vector<float> values={0.0f,-0.0f,fp(1),fp(0x80000001u),fp(0x007fffffu),fp(0x00800000u)};
  uint32_t state=7319;
  for(unsigned exponent=0;exponent<255;++exponent)for(uint32_t mantissa:{0u,1u,0x3fffffu,0x7fffffu})
    for(uint32_t sign:{0u,0x80000000u}){
      float v=fp(sign|(exponent<<23)|mantissa);
      const double old=std::fabs(static_cast<double>(v));
      const double current=static_cast<double>(fp(bits(v)&0x7fffffffu));
      check(bits(old)==bits(current),"integer abs bits match finite double fabs");
    }
  for(size_t i=0;i<4096;++i){state=1664525u*state+1013904223u;uint32_t v=state;if((v&0x7f800000u)==0x7f800000u)v^=0x00800000u;values.push_back(fp(v));}
  double old=0;uint32_t current=0;
  for(float v:values){old=std::max(old,std::fabs(static_cast<double>(v)));current=std::max(current,bits(v)&0x7fffffffu);check(bits(old)==bits(static_cast<double>(fp(current))),"prefix maximum bits match independent FP scan");}
}
void compare_views(const FlatIndex::ResidentView& a,const FlatIndex::ResidentView& b){
  check(a.count==b.count && a.dimension==b.dimension,"index shape");
  check(bool(a.quantized)==bool(b.quantized) && bits(a.quantization_step)==bits(b.quantization_step),"code admission and step bits");
  check(a.quantization_scales.present()==b.quantization_scales.present(),"block admission");
  for(size_t i=0;i<a.count;++i){check(a.keys[i]==b.keys[i] && a.slots[i]==b.slots[i],"key and slot order");check(std::memcmp(a.rows[i],b.rows[i],a.dimension*sizeof(float))==0,"borrowed payload bits");}
  if(a.quantized)check(std::memcmp(a.quantized,b.quantized,a.count*a.dimension)==0,"all quantized code bytes");
  if(a.quantization_scales.present()){
    const auto& x=a.quantization_scales;const auto& y=b.quantization_scales;
    check(x.block_count==y.block_count && x.step_count==y.step_count && x.row_count==y.row_count && x.query_cache_bytes==y.query_cache_bytes,"block metadata");
    check(std::memcmp(x.block_ids,y.block_ids,x.block_count)==0,"block ids");
    check(std::memcmp(x.steps,y.steps,x.step_count*sizeof(double))==0,"block step bits");
  }
}
std::vector<float> fixture(size_t n,size_t dimension,unsigned kind){
  std::vector<float> out(n*dimension);
  for(size_t i=0;i<out.size();++i){
    if(kind==0)out[i]=float(int((i*13)%251)-125)*0.125f;
    else if(kind==1)out[i]=(i&1)?-0.0f:0.0f;
    else if(kind==2)out[i]=fp((i&1?0x80000000u:0u)|uint32_t(1+i%31));
    else if(kind==3)out[i]=(i&1)?-std::numeric_limits<float>::max():std::numeric_limits<float>::max();
    else out[i]=float(int((i*13)%251)-125)*(i<dimension*1024 ? 0.000125f : 1024.0f);
  }
  return out;
}
void physical(std::vector<float> values,size_t dimension,size_t budget,unsigned invalid=0,bool expect_local=false){
  Directory dir;const size_t n=values.size()/dimension;
  FlatIndex old(dir.path,dimension,n,budget),hinted(dir.path,dimension,n,budget);
  check(old.resident_bytes()<=budget && hinted.resident_bytes()<=budget,"initial budget");
  const bool eligible=hinted.wants_global_max_hint();
#if defined(MVDB_DISABLE_NORM_ORDER) || defined(MVDB_DISABLE_QUANTIZED_BOUND) || defined(MVDB_ENABLE_BLOCK_QUANT_SCALE)
  check(!eligible,"compile-time feature fallback");
#endif
  for(size_t i=0;i<n;++i){old.add(int64_t(n-i),i*3,values.data()+i*dimension,dimension);hinted.add(int64_t(n-i),i*3,values.data()+i*dimension,dimension);}
  FlatIndex::GlobalMaxHint hint{integer_max(values),n,dimension};
  if(invalid==1)++hint.rows;
  if(invalid==2)++hint.dimension;
  if(invalid==3)hint.max_abs_bits=0x7f800000u;
  if(invalid==4)hint.max_abs_bits=0x7fc00001u;
  if(invalid==5)hint.max_abs_bits=0x80000000u;
  clear_flags();for(size_t i=0;i<n;++i)norm_sink=original_norm(values.data()+i*dimension,dimension);
  global_max_hint_diagnostics={};old.finish();const auto before=global_max_hint_diagnostics;const unsigned old_flags=flags();
  clear_flags();for(size_t i=0;i<n;++i)norm_sink=original_norm(values.data()+i*dimension,dimension);
  global_max_hint_diagnostics={};hinted.finish(&hint);const auto after=global_max_hint_diagnostics;const unsigned new_flags=flags();
  check(old_flags==new_flags,"complete norm-to-finish FP status");
  const bool accepted=eligible && invalid==0 && hint.max_abs_bits<0x7f800000u;
  check(before.used_hints==0 && after.used_hints==(accepted?1u:0u),"hint acceptance counter");
  check(after.max_scan_coordinates==(accepted?0:before.max_scan_coordinates),"only accepted hint skips max scan");
  check(before.norm_scan_coordinates==0 && after.norm_scan_coordinates==0,"standalone index does not claim DB norm work");
  const auto saved=global_max_hint_diagnostics;hinted.finish(&hint);
  check(global_max_hint_diagnostics.used_hints==saved.used_hints && global_max_hint_diagnostics.max_scan_coordinates==saved.max_scan_coordinates,"finish idempotence");
  check(!hinted.wants_global_max_hint(),"finished index no longer wants hint");
  check(old.resident_bytes()<=budget && hinted.resident_bytes()<=budget,"finished budget");
  FlatIndex::ResidentView a,b;
  const bool ar=old.resident_view(&a),br=hinted.resident_view(&b);check(ar==br,"resident admission");
  if(ar){
    compare_views(a,b);
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
    if(expect_local)check(a.quantization_scales.present() && b.quantization_scales.present(),"block fallback exercises admitted local scales");
#else
    (void)expect_local;
#endif
  }
  else {
    std::vector<uint32_t> x,y;
    const auto scan=[&](FlatIndex& index,std::vector<uint32_t>& result){index.scan([&](int64_t pk,uint64_t slot,const float* p,size_t d){result.push_back(uint32_t(pk));result.push_back(uint32_t(slot));for(size_t j=0;j<d;++j)result.push_back(bits(p[j]));return true;});};
    scan(old,x);scan(hinted,y);check(x==y,"disk fallback records identical");
  }
}
void physical_cases(){
  for(unsigned distribution=0;distribution<4;++distribution){physical(fixture(3,16,distribution),16,1u<<20);physical(fixture(19,128,distribution),128,1u<<20);}
  for(unsigned invalid=1;invalid<=5;++invalid)physical(fixture(3,16,0),16,1u<<20,invalid);
  for(uint32_t bad:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u}){
    auto row=fixture(3,16,0);row[17]=fp(bad);physical(row,16,1u<<20);
  }
  physical(fixture(1,16,0),16,1u<<20);physical({},16,1u<<20);
  physical(fixture(3,15,0),15,1u<<20);physical(fixture(3,16,0),16,0);
  const size_t metadata=3*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  const size_t posting=3*(2*sizeof(int64_t)+sizeof(uint32_t));
  physical(fixture(3,16,0),16,metadata);physical(fixture(3,16,0),16,metadata+posting+3*16-1);
  physical(fixture(3,16,0),16,metadata+posting+3*16);
  physical(fixture(1280,128,4),128,16u<<20,0,true);
  Directory dir;FlatIndex incomplete(dir.path,16,2,1u<<20);auto row=fixture(1,16,0);incomplete.add(1,0,row.data(),16);
  bool caught=false;try{FlatIndex::GlobalMaxHint h{integer_max(row),1,16};incomplete.finish(&h);}catch(const std::runtime_error&){caught=true;}check(caught,"incomplete index still rejects hint");
}
void fp_controls(){
  auto row=fixture(3,16,0);row[0]=fp(1);row[1]=-0.0f;
  for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){check(std::fesetround(mode)==0,"set nondefault rounding");physical(row,16,1u<<20);check(std::fesetround(FE_TONEAREST)==0,"restore rounding");}
#ifdef MVDB_SIMD_BOUND_X86
  const unsigned saved=_mm_getcsr();
  for(unsigned mask:{1u<<6,1u<<15,(1u<<6)|(1u<<15)}){_mm_setcsr(saved|mask);physical(row,16,1u<<20);_mm_setcsr(saved);}
#endif
}
using Model=std::map<int64_t,std::vector<float>>;
std::string sql(Database& db,const std::string& text){Statement st;std::string error,out;int code=0;check(parse_sql(text,&st,&error,&code,false),"parse SQL");check(execute_json(db,st,&out,&error,&code),"execute SQL");return out;}
void inspect_database(Database& db,const Model& model){
  Table* table=db.table("t");check(table && table->live_count()==model.size() && table->has_index,"live table/index model");
  FlatIndex::ResidentView actual;check(db.resident_vector_view("t",&actual),"DB resident index exists");
  const auto diag=global_max_hint_diagnostics;const size_t dimension=16;
#ifdef MVDB_DISABLE_NORM_ORDER
  check(diag.norm_scan_coordinates==0,"disabled norm pass absent");
#else
  check(diag.norm_scan_coordinates==model.size()*dimension,"original full norm pass retained");
#endif
#if !defined(MVDB_DISABLE_NORM_ORDER) && !defined(MVDB_DISABLE_QUANTIZED_BOUND) && !defined(MVDB_ENABLE_BLOCK_QUANT_SCALE)
  check(diag.used_hints==1 && diag.max_scan_coordinates==0,"actual DB norm path supplies and consumes hint");
#else
  check(diag.used_hints==0,"DB disabled/block fallback does not consume hint");
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  check(diag.max_scan_coordinates==model.size()*dimension,"DB fallback retains complete max scan");
#endif
#endif
  std::vector<std::pair<double,size_t>> order;
  for(size_t slot=0;slot<table->rows.size();++slot){if(table->dead[slot])continue;const Row& r=table->rows[slot];auto it=model.find(r.vals[0].i);check(it!=model.end(),"live key in independent model");check(r.vals[1].v.size()==dimension && std::memcmp(r.vals[1].v.data(),it->second.data(),dimension*sizeof(float))==0,"persisted float bits match model");order.emplace_back(original_norm(it->second.data(),dimension),slot);}
#ifndef MVDB_DISABLE_NORM_ORDER
  std::sort(order.begin(),order.end());
#endif
  FlatIndex reference(db.data_directory(),dimension,model.size(),16u<<20);
  for(const auto& item:order){const Row& r=table->rows[item.second];reference.add(r.vals[0].i,item.second,r.vals[1].v.data(),dimension);}
  reference.finish();FlatIndex::ResidentView old;check(reference.resident_view(&old),"independent no-hint reference");compare_views(old,actual);
  std::vector<std::pair<double,int64_t>> expected;
  for(const auto& item:model)expected.emplace_back(original_norm(item.second.data(),dimension),item.first);
  std::sort(expected.begin(),expected.end());
  auto output=nlohmann::json::parse(sql(db,"SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]') LIMIT 7;"));
  check(output["rows"].size()==7,"exact TopK row count");
  for(size_t i=0;i<7;++i)check(output["rows"][i][0].get<int64_t>()==expected[i].second,"actual executor exact TopK independent norm oracle");
}
void database_lifecycle(){
  Directory dir;Database db;std::string error;int code=0;check(db.open(dir.path,&error),"open DB");
  check(db.create_table("t",{{"id",Type::BIGINT,0,true},{"v",Type::VECTOR,16,false}},&error,&code),"create model table");
  Model committed;check(db.begin(&error,&code),"begin model inserts");
  for(int64_t id=24;id>=1;--id){std::vector<float> v(16);for(size_t d=0;d<16;++d)v[d]=float(int((id%7+d)%9)-4)*0.125f;v[0]=(id&1)?-0.0f:0.0f;committed[id]=v;check(db.insert("t",{Value::ofInt(id),Value::ofVec(v)},&error,&code),"insert model row");}
  check(db.commit(&error,&code),"commit model inserts");global_max_hint_diagnostics={};
  check(db.create_vector_index("t","v","vi",&error,&code),"create physical index");inspect_database(db,committed);
  sql(db,"BEGIN;");sql(db,"UPDATE t SET v='[3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]' WHERE id=4;");sql(db,"DELETE FROM t WHERE id=7;");
  Model temporary=committed;temporary[4]=std::vector<float>(16);temporary[4][0]=3;temporary.erase(7);global_max_hint_diagnostics={};inspect_database(db,temporary);
  global_max_hint_diagnostics={};sql(db,"ROLLBACK;");inspect_database(db,committed);
  sql(db,"BEGIN;");sql(db,"UPDATE t SET v='[5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]' WHERE id=6;");sql(db,"DELETE FROM t WHERE id=9;");sql(db,"INSERT INTO t VALUES (99,'[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]');");sql(db,"COMMIT;");
  committed[6]=std::vector<float>(16);committed[6][0]=5;committed.erase(9);committed[99]=std::vector<float>(16);global_max_hint_diagnostics={};inspect_database(db,committed);
  db.close();global_max_hint_diagnostics={};check(db.open(dir.path,&error),"WAL reopen");inspect_database(db,committed);
  check(db.checkpoint(&error),"checkpoint");db.close();global_max_hint_diagnostics={};check(db.open(dir.path,&error),"snapshot reopen");inspect_database(db,committed);db.close();
}
}
int main(){Environment environment;max_oracle();physical_cases();fp_controls();database_lifecycle();std::printf("global_max_hint_checks=%zu\nglobal_max_hint=PASS\n",checks);}

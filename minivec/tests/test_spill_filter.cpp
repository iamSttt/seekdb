// Adapted for isolated 9ed + spill-only, 2026-11-14; not executed on Linux.
// g++ -std=c++17 -O2 -pthread -Isrc -DMVDB_ENABLE_SPILL_FILTER
//   -DMVDB_SPILL_BYTES=1 tests/test_spill_filter.cpp
//   src/{db,exec,sql,value,flat_index}.cpp -Wl,--wrap=pread -o /tmp/test_spill_filter
// Candidate code is directly enabled. ENABLE selects test expectations only.
// Also compile with MVDB_FLAT_INDEX_MEMORY_BYTES=1 / 8192; use the
// unmodified source baseline only when deliberately testing without ENABLE.
#include "db.h"
#include "exec.h"
#include "distance.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <new>
#include <unistd.h>

namespace {
int fail_after=-1;
bool meter=false;
size_t live=0,peak=0;
struct Allocation {void* pointer=nullptr;size_t bytes=0;} allocations[32];
void release_array(void* pointer) {
  for(auto& allocation:allocations)if(pointer && pointer==allocation.pointer) {
    live-=allocation.bytes;allocation=Allocation{};break;
  }
  std::free(pointer);
}
bool trace=false;
size_t payload_reads=0,flat_reads=0,payload_bytes=0;
bool flat_interrupt=false,flat_short=false,flat_fail=false;
#ifdef MVDB_TEST_WRAP_PWRITE
bool fail_flat_write=false,interrupt_flat_write=false,short_flat_write=false;
size_t flat_write_calls=0;
size_t injected_flat_write_failures=0;
#endif
}
void* operator new[](size_t n) {if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void operator delete[](void* p) noexcept {release_array(p);}
void operator delete[](void* p,size_t) noexcept {release_array(p);}
void* operator new[](size_t n,const std::nothrow_t&) noexcept {
  if(fail_after>=0 && fail_after--==0)return nullptr;
  void* p=nullptr;try {p=::operator new[](n);}catch(...) {return nullptr;}
  if(meter) {
    bool saved=false;
    for(auto& allocation:allocations)if(!allocation.pointer) {allocation={p,n};saved=true;break;}
    assert(saved);live+=n;peak=std::max(peak,live);
  }
  return p;
}
void operator delete[](void* p,const std::nothrow_t&) noexcept {release_array(p);}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t bytes,off_t offset) {
  char path[64],target[512];std::snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
  const ssize_t n=::readlink(path,target,sizeof(target)-1);
  if(n>=0)target[n]='\0';else target[0]='\0';
  const bool payload=std::strstr(target,"/.payload-")!=nullptr;
  const bool flat=std::strstr(target,"/.flat-")!=nullptr;
  if(trace && payload)++payload_reads;
  if(trace && flat)++flat_reads;
  if(flat && flat_interrupt) {flat_interrupt=false;errno=EINTR;return -1;}
  if(flat && flat_fail) {errno=EIO;return -1;}
  if(flat && flat_short)bytes=std::min<size_t>(bytes,31);
  const ssize_t result=__real_pread(fd,data,bytes,offset);
  if(trace && payload && result>0)payload_bytes+=static_cast<size_t>(result);
  return result;
}
#ifdef MVDB_TEST_WRAP_PWRITE
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t bytes,off_t offset) {
  ++flat_write_calls;
  if(fail_flat_write) {++injected_flat_write_failures;errno=ENOSPC;return -1;}
  if(interrupt_flat_write) {interrupt_flat_write=false;errno=EINTR;return -1;}
  if(short_flat_write)bytes=std::min<size_t>(bytes,17);
  return __real_pwrite(fd,data,bytes,offset);
}
#endif

using namespace mvdb;
static std::string err;static int code=0;
struct Directory {
  std::string path;
  Directory() {char p[]="/tmp/mvdb-spill-filter-XXXXXX";assert(::mkdtemp(p));path=p;}
  ~Directory() {std::filesystem::remove_all(path);}
};
template<class F> static void throws(F&& action) {
  bool caught=false;try {action();}catch(const std::runtime_error&) {caught=true;}assert(caught);
}
#ifdef MVDB_ENABLE_SPILL_FILTER
static void direct(const std::string& path) {
  constexpr size_t n=257,budget=65536;
  const size_t columns[2]={3,7};const float vector[5]={0,1,2,3,4};
  auto value=[](unsigned kind,size_t ordinal)->int64_t {
    switch(kind) {
      case 0:return -17;
      case 1:return int64_t(ordinal%256)-128;
      case 2:return INT64_MIN+int64_t(ordinal*255);
      case 3:return INT64_MIN+int64_t(ordinal*16777215ull);
      default:return ordinal%2 ? INT64_MAX : INT64_MIN;
    }
  };
  for(unsigned kind=0;kind<5;++kind) {
    assert(!live);meter=true;peak=0;
    {
      FlatIndex index(path,5,n,0);assert(index.disk_backed());
      assert(index.configure_spill_integers(columns,2,budget)==2);
      int64_t integer=0;assert(!index.spill_integer_value(0,3,&integer));
      for(size_t i=0;i<n;++i) {
        const int64_t integers[2]={value(kind,i),int64_t(i%13)};
        index.add(int64_t(i)-900,i*9+3,vector,5,integers,2);
      }
      index.finish();index.finish();
      const size_t widths[5]={0,1,2,4,8};
      assert(index.resident_bytes()==n*(widths[kind]+1) && peak<=budget);
      assert(index.has_spill_integer(3) && index.has_spill_integer(7) && !index.has_spill_integer(9));
      for(size_t i=0;i<n;++i) {
        assert(index.spill_integer_value(i,3,&integer) && integer==value(kind,i));
        assert(index.spill_integer_value(i,7,&integer) && integer==int64_t(i%13));
      }
      assert(!index.spill_integer_value(n,3,&integer));
      size_t visits=0;
      flat_interrupt=true;flat_short=true;
      index.scan_filtered([&](size_t ordinal,int64_t pk,uint64_t slot) {
        assert(pk==int64_t(ordinal)-900 && slot==ordinal*9+3);
        return ordinal%13==7;
      },[&](int64_t pk,uint64_t,const float* data,size_t dimension) {
        assert((pk+900)%13==7 && dimension==5 && data[4]==4);++visits;return true;
      });
      flat_short=false;assert(visits==20);
      flat_fail=true;throws([&]{index.scan_filtered([](size_t,int64_t,uint64_t){return true;},
          [](int64_t,uint64_t,const float*,size_t){return true;});});flat_fail=false;
      visits=0;index.scan_filtered([](size_t,int64_t,uint64_t){return true;},
          [&](int64_t,uint64_t,const float*,size_t){++visits;return false;});assert(visits==1);
    }
    assert(!live);meter=false;
  }
  // Allocation failures: initial raw arrays admit a prefix; failed compression
  // keeps complete raw values. A one-byte budget deficit never over-allocates.
  for(int fault=0;fault<2;++fault) {
    FlatIndex index(path,5,n,0);fail_after=fault;
    const size_t admitted=index.configure_spill_integers(columns,2,budget);fail_after=-1;
    assert(admitted==size_t(fault));
    for(size_t i=0;i<n;++i) {const int64_t integers[2]={int64_t(i%13),int64_t(i)};
      index.add(int64_t(i),i,vector,5,integers,admitted);}
    index.finish();assert(index.has_spill_integer(3)==(admitted!=0));
  }
  for(size_t room:{n*8-1,n*8,n*9}) {
    FlatIndex index(path,5,n,0);
    const size_t admitted=index.configure_spill_integers(columns,1,room);
    assert(admitted==(room>=n*8 ? 1u : 0u));
    for(size_t i=0;i<n;++i) {const int64_t v=int64_t(i%13);index.add(int64_t(i),i,vector,5,&v,admitted);}
    index.finish();assert(index.resident_bytes()==(room<n*8 ? 0 : room<n*9 ? n*8 : n));
  }
  {
    FlatIndex index(path,5,n,0);assert(index.configure_spill_integers(columns,1,budget)==1);
    throws([&]{index.add(0,0,vector,5);}); // Cannot leave configured values unwritten.
    for(size_t i=0;i<n;++i) {const int64_t v=int64_t(i%13);index.add(int64_t(i),i,vector,5,&v,1);}
    fail_after=0;index.finish();fail_after=-1;assert(index.resident_bytes()==n*8);
    int64_t v=0;assert(index.spill_integer_value(15,3,&v) && v==2);
    throws([&]{index.configure_spill_integers(columns,1,budget);});
  }
  FlatIndex too_large(path,5,(2u<<20)/8+1,0);
  assert(!too_large.configure_spill_integers(columns,2,std::numeric_limits<size_t>::max()));
#ifdef MVDB_TEST_WRAP_PWRITE
  // EE writes each record inside add(), not in a batch at finish().
  // Fail the first, middle and last record; no incomplete cache may publish.
  for(size_t fault_row:{size_t(0),n/2,n-1}) {
    FlatIndex failed(path,5,n,0);
    assert(failed.configure_spill_integers(columns,2,256u<<10)==2);
    for(size_t i=0;i<fault_row;++i) {
      const int64_t integers[2]={int64_t(i%13),int64_t(i)};
      failed.add(int64_t(i),i,vector,5,integers,2);
    }
    injected_flat_write_failures=0;fail_flat_write=true;
    const int64_t integers[2]={int64_t(fault_row%13),int64_t(fault_row)};
    throws([&]{failed.add(int64_t(fault_row),fault_row,vector,5,integers,2);});
    fail_flat_write=false;assert(injected_flat_write_failures==1);
    int64_t integer=0;
    assert(!failed.has_spill_integer(3) && !failed.spill_integer_value(0,3,&integer));
    throws([&]{failed.finish();});
    throws([&]{failed.scan_filtered([](size_t,int64_t,uint64_t){return true;},
        [](int64_t,uint64_t,const float*,size_t){return true;});});
  }
  // EINTR and short pwrite must complete each record without publishing a
  // partial file. Once every add succeeded, finish itself must issue no write.
  {
    FlatIndex complete(path,5,n,0);
    assert(complete.configure_spill_integers(columns,2,256u<<10)==2);
    interrupt_flat_write=true;short_flat_write=true;flat_write_calls=0;
    for(size_t i=0;i<n;++i) {
      const int64_t integers[2]={int64_t(i%13),int64_t(i)};
      complete.add(int64_t(i),i,vector,5,integers,2);
    }
    short_flat_write=false;
    assert(!interrupt_flat_write && flat_write_calls>n);
    injected_flat_write_failures=0;fail_flat_write=true;
    complete.finish();fail_flat_write=false;
    assert(injected_flat_write_failures==0 && complete.has_spill_integer(3));
    size_t visited=0;
    complete.scan_filtered([](size_t,int64_t,uint64_t){return true;},
        [&](int64_t pk,uint64_t slot,const float* data,size_t dimension) {
          assert(pk==int64_t(visited) && slot==visited && dimension==5);
          for(size_t j=0;j<5;++j)assert(data[j]==vector[j]);
          ++visited;return true;
        });
    assert(visited==n);
  }
#endif
}
#endif
static Cond cond(const char* column,Cond::Op op,int64_t value) {
  Cond c;c.column=column;c.op=op;c.value=Value::ofInt(value);return c;
}
struct Reads {size_t calls,bytes;};
static Reads check(Database& db,const std::vector<Cond>& conds) {
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan("t",conds,[&](const Row& row){expected.emplace(row.vals[0].i,row.vals[5].v);return true;});
  // Force any lazy rebuild outside the read counter, as we are testing the
  // steady filter path. Build/restart cost must still be benchmarked separately.
  (void)db.resident_vector_index("t");
  payload_reads=flat_reads=payload_bytes=0;trace=true;
  db.scan_vectors("t",conds,[&](int64_t key,const float* v,size_t n) {
    assert(actual.emplace(key,std::vector<float>(v,v+n)).second);return true;
  });
  trace=false;assert(actual==expected && flat_reads>0);
  return {payload_reads,payload_bytes};
}
static void database(const std::string& path) {
  Database db;assert(db.open(path,&err));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column a;a.name="a";a.type=Type::BIGINT;Column b=a;b.name="b";Column c=a;c.name="c";
  Column s;s.name="s";s.type=Type::VARCHAR;s.len=1000;
  Column v;v.name="v";v.type=Type::VECTOR;v.len=5;
  assert(db.create_table("t",{id,a,b,c,s,v},&err,&code));
  assert(db.begin(&err,&code));
  for(int64_t i=1023;i>=0;--i)assert(db.insert("t",{Value::ofInt(i),Value::ofInt(i%2),
      Value::ofInt(i%17),Value::ofInt(i%11),Value::ofStr(i%3 ? "keep" : "drop"),
      Value::ofVec({float(i%13),1,2,3,4})},&err,&code));
  assert(db.commit(&err,&code));assert(db.table("t")->payload_store);
  assert(db.create_vector_index("t","v","vi",&err,&code));
  auto index=db.table("t")->vector_index;assert(index->disk_backed());
  const std::vector<Cond> filter={cond("a",Cond::GE,0),cond("b",Cond::EQ,7)};
  const Reads cached=check(db,filter);
#ifdef MVDB_ENABLE_SPILL_FILTER
  const bool complete=index->has_spill_integer(1) && index->has_spill_integer(2);
  assert((cached.calls==0)==complete);
  assert(check(db,{cond("id",Cond::GT,1000)}).calls==0); // PK needs no array.
  Cond text;text.column="s";text.value=Value::ofStr("keep");
  auto mixed=filter;mixed.push_back(text);
  const Reads remaining=check(db,mixed);
  assert(remaining.calls>0);
  if(complete)assert(remaining.calls==2*60); // Only grp=7 candidates materialise.
#else
  assert(cached.calls==2*1024);
#endif
  index.reset(); // Never keep the borrowed index alive across mutations.
  for(Cond::Op op:{Cond::EQ,Cond::NE,Cond::LT,Cond::LE,Cond::GT,Cond::GE})
    for(int64_t value:{INT64_MIN,int64_t(-1),int64_t(0),int64_t(8),INT64_MAX})
      check(db,{cond("a",Cond::GE,0),cond("b",op,value),cond("c",Cond::NE,3)});
  check(db,{cond("b",Cond::GT,8),cond("b",Cond::LE,8)});
  assert(db.begin_statement(&err,&code));
  assert(db.update_rows("t",{cond("id",Cond::LT,30)},{{"b",Value::ofInt(7)}},&err,&code)==30);
  check(db,filter); // Build a cache over the uncommitted replacement values.
  assert(!db.end_statement(false,&err,&code));
  assert(!db.table("t")->vector_index);check(db,filter);
  assert(db.begin(&err,&code));
  assert(db.update_rows("t",{cond("id",Cond::LT,30)},{{"b",Value::ofInt(7)}},&err,&code)==30);
  assert(!db.table("t")->vector_index);check(db,filter);
  assert(db.delete_rows("t",{cond("id",Cond::LT,16)},&err,&code)==16);check(db,filter);
  assert(db.rollback(&err,&code));assert(!db.table("t")->vector_index);check(db,filter);
  assert(db.delete_rows("t",{cond("id",Cond::LT,16)},&err,&code)==16);check(db,filter);

  // Rollback reclaims an appended tail slot. Build a disk index over the
  // transaction's row, then reuse that exact slot with different scalars and
  // floats. Test both complete/partial caches and the zero-cache budget path.
  const size_t tail_slot=db.table("t")->rows.size();
  const size_t before_insert=db.table("t")->live_count();
  const std::vector<float> rolled_back_vector={500,6,7,8,9};
  const std::vector<float> replacement_vector={-500,10,11,12,13};
  const auto verify_marker=[&](int64_t a_value,int64_t b_value,int64_t key,
                             const std::vector<float>& vector,bool present) {
    const std::vector<Cond> conditions={cond("a",Cond::EQ,a_value),cond("b",Cond::EQ,b_value)};
    size_t found=0;
    db.scan("t",conditions,[&](const Row& row) {
      assert(row.vals[0].i==key && row.vals[1].i==a_value && row.vals[2].i==b_value);
      assert(row.vals[5].v==vector);++found;return true;
    });
    assert(found==(present ? 1u : 0u));
    check(db,conditions); // Exact indexed PK/vector equality against that scan.
  };
  assert(db.begin(&err,&code));
  assert(db.insert("t",{Value::ofInt(2000),Value::ofInt(41),Value::ofInt(43),Value::ofInt(47),
      Value::ofStr("rolled back"),Value::ofVec(rolled_back_vector)},&err,&code));
  assert(!db.table("t")->vector_index && db.table("t")->pk_pos.at(2000)==tail_slot);
  verify_marker(41,43,2000,rolled_back_vector,true);
  verify_marker(-41,-43,3000,replacement_vector,false);
  assert(db.table("t")->vector_index->disk_backed());
  const std::weak_ptr<FlatIndex> rolled_back_index=db.table("t")->vector_index;
  assert(db.rollback(&err,&code));
  assert(!db.table("t")->vector_index && rolled_back_index.expired());
  assert(db.table("t")->rows.size()==tail_slot && db.table("t")->live_count()==before_insert);
  assert(db.table("t")->payload_offsets.size()==tail_slot && !db.table("t")->pk_pos.count(2000));
  verify_marker(41,43,2000,rolled_back_vector,false);
  verify_marker(-41,-43,3000,replacement_vector,false);
  check(db,filter);
  assert(db.insert("t",{Value::ofInt(3000),Value::ofInt(-41),Value::ofInt(-43),Value::ofInt(-47),
      Value::ofStr("replacement"),Value::ofVec(replacement_vector)},&err,&code));
  assert(!db.table("t")->vector_index && db.table("t")->pk_pos.at(3000)==tail_slot);
  assert(db.table("t")->rows.size()==tail_slot+1 && db.table("t")->live_count()==before_insert+1);
  verify_marker(41,43,2000,rolled_back_vector,false);
  verify_marker(-41,-43,3000,replacement_vector,true);
  assert(db.checkpoint(&err));db.close();assert(db.open(path,&err));check(db,filter);
  assert(db.table("t")->payload_store && !db.table("t")->pk_pos.count(2000));
  assert(db.table("t")->live_count()==before_insert+1);
  verify_marker(41,43,2000,rolled_back_vector,false);
  verify_marker(-41,-43,3000,replacement_vector,true);
  // Projection already fetches only final K rows. Verify ranked identities;
  // this test does not count that existing behavior as an I07 improvement.
  Statement statement;assert(parse_sql("SELECT id FROM t WHERE a=1 AND b>=2 AND b<9 ORDER BY L2_DISTANCE(v,'[0,0,0,0,0]') LIMIT 7;",
      &statement,&err,&code));
  const float zero[5]={};std::vector<std::pair<double,int64_t>> expected;
  db.scan("t",statement.conds,[&](const Row& row){expected.emplace_back(distance_squared(row.vals[5].v.data(),zero,5),row.vals[0].i);return true;});
  std::sort(expected.begin(),expected.end());expected.resize(std::min<size_t>(7,expected.size()));
  std::string output;assert(execute_json(db,statement,&output,&err,&code));
  const auto rows=nlohmann::json::parse(output).at("rows");assert(rows.size()==expected.size());
  for(size_t i=0;i<expected.size();++i)assert(rows[i][0].get<int64_t>()==expected[i].second);
  db.close();
}
int main() {
  Directory physical,logical;
#ifdef MVDB_ENABLE_SPILL_FILTER
  direct(physical.path);
#endif
  database(logical.path);
  std::cout<<"PASS spill scalar budget/compression/failure, payload read avoidance, exact filters, invalidation and rollback slot reuse\n";
}

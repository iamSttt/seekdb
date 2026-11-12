// Link db.cpp, flat_index.cpp and value.cpp. Repeat with
// -DMVDB_FLAT_INDEX_MEMORY_BYTES=1 -DMVDB_SPILL_BYTES=1 to exercise disk paths.
#include "db.h"
#include "flat_index.h"
#include "distance.h"
#include "quantized_bound.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>
using namespace mvdb;
static std::string err;
static int code=0;
static std::vector<Column> schema() {
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column group;group.name="g";group.type=Type::BIGINT;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=5;
  return {id,group,vector};
}
static void insert(Database& db,int64_t id) {
  assert(db.insert("t",{Value::ofInt(id),Value::ofInt(id%3),
      Value::ofVec({float(id),1,2,3,4})},&err,&code));
}
static void check_quantized_view(const FlatIndex::ResidentView& view) {
  if(!view.quantized) {
    assert(view.quantization_step==0 && view.quantization_error_radius==-1);return;
  }
  assert(view.count && view.dimension>=16 && view.dimension<=65536);
  assert(view.quantization_step>0 && std::isfinite(view.quantization_step));
  for(size_t i=0;i<view.count*view.dimension;++i) {
    assert(view.quantized[i]>=-127 && view.quantized[i]<=127);
    const long double error=std::fabs(static_cast<long double>(
        view.rows[i/view.dimension][i%view.dimension])-
        static_cast<long double>(view.quantized[i])*static_cast<long double>(view.quantization_step));
    assert(error<=static_cast<long double>(view.quantization_step)*0.5L);
  }
#ifndef MVDB_DISABLE_RESIDUAL_BOUND
  assert(view.quantization_error_radius>=0 && std::isfinite(view.quantization_error_radius));
  for(size_t row=0;row<view.count;++row) {
    long double squared=0;
    for(size_t d=0;d<view.dimension;++d) {
      const long double error=static_cast<long double>(view.rows[row][d])/view.quantization_step-
          view.quantized[row*view.dimension+d];
      squared+=error*error;
    }
    assert(static_cast<long double>(view.quantization_error_radius)*view.quantization_error_radius>=squared);
  }
#else
  assert(view.quantization_error_radius==-1);
#endif
}
static void check_view(Database& db) {
  const Table* table=db.table("t");assert(table);
  FlatIndex::ResidentView view;
  const bool available=db.resident_vector_view("t",&view);
  if(!table->has_index) {
    assert(!available && !table->vector_index);
  } else {
    assert(table->vector_index);
    assert(available==!table->vector_index->disk_backed());
  }
  if(!available) {
    assert(!view.rows && !view.keys && !view.dimension && !view.count);
    assert(!view.quantized && view.quantization_step==0 && view.quantization_error_radius==-1);
    return;
  }
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan("t",{},[&](const Row& row){expected[row.vals[0].i]=row.vals[2].v;return true;});
  assert(view.dimension==5 && view.count==expected.size());
  check_quantized_view(view);assert(!view.quantized); // unsupported dimension
  // Contract: a resident index does not own a contiguous float array. It owns one
  // borrowed pointer per row, so a row's payload is reachable only through
  // view.rows[i] and is exactly view.dimension floats long. Ownership stays with
  // the table, which rebuilds this index on every mutation.
  for(size_t i=0;i<view.count;++i) {
    const float* values=view.rows[i];
    assert(values);
    assert(actual.emplace(view.keys[i],std::vector<float>(values,values+view.dimension)).second);
  }
  assert(view.rows[0]!=view.rows[view.count-1] || view.count==1);
  assert(actual==expected);
}
static void check(Database& db,const std::vector<Cond>& conds={}) {
  // Obtain a fresh view after every mutation/rebuild/rollback; no borrowed
  // pointer is retained across those lifetime boundaries.
  check_view(db);
  std::map<int64_t,std::vector<float>> expected,actual;
  db.scan("t",conds,[&](const Row& row){expected[row.vals[0].i]=row.vals[2].v;return true;});
  const std::vector<float> query{0,1,2,0,0};
  std::vector<std::pair<double,int64_t>> a,b;
  db.scan_vectors("t",conds,[&](int64_t pk,const float* values,size_t size) {
    assert(actual.emplace(pk,std::vector<float>(values,values+size)).second);
    a.emplace_back(distance_squared(values,query.data(),size),pk);return true;
  });
  assert(actual==expected);
  for(const auto& kv:expected)b.emplace_back(distance_squared(kv.second.data(),query.data(),query.size()),kv.first);
  std::sort(a.begin(),a.end());std::sort(b.begin(),b.end());assert(a==b);
  size_t visited=0;
  db.scan_vectors("t",conds,[&](int64_t,const float*,size_t){++visited;return false;});
  assert(visited==(expected.empty()?0:1));
}
static void norm_order_regression() {
  char path[]="/tmp/mvdb-norm-order-XXXXXX";assert(::mkdtemp(path));
  Database db;assert(db.open(path,&err));assert(db.create_table("t",schema(),&err,&code));
  const std::vector<std::pair<int64_t,std::vector<float>>> rows={
    {50,{3,0,0,0,0}},{70,{-1,0,0,0,0}},{10,{0,1,0,0,0}},
    {20,{0,0,0,0,0}},{30,{0,0,0,0,2}}
  };
  assert(db.begin(&err,&code));
  for(const auto& row:rows)
    assert(db.insert("t",{Value::ofInt(row.first),Value::ofInt(0),Value::ofVec(row.second)},&err,&code));
  assert(db.commit(&err,&code));
  assert(db.create_vector_index("t","v","norm_idx",&err,&code));
  auto verify=[&](const std::vector<int64_t>& norm_keys) {
    check(db); // maps and exact distances agree across table/view/index scans
    const Table* table=db.table("t");assert(table && table->vector_index);
    std::vector<int64_t> slot_keys,actual;
    db.scan("t",{},[&](const Row& row){slot_keys.push_back(row.vals[0].i);return true;});
    db.scan_vectors("t",{},[&](int64_t pk,const float*,size_t){actual.push_back(pk);return true;});
    bool expect_norm=!table->payload_store && !table->vector_index->disk_backed();
#ifdef MVDB_DISABLE_NORM_ORDER
    expect_norm=false;
#endif
    assert(actual==(expect_norm?norm_keys:slot_keys));
    FlatIndex::ResidentView view;
    if(db.resident_vector_view("t",&view)) {
      assert(view.count==actual.size());
      for(size_t i=0;i<view.count;++i)assert(view.keys[i]==actual[i]);
    }
  };
  // The equal-norm rows deliberately have descending PKs: norm ties retain
  // source slot order, while query ranking still has its own PK tie breaker.
  verify({20,70,10,30,50});
  assert(db.begin(&err,&code));
  assert(db.insert("t",{Value::ofInt(40),Value::ofInt(0),Value::ofVec({0.5f,0,0,0,0})},&err,&code));
  assert(!db.table("t")->vector_index);verify({20,40,70,10,30,50});
  assert(db.rollback(&err,&code));verify({20,70,10,30,50});
  Cond id;id.column="id";id.value=Value::ofInt(20);
  assert(db.begin(&err,&code));assert(db.delete_rows("t",{id},&err,&code)==1);
  verify({70,10,30,50});assert(db.rollback(&err,&code));verify({20,70,10,30,50});
  // UPDATE may force payload spilling for undo. It must retain correct view
  // data and use source slots rather than sorting or rereading spilled rows.
  id.value=Value::ofInt(50);assert(db.begin(&err,&code));
  assert(db.update_rows("t",{id},{{"v",Value::ofVec({0.25f,0,0,0,0})}},&err,&code)==1);
  verify({20,50,70,10,30});assert(db.rollback(&err,&code));verify({20,70,10,30,50});
  std::map<int64_t,std::vector<float>> restored;
  db.scan("t",{},[&](const Row& row){restored[row.vals[0].i]=row.vals[2].v;return true;});
  for(const auto& row:rows)assert(restored.at(row.first)==row.second);
}
static void quantized_index_regression() {
  char path[]="/tmp/mvdb-quant-index-XXXXXX";assert(::mkdtemp(path));
  auto verify_physical=[&](size_t dimension,size_t count,size_t budget,bool expect_codes) {
    FlatIndex index(path,dimension,count,budget);
    assert(index.resident_bytes()<=budget);
    // A resident index borrows each payload, so every row needs its own slice:
    // one reused scratch vector would leave count pointers aliasing one row.
    std::vector<float> values(dimension*count);
    for(size_t i=0;i<count;++i) {
      float* row=values.data()+i*dimension;
      for(size_t j=0;j<dimension;++j)row[j]=float(int((i*17+j*5)%127)-63)*0.125f;
      index.add(int64_t(count-i),i,row,dimension);
    }
    index.finish();FlatIndex::ResidentView view;
    const bool resident=index.resident_view(&view);
#ifdef MVDB_DISABLE_QUANTIZED_BOUND
    expect_codes=false;
#endif
    if(!resident) {
      assert(!expect_codes && !view.quantized && view.quantization_step==0);return;
    }
    assert(bool(view.quantized)==expect_codes);check_quantized_view(view);
    assert(index.resident_bytes()<=budget);
    for(size_t i=0;i<count;++i) {
      assert(view.keys[i]==int64_t(count-i));
      for(size_t j=0;j<dimension;++j)
        assert(view.rows[i][j]==float(int((i*17+j*5)%127)-63)*0.125f);
    }
  };
  const size_t dim=16,count=3;
  const size_t base=count*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  const size_t postings=count*(2*sizeof(int64_t)+sizeof(uint32_t)),codes=count*dim;
  verify_physical(dim,count,base+postings,false);
  verify_physical(dim,count,base+postings+codes-1,false);
  verify_physical(dim,count,base+postings+codes,true);
  verify_physical(17,3,4096,true);verify_physical(128,3,4096,true);
  verify_physical(65536,1,1u<<20,true);verify_physical(65537,1,1u<<20,false);
  verify_physical(16,3,0,false); // disk must never expose code storage
  verify_physical(16,0,4096,false);
  // Exactly 4 MiB is eligible; one further row must retain only float storage.
  verify_physical(65536,64,32u<<20,true);
  verify_physical(65536,65,32u<<20,false);
  // An unready view and a failed optional quantization must never expose a
  // stale radius. Non-finite data leaves the exact float scan available.
  for(float invalid:{std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
    FlatIndex index(path,128,1,4096);
    FlatIndex::ResidentView view;view.quantization_error_radius=99;
    assert(!index.resident_view(&view) && view.quantization_error_radius==-1);
    std::vector<float> values(128,0.1f);values[127]=invalid;
    index.add(1,0,values.data(),values.size());index.finish();
    assert(index.resident_view(&view));check_quantized_view(view);
    assert(!view.quantized && view.quantization_error_radius==-1);
  }
  {
    FlatIndex index(path,128,4,4096);
    std::vector<float> values(4*128);
    for(size_t row=0;row<4;++row) {
      for(size_t d=0;d<128;++d)values[row*128+d]=float(int((row*17+d*5)%127)-63)*0.123f;
      index.add(row,row,values.data()+row*128,128);
    }
    index.finish();FlatIndex::ResidentView view;
    assert(index.resident_view(&view));check_quantized_view(view);
#if !defined(MVDB_DISABLE_QUANTIZED_BOUND) && !defined(MVDB_DISABLE_RESIDUAL_BOUND)
    assert(view.quantization_error_radius>0 && view.quantization_error_radius<std::sqrt(128.0)*0.5);
#endif
  }

  Database db;assert(db.open(path,&err));auto cols=schema();cols[2].len=16;
  assert(db.create_table("q",cols,&err,&code));
  auto add=[&](int64_t pk,float scale) {
    std::vector<float> value(16);
    for(size_t j=0;j<value.size();++j)value[j]=scale*float(int(j)-8);
    assert(db.insert("q",{Value::ofInt(pk),Value::ofInt(0),Value::ofVec(value)},&err,&code));
  };
  add(9,0.5f);add(-4,-0.25f);add(2,1.0f);
  assert(db.create_vector_index("q","v","q_idx",&err,&code));
  auto verify_database=[&] {
    std::map<int64_t,std::vector<float>> expected,actual;
    db.scan("q",{},[&](const Row& row){expected[row.vals[0].i]=row.vals[2].v;return true;});
    FlatIndex::ResidentView view;
    if(db.resident_vector_view("q",&view)) {
      check_quantized_view(view);
      assert(view.count==expected.size());
      for(size_t i=0;i<view.count;++i)
        assert(actual.emplace(view.keys[i],std::vector<float>(view.rows[i],
               view.rows[i]+view.dimension)).second);
      assert(actual==expected);
    }
    actual.clear();db.scan_vectors("q",{},[&](int64_t pk,const float* values,size_t n) {
      actual.emplace(pk,std::vector<float>(values,values+n));return true;
    });assert(actual==expected);
    // A failed request must clear every borrowed code pointer as well.
    assert(!db.resident_vector_view("missing",&view));
    assert(!view.quantized && !view.quantization_step && !view.rows && !view.keys &&
           view.quantization_error_radius==-1);
  };
  verify_database();assert(db.begin(&err,&code));add(7,0.75f);
  assert(!db.table("q")->vector_index);verify_database();
  assert(db.rollback(&err,&code));assert(!db.table("q")->vector_index);verify_database();
  Cond id;id.column="id";id.value=Value::ofInt(-4);
  assert(db.begin(&err,&code));
  assert(db.update_rows("q",{id},{{"v",Value::ofVec(std::vector<float>(16,-2.0f))}},&err,&code)==1);
  verify_database();assert(db.rollback(&err,&code));verify_database();
  assert(db.delete_rows("q",{id},&err,&code)==1);verify_database();
  assert(db.checkpoint(&err));db.close();assert(db.open(path,&err));verify_database();
}
int main() {
  char path[]="/tmp/mvdb-flat-index-XXXXXX";assert(::mkdtemp(path));
  Database db;assert(db.open(path,&err));assert(db.create_table("t",schema(),&err,&code));
  FlatIndex::ResidentView view;
  assert(!db.resident_vector_view("missing",&view));
  assert(!db.resident_vector_view("t",nullptr));
  check(db); // non-indexed fallback, including empty tables
  assert(db.begin_statement(&err,&code));
  assert(db.create_vector_index("t","v","idx",&err,&code));
  assert(db.end_statement(true,&err,&code));
  assert(db.table("t")->vector_index && db.table("t")->vector_index->size()==0);
  assert(db.resident_vector_view("t",&view) && view.count==0 && view.dimension==5);
  assert(!db.resident_vector_view("missing",&view));
  assert(!view.rows && !view.keys && !view.dimension && !view.count);
  assert(db.begin(&err,&code));for(int i=99;i>=0;--i)insert(db,i);assert(db.commit(&err,&code));
  assert(!db.table("t")->vector_index);check(db);assert(db.table("t")->vector_index->size()==100);
  Cond group;group.column="g";group.value=Value::ofInt(1);
  check(db,{group});
  // Coercion, contradictory conjunctions, and column switching must use the
  // exact same predicate semantics as the ordinary table scan.
  Cond text_group=group;text_group.value=Value::ofStr("+01");check(db,{text_group});
  Cond other_group=group;other_group.value=Value::ofInt(2);check(db,{group,other_group});
  Cond range;range.column="id";range.op=Cond::GE;range.value=Value::ofInt(40);
  check(db,{range,group});
  Cond id;id.column="id";id.value=Value::ofInt(52);check(db,{id});check(db,{group});
  Cond missing;missing.column="id";missing.value=Value::ofInt(-100);check(db,{missing});
  assert(db.begin(&err,&code));
  assert(db.begin_statement(&err,&code));
  assert(db.update_rows("t",{group},{{"v",Value::ofVec({7,6,5,4,3})}},&err,&code)==33);
  assert(db.end_statement(true,&err,&code));check(db,{group});
  // Changing the indexed scalar itself must discard cached postings, even if
  // the vector values are untouched. Rollback below must restore both.
  assert(db.update_rows("t",{id},{{"g",Value::ofInt(2)}},&err,&code)==1);
  assert(!db.table("t")->vector_index);
  check(db,{group});check(db,{other_group});
  assert(db.update_rows("t",{id},{{"g",Value::ofInt(1)}},&err,&code)==1);
  assert(!db.table("t")->vector_index);
  check(db,{group});
  assert(db.begin_statement(&err,&code));
  insert(db,1000);
  assert(!db.insert("t",{Value::ofInt(1),Value::ofInt(0),Value::ofVec({0,0,0,0,0})},&err,&code));
  assert(!db.end_statement(false,&err,&code));check(db);
  assert(db.delete_rows("t",{group},&err,&code)==33);check(db);
  assert(db.rollback(&err,&code));check(db);
  db.scan("t",{},[](const Row& row){assert(row.vals[2].v[0]==float(row.vals[0].i));return true;});
  // A dropped indexed table must return with valid contents after rollback.
  assert(db.begin(&err,&code));assert(db.drop_table("t",&err,&code));
  assert(db.create_table("t",schema(),&err,&code));insert(db,9999);
  assert(db.rollback(&err,&code));check(db);
  assert(db.table("t")->vector_index->size()==100);
  // Metadata and a real physical structure are restored before open returns.
  db.close();assert(db.open(path,&err));
  assert(db.has_vector_index("t") && db.table("t")->vector_index);check(db);
  assert(db.checkpoint(&err));db.close();assert(db.open(path,&err));
  assert(db.table("t")->vector_index);check(db,{group});
  // Creating an index inside an aborted transaction must leave no index.
  assert(db.create_table("plain",schema(),&err,&code));
  assert(db.begin(&err,&code));assert(db.create_vector_index("plain","v","pidx",&err,&code));
  assert(db.table("plain")->vector_index);assert(db.rollback(&err,&code));
  assert(!db.has_vector_index("plain") && !db.table("plain")->vector_index);
  assert(!db.resident_vector_view("plain",&view));
  // Direct physical disk index test, independent of the build-budget macro.
  FlatIndex disk(path,5,3,0);
  for(int64_t pk:{9,-2,4}) {float v[5]={float(pk),1,2,3,4};disk.add(pk,static_cast<uint64_t>(pk+2),v,5);}
  disk.finish();assert(disk.disk_backed() && disk.resident_bytes()==0);
  assert(!disk.resident_view(&view));
  assert(!view.rows && !view.keys && !view.dimension && !view.count);
  std::vector<int64_t> seen;
  disk.scan([&](int64_t pk,uint64_t slot,const float* values,size_t dim){
    assert(slot==static_cast<uint64_t>(pk+2) && dim==5 && values[0]==float(pk));seen.push_back(pk);return true;
  });
  assert((seen==std::vector<int64_t>{9,-2,4}));
  assert(!disk.scan_integer_equal(0,9,[](uint64_t){return 9;},
      [](int64_t,uint64_t,const float*,size_t){assert(false);return true;}));
  // A direct resident index proves postings cache one column, retain ordinal
  // order for equal keys, and honor early termination without reading again.
  FlatIndex resident(path,5,3,4096);
  assert(!resident.resident_view(&view)); // never expose partially built arrays
  // A resident index borrows each payload instead of copying it, so the payloads
  // must outlive the index: hold them in a stable array, not in a per-iteration
  // stack temporary that this contract would leave dangling.
  const int64_t resident_keys[]={9,-2,4};
  std::vector<float> payload(3*5);
  for(size_t i=0;i<3;++i) {
    const float v[5]={float(resident_keys[i]),1,2,3,4};
    std::copy(v,v+5,payload.begin()+static_cast<std::ptrdiff_t>(i*5));
    resident.add(resident_keys[i],uint64_t(resident_keys[i]+2),payload.data()+i*5,5);
  }
  assert(!resident.resident_view(&view));
  resident.finish();size_t reads=0;seen.clear();
  assert(resident.resident_view(&view) && view.dimension==5 && view.count==3);
  for(size_t i=0;i<view.count;++i) {
    assert(view.keys[i]==resident_keys[i]);
    assert(view.rows[i]==payload.data()+i*view.dimension);
    assert(view.rows[i][0]==float(view.keys[i]));
    for(size_t j=1;j<view.dimension;++j)assert(view.rows[i][j]==float(j));
  }
  assert(!resident.resident_view(nullptr));
  FlatIndex empty(path,5,0,0);
  assert(!empty.resident_view(&view));empty.finish();
  assert(empty.resident_view(&view) && view.dimension==5 && view.count==0);
  auto scalar=[&](uint64_t slot){++reads;return slot==0?int64_t(-1):int64_t(7);};
  auto collect=[&](int64_t pk,uint64_t,const float*,size_t){seen.push_back(pk);return true;};
  assert(resident.scan_integer_equal(1,7,scalar,collect));
  assert(reads==3 && (seen==std::vector<int64_t>{9,4}));seen.clear();
  assert(resident.scan_integer_equal(1,-1,scalar,collect));
  assert(reads==3 && (seen==std::vector<int64_t>{-2}));
  assert(resident.scan_integer_equal(0,7,scalar,collect) && reads==6);
  norm_order_regression();
  quantized_index_regression();
  std::cout<<"PASS physical flat index, filters, mutation/statement/transaction rollback, eager restart rebuild, disk scan\n";
}

// Link db.cpp, flat_index.cpp, value.cpp with:
//   -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite
// Uses the production spill threshold; a test override must be at least 1 MiB.
#include "db.h"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#ifndef MVDB_TEST_WRAP_PWRITE
#error "test_payload_batch requires -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite"
#endif

using namespace mvdb;

namespace {
constexpr size_t kBatchBytes=256u<<10;
#ifdef MVDB_SPILL_BYTES
constexpr uint64_t kSpillBytes=MVDB_SPILL_BYTES;
#else
constexpr uint64_t kSpillBytes=32ull<<20;
#endif
static_assert(kSpillBytes>=(1u<<20),"payload batching fixture needs at least 1 MiB");
struct Write {
  off_t offset;
  size_t requested;
  ssize_t result;
};
struct Trace {
  std::vector<Write> writes;
  bool active=false, interrupt=false;
  size_t short_limit=0, partial_first=0, fail_call=0, zero_call=0;
  int fd=-1;
} trace;

void start_trace() { trace=Trace{};trace.active=true; }

struct TempDir {
  std::string path;
  TempDir() {
    char name[]="/tmp/mvdb-payload-batch-XXXXXX";
    assert(mkdtemp(name));path=name;
  }
  ~TempDir() { assert(rmdir(path.c_str())==0); }
};

Row make_row(size_t key,bool oversized=false) {
  size_t bytes=7900+(key%7)*97;
  if(oversized)bytes=kBatchBytes+1123;
  std::string text(bytes,static_cast<char>('a'+key%20));
  text[1]='\0';text[3]='\xc3';text[4]='\xa9';
  std::vector<float> vector(8);
  for(size_t j=0;j<vector.size();++j)vector[j]=float(key)*0.25f+float(j);
  Row row;row.rid=static_cast<int64_t>(key);
  row.vals={Value::ofInt(static_cast<int64_t>(key)),Value::ofStr(std::move(text)),Value::ofVec(std::move(vector))};
  return row;
}
uint64_t heap_bytes(const Row& row) {
  uint64_t bytes=row.vals.size()*sizeof(Value);
  for(const Value& value:row.vals)bytes+=value.s.size()+value.v.size()*sizeof(float);
  return bytes;
}
void same_row(const Row& actual,const Row& expected) {
  assert(actual.rid==expected.rid && actual.vals.size()==3);
  assert(actual.vals[0].type==Type::BIGINT && actual.vals[0].i==expected.vals[0].i);
  assert(actual.vals[1].type==Type::VARCHAR && actual.vals[1].s==expected.vals[1].s);
  assert(actual.vals[2].type==Type::VECTOR && actual.vals[2].v==expected.vals[2].v);
}

struct Fixture {
  TempDir dir;  // Destroy the table (and its unlinked files) before the directory.
  Table table;
  Row pending;
  bool oversized;
  size_t before=0;
  uint64_t before_bytes=0;
  std::shared_ptr<FlatIndex> original_index;
  explicit Fixture(bool large=false):oversized(large) {
    table.storage_dir=dir.path;
    for(size_t key=1;;++key) {
      Row row=make_row(key,oversized && key==4);
      if(table.payload_bytes+heap_bytes(row)>kSpillBytes) {
        pending=make_row(key,oversized);break;
      }
      table.append_row(std::move(row));
    }
    assert(!table.payload_store);
    before=table.rows.size();before_bytes=table.payload_bytes;
    assert(before>64 && before_bytes+heap_bytes(pending)>kSpillBytes);
    original_index=std::make_shared<FlatIndex>(dir.path,8,before,4u<<20);
    for(const Row& row:table.rows)
      original_index->add(row.vals[0].i,static_cast<uint64_t>(row.rid-1),row.vals[2].v.data(),8);
    original_index->finish();table.vector_index=original_index;
  }
  void check_rows(bool spilled) {
    assert(bool(table.payload_store)==spilled);
    const size_t count=before+(spilled?1:0);
    assert(table.rows.size()==count);
    Row scratch;
    for(size_t i=0;i<count;++i) {
      const Row expected=i==before?pending:make_row(i+1,oversized && i+1==4);
      same_row(table.read_row(i,scratch),expected);
    }
  }
  void check_failed() {
    assert(!table.payload_store && table.payload_offsets.empty());
    assert(table.payload_bytes==before_bytes);
    assert(table.vector_index==original_index);
    check_rows(false);
  }
  void spill() { table.append_row(pending); }
};

uint64_t check_offsets(const Table& table) {
  assert(trace.fd>=0 && table.payload_offsets.size()==table.rows.size());
  uint64_t end=0;
  for(uint64_t offset:table.payload_offsets) {
    assert(offset==end);
    uint32_t bytes=0;
    assert(pread(trace.fd,&bytes,sizeof(bytes),static_cast<off_t>(offset))==sizeof(bytes));
    assert(bytes>0);end+=sizeof(bytes)+bytes;
  }
  struct stat info{};assert(fstat(trace.fd,&info)==0);
  assert(static_cast<uint64_t>(info.st_size)==end);
  return end;
}
void check_contiguous_writes(uint64_t bytes) {
  uint64_t end=0;
  for(const Write& write:trace.writes) {
    assert(write.offset==static_cast<off_t>(end));
    if(write.result>0)end+=static_cast<uint64_t>(write.result);
  }
  assert(end==bytes);
}

size_t normal_spill() {
  Fixture fixture;start_trace();fixture.spill();
  assert(!fixture.table.vector_index);
  fixture.check_rows(true);
  const uint64_t end=check_offsets(fixture.table);
  check_contiguous_writes(end);
  // append_row pushes pending into resident rows before spill_payloads visits
  // them. There is no second pending write after publication; this last call is
  // the migration's final flush (or final row when batching is disabled).
  const size_t calls=trace.writes.size();
#ifndef MVDB_DISABLE_PAYLOAD_BATCH
  assert(calls>1 && calls<fixture.table.rows.size()/8);
  for(const Write& write:trace.writes)assert(write.requested<=kBatchBytes);
#else
  assert(calls==fixture.table.rows.size());
#endif
  // Published stores still synchronously append and replace individual rows.
  const Row extra=make_row(fixture.table.rows.size()+1);
  start_trace();fixture.table.append_row(extra);
  assert(trace.writes.size()==1 && fixture.table.payload_offsets.back()==end);
  Row scratch;same_row(fixture.table.read_row(fixture.table.rows.size()-1,scratch),extra);
  Row replacement=make_row(1,true);
  fixture.table.replace_row(0,replacement);
  assert(trace.writes.size()==2);
  same_row(fixture.table.read_row(0,scratch),replacement);
  std::cout<<"PASS payload batching, exact offsets and immediate post-spill writes: "<<calls<<" writes\n";
  return calls;
}

void interrupted_short_writes() {
  Fixture fixture;start_trace();trace.interrupt=true;trace.short_limit=4093;
  fixture.spill();assert(!trace.interrupt);
  fixture.check_rows(true);
  check_contiguous_writes(check_offsets(fixture.table));
  assert(trace.writes.front().result==-1);
  bool short_seen=false;
  for(const Write& write:trace.writes)
    short_seen|=write.result>0 && static_cast<size_t>(write.result)<write.requested;
  assert(short_seen);
  std::cout<<"PASS payload EINTR and short writes\n";
}

void oversized_rows() {
  Fixture fixture(true);start_trace();fixture.spill();
  fixture.check_rows(true);check_contiguous_writes(check_offsets(fixture.table));
  size_t large=0;
  for(const Write& write:trace.writes)if(write.requested>kBatchBytes)++large;
  assert(large==2);
  std::cout<<"PASS oversized payload records between batches and at the end\n";
}

void failed_spill(size_t fail_call,bool zero,bool partial) {
  Fixture fixture;start_trace();
  if(zero)trace.zero_call=fail_call;else trace.fail_call=fail_call;
  if(partial)trace.partial_first=17;
  bool failed=false;
  try { fixture.spill(); } catch(const std::runtime_error&) { failed=true; }
  assert(failed && trace.writes.size()==fail_call);
  if(partial)assert(trace.writes.front().result==17);
  fixture.check_failed();
  // The discarded private store cannot leak an offset or poison a later retry.
  start_trace();fixture.spill();fixture.check_rows(true);
  check_contiguous_writes(check_offsets(fixture.table));
}
}  // namespace

extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t bytes,off_t offset) {
  if(!trace.active)return __real_pwrite(fd,data,bytes,offset);
  trace.fd=fd;
  const size_t call=trace.writes.size()+1;
  const size_t requested=bytes;
  ssize_t result;
  if(trace.interrupt) {trace.interrupt=false;errno=EINTR;result=-1;}
  else if(call==trace.fail_call) {errno=ENOSPC;result=-1;}
  else if(call==trace.zero_call)result=0;
  else {
    if(call==1 && trace.partial_first)bytes=std::min(bytes,trace.partial_first);
    if(trace.short_limit)bytes=std::min(bytes,trace.short_limit);
    result=__real_pwrite(fd,data,bytes,offset);
  }
  trace.writes.push_back({offset,requested,result});
  return result;
}

int main() {
  alarm(120);
  const size_t calls=normal_spill();
  interrupted_short_writes();oversized_rows();
  failed_spill(2,false,true);
  failed_spill(2,true,false);
  failed_spill(calls,false,false);
  std::cout<<"PASS payload partial ENOSPC, zero write and final-batch failure preserve resident state\n";
}

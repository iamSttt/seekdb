// Production-budget UPDATE Undo tests. Link db.cpp, flat_index.cpp, value.cpp.
// Optional: -DMVDB_TEST_WRAP_PWRITE -Wl,--wrap=pwrite checks spill-write failure.
#include "db.h"
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

using namespace mvdb;

#ifdef MVDB_TEST_WRAP_PWRITE
static int fail_payload_writes = 0;
extern "C" ssize_t __real_pwrite(int, const void*, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void* p, size_t n, off_t off) {
  if (fail_payload_writes && --fail_payload_writes == 0) {
    errno = ENOSPC;
    return -1;
  }
  return __real_pwrite(fd,p,n,off);
}
#endif

static Cond eq(int64_t key) {
  Cond c;c.column="id";c.op=Cond::EQ;c.value=Value::ofInt(key);return c;
}

int main() {
  alarm(120);
  char path[]="/tmp/mvdb-resident-update-XXXXXX";assert(mkdtemp(path));
  Database db;std::string error;int code=0;assert(db.open(path,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column text;text.name="s";text.type=Type::VARCHAR;text.len=30000;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=128;
  const std::vector<Column> schema{id,text,vector};
  auto create=[&](const char* name) { assert(db.create_table(name,schema,&error,&code)); };
  auto insert=[&](const char* name,int64_t key,const std::string& value) {
    return db.insert(name,{Value::ofInt(key),Value::ofStr(value),
        Value::ofVec(std::vector<float>(128,float(key)))},&error,&code);
  };
  auto read=[&](const char* name,int64_t key) {
    std::string value;size_t count=0;
    db.scan(name,{eq(key)},[&](const Row& row) {
      value=row.vals[1].s;++count;return true;
    });
    assert(count==1);return value;
  };
  auto update=[&](const char* name,int64_t key,const std::string& value) {
    return db.update_rows(name,{eq(key)},{{"s",Value::ofStr(value)}},&error,&code);
  };
  auto statement=[&](const char* name,int64_t key,const std::string& value,bool commit) {
    assert(db.begin_statement(&error,&code));
    assert(update(name,key,value)==1);
    assert(db.end_statement(commit,&error,&code)==commit);
  };
  auto verify_vectors=[&](const char* name,size_t expected) {
    size_t count=0;
    db.scan_vectors(name,{},[&](int64_t key,const float* v,size_t dim) {
      assert(dim==128);for(size_t j=0;j<dim;++j)assert(v[j]==float(key));
      ++count;return true;
    });
    assert(count==expected);
  };

  create("t");assert(insert("t",1,"original"));assert(insert("t",2,"second"));
  assert(db.create_vector_index("t","v","tv",&error,&code));
  assert(db.table("t")->vector_index);
  statement("t",1,"committed",true);
  assert(!db.table("t")->payload_store);
#ifdef MVDB_ENABLE_INDEX_DELTA
  assert(db.table("t")->vector_index && db.table("t")->index_delta);
  FlatIndex::ResidentView view;assert(!db.resident_vector_view("t",&view));
  verify_vectors("t",2);
  assert(db.resident_vector_index("t")); // Explicit ANN accessor requires a rebuild.
  assert(!db.table("t")->index_delta && db.resident_vector_view("t",&view));
#else
  assert(!db.table("t")->vector_index);
  FlatIndex::ResidentView view;assert(db.resident_vector_view("t",&view));
#endif
  verify_vectors("t",2);
  statement("t",1,"aborted implicit",false);
  assert(read("t",1)=="committed");assert(!db.table("t")->payload_store);
  assert(db.begin(&error,&code));
  statement("t",1,"first",true);
  statement("t",1,"aborted second",false);
  assert(read("t",1)=="first");
  statement("t",1,"third",true);
  assert(db.rollback(&error,&code));assert(read("t",1)=="committed");
  assert(!db.table("t")->payload_store);

  // Old images follow their original Table across DROP/recreate/ROLLBACK.
  assert(db.begin(&error,&code));statement("t",1,"before drop",true);
  assert(db.drop_table("t",&error,&code));create("t");assert(insert("t",1,"new table"));
  assert(db.rollback(&error,&code));assert(read("t",1)=="committed");
  verify_vectors("t",2);

  // Repeated updates cannot retain unbounded Row copies. Crossing 1 MiB
  // migrates prior images before the table switches to its payload file.
  const std::string large(28000,'x');
  create("bounded");assert(insert("bounded",1,large));
  assert(db.create_vector_index("bounded","v","bounded_vector",&error,&code));
  assert(db.begin(&error,&code));
  for(int i=0;i<80;++i) {
    statement("bounded",1,std::to_string(i)+large,true);
#ifdef MVDB_ENABLE_INDEX_DELTA
    if(!db.table("bounded")->payload_store)assert(db.table("bounded")->index_delta);
#endif
  }
  assert(db.table("bounded")->payload_store);
#ifdef MVDB_ENABLE_INDEX_DELTA
  assert(!db.table("bounded")->index_delta && !db.table("bounded")->vector_index);
#endif
  assert(db.rollback(&error,&code));assert(read("bounded",1)==large);
  verify_vectors("bounded",1);

#ifdef MVDB_TEST_WRAP_PWRITE
  // Fail while migrating already active old images, after one successful write.
  // The failed spill must leave the resident table and all prior Undo intact.
  create("failed");assert(insert("failed",1,large));
  assert(db.create_vector_index("failed","v","failed_vector",&error,&code));
  assert(db.begin(&error,&code));bool saw_failure=false;
  for(int i=0;i<80;++i) {
#ifdef MVDB_ENABLE_INDEX_DELTA
    if(i)assert(db.table("failed")->index_delta && db.table("failed")->vector_index);
#endif
    assert(db.begin_statement(&error,&code));fail_payload_writes=2;
    const int64_t n=update("failed",1,std::to_string(i)+large);
    fail_payload_writes=0;
    if(n<0) {
#ifdef MVDB_ENABLE_INDEX_DELTA
      // Failed migration has not published the spill or discarded the base.
      assert(db.table("failed")->index_delta && db.table("failed")->vector_index);
#endif
      assert(!db.end_statement(false,&error,&code));saw_failure=true;break;
    }
    assert(n==1);assert(db.end_statement(true,&error,&code));
  }
  assert(saw_failure);assert(!db.table("failed")->payload_store);
  assert(db.rollback(&error,&code));assert(read("failed",1)==large);
#endif

  // Another table's INSERT crosses the real 64 MiB global payload budget.
  // A's old image must become offset Undo before A's resident rows disappear.
  create("a");create("b");create("c");
  for(const auto& item : {std::pair<const char*,int>{"a",1000},{"b",850}}) {
    assert(db.begin(&error,&code));
    for(int i=0;i<item.second;++i)assert(insert(item.first,i,large));
    assert(db.commit(&error,&code));
  }
  assert(!db.table("a")->payload_store);
  assert(db.create_vector_index("a","v","av",&error,&code));
  assert(db.begin(&error,&code));statement("a",1,"before cross-table spill",true);
#ifdef MVDB_ENABLE_INDEX_DELTA
  assert(db.table("a")->index_delta);
#endif
  assert(db.begin_statement(&error,&code));
  int inserted=0;
  while(!db.table("a")->payload_store && inserted<850)
    assert(insert("c",inserted++,large));
  assert(db.table("a")->payload_store);
#ifdef MVDB_ENABLE_INDEX_DELTA
  assert(!db.table("a")->index_delta && !db.table("a")->vector_index);
#endif
  assert(!db.end_statement(false,&error,&code));
  assert(read("a",1)=="before cross-table spill");
  assert(db.rollback(&error,&code));assert(read("a",1)==large);
  verify_vectors("a",1000);

  // Confirm committed values survive a fresh WAL replay and uncommitted updates
  // do not. close() flushes WAL without manufacturing a COMMIT.
  assert(db.begin(&error,&code));statement("t",1,"not committed",true);
  db.close();assert(db.open(path,&error));
  assert(read("t",1)=="committed");assert(read("a",1)==large);
  assert(read("bounded",1)==large);verify_vectors("t",2);db.close();
  for(const char* file:{"data.log","snapshot.bin","snapshot.lsn"}) {
    const std::string name=std::string(path)+"/"+file;unlink(name.c_str());
  }
  rmdir(path);
  std::cout<<"PASS resident UPDATE, bounded Undo, failed statement, spill, drop and recovery\n";
}

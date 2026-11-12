// Uses the production 64 MiB global budget; no budget macros or source changes.
// Link src/db.cpp src/flat_index.cpp src/value.cpp. Run with ASan as well.
#include "db.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
using namespace mvdb;
int main() {
  alarm(90);
  char dir[]="/tmp/mvdb-cross-spill-XXXXXX";assert(mkdtemp(dir));
  Database db;std::string error;int code=0;assert(db.open(dir,&error));
  Column id;id.name="id";id.type=Type::BIGINT;id.is_primary=true;
  Column text;text.name="s";text.type=Type::VARCHAR;text.len=30000;
  Column vector;vector.name="v";vector.type=Type::VECTOR;vector.len=16;
  for(const char* name:{"a","b","c"})assert(db.create_table(name,{id,text,vector},&error,&code));
  const std::string payload(28000,'x');
  auto fill=[&](const char* name,int count) {
    assert(db.begin(&error,&code));
    for(int i=0;i<count;++i)assert(db.insert(name,{Value::ofInt(i),Value::ofStr(payload),
      Value::ofVec(std::vector<float>(16,float(i)))},&error,&code));
    assert(db.commit(&error,&code));
  };
  // A is the largest resident payload but remains below the 32 MiB table cap.
  fill("a",1000);fill("b",850);
  assert(!db.table("a")->payload_store);
  assert(db.create_vector_index("a","v","av",&error,&code));
  assert(db.table("a")->vector_index);
  assert(db.begin(&error,&code));
  for(int i=0;i<850 && !db.table("a")->payload_store;++i)
    assert(db.insert("c",{Value::ofInt(i),Value::ofStr(payload),
      Value::ofVec(std::vector<float>(16,-1))},&error,&code));
  assert(db.table("a")->payload_store);
  // Original 72 fails here: index still contains freed vector pointers.
  assert(!db.table("a")->vector_index);
  auto verify=[&] {
    size_t count=0;
    db.scan_vectors("a",{},[&](int64_t pk,const float* values,size_t n) {
      assert(n==16);for(size_t j=0;j<n;++j)assert(values[j]==float(pk));++count;return true;
    });
    assert(count==1000);
  };
  verify();assert(db.rollback(&error,&code));verify();
  assert(db.begin(&error,&code));assert(db.drop_table("a",&error,&code));
  assert(db.rollback(&error,&code));verify();
  db.close();assert(db.open(dir,&error));verify();db.close();
  for(const char* file:{"data.log","snapshot.bin","snapshot.lsn"}) {
    const std::string path=std::string(dir)+"/"+file;unlink(path.c_str());
  }
  rmdir(dir);
  std::cout<<"PASS production-budget cross-table spill, rollback/drop/restart\n";
}

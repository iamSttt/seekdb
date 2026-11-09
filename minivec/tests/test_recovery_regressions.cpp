#include "db.h"
#include "exec.h"
#include "sql.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
using namespace mvdb;
static bool run(Database& d, const std::string& sql) {
  Statement s; std::string e, out; int c=0;
  if (!parse_sql(sql, &s, &e, &c)) { std::cerr << e << '\n'; return false; }
  return execute(d,s,&out,&e,&c);
}
static void ok(Database& d, const std::string& sql) {
  if (!run(d,sql)) { std::cerr << "unexpected SQL failure: " << sql << '\n'; std::abort(); }
}
static std::vector<int64_t> keys(Database& d, const char* table="t") {
  std::vector<int64_t> k;
  d.scan(table,{},[&](const Row& r){k.push_back(r.vals[0].i);return true;});
  std::sort(k.begin(),k.end()); return k;
}
static void expect(Database& d, std::initializer_list<int64_t> k) {
  assert(keys(d)==std::vector<int64_t>(k));
}
int main() {
  char tmp[]="/tmp/mvdb-recovery-XXXXXX";
  const char* p=::mkdtemp(tmp); assert(p); const std::string dir=p;
  Database d; std::string e;
  assert(d.open(dir,&e));
  ok(d,"CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))");
  ok(d,"INSERT INTO t VALUES (1,'[1,2]')");
  ok(d,"BEGIN");
  ok(d,"INSERT INTO t VALUES (2,'[2,3]')");
  assert(!run(d,"INSERT INTO t VALUES (3,'[3,4]'),(1,'[0,0]')"));
  expect(d,{1,2});
  assert(!run(d,"INSERT INTO t VALUES (4,'[1,2]'),(5,'[1]')"));
  expect(d,{1,2}); ok(d,"COMMIT");
  d.close(); assert(d.open(dir,&e)); expect(d,{1,2});
  assert(!run(d,"UPDATE t SET id=9")); expect(d,{1,2});
  ok(d,"BEGIN"); ok(d,"CREATE TABLE ephemeral (id BIGINT PRIMARY KEY)");
  ok(d,"ROLLBACK"); assert(!d.has_table("ephemeral"));
  ok(d,"BEGIN"); ok(d,"DELETE FROM t WHERE id=2");
  assert(!run(d,"INSERT INTO t VALUES (3,'[1,2]'),(1,'[1,2]')"));
  expect(d,{1}); ok(d,"ROLLBACK"); expect(d,{1,2});
  assert(d.checkpoint(&e));
  ok(d,"BEGIN"); ok(d,"DROP TABLE t");
  ok(d,"CREATE TABLE t (id BIGINT PRIMARY KEY, x VARCHAR(20))");
  ok(d,"INSERT INTO t VALUES (99,'uncommitted')");
  // close does not roll back: bytes remain, as after a process crash.
  d.close(); assert(d.open(dir,&e)); expect(d,{1,2});
  assert(d.table("t")->schema.cols[1].type==Type::VECTOR);
  ok(d,"BEGIN"); ok(d,"CREATE TABLE ghost (id BIGINT PRIMARY KEY)");
  d.close(); assert(d.open(dir,&e)); assert(!d.has_table("ghost"));
  assert(d.begin(&e,nullptr)); assert(d.begin_statement(&e,nullptr));
  assert(d.create_vector_index("t","v","ix",&e,nullptr));
  assert(d.end_statement(true,&e,nullptr)); assert(d.has_vector_index("t"));
  assert(d.rollback(&e,nullptr)); assert(!d.has_vector_index("t"));
  assert(d.begin_statement(&e,nullptr));
  assert(d.create_vector_index("t","v","ix",&e,nullptr));
  assert(d.end_statement(true,&e,nullptr));
  d.close(); assert(d.open(dir,&e)); assert(d.has_vector_index("t"));
  d.close();
  { std::ofstream f(dir+"/data.log",std::ios::binary|std::ios::app); f.write("bad",3); }
  assert(d.open(dir,&e)); expect(d,{1,2});
  ok(d,"INSERT INTO t VALUES (6,'[6,7]')");
  d.close(); assert(d.open(dir,&e)); expect(d,{1,2,6});
  assert(d.checkpoint(&e)); d.close();
  assert(d.open(dir,&e)); expect(d,{1,2,6}); assert(d.has_vector_index("t"));
  ok(d,"BEGIN");
  ok(d,"UPDATE t SET v='[77,88]'");
  ok(d,"ROLLBACK");
  d.scan("t",{},[](const Row& r){assert(r.vals[1].v[0]==static_cast<float>(r.vals[0].i));return true;});
  ok(d,"BEGIN"); ok(d,"CREATE TABLE both (id BIGINT PRIMARY KEY)");
  ok(d,"INSERT INTO both VALUES (1)"); ok(d,"DROP TABLE both");
  ok(d,"ROLLBACK"); assert(!d.has_table("both"));
  d.close();
  int pipefd[2]; assert(::pipe(pipefd)==0);
  const pid_t child=::fork(); assert(child>=0);
  if (child==0) {
    ::close(pipefd[0]); Database c;
    if (!c.open(dir,&e)) ::_exit(2);
    ok(c,"BEGIN"); ok(c,"INSERT INTO t VALUES (7,'[7,8]'),(8,'[8,9]')"); ok(c,"COMMIT");
    ok(c,"BEGIN"); ok(c,"DROP TABLE t");
    ok(c,"CREATE TABLE t (id BIGINT PRIMARY KEY, x VARCHAR(9))");
    const char ready='R'; assert(::write(pipefd[1],&ready,1)==1);
    for (;;) ::pause();
  }
  ::close(pipefd[1]); char ready=0; assert(::read(pipefd[0],&ready,1)==1); ::close(pipefd[0]);
  assert(::kill(child,SIGKILL)==0); int status=0; assert(::waitpid(child,&status,0)==child);
  assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
  assert(d.open(dir,&e)); expect(d,{1,2,6,7,8});
  assert(d.table("t")->schema.cols[1].type==Type::VECTOR);
  std::cout << "PASS transaction, DDL generation, WAL-tail and checkpoint regressions: " << dir << '\n';
}

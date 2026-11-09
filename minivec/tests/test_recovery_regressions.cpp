#include "db.h"
#include "exec.h"
#include "sql.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
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
static void durable_format_limits(const std::string& dir) {
  Database d; std::string e; int code=0;
  assert(d.open(dir,&e));
  ok(d,"CREATE TABLE marker (id BIGINT PRIMARY KEY)");
  std::string columns="id BIGINT PRIMARY KEY";
  std::string values="1";
  for (int i=1;i<4096;++i) {
    columns+=",c"+std::to_string(i)+" BIGINT";
    values+=",0";
  }
  // The inclusive reader limit remains supported, including actual row data.
  ok(d,"CREATE TABLE wide ("+columns+")");
  ok(d,"INSERT INTO wide VALUES ("+values+")");
  const std::string too_wide="CREATE TABLE rejected ("+columns+",overflow BIGINT)";
  assert(!run(d,too_wide));
  ok(d,"INSERT INTO marker VALUES (1)");
  ok(d,"BEGIN");
  ok(d,"INSERT INTO marker VALUES (2)");
  assert(!run(d,too_wide));
  ok(d,"INSERT INTO marker VALUES (3)");
  ok(d,"COMMIT");
  // Direct API callers must not bypass the vector dimension decoder bound.
  Statement schema;
  assert(parse_sql("CREATE TABLE oversized (id BIGINT PRIMARY KEY, v VECTOR(1))",
                   &schema,&e,&code));
  schema.columns[1].len=(1<<20)+1;
  assert(!d.create_table("oversized",schema.columns,&e,&code));
  d.close();
  // WAL-only reopen catches the original bug: the invalid schema used to make
  // recovery truncate all later acknowledged transactions as a corrupt tail.
  assert(d.open(dir,&e));
  assert(!d.has_table("rejected") && !d.has_table("oversized"));
  assert(keys(d,"marker")==std::vector<int64_t>({1,2,3}));
  assert(keys(d,"wide")==std::vector<int64_t>({1}));
  assert(d.table("wide")->schema.cols.size()==4096);
  assert(d.checkpoint(&e)); d.close();
  assert(d.open(dir,&e));
  assert(keys(d,"marker")==std::vector<int64_t>({1,2,3}));
  assert(keys(d,"wide")==std::vector<int64_t>({1}));
}

static void failed_payload_writes(const std::string& dir) {
  Database d;
  std::string error;
  int code = 0;
  assert(d.open(dir, &error));
  ok(d, "CREATE TABLE spill (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(d, "CREATE TABLE marker (id BIGINT PRIMARY KEY)");
  ok(d, "CREATE TABLE resident (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(d, "INSERT INTO spill VALUES (1,'first')");
  ok(d, "UPDATE spill SET s='" + std::string(8192, 'x') + "'");
  ok(d, "UPDATE spill SET s='kept'");
  const uint64_t kept_offset = d.table("spill")->payload_offsets[0];
  assert(kept_offset > 4096);
  assert(d.checkpoint(&error));
  const auto wal_size = [&] { return std::filesystem::file_size(dir + "/data.log"); };
  const auto expect_kept = [&] {
    size_t count = 0;
    d.scan("spill", {}, [&](const Row& row) {
      assert(row.vals[1].s == "kept");
      ++count;
      return true;
    });
    assert(count == 1);
  };

  // Keep enough space in the empty WAL for a small INSERT or UPDATE, while writes to
  // the end of the larger spilled row cache fail with EFBIG.
  struct rlimit original;
  assert(::getrlimit(RLIMIT_FSIZE, &original) == 0);
  struct rlimit limited = original;
  limited.rlim_cur = 4096;
  const auto previous_handler = ::signal(SIGXFSZ, SIG_IGN);
  assert(previous_handler != SIG_ERR);
  assert(::setrlimit(RLIMIT_FSIZE, &limited) == 0);

  assert(d.update_rows("spill", {}, {{"s", Value::ofStr("failed")}}, &error, &code) == -1);
  assert(code == static_cast<int>(ErrCode::TXN));
  assert(!d.insert("spill", {Value::ofInt(2), Value::ofStr("failed")}, &error, &code));
  assert(code == static_cast<int>(ErrCode::TXN));
  assert(!run(d, "INSERT INTO spill VALUES (2,'failed')"));
  assert(wal_size() == 0);
  expect_kept();

  // A resident row can be staged even though its WAL write fails partway.
  assert(!d.insert("resident", {Value::ofInt(1), Value::ofStr(std::string(8192, 'x'))},
                   &error, &code));
  assert(code == static_cast<int>(ErrCode::TXN) && error == "WAL write failed");
  assert(keys(d, "resident").empty() && wal_size() == 0);

  ok(d, "BEGIN");
  ok(d, "INSERT INTO marker VALUES (2)");
  const auto before_commit = wal_size();
  assert(!run(d, "UPDATE spill SET s='failed'"));
  assert(!run(d, "INSERT INTO spill VALUES (2,'failed')"));
  assert(wal_size() == before_commit);
  expect_kept();
  assert(keys(d, "marker") == std::vector<int64_t>{2});
  ok(d, "COMMIT");

  // After the kept record, the cache cap allows one more short row and a
  // partial second write. Both rows of the failed batch must disappear.
  limited.rlim_cur = kept_offset + 64;
  assert(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
  const auto committed_size = wal_size();
  ok(d, "BEGIN");
  ok(d, "INSERT INTO marker VALUES (3)");
  const auto before_batch = wal_size();
  assert(!run(d, "INSERT INTO spill VALUES (2,'fits'),(3,'" + std::string(128, 'x') + "')"));
  assert(wal_size() == before_batch);
  expect_kept();
  assert((keys(d, "marker") == std::vector<int64_t>{2, 3}));
  ok(d, "ROLLBACK");
  assert(wal_size() == committed_size);

  assert(::setrlimit(RLIMIT_FSIZE, &original) == 0);
  assert(::signal(SIGXFSZ, previous_handler) != SIG_ERR);
  ok(d, "INSERT INTO spill VALUES (2,'retry')");
  assert((keys(d, "spill") == std::vector<int64_t>{1, 2}));
  ok(d, "DELETE FROM spill WHERE id=2");
  d.close();
  assert(d.open(dir, &error));
  assert(keys(d, "marker") == std::vector<int64_t>{2});
  assert(keys(d, "resident").empty());
  expect_kept();
}

static void update_undo_payloads(const std::string& dir) {
  Database d; std::string e; int code=0;
  assert(d.open(dir,&e));
  ok(d,"CREATE TABLE u (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(d,"INSERT INTO u VALUES (1,'original')");
  auto expect_value=[&](const std::string& value) {
    size_t count=0;
    d.scan("u",{},[&](const Row& r){assert(r.vals[1].s==value);++count;return true;});
    assert(count==1);
  };
  ok(d,"UPDATE u SET s='missing' WHERE id=99");
  ok(d,"UPDATE u SET s='missing' WHERE s='absent'");
  assert(!d.table("u")->payload_store);
  ok(d,"BEGIN");
  ok(d,"CREATE TABLE prior (id BIGINT PRIMARY KEY)");
  // Force mkstemp failure before mutation; the earlier transaction survives.
  d.table("u")->storage_dir=dir+"/nonexistent/subdir";
  assert(!run(d,"UPDATE u SET s='failed' WHERE id=1"));
  assert(!d.table("u")->payload_store);
  expect_value("original");
  d.table("u")->storage_dir=dir;
  ok(d,"COMMIT");
  assert(d.has_table("prior"));
  ok(d,"BEGIN");
  ok(d,"UPDATE u SET s='kept' WHERE id=1");
  assert(d.table("u")->payload_store);
  const uint64_t kept_offset=d.table("u")->payload_offsets[0];
  // Exercise the same savepoint unwind used after a later statement error.
  assert(d.begin_statement(&e,&code));
  assert(d.update_rows("u",{},{{"s",Value::ofStr("discarded")}},&e,&code)==1);
  assert(!d.end_statement(false,&e,&code));
  assert(d.table("u")->payload_offsets[0]==kept_offset);
  expect_value("kept");
  for (int i=0;i<128;++i) ok(d,"UPDATE u SET s='"+std::string(1024,'x')+"'");
  ok(d,"DROP TABLE u");
  ok(d,"CREATE TABLE u (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(d,"INSERT INTO u VALUES (9,'new generation')");
  ok(d,"ROLLBACK");
  expect_value("original");
  ok(d,"UPDATE u SET s='durable' WHERE id=1");
  d.close(); assert(d.open(dir,&e)); expect_value("durable");
  assert(d.checkpoint(&e)); d.close();
  assert(d.open(dir,&e)); expect_value("durable");
}

static void discard_uncommitted_wal_tail(const std::string& dir) {
  Database db;
  std::string error;
  assert(db.open(dir, &error));
  ok(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(db, "INSERT INTO t VALUES (1,'committed')");
  const std::string log = dir + "/data.log";
  const auto committed_size = std::filesystem::file_size(log);

  for (bool torn : {false, true}) {
    ok(db, "BEGIN");
    ok(db, "DELETE FROM t WHERE id=1");
    ok(db, "INSERT INTO t VALUES (2,'" + std::string(60000, 'x') + "')");
    db.close();  // Leaves a valid but uncommitted WAL tail, as a crash would.
    if (torn) {
      std::ofstream tail(log, std::ios::binary | std::ios::app);
      const char header[8] = {0, 0, 0, 32};  // 512 MiB payload never written.
      tail.write(header, sizeof(header));
      assert(tail.good());
    }
    assert(std::filesystem::file_size(log) > committed_size);
    assert(db.open(dir, &error));
    expect(db, {1});
    assert(std::filesystem::file_size(log) == committed_size);
  }
  ok(db, "INSERT INTO t VALUES (3,'after recovery')");
  db.close();
  assert(db.open(dir, &error));
  expect(db, {1, 3});
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
  durable_format_limits(dir+"/format-limits");
  std::cout << "PASS durable format limits and later commits\n";
  update_undo_payloads(dir+"/update-undo");
  std::cout << "PASS spilled update Undo, savepoints and table generations\n";
  failed_payload_writes(dir + "/cache-write-failure");
  std::cout << "PASS spilled INSERT and UPDATE failures, partial WAL and transaction boundaries\n";
  discard_uncommitted_wal_tail(dir+"/discard-wal-tail");
  std::cout << "PASS recovery reclaims complete and torn uncommitted WAL tails\n";
}

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
#include <iterator>
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
  // Force mkstemp failure when one statement exhausts the bounded Undo images;
  // unwind its earlier rows while retaining the preceding transaction work.
  d.table("u")->storage_dir=dir+"/nonexistent/subdir";
  assert(d.begin_statement(&e,&code));
  bool spill_failed=false;
  for(int i=0;i<64;++i) {
    if(d.update_rows("u",{},{{"s",Value::ofStr(std::string(32768,'x'))}},&e,&code)<0) {
      spill_failed=true;break;
    }
  }
  assert(spill_failed);
  assert(!d.end_statement(false,&e,&code));
  assert(!d.table("u")->payload_store);
  expect_value("original");
  d.table("u")->storage_dir=dir;
  ok(d,"COMMIT");
  assert(d.has_table("prior"));
  ok(d,"BEGIN");
  ok(d,"UPDATE u SET s='kept' WHERE id=1");
  assert(!d.table("u")->payload_store);
  for(int i=0;i<64 && !d.table("u")->payload_store;++i)
    ok(d,"UPDATE u SET s='"+std::string(32768,'x')+"'");
  assert(d.table("u")->payload_store);
  ok(d,"UPDATE u SET s='kept' WHERE id=1");
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

static std::string staging_wal(const std::string& dir) {
  std::ifstream file(dir + "/data.log", std::ios::binary);
  assert(file.good());
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

static std::string staging_batch(const char* table, int first, int count, size_t width) {
  std::string sql = std::string("INSERT INTO ") + table + " VALUES ";
  for (int i = 0; i < count; ++i) {
    if (i) sql += ',';
    sql += '(' + std::to_string(first + i) + ",'" + std::string(width, 'p') + "')";
  }
  return sql;
}

static std::vector<std::pair<int64_t, std::string>> staging_rows(Database& db) {
  std::vector<std::pair<int64_t, std::string>> result;
  db.scan("t", {}, [&](const Row& row) {
    result.emplace_back(row.vals[0].i, row.vals[1].s);
    return true;
  });
  std::sort(result.begin(), result.end());
  return result;
}

// For p(id BIGINT,s VARCHAR), a framed INSERT is 41 + string bytes:
// frame 8, kind/flags/LSN 10, table 5, count 4, BIGINT 9, VARCHAR 5.
// Sixteen rows and BEGIN stay just below the 1 MiB staging flush threshold.
static constexpr uint64_t kStagedPriorBytes = 22 + 16 * (41 + 65000);
static_assert(kStagedPriorBytes < (1u << 20), "prior statement must stay staged");

static void verify_staged_prior(Database& db) {
  const auto ids = keys(db, "p");
  assert(ids.size() == 16);
  for (int i = 0; i < 16; ++i) assert(ids[i] == i + 1);
  db.scan("p", {}, [&](const Row& row) {
    assert(row.vals[1].s == std::string(65000, 'p'));
    return true;
  });
}

static void staging_dml_short_writes(const std::string& dir, bool observe) {
  struct rlimit original;
  assert(::getrlimit(RLIMIT_FSIZE, &original) == 0);
  const auto previous_handler = ::signal(SIGXFSZ, SIG_IGN);
  assert(previous_handler != SIG_ERR);
  for (const std::string operation : {"INSERT", "UPDATE", "DELETE"}) {
    Database db;
    std::string error;
    const std::string path = dir + "/" + operation;
    assert(db.open(path, &error));
    ok(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
    ok(db, "CREATE TABLE p (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
    if (operation == "UPDATE") ok(db, "INSERT INTO t VALUES (1,'original')");
    if (operation == "DELETE") ok(db, staging_batch("t", 1, 300, 0));
    const auto original_rows = staging_rows(db);
    const std::string committed = staging_wal(path);
    ok(db, "BEGIN");
    ok(db, staging_batch("p", 1, 16, 65000));
    const uint64_t prior_lsn = db.committed_rows();
    // This acknowledged statement is still staged, not physically written.
    assert(staging_wal(path) == committed);
    const std::string sql = operation == "INSERT" ? staging_batch("t", 1, 1, 10000) :
        operation == "UPDATE" ? "UPDATE t SET s='" + std::string(10000, 'u') + "'" :
                                "DELETE FROM t";
    struct rlimit limited = original;
    limited.rlim_cur = static_cast<rlim_t>(committed.size() + 32768);
    assert(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
    const bool accepted = run(db, sql);
    assert(::setrlimit(RLIMIT_FSIZE, &original) == 0);
    const std::string partial = staging_wal(path);
    assert(partial.size() == committed.size() + 32768);
    assert(partial.compare(0, committed.size(), committed) == 0);
    std::cout << "WAL threshold probe op=" << operation << " accepted=" << accepted
              << " partial_bytes=32768 prior_statement_still_staged=true\n" << std::flush;
    if (!observe) assert(!accepted);
    assert(db.in_txn());
    verify_staged_prior(db);
    std::vector<std::pair<int64_t, std::string>> expected = original_rows;
    if (!accepted) {
      assert(db.committed_rows() == prior_lsn);
    } else if (operation == "INSERT") {
      expected = {{1, std::string(10000, 'p')}};
    } else if (operation == "UPDATE") {
      expected = {{1, std::string(10000, 'u')}};
    } else {
      expected.clear();
    }
    assert(staging_rows(db) == expected);
    // The default regression requires statement failure and its complete undo.
    // Observation mode also documents the old implementation: swallowed errors
    // can still recover consistently after space is restored and COMMIT retries.
    ok(db, "COMMIT");
    db.close();
    assert(db.open(path, &error));
    assert(staging_rows(db) == expected);
    verify_staged_prior(db);
  }
  assert(::signal(SIGXFSZ, previous_handler) != SIG_ERR);
}

static void staging_partial_commits(const std::string& dir) {
  struct rlimit original;
  assert(::getrlimit(RLIMIT_FSIZE, &original) == 0);
  const auto previous_handler = ::signal(SIGXFSZ, SIG_IGN);
  assert(previous_handler != SIG_ERR);
  int case_number = 0;
  for (bool marker_partial : {false, true}) {
    for (bool retry : {false, true}) {
      Database db;
      std::string error;
      const std::string path = dir + "/case-" + std::to_string(case_number++);
      assert(db.open(path, &error));
      ok(db, "CREATE TABLE p (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
      const std::string committed = staging_wal(path);
      ok(db, "BEGIN");
      ok(db, staging_batch("p", 1, 16, 65000));
      const uint64_t prior_lsn = db.committed_rows();
      assert(staging_wal(path) == committed);
      // Either fail in BEGIN's header while prior records are still pending,
      // or write all prior rows and only five bytes of the COMMIT header.
      const uint64_t allowance = marker_partial ? kStagedPriorBytes + 5 : 5;
      struct rlimit limited = original;
      limited.rlim_cur = static_cast<rlim_t>(committed.size() + allowance);
      assert(::setrlimit(RLIMIT_FSIZE, &limited) == 0);
      const bool accepted = run(db, "COMMIT");
      assert(!accepted && db.in_txn() && db.committed_rows() == prior_lsn);
      const auto physical_size = std::filesystem::file_size(path + "/data.log");
      assert(physical_size == committed.size() + (marker_partial ? kStagedPriorBytes : 5));
      if (!retry) {
        // Leave the limit active during close: an unwritten suffix cannot be
        // silently completed before the subsequent crash-style reopen.
        db.close();
        assert(::setrlimit(RLIMIT_FSIZE, &original) == 0);
        assert(db.open(path, &error) && keys(db, "p").empty());
        assert(staging_wal(path) == committed);
      } else {
        assert(::setrlimit(RLIMIT_FSIZE, &original) == 0);
        verify_staged_prior(db);
        ok(db, "COMMIT");
        db.close();
        assert(db.open(path, &error));
        verify_staged_prior(db);
        assert(db.checkpoint(&error));
        db.close();
        assert(db.open(path, &error));
        verify_staged_prior(db);
      }
      std::cout << "WAL partial COMMIT probe marker=" << marker_partial
                << " retry=" << retry << " recovered=true\n";
    }
  }
  assert(::signal(SIGXFSZ, previous_handler) != SIG_ERR);
}

static void staging_later_duplicate_key(const std::string& dir) {
  Database db;
  std::string error;
  assert(db.open(dir, &error));
  ok(db, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  ok(db, "CREATE TABLE p (id BIGINT PRIMARY KEY, s VARCHAR(65535))");
  const uint64_t committed_size = staging_wal(dir).size();
  ok(db, "BEGIN");
  ok(db, staging_batch("p", 1, 16, 65000));
  const uint64_t prior_lsn = db.committed_rows();
  assert(staging_wal(dir).size() == committed_size);
  // Eight new records cross 1 MiB and physically write the earlier statement;
  // the final duplicate then requires truncating its entire failed successor.
  assert(!run(db, staging_batch("t", 1, 16, 1000) + ",(1,'duplicate')"));
  assert(db.in_txn() && db.committed_rows() == prior_lsn && keys(db).empty());
  assert(staging_wal(dir).size() == committed_size + kStagedPriorBytes);
  verify_staged_prior(db);
  ok(db, "INSERT INTO t VALUES (99,'after failed statement')");
  ok(db, "COMMIT");
  db.close();
  assert(db.open(dir, &error));
  assert(keys(db) == std::vector<int64_t>{99});
  verify_staged_prior(db);
  assert(db.checkpoint(&error));
  db.close();
  assert(db.open(dir, &error) && keys(db) == std::vector<int64_t>{99});
  verify_staged_prior(db);
}

static void staging_regressions(const std::string& dir, bool observe) {
  staging_dml_short_writes(dir + "/dml", observe);
  staging_partial_commits(dir + "/commit");
  staging_later_duplicate_key(dir + "/duplicate");
  if (observe)
    std::cout << "OBSERVED WAL error handling and verified recovery; accepted flags are diagnostic\n";
  else
    std::cout << "PASS 1 MiB WAL staging, partial writes, commit retry and recovery\n";
}

int main(int argc, char** argv) {
  bool staging_only = false, observe = false;
  if (argc == 2) {
    const std::string option = argv[1];
    staging_only = option == "--wal-staging" || option == "--wal-staging-observe";
    observe = option == "--wal-staging-observe";
    if (!staging_only) return 2;
  } else if (argc != 1) return 2;
  char tmp[]="/tmp/mvdb-recovery-XXXXXX";
  const char* p=::mkdtemp(tmp); assert(p); const std::string dir=p;
  if (staging_only) {
    staging_regressions(dir + "/staging", observe);
    return 0;
  }
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
  staging_regressions(dir + "/staging", false);
}

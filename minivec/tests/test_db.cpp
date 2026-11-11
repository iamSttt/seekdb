// test_db.cpp — self-contained storage-engine tests (no test framework).
//
//   g++ -std=c++17 -O2 -Isrc tests/test_db.cpp src/db.cpp src/value.cpp -o /tmp/test_db
//
// Covers: create/insert/scan, PK duplicate rejection, VARCHAR Unicode length,
// vector validation, transaction rollback of insert/update/delete, checkpoint +
// reopen, WAL replay into a second Database on the same directory (the crash
// case, without a checkpoint), and tolerance of a torn WAL tail.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "db.h"
#include "value.h"

using namespace mvdb;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, msg)                                     \
  do {                                                       \
    if (cond) {                                              \
      ++g_pass;                                              \
      std::printf("  ok   %s\n", msg);                       \
    } else {                                                 \
      ++g_fail;                                              \
      std::printf("  FAIL %s  (%s:%d)\n", msg, __FILE__, __LINE__); \
    }                                                        \
  } while (0)

static std::vector<Column> simple_schema() {
  Column id;
  id.name = "id";
  id.type = Type::BIGINT;
  id.is_primary = true;
  Column name;
  name.name = "name";
  name.type = Type::VARCHAR;
  name.len = 8;
  Column emb;
  emb.name = "emb";
  emb.type = Type::VECTOR;
  emb.len = 3;
  return {id, name, emb};
}

static bool ins(Database& db, const std::string& t, int64_t id, const std::string& nm,
                std::vector<float> vec, int* out_code = nullptr) {
  std::vector<Value> vals;
  vals.push_back(Value::ofInt(id));
  vals.push_back(Value::ofStr(nm));
  vals.push_back(Value::ofVec(std::move(vec)));
  std::string err;
  int code = 0;
  const bool ok = db.insert(t, std::move(vals), &err, &code);
  if (out_code) *out_code = code;
  if (!ok) std::printf("     (insert failed: %d %s)\n", code, err.c_str());
  return ok;
}

static size_t count_rows(Database& db, const std::string& t,
                         const std::vector<Cond>& conds = std::vector<Cond>()) {
  size_t n = 0;
  db.scan(t, conds, [&](const Row&) {
    ++n;
    return true;
  });
  return n;
}

static Cond cond_eq(const std::string& col, Value v) {
  Cond c;
  c.op = Cond::EQ;
  c.column = col;
  c.value = std::move(v);
  return c;
}
static Cond cond_ge(const std::string& col, Value v) {
  Cond c;
  c.op = Cond::GE;
  c.column = col;
  c.value = std::move(v);
  return c;
}

static std::string find_name(Database& db, const std::string& t, int64_t id) {
  std::string got;
  db.scan(t, {cond_eq("id", Value::ofInt(id))}, [&](const Row& r) {
    if (r.vals.size() > 1) got = r.vals[1].s;
    return false;
  });
  return got;
}

static long long file_size(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 ? (long long)st.st_size : -1;
}

static double now_ms() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// ===========================================================================
// Large-data section: the whole point of the disk-resident rewrite is that a
// table many times the size of the memory budget still works. These checks run
// by default; MVDB_TEST_BIG=0 skips the 2M row load (the rest stays).
// ===========================================================================
static const int kBigDim = 128;

static std::vector<Column> big_schema() {
  Column id;
  id.name = "id";
  id.type = Type::BIGINT;
  id.is_primary = true;
  Column emb;
  emb.name = "emb";
  emb.type = Type::VECTOR;
  emb.len = kBigDim;
  return {id, emb};
}

static void fill_vec(std::vector<float>& v, int64_t i) {
  v.resize(kBigDim);
  for (int d = 0; d < kBigDim; ++d) v[(size_t)d] = (float)((i * 31 + d * 7) % 1000) / 1000.0f;
}

static bool ins_big(Database& db, const std::string& t, int64_t i) {
  std::vector<float> v;
  fill_vec(v, i);
  std::vector<Value> vals;
  vals.push_back(Value::ofInt(i));
  vals.push_back(Value::ofVec(std::move(v)));
  std::string err;
  int code = 0;
  return db.insert(t, std::move(vals), &err, &code);
}

static long rss_kb() {
  struct rusage ru;
  ::getrusage(RUSAGE_SELF, &ru);
  return ru.ru_maxrss;  // KiB on Linux
}

int main() {
  char tmpl[] = "/tmp/minivec_tdbXXXXXX";
  char* dir_c = ::mkdtemp(tmpl);
  if (!dir_c) {
    std::printf("mkdtemp failed\n");
    return 1;
  }
  const std::string dir = dir_c;
  std::printf("test dir: %s\n", dir.c_str());
  std::string err;
  int code = 0;

  // ---------------------------------------------------------------- utf8 ---
  std::printf("[utf8 helpers]\n");
  CHECK(utf8_length("") == 0, "utf8_length(\"\") == 0");
  CHECK(utf8_length("abc") == 3, "utf8_length ascii == 3");
  CHECK(utf8_length("\xE6\xB1\x89\xE5\xAD\x97") == 2, "utf8_length 汉字 == 2 (6 bytes)");
  CHECK(utf8_length("h\xC3\xA9llo") == 5, "utf8_length héllo == 5");
  CHECK(utf8_length("\xF0\x9F\x98\x80") == 1, "utf8_length emoji == 1 (4 bytes)");
  CHECK(utf8_compare("abc", "abd") < 0, "utf8_compare abc < abd");
  CHECK(utf8_compare("abc", "abc") == 0, "utf8_compare equal");
  CHECK(utf8_compare("ab", "abc") < 0, "utf8_compare prefix shorter first");
  CHECK(utf8_compare("\xE6\xB1\x89", "z") > 0, "utf8_compare 汉 > z (code point order)");
  CHECK(utf8_compare("\xC3\xA9", "e") > 0, "utf8_compare é > e");

  // ------------------------------------------------ create / insert / scan --
  std::printf("[create + insert + scan]\n");
  {
    Database db;
    CHECK(db.open(dir, &err), "open fresh dir");
    CHECK(db.create_table("t1", simple_schema(), &err, &code), "create table t1");
    CHECK(db.has_table("t1"), "has_table");
    CHECK(!db.create_table("t1", simple_schema(), &err, &code) && code == 1050,
          "duplicate table -> 1050");

    CHECK(ins(db, "t1", 1, "alpha", {1.f, 0.f, 0.f}), "insert id=1");
    CHECK(ins(db, "t1", 2, "beta", {0.f, 1.f, 0.f}), "insert id=2");
    CHECK(ins(db, "t1", 3, "gamma", {0.f, 0.f, 1.f}), "insert id=3");
    CHECK(count_rows(db, "t1") == 3, "scan counts 3 rows");
    CHECK(db.table("t1")->live_count() == 3, "live_count == 3");

    // filtered scan
    CHECK(count_rows(db, "t1", {cond_eq("name", Value::ofStr("beta"))}) == 1,
          "scan name = 'beta' -> 1");
    CHECK(count_rows(db, "t1", {cond_ge("id", Value::ofInt(2))}) == 2, "scan id >= 2 -> 2");
    CHECK(count_rows(db, "t1", {cond_ge("id", Value::ofInt(2)),
                                cond_eq("name", Value::ofStr("gamma"))}) == 1,
          "ANDed conds -> 1");
    CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(99))}) == 0, "pk lookup miss -> 0");
    CHECK(find_name(db, "t1", 3) == "gamma", "pk equality returns the right row");

    // value validation
    CHECK(ins(db, "t1", 4, "汉字", {0.f, 0.f, 0.f}), "VARCHAR counts code points (2/8)");
    CHECK(!ins(db, "t1", 5, "123456789", {0.f, 0.f, 0.f}, &code) && code == 1264,
          "VARCHAR(8) rejects 9 chars -> 1264");
    CHECK(!ins(db, "t1", 6, "ok", {0.f, 0.f}, &code) && code == 1264,
          "vector dim mismatch -> 1264");
    CHECK(!ins(db, "t1", 7, "ok", {0.f, 0.f, std::nanf("")}, &code) && code == 1264,
          "NaN in vector -> 1264");
    CHECK(!ins(db, "t1", 8, "ok", {0.f, 0.f, 1.f / 0.f}, &code) && code == 1264,
          "Inf in vector -> 1264");
    {
      // arity mismatch
      std::vector<Value> two;
      two.push_back(Value::ofInt(10));
      two.push_back(Value::ofStr("x"));
      CHECK(!db.insert("t1", std::move(two), &err, &code) && code == 1264,
            "arity mismatch -> 1264");
    }
    CHECK(count_rows(db, "t1") == 4, "still 4 live rows after rejected inserts");

    // scoping: rows written above are autocommit -> durable; keep this db open
    // for the WAL-replay test below.
    std::printf("[pk duplicate]\n");
    int dupc = 0;
    CHECK(!ins(db, "t1", 1, "dup", {0.f, 0.f, 0.f}, &dupc) && dupc == 1062,
          "duplicate PK -> 1062");
    CHECK(count_rows(db, "t1") == 4, "duplicate insert did not add a row");

    // ------------------------------------------------------- transactions --
    std::printf("[transactions]\n");
    CHECK(db.begin(&err, &code), "BEGIN");
    CHECK(db.in_txn(), "in_txn() true");
    CHECK(!db.begin(&err, &code) && code == 1568, "nested BEGIN -> 1568");
    CHECK(ins(db, "t1", 100, "txn1", {1.f, 1.f, 1.f}), "txn insert id=100");
    CHECK(ins(db, "t1", 101, "txn2", {1.f, 1.f, 1.f}), "txn insert id=101");
    CHECK(count_rows(db, "t1") == 6, "reads see uncommitted rows (6)");
    {
      std::vector<std::pair<std::string, Value>> sets;
      sets.emplace_back("name", Value::ofStr("changed"));
      CHECK(db.update_rows("t1", {cond_eq("id", Value::ofInt(2))}, sets, &err, &code) == 1,
            "txn update id=2");
      CHECK(find_name(db, "t1", 2) == "changed", "update visible before commit");
      CHECK(db.delete_rows("t1", {cond_eq("id", Value::ofInt(3))}, &err, &code) == 1,
            "txn delete id=3");
      CHECK(count_rows(db, "t1") == 5, "5 rows after txn insert/update/delete");
    }
    CHECK(db.rollback(&err, &code), "ROLLBACK");
    CHECK(!db.in_txn(), "in_txn() false after rollback");
    CHECK(count_rows(db, "t1") == 4, "rollback removed the 2 inserted rows");
    CHECK(find_name(db, "t1", 2) == "beta", "rollback restored the updated row");
    CHECK(find_name(db, "t1", 3) == "gamma", "rollback restored the deleted row");
    CHECK(ins(db, "t1", 100, "after", {1.f, 1.f, 1.f}), "insert id=100 works again after rollback");

    // committed transaction survives
    CHECK(db.begin(&err, &code), "BEGIN (2)");
    CHECK(ins(db, "t1", 200, "keep", {2.f, 2.f, 2.f}), "txn insert id=200");
    CHECK(db.commit(&err, &code), "COMMIT");
    CHECK(db.commit(&err, &code), "COMMIT outside txn is a no-op");
    CHECK(db.rollback(&err, &code), "ROLLBACK outside txn is a no-op");
    CHECK(count_rows(db, "t1") == 6, "6 rows after commit");

    // ------------------------------------------------ WAL replay (crash) ---
    std::printf("[wal replay into a second Database (no checkpoint)]\n");
    {
      Database other;  // same directory, first Database is still open
      CHECK(other.open(dir, &err), "second open() on the same dir replays the WAL");
      CHECK(count_rows(other, "t1") == 6, "second instance sees all 6 committed rows");
      CHECK(find_name(other, "t1", 2) == "beta", "replayed row content matches");
      CHECK(find_name(other, "t1", 200) == "keep", "committed txn row was replayed");
      CHECK(count_rows(other, "t1", {cond_eq("id", Value::ofInt(100))}) == 1,
            "row inserted after a rollback is present exactly once");
      CHECK(other.has_table("t1"), "catalog round-trips the schema");
      CHECK(other.table("t1")->schema.cols.size() == 3, "3 columns after reopen");
    }

    // -------------------------------------------------------- checkpoint ---
    std::printf("[checkpoint + reopen]\n");
    CHECK(db.checkpoint(&err), "checkpoint");
    CHECK(file_size(dir + "/data.log") == 0, "WAL truncated after checkpoint");
    CHECK(file_size(dir + "/snapshot.bin") > 0, "snapshot.bin written");
    CHECK(file_size(dir + "/snapshot.lsn") > 0, "snapshot.lsn written");
    CHECK(db.checkpoint(&err), "second checkpoint is a no-op success");
  }
  {
    Database db;
    CHECK(db.open(dir, &err), "reopen after checkpoint");
    CHECK(count_rows(db, "t1") == 6, "checkpoint + reopen preserved 6 rows");
    CHECK(find_name(db, "t1", 1) == "alpha", "row content preserved");
    CHECK(find_name(db, "t1", 100) == "after", "rolled-back row 100 is the committed one");
    CHECK(db.table("t1")->schema.find("emb") == 2, "schema round-trip");

    // deletes + checkpoint
    CHECK(db.delete_rows("t1", {cond_eq("id", Value::ofInt(1))}, &err, &code) == 1,
          "delete id=1");
    CHECK(count_rows(db, "t1") == 5, "5 rows after delete");
    CHECK(db.checkpoint(&err), "checkpoint after delete");
    CHECK(ins(db, "t1", 300, "last", {3.f, 3.f, 3.f}), "insert after checkpoint");
    CHECK(db.committed_rows() > 0, "committed_rows() advances");
  }
  {
    Database db;
    CHECK(db.open(dir, &err), "reopen after post-checkpoint insert");
    CHECK(count_rows(db, "t1") == 6, "snapshot + WAL replay = 6 rows");
    CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(1))}) == 0, "deleted row stayed deleted");

    // ---------------------------------------------------------- schema ----
    std::printf("[create_table validation]\n");
    {
      std::vector<Column> bad = simple_schema();
      bad[1].name = "id";
      CHECK(!db.create_table("bad1", bad, &err, &code), "duplicate column name rejected");
    }
    {
      std::vector<Column> bad = simple_schema();
      bad[1].is_primary = true;
      bad[1].type = Type::BIGINT;
      CHECK(!db.create_table("bad2", bad, &err, &code), "two primary keys rejected");
    }
    {
      std::vector<Column> bad = simple_schema();
      bad[0].is_primary = false;
      CHECK(!db.create_table("bad3", bad, &err, &code), "missing primary key rejected");
    }
    {
      std::vector<Column> bad = simple_schema();
      bad[1].len = 0;
      CHECK(!db.create_table("bad4", bad, &err, &code), "VARCHAR(0) rejected");
    }
    {
      std::vector<Column> bad = simple_schema();
      bad[2].len = 0;
      CHECK(!db.create_table("bad5", bad, &err, &code), "VECTOR(0) rejected");
    }
    {
      std::vector<Column> bad = simple_schema();
      Column extra;
      extra.name = "emb2";
      extra.type = Type::VECTOR;
      extra.len = 4;
      bad.push_back(extra);
      CHECK(!db.create_table("bad6", bad, &err, &code), "two VECTOR columns rejected");
    }
    {
      Column id;
      id.name = "id";
      id.type = Type::VARCHAR;
      id.len = 4;
      id.is_primary = true;
      std::vector<Column> bad{id};
      CHECK(!db.create_table("bad7", bad, &err, &code), "non-BIGINT primary key rejected");
    }

    // ------------------------------------------------------ vector index --
    std::printf("[vector index flag]\n");
    CHECK(!db.has_vector_index("t1"), "no index initially");
    CHECK(db.create_vector_index("t1", "emb", "idx_emb", &err, &code), "create_vector_index");
    CHECK(db.has_vector_index("t1"), "has_vector_index true");
    CHECK(!db.create_vector_index("t1", "name", "idx_name", &err, &code),
          "index on a non-VECTOR column rejected");
    CHECK(!db.create_vector_index("nope", "emb", "x", &err, &code), "index on missing table rejected");
    CHECK(db.table_names().size() == 1, "only t1 exists");

    // ------------------------------------------------------ update / delete
    std::printf("[update / delete]\n");
    {
      std::vector<std::pair<std::string, Value>> sets;
      sets.emplace_back("name", Value::ofStr("bulk"));
      CHECK(db.update_rows("t1", {}, sets, &err, &code) == 6, "update all 6 rows");
      CHECK(count_rows(db, "t1", {cond_eq("name", Value::ofStr("bulk"))}) == 6, "all renamed");
      // pk update keeps pk_pos consistent
      std::vector<std::pair<std::string, Value>> sets2;
      sets2.emplace_back("id", Value::ofInt(999));
      CHECK(db.update_rows("t1", {cond_eq("id", Value::ofInt(2))}, sets2, &err, &code) == 1,
            "update the primary key");
      CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(2))}) == 0, "old pk gone");
      CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(999))}) == 1, "new pk present");
      std::vector<std::pair<std::string, Value>> sets3;
      sets3.emplace_back("id", Value::ofInt(999));
      CHECK(db.update_rows("t1", {cond_eq("id", Value::ofInt(300))}, sets3, &err, &code) == -1 &&
                code == 1062,
            "update to an existing pk -> 1062");
    }
  }

  // ------------------------------------------------------------ torn tail ---
  std::printf("[torn WAL tail]\n");
  {
    Database db;
    CHECK(db.open(dir, &err), "reopen before corruption");
    const size_t before = count_rows(db, "t1");
    CHECK(ins(db, "t1", 400, "tail", {4.f, 4.f, 4.f}), "insert before tearing");
    // Simulate a crash mid-append: a partial record header + garbage.
    {
      FILE* f = std::fopen((dir + "/data.log").c_str(), "ab");
      CHECK(f != nullptr, "open data.log for corruption");
      const char junk[11] = {0x40, 0x00, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
      if (f) {
        std::fwrite(junk, 1, sizeof(junk), f);
        std::fclose(f);
      }
    }
    const long long with_junk = file_size(dir + "/data.log");
    CHECK(with_junk > 0, "data.log has a torn tail");
    db.close();

    Database db2;
    CHECK(db2.open(dir, &err), "reopen with a torn tail (must not fail)");
    CHECK(count_rows(db2, "t1") == before + 1, "all committed rows survived the torn tail");
    CHECK(file_size(dir + "/data.log") == 0 || file_size(dir + "/data.log") < with_junk,
          "torn tail truncated on replay");
    // Appends after the repair must be readable again.
    CHECK(ins(db2, "t1", 401, "postrep", {4.f, 4.f, 4.f}), "insert after repair");
    db2.close();
  }
  {
    Database db;
    CHECK(db.open(dir, &err), "reopen after repair + insert");
    CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(401))}) == 1,
          "post-repair insert survived");
    CHECK(ins(db, "t1", 402, "postrep2", {4.f, 4.f, 4.f}), "and another insert");
  }

  // ------------------------------------------------------------- bulk perf --
  std::printf("[bulk insert]\n");
  {
    const std::string pdir = dir + "/perf";
    Database db;
    CHECK(db.open(pdir, &err), "open perf dir");
    CHECK(db.create_table("big", simple_schema(), &err, &code), "create perf table");

    double t0 = now_ms();
    CHECK(db.begin(&err, &code), "BEGIN bulk");
    const int N = 20000;
    bool all_ok = true;
    for (int i = 0; i < N; ++i) {
      if (!ins(db, "big", i, "row", {float(i), float(i % 7), 1.5f})) {
        all_ok = false;
        break;
      }
    }
    CHECK(all_ok, "20000 inserts in one txn");
    CHECK(db.commit(&err, &code), "COMMIT bulk");
    double t1 = now_ms();
    std::printf("     20000 txn rows: %.1f ms\n", t1 - t0);

    double t2 = now_ms();
    CHECK(count_rows(db, "big") == (size_t)N, "scan sees all 20000");
    double t3 = now_ms();
    std::printf("     full scan: %.1f ms\n", t3 - t2);

    // autocommit durability cost (each insert fdatasyncs)
    double t4 = now_ms();
    for (int i = 0; i < 300; ++i) ins(db, "big", 100000 + i, "ac", {0.f, 0.f, 0.f});
    double t5 = now_ms();
    std::printf("     300 autocommit rows (fdatasync each): %.1f ms\n", t5 - t4);

    double t6 = now_ms();
    CHECK(db.checkpoint(&err), "checkpoint 20000 rows");
    double t7 = now_ms();
    std::printf("     checkpoint: %.1f ms\n", t7 - t6);
    db.close();

    Database db2;
    double t8 = now_ms();
    CHECK(db2.open(pdir, &err), "reopen 20000-row snapshot");
    double t9 = now_ms();
    CHECK(count_rows(db2, "big") == (size_t)(N + 300), "all rows after reopen");
    std::printf("     open+load snapshot: %.1f ms\n", t9 - t8);
  }

  // Autocommit DML must fold the WAL into the heap by itself: nothing else
  // calls checkpoint(). MVDB_CKPT_BYTES lowers the threshold to 8 KiB so the
  // test does not have to write 64 MiB of log first.
  std::printf("[auto checkpoint]\n");
  {
    const std::string adir = dir + "/autockpt";
    ::setenv("MVDB_CKPT_BYTES", "8192", 1);
    Database d;
    CHECK(d.open(adir, &err), "open auto-checkpoint dir");
    CHECK(d.create_table("a", simple_schema(), &err, &code), "create table a");
    for (int i = 0; i < 400; ++i)
      if (!ins(d, "a", i, "x", {1.f, 2.f, 3.f})) break;
    CHECK(file_size(adir + "/snapshot.bin") > 0, "snapshot written without an explicit checkpoint");
    CHECK(file_size(adir + "/t_a.dat") > 64, "compacted heap file exists");
    CHECK(count_rows(d, "a") == 400, "all rows still visible");
    d.close();
    Database d2;
    CHECK(d2.open(adir, &err), "reopen auto-checkpointed dir");
    CHECK(count_rows(d2, "a") == 400, "every row survived the automatic checkpoint");
    ::unsetenv("MVDB_CKPT_BYTES");
  }

  // ------------------------------------------- real crash (fork + _exit) ---
  // A child commits rows, then dies via _exit(0) in the middle of a second
  // transaction: no destructors, no close(), no checkpoint. Everything
  // committed must come back; the uncommitted transaction must not.
  std::printf("[crash: _exit mid-transaction, no clean shutdown]\n");
  {
    const std::string cdir = dir + "/crash";
    pid_t pid = ::fork();
    if (pid == 0) {
      Database d;
      std::string e;
      int c = 0;
      if (!d.open(cdir, &e)) ::_exit(2);
      if (!d.create_table("cr", simple_schema(), &e, &c)) ::_exit(3);
      for (int i = 0; i < 50; ++i)
        if (!ins(d, "cr", i, "auto", {1.f, 2.f, 3.f})) ::_exit(4);
      if (!d.begin(&e, &c)) ::_exit(5);
      for (int i = 50; i < 60; ++i) ins(d, "cr", i, "txn", {1.f, 2.f, 3.f});
      if (!d.commit(&e, &c)) ::_exit(6);
      if (!d.begin(&e, &c)) ::_exit(7);
      for (int i = 100; i < 110; ++i) ins(d, "cr", i, "rolled", {1.f, 2.f, 3.f});
      ::_exit(0);  // kill -9 equivalent: no flush, no close, no checkpoint
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child committed then died via _exit");
    Database d;
    CHECK(d.open(cdir, &err), "reopen after crash");
    CHECK(count_rows(d, "cr") == 60, "60 committed rows recovered, 10 uncommitted dropped");
    CHECK(count_rows(d, "cr", {cond_eq("id", Value::ofInt(105))}) == 0,
          "row from the uncommitted txn is gone");
    CHECK(find_name(d, "cr", 55) == "txn", "row from the committed txn came back");
    CHECK(find_name(d, "cr", 7) == "auto", "autocommit row came back");
  }


  // ------------------------------------- heap truncated behind the header ---
  // A machine crash can leave the heap shorter than the commit watermark. The
  // engine must fall back to the checkpoint and rebuild from the WAL.
  std::printf("[heap shorter than the watermark -> WAL rebuild]\n");
  {
    const std::string rdir = dir + "/rebuild";
    {
      Database db;
      CHECK(db.open(rdir, &err), "open rebuild dir");
      CHECK(db.create_table("rb", simple_schema(), &err, &code), "create rb");
      for (int i = 0; i < 100; ++i) ins(db, "rb", i, "a", {1.f, 0.f, 0.f});
      CHECK(db.checkpoint(&err), "checkpoint rb");
      const long long ck = file_size(rdir + "/t_rb.dat");
      CHECK(ck > 64, "compacted heap has the 100 rows");
      for (int i = 100; i < 200; ++i) ins(db, "rb", i, "b", {0.f, 1.f, 0.f});
      {
        std::vector<std::pair<std::string, Value>> sets;
        sets.emplace_back("name", Value::ofStr("upd"));
        CHECK(db.update_rows("rb", {cond_eq("id", Value::ofInt(5))}, sets, &err, &code) == 1,
              "update before the simulated crash");
      }
      CHECK(db.delete_rows("rb", {cond_eq("id", Value::ofInt(7))}, &err, &code) == 1,
            "delete before the simulated crash");
      db.close();
      // Simulate lost tail pages: keep only what the checkpoint made durable.
      CHECK(::truncate((rdir + "/t_rb.dat").c_str(), (off_t)ck) == 0, "truncate heap to checkpoint size");
    }
    Database db;
    CHECK(db.open(rdir, &err), "reopen with a short heap");
    CHECK(count_rows(db, "rb") == 199, "all committed rows rebuilt from the WAL");
    CHECK(find_name(db, "rb", 150) == "b", "row added after the checkpoint came back");
    CHECK(find_name(db, "rb", 5) == "upd", "update after the checkpoint was replayed");
    CHECK(count_rows(db, "rb", {cond_eq("id", Value::ofInt(7))}) == 0,
          "delete after the checkpoint was replayed");
    CHECK(find_name(db, "rb", 5) != "", "row 5 still present");
  }

  // --------------------------------------------- drop + recreate reuses files
  std::printf("[drop + recreate the same table name]\n");
  {
    const std::string ddir = dir + "/recreate";
    Database db;
    CHECK(db.open(ddir, &err), "open recreate dir");
    CHECK(db.create_table("d", simple_schema(), &err, &code), "create d");
    for (int i = 0; i < 50; ++i) ins(db, "d", i, "x", {1.f, 1.f, 1.f});
    CHECK(count_rows(db, "d") == 50, "50 rows before the drop");
    CHECK(db.drop_table("d", &err, &code), "drop d");
    CHECK(db.create_table("d", simple_schema(), &err, &code), "recreate d");
    CHECK(count_rows(db, "d") == 0, "recreated table starts empty (old heap files are gone)");
    CHECK(ins(db, "d", 1, "y", {2.f, 2.f, 2.f}), "insert into the recreated table");
    CHECK(count_rows(db, "d") == 1, "exactly one row");
    db.close();
    Database db2;
    CHECK(db2.open(ddir, &err), "reopen after recreate");
    CHECK(count_rows(db2, "d") == 1, "recreated table survived the reopen");
    CHECK(find_name(db2, "d", 1) == "y", "recreated row content is right");

    // a table dropped inside a committed transaction
    CHECK(db2.begin(&err, &code), "BEGIN drop");
    CHECK(db2.drop_table("d", &err, &code), "drop in txn");
    CHECK(db2.commit(&err, &code), "COMMIT drop");
    CHECK(!db2.has_table("d"), "table gone after commit");
    db2.close();
    Database db3;
    CHECK(db3.open(ddir, &err), "reopen after committed drop");
    CHECK(!db3.has_table("d"), "dropped table stays dropped");
    CHECK(db3.create_table("d", simple_schema(), &err, &code), "same name again after a committed drop");
    CHECK(count_rows(db3, "d") == 0, "and it is empty");
  }

  // ========================================================= large tables ==
  const bool big = []() {
    const char* e = std::getenv("MVDB_TEST_BIG");
    return !(e && (std::strcmp(e, "0") == 0 || std::strcmp(e, "off") == 0));
  }();

  if (big) {
    const int N = 2000000;
    const std::string ldir = dir + "/large";
    std::printf("[large: %d x %d-d on disk, RSS gate]\n", N, kBigDim);
    {
      Database db;
      CHECK(db.open(ldir, &err), "open large dir");
      CHECK(db.create_table("big", big_schema(), &err, &code), "create 2M-row table");

      double t0 = now_ms();
      bool all_ok = true;
      for (int base = 0; base < N && all_ok; base += 100) {
        if (!db.begin(&err, &code)) { all_ok = false; break; }
        const int hi = (base + 100 < N) ? base + 100 : N;
        for (int i = base; i < hi; ++i)
          if (!ins_big(db, "big", i)) { all_ok = false; break; }
        if (!db.commit(&err, &code)) all_ok = false;
      }
      double t1 = now_ms();
      CHECK(all_ok, "2,000,000 inserts in 100-row transactions");
      const long rss_mb = rss_kb() / 1024;
      std::printf("     load: %.1f s -> %.0f rows/s, peak RSS %ld MB, table %.0f MB\n",
                  (t1 - t0) / 1000.0, N / ((t1 - t0) / 1000.0), rss_mb,
                  (double)file_size(ldir + "/t_big.dat") / 1048576.0);
      CHECK(db.table("big")->live_count() == (size_t)N, "live_count == 2,000,000");
      CHECK(rss_mb < 300, "peak RSS < 300 MB for a ~1 GB table");

      // full scan
      size_t n = 0;
      double s0 = now_ms();
      db.scan("big", {}, [&](const Row&) { ++n; return true; });
      double s1 = now_ms();
      std::printf("     scan: %.2f s -> %.0f rows/s\n", (s1 - s0) / 1000.0,
                  n / ((s1 - s0) / 1000.0));
      CHECK(n == (size_t)N, "full scan sees every row");

      // scan with a WHERE filter, and check the payload, not just the count
      Cond ge;
      ge.op = Cond::GE;
      ge.column = "id";
      ge.value = Value::ofInt(N / 2);
      size_t half = 0;
      bool payload_ok = true;
      db.scan("big", {ge}, [&](const Row& r) {
        ++half;
        if (half == 1) {
          std::vector<float> want;
          fill_vec(want, r.vals[0].i);
          if (r.vals[1].v != want) payload_ok = false;
        }
        return true;
      });
      CHECK(half == (size_t)(N - N / 2), "filtered scan (id >= N/2) returns half the rows");
      CHECK(payload_ok, "first filtered row's 128-d payload is byte exact");

      // pk lookups are single random reads
      double p0 = now_ms();
      int hits = 0;
      for (int k = 0; k < 200; ++k) {
        const int64_t want_pk = (int64_t)k * 7919;
        Cond eq;
        eq.op = Cond::EQ;
        eq.column = "id";
        eq.value = Value::ofInt(want_pk);
        db.scan("big", {eq}, [&](const Row& r) {
          std::vector<float> exp;
          fill_vec(exp, want_pk);
          if (r.vals[0].i == want_pk && r.vals[1].v == exp) ++hits;
          return false;
        });
      }
      double p1 = now_ms();
      std::printf("     pk lookup: %.1f us/op\n", (p1 - p0) * 1000.0 / 200.0);
      CHECK(hits == 200, "200 random pk lookups return exact rows");

      // update / delete across a checkpoint + reopen
      std::vector<std::pair<std::string, Value>> sets;
      sets.emplace_back("emb", Value::ofVec(std::vector<float>(kBigDim, 0.5f)));
      Cond idrange;
      idrange.op = Cond::GE;
      idrange.column = "id";
      idrange.value = Value::ofInt(N - 1000);
      CHECK(db.update_rows("big", {idrange}, sets, &err, &code) == 1000, "update 1000 rows by range");
      Cond lt;
      lt.op = Cond::LT;
      lt.column = "id";
      lt.value = Value::ofInt(1000);
      CHECK(db.delete_rows("big", {lt}, &err, &code) == 1000, "delete 1000 rows by range");
      CHECK(db.table("big")->live_count() == (size_t)(N - 1000), "live_count after update/delete");

      double c0 = now_ms();
      CHECK(db.checkpoint(&err), "checkpoint a 1 GB table");
      double c1 = now_ms();
      std::printf("     checkpoint: %.2f s\n", (c1 - c0) / 1000.0);
      CHECK(file_size(ldir + "/data.log") == 0, "WAL truncated by the large checkpoint");
      CHECK(file_size(ldir + "/t_big.dat") < (long long)N * (16 + 4 * kBigDim) + (1 << 20),
            "compacted heap is close to the live footprint");
      db.close();

      double r0 = now_ms();
      Database db2;
      CHECK(db2.open(ldir, &err), "reopen the 1 GB table");
      double r1 = now_ms();
      std::printf("     reopen: %.2f s\n", (r1 - r0) / 1000.0);
      CHECK(db2.table("big")->live_count() == (size_t)(N - 1000), "row count survives the reopen");
      size_t n2 = 0;
      db2.scan("big", {}, [&](const Row&) { ++n2; return true; });
      CHECK(n2 == (size_t)(N - 1000), "scan after reopen sees every live row");

      Cond upl;
      upl.op = Cond::EQ;
      upl.column = "id";
      upl.value = Value::ofInt(N - 5);
      bool up_ok = false;
      db2.scan("big", {upl}, [&](const Row& r) {
        up_ok = r.vals[1].v.size() == (size_t)kBigDim && r.vals[1].v[0] == 0.5f &&
                r.vals[1].v[kBigDim - 1] == 0.5f;
        return false;
      });
      CHECK(up_ok, "updated row kept its new payload across the reopen");

      Cond del;
      del.op = Cond::EQ;
      del.column = "id";
      del.value = Value::ofInt(500);
      CHECK(count_rows(db2, "big", {del}) == 0, "deleted row stayed deleted");
      Cond keep;
      keep.op = Cond::EQ;
      keep.column = "id";
      keep.value = Value::ofInt(1500);
      CHECK(count_rows(db2, "big", {keep}) == 1, "row above the delete range survived");

      // A pk equality UPDATE/DELETE must be one random read, not a 2M row scan.
      double u0 = now_ms();
      std::vector<std::pair<std::string, Value>> one_set;
      one_set.emplace_back("emb", Value::ofVec(std::vector<float>(kBigDim, 0.25f)));
      CHECK(db2.update_rows("big", {cond_eq("id", Value::ofInt(1234567))}, one_set, &err, &code) == 1,
            "single-pk update affected one row");
      CHECK(db2.delete_rows("big", {cond_eq("id", Value::ofInt(1234568))}, &err, &code) == 1,
            "single-pk delete affected one row");
      CHECK(db2.update_rows("big", {cond_eq("id", Value::ofInt(-1))}, one_set, &err, &code) == 0,
            "single-pk update on a missing key affects 0");
      double u1 = now_ms();
      std::printf("     single-pk update+delete: %.2f ms\n", u1 - u0);
      CHECK(u1 - u0 < 50, "single-pk update+delete on 2M rows stays under 50 ms");
      CHECK(db2.table("big")->live_count() == (size_t)(N - 1001), "live_count after the single delete");
    }

    // ------------------------------------------------------- kill -9 crash --
    std::printf("[large: kill -9 with a big table]\n");
    {
      const std::string cdir = dir + "/largecrash";
      const int M = 300000;
      pid_t pid = ::fork();
      if (pid == 0) {
        Database d;
        std::string e;
        int c = 0;
        if (!d.open(cdir, &e)) ::_exit(2);
        if (!d.create_table("big", big_schema(), &e, &c)) ::_exit(3);
        for (int base = 0; base < M; base += 100) {
          if (!d.begin(&e, &c)) ::_exit(4);
          for (int i = base; i < base + 100; ++i)
            if (!ins_big(d, "big", i)) ::_exit(5);
          if (!d.commit(&e, &c)) ::_exit(6);
        }
        if (!d.begin(&e, &c)) ::_exit(7);
        for (int i = M; i < M + 50; ++i) ins_big(d, "big", i);  // uncommitted
        ::kill(::getpid(), SIGKILL);                            // no destructor, no flush
        ::_exit(8);
      }
      int st = 0;
      ::waitpid(pid, &st, 0);
      CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "writer died from SIGKILL");

      double c0 = now_ms();
      Database d;
      CHECK(d.open(cdir, &err), "reopen after kill -9 (recovery succeeds)");
      double c1 = now_ms();
      std::printf("     recovery: %.2f s\n", (c1 - c0) / 1000.0);
      CHECK(d.table("big")->live_count() == (size_t)M, "every committed row recovered");
      size_t got = 0;
      d.scan("big", {}, [&](const Row&) { ++got; return true; });
      CHECK(got == (size_t)M, "scan after recovery returns exactly the committed rows");
      Cond extra;
      extra.op = Cond::EQ;
      extra.column = "id";
      extra.value = Value::ofInt(M + 10);
      CHECK(count_rows(d, "big", {extra}) == 0, "row from the in-flight transaction is gone");
      bool spot = false;
      Cond spot_c;
      spot_c.op = Cond::EQ;
      spot_c.column = "id";
      spot_c.value = Value::ofInt(M - 1);
      d.scan("big", {spot_c}, [&](const Row& r) {
        std::vector<float> exp;
        fill_vec(exp, M - 1);
        spot = (r.vals[1].v == exp);
        return false;
      });
      CHECK(spot, "recovered row payload is byte exact");
      // and writes keep working after recovery
      CHECK(ins_big(d, "big", M + 1000), "insert after recovery");
      CHECK(count_rows(d, "big") == (size_t)M + 1, "post-recovery insert is visible");
    }
  } else {
    std::printf("[large] skipped (MVDB_TEST_BIG=0)\n");
  }

  std::printf("\n%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::printf("(cleanup failed, dir kept: %s)\n", dir.c_str());
  return g_fail == 0 ? 0 : 1;
}

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

#include <sys/stat.h>
#include <sys/types.h>
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
      // The competition explicitly excludes primary-key updates.
      std::vector<std::pair<std::string, Value>> sets2;
      sets2.emplace_back("id", Value::ofInt(999));
      CHECK(db.update_rows("t1", {cond_eq("id", Value::ofInt(2))}, sets2, &err, &code) == -1 && code == 1235,
            "primary-key update rejected");
      CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(2))}) == 1, "original pk retained");
      CHECK(count_rows(db, "t1", {cond_eq("id", Value::ofInt(999))}) == 0, "no new pk created");
      std::vector<std::pair<std::string, Value>> sets3;
      sets3.emplace_back("id", Value::ofInt(999));
      CHECK(db.update_rows("t1", {cond_eq("id", Value::ofInt(300))}, sets3, &err, &code) == -1 &&
                code == 1235,
            "all primary-key updates rejected");
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

#ifdef MVDB_SMALL_CKPT
  // Built with -DMVDB_AUTO_CHECKPOINT_BYTES=<small>: autocommit DML must fold
  // the WAL into a snapshot by itself (nothing else calls checkpoint()).
  std::printf("[auto checkpoint]\n");
  {
    const std::string adir = dir + "/autockpt";
    Database d;
    CHECK(d.open(adir, &err), "open auto-checkpoint dir");
    CHECK(d.create_table("a", simple_schema(), &err, &code), "create table a");
    for (int i = 0; i < 400; ++i)
      if (!ins(d, "a", i, "x", {1.f, 2.f, 3.f})) break;
    CHECK(file_size(adir + "/snapshot.bin") > 0, "snapshot written without an explicit checkpoint");
    CHECK(file_size(adir + "/data.log") < 400 * 60,
          "WAL stays bounded instead of growing with every row");
    CHECK(count_rows(d, "a") == 400, "all rows still visible");
    d.close();
    Database d2;
    CHECK(d2.open(adir, &err), "reopen auto-checkpointed dir");
    CHECK(count_rows(d2, "a") == 400, "every row survived the automatic checkpoint");
  }
#endif

  // PK equality DML must still check every other predicate, including on the
  // spilled path, and its undo records must restore the original row slot.
  {
    Database d; const std::string pdir=dir+"/pk_dml";
    CHECK(d.open(pdir,&err),"open PK DML fixture");
    CHECK(d.create_table("t1",simple_schema(),&err,&code),"create PK DML fixture");
    CHECK(ins(d,"t1",-7,"old",{1,2,3}),"insert negative PK fixture");
    CHECK(ins(d,"t1",8,"other",{4,5,6}),"insert second PK fixture");
    const std::vector<std::pair<std::string,Value>> sets{{"name",Value::ofStr("new")}};
    const Cond key=cond_eq("id",Value::ofInt(-7));
    const Cond wrong=cond_eq("name",Value::ofStr("wrong"));
    CHECK(d.update_rows("t1",{key,wrong},sets,&err,&code)==0,"PK UPDATE checks residual predicate");
    CHECK(d.delete_rows("t1",{key,wrong},&err,&code)==0,"PK DELETE checks residual predicate");
    CHECK(d.update_rows("t1",{cond_eq("id",Value::ofInt(999))},sets,&err,&code)==0,"PK UPDATE miss");
    CHECK(d.delete_rows("t1",{cond_eq("id",Value::ofInt(999))},&err,&code)==0,"PK DELETE miss");
    CHECK(d.begin(&err,&code),"begin PK DML rollback");
    CHECK(d.update_rows("t1",{key},sets,&err,&code)==1,"PK UPDATE selects exactly one row");
    CHECK(find_name(d,"t1",-7)=="new" && find_name(d,"t1",8)=="other","PK UPDATE preserves other row");
    CHECK(d.delete_rows("t1",{key},&err,&code)==1,"PK DELETE selects exactly one row");
    CHECK(count_rows(d,"t1")==1,"one row remains after PK DELETE");
    CHECK(d.rollback(&err,&code),"rollback PK DML");
    CHECK(find_name(d,"t1",-7)=="old" && count_rows(d,"t1")==2,"PK DML undo restores original row");
    CHECK(d.delete_rows("t1",{key,cond_eq("id",Value::ofInt(8))},&err,&code)==0,"contradictory PK equalities match no row");
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


  std::printf("\n%s: %d passed, %d failed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_fail);
  std::string cleanup = "rm -rf '" + dir + "'";
  if (std::system(cleanup.c_str()) != 0) std::printf("(cleanup failed, dir kept: %s)\n", dir.c_str());
  return g_fail == 0 ? 0 : 1;
}

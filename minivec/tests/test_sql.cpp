// tests/test_sql.cpp — standalone harness for mvdb::parse_sql.
// Build:
//   g++ -std=c++17 -O2 -Isrc tests/test_sql.cpp src/sql.cpp src/value.cpp -o /tmp/test_sql
// (drop src/value.cpp if that TU is not present yet; the parser does not use it)
#include <cstdio>
#include <string>
#include <vector>

#include "sql.h"

using namespace mvdb;

static int g_pass = 0;
static int g_fail = 0;

// Bounds-safe accessor: a failed parse leaves the AST empty, and the checks
// must report that instead of walking off the end of a vector.
template <class T>
static const T& AT(const std::vector<T>& v, size_t i) {
  static const T dflt{};
  return i < v.size() ? v[i] : dflt;
}

#define CHECK(cond)                                                  \
  do {                                                               \
    if (cond) {                                                      \
      ++g_pass;                                                      \
    } else {                                                         \
      ++g_fail;                                                      \
      std::printf("   FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                \
  } while (0)

static const char* kind_name(Statement::Kind k) {
  switch (k) {
    case Statement::EMPTY: return "EMPTY";
    case Statement::CREATE_TABLE: return "CREATE_TABLE";
    case Statement::DROP_TABLE: return "DROP_TABLE";
    case Statement::INSERT: return "INSERT";
    case Statement::SELECT: return "SELECT";
    case Statement::UPDATE: return "UPDATE";
    case Statement::DELETE_ROWS: return "DELETE_ROWS";
    case Statement::BEGIN_TXN: return "BEGIN_TXN";
    case Statement::COMMIT_TXN: return "COMMIT_TXN";
    case Statement::ROLLBACK_TXN: return "ROLLBACK_TXN";
    case Statement::CREATE_INDEX: return "CREATE_INDEX";
    case Statement::SHOW_TABLES: return "SHOW_TABLES";
    case Statement::UNKNOWN: return "UNKNOWN";
  }
  return "?";
}

static const char* type_name(Type t) {
  switch (t) {
    case Type::BIGINT: return "BIGINT";
    case Type::VARCHAR: return "VARCHAR";
    case Type::VECTOR: return "VECTOR";
  }
  return "?";
}

static const char* op_name(Cond::Op op) {
  switch (op) {
    case Cond::EQ: return "=";
    case Cond::NE: return "<>";
    case Cond::LT: return "<";
    case Cond::LE: return "<=";
    case Cond::GT: return ">";
    case Cond::GE: return ">=";
  }
  return "?";
}

static std::string val_str(const Value& v) {
  char buf[64];
  switch (v.type) {
    case Type::BIGINT:
      std::snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
      return buf;
    case Type::VARCHAR:
      return "\"" + v.s + "\"";
    case Type::VECTOR: {
      std::string s = "VEC[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) s += ",";
        std::snprintf(buf, sizeof(buf), "%g", (double)v.v[i]);
        s += buf;
      }
      return s + "]";
    }
  }
  return "?";
}

static void dump(const Statement& st) {
  std::printf("   kind=%s raw=\"%s\"\n", kind_name(st.kind), st.raw.c_str());
  if (!st.table.empty())
    std::printf("   table=%s if_not_exists=\"%s\"\n", st.table.c_str(), st.if_not_exists.c_str());
  if (!st.columns.empty()) {
    std::printf("   columns(%zu):", st.columns.size());
    for (const auto& c : st.columns)
      std::printf(" %s:%s(%d)%s", c.name.c_str(), type_name(c.type), (int)c.len,
                  c.is_primary ? " PK" : "");
    std::printf("\n");
  }
  if (!st.rows.empty()) {
    std::printf("   rows(%zu):\n", st.rows.size());
    for (const auto& r : st.rows) {
      std::printf("     (");
      for (size_t i = 0; i < r.size(); ++i)
        std::printf("%s%s", i ? ", " : "", val_str(r[i]).c_str());
      std::printf(")\n");
    }
  }
  if (!st.items.empty()) {
    std::printf("   items(%zu):", st.items.size());
    for (const auto& it : st.items) {
      if (it.is_distance) {
        std::printf(" L2_DISTANCE(%s, ", it.column.c_str());
        for (size_t i = 0; i < it.query.size(); ++i)
          std::printf("%s%g", i ? "," : "", (double)it.query[i]);
        std::printf(")");
      } else {
        std::printf(" %s", it.column.c_str());
      }
      if (!it.alias.empty()) std::printf(" AS %s", it.alias.c_str());
      std::printf(";");
    }
    std::printf("\n");
  }
  if (!st.conds.empty()) {
    std::printf("   conds(%zu):", st.conds.size());
    for (const auto& c : st.conds)
      std::printf(" %s %s %s;", c.column.c_str(), op_name(c.op), val_str(c.value).c_str());
    std::printf("\n");
  }
  if (!st.order_by.empty())
    std::printf("   order_by=%s desc=%d\n", st.order_by.c_str(), (int)st.order_desc);
  if (st.limit >= 0) std::printf("   limit=%lld\n", (long long)st.limit);
  if (!st.sets.empty()) {
    std::printf("   sets(%zu):", st.sets.size());
    for (const auto& s : st.sets) std::printf(" %s=%s;", s.first.c_str(), val_str(s.second).c_str());
    std::printf("\n");
  }
  if (!st.index_name.empty() || !st.index_column.empty())
    std::printf("   index=%s on column=%s\n", st.index_name.c_str(), st.index_column.c_str());
}

// Parses expecting success, dumps the AST, returns it.
static Statement expect_ok(const std::string& sql) {
  Statement st;
  std::string err;
  int code = 0;
  std::printf("\n-- OK   %s\n", sql.c_str());
  if (!parse_sql(sql, &st, &err, &code)) {
    std::printf("   UNEXPECTED FAILURE: ERROR %d: %s\n", code, err.c_str());
    ++g_fail;
    return Statement();
  }
  ++g_pass;
  dump(st);
  return st;
}

// Parses expecting failure with `want_code`.
static void expect_err(const std::string& sql, int want_code) {
  Statement st;
  std::string err;
  int code = 0;
  std::printf("\n-- ERR  %s\n", sql.c_str());
  if (parse_sql(sql, &st, &err, &code)) {
    std::printf("   ACCEPTED but should have failed (kind=%s)\n", kind_name(st.kind));
    ++g_fail;
    return;
  }
  std::printf("   ERROR %d: %s\n", code, err.c_str());
  if (code != want_code) {
    std::printf("   FAIL wrong code: want %d got %d\n", want_code, code);
    ++g_fail;
  } else {
    ++g_pass;
  }
  if (err.empty()) {
    std::printf("   FAIL empty error message\n");
    ++g_fail;
  } else {
    ++g_pass;
  }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep partial output on any crash

  // ---------------------------------------------------------------- CREATE --
  {
    Statement st = expect_ok(
        "CREATE TABLE IF NOT EXISTS docs (id BIGINT PRIMARY KEY, name VARCHAR(20), "
        "emb VECTOR(4))");
    CHECK(st.kind == Statement::CREATE_TABLE);
    CHECK(st.table == "docs");
    CHECK(st.if_not_exists == "IF NOT EXISTS");
    CHECK(st.columns.size() == 3);
    CHECK(AT(st.columns, 0).name == "id" && AT(st.columns, 0).type == Type::BIGINT &&
          AT(st.columns, 0).is_primary);
    CHECK(AT(st.columns, 1).type == Type::VARCHAR && AT(st.columns, 1).len == 20);
    CHECK(AT(st.columns, 2).type == Type::VECTOR && AT(st.columns, 2).len == 4);
    CHECK(st.raw ==
          "CREATE TABLE IF NOT EXISTS docs (id BIGINT PRIMARY KEY, name VARCHAR(20), emb VECTOR(4))");
  }
  {
    Statement st = expect_ok("create table t (id int primary key, v vector( 3 ), s varchar(8))");
    CHECK(st.kind == Statement::CREATE_TABLE && st.table == "t");
    CHECK(st.columns.size() == 3);
    CHECK(AT(st.columns, 0).type == Type::BIGINT);  // INT alias
    CHECK(AT(st.columns, 1).type == Type::VECTOR && AT(st.columns, 1).len == 3);
    CHECK(st.if_not_exists.empty());
  }
  {
    Statement st = expect_ok("CREATE TABLE `weird name` (`select` BIGINT PRIMARY KEY, `v` VECTOR(2))");
    CHECK(st.table == "weird name");
    CHECK(AT(st.columns, 0).name == "select");
    CHECK(AT(st.columns, 1).name == "v");
  }
  expect_ok("CREATE TABLE t (id BIGINT PRIMARY KEY NOT NULL, v VECTOR(8))");
  expect_err("CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(8) NULL)", 1235);
  expect_err("CREATE TABLE t3 (id BIGINT AUTO_INCREMENT PRIMARY KEY, v VECTOR(2))", 1235);

  // ------------------------------------------------------------------ DROP --
  {
    Statement st = expect_ok("DROP TABLE IF EXISTS docs");
    CHECK(st.kind == Statement::DROP_TABLE && st.table == "docs" && st.if_not_exists == "IF EXISTS");
  }
  {
    Statement st = expect_ok("drop table docs");
    CHECK(st.kind == Statement::DROP_TABLE && st.table == "docs" && st.if_not_exists.empty());
  }

  // ---------------------------------------------------------------- INSERT --
  {
    Statement st = expect_ok("INSERT INTO docs VALUES (1, 'alpha', '[0.1,0.2,0.3,0.4]')");
    CHECK(st.kind == Statement::INSERT && st.table == "docs");
    CHECK(st.rows.size() == 1 && AT(st.rows, 0).size() == 3);
    CHECK(AT(AT(st.rows, 0), 0).type == Type::BIGINT && AT(AT(st.rows, 0), 0).i == 1);
    CHECK(AT(AT(st.rows, 0), 1).type == Type::VARCHAR && AT(AT(st.rows, 0), 1).s == "alpha");
    CHECK(AT(AT(st.rows, 0), 2).type == Type::VECTOR && AT(AT(st.rows, 0), 2).v.size() == 4);
    CHECK(AT(AT(st.rows, 0), 2).v.size() == 4 && AT(AT(st.rows, 0), 2).v[3] > 0.399f &&
          AT(AT(st.rows, 0), 2).v[3] < 0.401f);
  }
  {
    Statement st = expect_ok(
        "insert into docs (name, id, emb) values ('it''s', -7, [1, 2.5, -3.5, 0]), "
        "('a\\'b', 8, '[0,0,0,0]'), ('tab\\there', 9, [1,1,1,1])");
    CHECK(st.kind == Statement::INSERT);
    CHECK(st.columns.size() == 3 && AT(st.columns, 0).name == "name" &&
          AT(st.columns, 2).name == "emb");
    CHECK(st.rows.size() == 3);
    CHECK(AT(AT(st.rows, 0), 1).i == -7);
    CHECK(AT(AT(st.rows, 0), 0).s == "it's");
    CHECK(AT(AT(st.rows, 1), 0).s == "a'b");
    CHECK(AT(AT(st.rows, 2), 0).s == "tab\there");
    CHECK(AT(AT(st.rows, 0), 2).type == Type::VECTOR && AT(AT(st.rows, 0), 2).v.size() == 4);
    CHECK(AT(AT(st.rows, 0), 2).v.size() == 4 && AT(AT(st.rows, 0), 2).v[2] < -3.49f &&
          AT(AT(st.rows, 0), 2).v[2] > -3.51f);
  }
  {
    Statement st = expect_ok("INSERT INTO t VALUES (-9223372036854775808, 9223372036854775807)");
    CHECK(AT(AT(st.rows, 0), 0).i == (-9223372036854775807LL - 1));
    CHECK(AT(AT(st.rows, 0), 1).i == 9223372036854775807LL);
  }
  expect_ok("INSERT INTO t VALUE (1)");
  expect_ok("INSERT t VALUES (1)");

  // ---------------------------------------------------------------- SELECT --
  {
    Statement st = expect_ok("SELECT * FROM docs");
    CHECK(st.kind == Statement::SELECT && st.table == "docs" && st.items.empty());
    CHECK(st.limit == -1 && st.order_by.empty() && st.conds.empty());
  }
  {
    Statement st = expect_ok(
        "select id, name, l2_distance(emb, '[1,2,3,4]') as d from docs "
        "where id >= 2 and name != 'x' and id < 100 order by d desc limit 5");
    CHECK(st.kind == Statement::SELECT && st.table == "docs");
    CHECK(st.items.size() == 3);
    CHECK(AT(st.items, 0).column == "id" && !AT(st.items, 0).is_distance);
    CHECK(AT(st.items, 2).is_distance && AT(st.items, 2).column == "emb" &&
          AT(st.items, 2).alias == "d");
    CHECK(AT(st.items, 2).query.size() == 4 && AT(AT(st.items, 2).query, 0) == 1.0f &&
          AT(AT(st.items, 2).query, 3) == 4.0f);
    CHECK(st.conds.size() == 3);
    CHECK(AT(st.conds, 0).column == "id" && AT(st.conds, 0).op == Cond::GE &&
          AT(st.conds, 0).value.i == 2);
    CHECK(AT(st.conds, 1).column == "name" && AT(st.conds, 1).op == Cond::NE &&
          AT(st.conds, 1).value.s == "x");
    CHECK(AT(st.conds, 2).op == Cond::LT && AT(st.conds, 2).value.i == 100);
    CHECK(st.order_by == "d" && st.order_desc);
    CHECK(st.limit == 5);
  }
  {
    Statement st = expect_ok("SELECT * FROM t WHERE (a = 1 AND b <= 2) AND c <> 'z'");
    CHECK(st.conds.size() == 3);
    CHECK(AT(st.conds, 0).op == Cond::EQ && AT(st.conds, 1).op == Cond::LE &&
          AT(st.conds, 2).op == Cond::NE);
  }
  {
    Statement st = expect_ok("SELECT a, b AS bee FROM t ORDER BY a ASC LIMIT 0");
    CHECK(st.items.size() == 2 && AT(st.items, 1).alias == "bee");
    CHECK(st.order_by == "a" && !st.order_desc && st.limit == 0);
  }
  {
    Statement st = expect_ok("SELECT L2_DISTANCE(emb, '[1, 2, 3, 4]') FROM t WHERE id > -3");
    CHECK(st.items.size() == 1 && AT(st.items, 0).is_distance && AT(st.items, 0).alias.empty());
    CHECK(st.conds.size() == 1 && AT(st.conds, 0).value.i == -3);
  }
  {
    Statement st = expect_ok("SELECT * FROM t WHERE v = [1,2]");  // bare vector in WHERE
    CHECK(st.conds.size() == 1 && AT(st.conds, 0).value.type == Type::VECTOR &&
          AT(st.conds, 0).value.v.size() == 2);
  }

  // ---------------------------------------------------------------- UPDATE --
  {
    Statement st = expect_ok(
        "update docs set name = 'beta', emb = '[1,2,3,4]', id = 42 where id = 1 and name = 'alpha'");
    CHECK(st.kind == Statement::UPDATE && st.table == "docs");
    CHECK(st.sets.size() == 3);
    CHECK(AT(st.sets, 0).first == "name" && AT(st.sets, 0).second.s == "beta");
    CHECK(AT(st.sets, 1).first == "emb" && AT(st.sets, 1).second.type == Type::VECTOR &&
          AT(st.sets, 1).second.v.size() == 4);
    CHECK(AT(st.sets, 2).first == "id" && AT(st.sets, 2).second.i == 42);
    CHECK(st.conds.size() == 2);
  }

  // ---------------------------------------------------------------- DELETE --
  {
    Statement st = expect_ok("DELETE FROM docs WHERE id = 3");
    CHECK(st.kind == Statement::DELETE_ROWS && st.table == "docs" && st.conds.size() == 1);
  }
  {
    Statement st = expect_ok("delete from docs");
    CHECK(st.kind == Statement::DELETE_ROWS && st.table == "docs" && st.conds.empty());
  }

  // --------------------------------------------------------- INDEX / SHOW ---
  {
    Statement st = expect_ok("CREATE VECTOR INDEX idx_emb ON docs (emb)");
    CHECK(st.kind == Statement::CREATE_INDEX);
    CHECK(st.index_name == "idx_emb" && st.table == "docs" && st.index_column == "emb");
  }
  {
    Statement st = expect_ok("SHOW TABLES");
    CHECK(st.kind == Statement::SHOW_TABLES && st.table.empty());
  }

  // ----------------------------------------------------------- TRANSACTIONS -
  CHECK(expect_ok("BEGIN").kind == Statement::BEGIN_TXN);
  CHECK(expect_ok("START TRANSACTION").kind == Statement::BEGIN_TXN);
  CHECK(expect_ok("begin work").kind == Statement::BEGIN_TXN);
  CHECK(expect_ok("COMMIT").kind == Statement::COMMIT_TXN);
  CHECK(expect_ok("commit work").kind == Statement::COMMIT_TXN);
  CHECK(expect_ok("ROLLBACK").kind == Statement::ROLLBACK_TXN);

  // --------------------------------------------------------- empty / blank --
  {
    Statement st;
    std::string err;
    int code = 0;
    std::printf("\n-- OK   <empty>\n");
    bool ok = parse_sql("   ", &st, &err, &code);
    CHECK(ok && st.kind == Statement::EMPTY);
    std::printf("   kind=%s\n", kind_name(st.kind));
    std::printf("-- OK   <comment only>\n");
    ok = parse_sql("/* hi */ -- bye", &st, &err, &code);
    CHECK(ok && st.kind == Statement::EMPTY);
  }

  // ----------------------------------------------------------------- ERRORS -
  expect_err("INSERT INTO t VALUES (1, 'unterminated)", 1064);         // unclosed quote
  expect_err("SELECT * FROM t WHERE name = 'abc", 1064);               // unclosed quote
  expect_err("CREATE TABLE t (id BIGINT PRIMARY KEY", 1064);           // missing ')'
  expect_err("INSERT INTO t VALUES (1, 2", 1064);                      // missing ')'
  expect_err("CREATE TABLE t (id BLOB)", 1064);                        // bad type
  expect_err("CREATE TABLE t (id VARCHAR)", 1064);                     // VARCHAR without length
  expect_err("CREATE TABLE t (v VECTOR(0))", 1064);                    // bad dimension
  expect_err("SELECT * FROM t WHERE a = 1 OR b = 2", 1235);            // OR
  expect_err("SELECT * FROM a JOIN b ON a.id = b.id", 1235);           // JOIN
  expect_err("SELECT a FROM t GROUP BY a", 1235);                      // GROUP BY
  expect_err("SELECT COUNT(*) FROM t", 1235);                          // aggregate
  expect_err("SELECT * FROM (SELECT * FROM t) x", 1235);               // subquery
  expect_err("SELECT * FROM t WHERE id IN (SELECT id FROM u)", 1235);  // IN / subquery
  expect_err("ALTER TABLE t ADD COLUMN x BIGINT", 1235);               // ALTER
  expect_err("INSERT INTO t VALUES (NULL)", 1064);                     // NULL
  expect_err("UPDATE t SET a = NULL", 1064);                           // NULL
  expect_err("INSERT INTO t VALUES (1.5)", 1064);                      // float literal
  expect_err("SELECT * FROM t LIMIT 10 OFFSET 5", 1235);               // OFFSET
  expect_err("SAVEPOINT s1", 1235);                                    // nested txn
  expect_err("ROLLBACK TO SAVEPOINT s1", 1235);                        // nested txn
  expect_err("CREATE INDEX i ON t (emb)", 1235);                       // non-vector index
  expect_err("SELECT * FROM t ORDER BY a, b", 1235);                   // multi-key order
  expect_err("UPDATE t SET a = b", 1235);                              // expression in SET
  expect_err("SELECT * FROM t WHERE a = 1 AND b = 2 AND", 1064);       // dangling AND
  expect_err("SELECT * FROM t garbage junk", 1064);                    // trailing token
  expect_err("gibberish statement here", 1235);                        // unknown verb
  expect_err("INSERT INTO t VALUES ('[1,2,x]', )", 1064);              // empty value
  expect_err("SELECT * FROM t WHERE v = [1,2", 1064);                  // bad bare vector
  expect_err("CREATE TABLE t ()", 1064);                               // no columns
  expect_err("CREATE TABLE t (a BIGINT,)", 1064);                      // trailing comma
  expect_err("CREATE TABLE t (a BIGINT PRIMARY KEY, b BIGINT PRIMARY KEY)", 1064);  // 2 PKs
  expect_err("SELECT * FROM t WHERE a = 1 AND (b = 2", 1064);          // unbalanced paren
  expect_err("SELECT * FROM t WHERE a IS NULL", 1235);                 // IS NULL
  expect_err("SELECT * FROM t WHERE a LIKE 'x%'", 1235);               // LIKE
  expect_err("SELECT * FROM t WHERE a BETWEEN 1 AND 2", 1235);         // BETWEEN
  expect_err("INSERT INTO t (a, b) VALUES (1)", 1064);                 // arity mismatch
  expect_err("DELETE t WHERE a = 1", 1064);                            // DELETE without FROM
  expect_err("ROLLBACK AND CHAIN", 1235);                              // chained txn
  expect_err("TRUNCATE TABLE t", 1235);                                // unsupported stmt

  // Table-level single-column PK and bare SELECT aliases retain strict errors.
  {
    Statement st; std::string error; int code=0;
    CHECK(parse_sql("CREATE TABLE t (id BIGINT, v VECTOR(2), PRIMARY KEY (id))",&st,&error,&code));
    CHECK(st.columns.size()==2 && AT(st.columns,0).is_primary && !AT(st.columns,1).is_primary);
    CHECK(parse_sql("CREATE TABLE t (PRIMARY KEY (`key`), `key` BIGINT)",&st,&error,&code));
    CHECK(st.columns.size()==1 && AT(st.columns,0).is_primary);
    CHECK(parse_sql("SELECT id ident, L2_DISTANCE(v,'[0,0]') dist FROM t ORDER BY dist DESC LIMIT 3",&st,&error,&code));
    CHECK(st.items.size()==2 && AT(st.items,0).alias=="ident" && AT(st.items,1).alias=="dist");
    CHECK(st.order_by=="dist" && st.order_desc && st.limit==3);
    CHECK(parse_sql("SELECT id `order` FROM t ORDER BY `order` ASC",&st,&error,&code));
    CHECK(AT(st.items,0).alias=="order" && !st.order_desc);
  }
  expect_err("CREATE TABLE t (id BIGINT, PRIMARY KEY (missing))",1072);
  expect_err("CREATE TABLE t (id BIGINT, PRIMARY KEY (id), PRIMARY KEY (id))",1064);
  expect_err("CREATE TABLE t (id BIGINT PRIMARY KEY, PRIMARY KEY (id))",1064);
  expect_err("CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT, PRIMARY KEY (n))",1064);
  expect_err("CREATE TABLE t (id BIGINT, n BIGINT, PRIMARY KEY (id,n))",1235);
  expect_err("CREATE TABLE t (id BIGINT, PRIMARY KEY ())",1064);
  expect_err("SELECT id first second FROM t",1064);
  expect_err("SELECT id FROM t ORDER BY id bogus",1064);
  expect_err("SELECT id FROM t ORDER BY id AS bogus",1064);

  // ------------------------------------------------------------ no-crash ----
  {
    const char* nasty[] = {
        "",         ";",           "   ;;",                 "'",          "\"",
        "`",        "[",           "[1,2",                  "]",          "(",
        ")",        ",",           "=",                     "SELECT",     "SELECT *",
        "SELECT * FROM",           "INSERT",                "INSERT INTO",
        "INSERT INTO t VALUES",    "INSERT INTO t VALUES (", "UPDATE",   "UPDATE t",
        "UPDATE t SET",            "DELETE",                "DELETE FROM", "CREATE",
        "CREATE TABLE",            "CREATE TABLE t",        "CREATE TABLE t (",
        "CREATE VECTOR",           "CREATE VECTOR INDEX",   "DROP",       "SHOW",
        "BEGIN BEGIN",             "COMMIT COMMIT",         "SELECT L2_DISTANCE(",
        "SELECT L2_DISTANCE(emb",  "SELECT L2_DISTANCE(emb,'[1,2'",       "?????",
        "\x01\x02\xff",            "SELECT * FROM t WHERE", "SELECT * FROM t LIMIT",
        "SELECT * FROM t ORDER BY", "INSERT INTO t (a,) VALUES (1)",      "/* unterminated",
        "SELECT * FROM t WHERE ((a = 1)",                  "INSERT INTO t VALUES ()",
        "CREATE TABLE t (a VECTOR(-1))",                   "CREATE TABLE t (a VARCHAR(0))",
        "SELECT * FROM t WHERE a = 9999999999999999999999",
    };
    int bad = 0;
    const size_t n = sizeof(nasty) / sizeof(nasty[0]);
    for (size_t i = 0; i < n; ++i) {
      Statement st;
      std::string err;
      int code = 0;
      if (parse_sql(nasty[i], &st, &err, &code)) {
        if (st.kind != Statement::EMPTY) {  // "BEGIN BEGIN" etc. must not pass
          std::printf("   FAIL silently accepted: \"%s\" as %s\n", nasty[i], kind_name(st.kind));
          ++bad;
          ++g_fail;
        }
      } else if (err.empty() || code == 0) {
        std::printf("   FAIL error without code/message: \"%s\"\n", nasty[i]);
        ++bad;
        ++g_fail;
      }
    }
    std::printf("\n-- no-crash sweep: %zu inputs, %d unexpected\n", n, bad);
    if (bad == 0) ++g_pass;
    else ++g_fail;
  }

  std::printf("\n==================== %d checks passed, %d failed ====================\n", g_pass,
              g_fail);
  return g_fail == 0 ? 0 : 1;
}

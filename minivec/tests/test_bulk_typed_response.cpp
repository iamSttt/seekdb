// Link db.cpp exec.cpp sql.cpp value.cpp flat_index.cpp with the retained four
// feature flags and MVDB_JSON_RESPONSE_DIAGNOSTICS. Run both without and with
// MVDB_TYPED_BULK_VECTOR_ONLY, including an ASan/UBSan candidate build.
#include "db.h"
#include "exec.h"
#include "json_response_diagnostics.h"
#include "sql.h"
#include "../third_party/json.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>
#include <unistd.h>

#if !defined(MVDB_JSON_RESPONSE_DIAGNOSTICS) || !defined(MVDB_ENABLE_TYPED_JSON) || !defined(MVDB_TYPED_WIDE_VECTOR_ONLY)
#error This test needs encoder diagnostics and the retained typed-wide guard.
#endif
#ifdef NDEBUG
#error Assertions are required for this test.
#endif

using namespace mvdb;
using Json = nlohmann::json;
#ifdef MVDB_TYPED_BULK_VECTOR_ONLY
static constexpr bool bulk_enabled = true;
#else
static constexpr bool bulk_enabled = false;
#endif

static std::vector<int64_t> ids(size_t count) {
  std::vector<int64_t> result(count);
  std::iota(result.begin(), result.end(), int64_t(0));
  return result;
}
static std::vector<float> vector_value(int64_t id, int dimension) {
  std::vector<float> value(static_cast<size_t>(dimension));
  value[0] = -0.0f;
  for (int j = 1; j < dimension; ++j)
    value[static_cast<size_t>(j)] = float((id + j) % 17 - 8) / 8.0f;
  return value;
}
static std::string text_value(int64_t id) {
  return "row" + std::to_string(id) + "\"\\\n\t\xe4\xb8\xad\xf0\x9f\x98\x80";
}
static std::string zero_vector(int dimension) {
  std::string value = "[";
  for (int j = 0; j < dimension; ++j) { if (j) value += ','; value += '0'; }
  return value + ']';
}

struct Fixture {
  Database db;
  std::string error, out;
  int code = 0;
  JsonResponseDiagnostics counts;
  explicit Fixture(const char* path) { assert(db.open(path, &error)); }
  bool call(const std::string& sql) {
    Statement statement;
    assert(parse_sql(sql, &statement, &error, &code, false));
    counts = {};
    struct CounterScope {
      JsonResponseDiagnostics* previous;
      explicit CounterScope(JsonResponseDiagnostics* current)
          : previous(json_response_diag_current) { json_response_diag_current = current; }
      ~CounterScope() { json_response_diag_current = previous; }
    } scope(&counts);
    return execute_json(db, statement, &out, &error, &code, false);
  }
  void insert(const std::string& name, int64_t id, int dimension) {
    assert(db.insert(name, {Value::ofInt(id), Value::ofInt(INT64_C(9007199254740993) + id),
        Value::ofStr(text_value(id)), Value::ofVec(vector_value(id, dimension))}, &error, &code));
  }
  void create(const std::string& name, int dimension, size_t rows) {
    assert(call("CREATE TABLE " + name + "(id BIGINT PRIMARY KEY,n BIGINT,s VARCHAR(65535),v VECTOR(" +
        std::to_string(dimension) + "));"));
    assert(db.begin(&error, &code));
    for (size_t i = 0; i < rows; ++i) insert(name, static_cast<int64_t>(i), dimension);
    assert(db.commit(&error, &code));
    assert(db.table(name)->pk_pos.size() == rows);
  }
  // Assert metadata was not touched on non-applicable projections. These are
  // actual execution counters; the test does not call a duplicate selector.
  void metadata(size_t limit, size_t conditions, size_t live_keys) const {
    assert(counts.bulk_limit_checks == (bulk_enabled ? limit : 0));
    assert(counts.bulk_condition_checks == (bulk_enabled ? conditions : 0));
    assert(counts.bulk_live_key_checks == (bulk_enabled ? live_keys : 0));
  }
  // The oracle uses fixture values/expected PKs, never the table's read or scan
  // methods. It checks every returned cell, int64 precision and signed zero.
  void check(const std::string& sql, int dimension, const std::vector<int64_t>& expected,
             bool typed, const std::vector<std::string>& columns = {"id", "n", "s", "v"}) {
    assert(call(sql)); assert(code == 0 && error.empty());
    assert(counts.typed_rows == (typed ? expected.size() : 0));
    assert(counts.dom_rows == (typed ? 0 : expected.size()));
    const auto result = Json::parse(out);
    assert(result.at("columns") == Json(columns));
    const auto& rows = result.at("rows"); assert(rows.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      const int64_t key = expected[i]; const auto& row = rows[i];
      assert(row.size() == columns.size());
      for (size_t j = 0; j < columns.size(); ++j) {
        const auto& value = row[j]; const auto& column = columns[j];
        if (column == "id" || column == "n") {
          assert(value.is_number_integer());
          assert(value.get<int64_t>() == (column == "id" ? key : INT64_C(9007199254740993) + key));
        } else if (column == "s") {
          assert(value == text_value(key));
        } else if (column == "v") {
          const auto wanted = vector_value(key, dimension); assert(value.size() == wanted.size());
          for (size_t k = 0; k < wanted.size(); ++k) {
            assert(value[k].is_number()); const double actual = value[k].get<double>();
            assert(actual == static_cast<double>(wanted[k]));
            if (wanted[k] == 0) assert(std::signbit(actual) == std::signbit(wanted[k]));
          }
        } else {
          assert(column == "d"); double squared = 0;
          for (float x : vector_value(key, dimension)) squared += double(x) * double(x);
          assert(std::abs(value.get<double>() - std::sqrt(squared)) <= 1e-12);
        }
      }
    }
  }
};

int main() {
  alarm(180);
  setenv("MVDB_QUERY_THREADS", "1", 1);
  char path[] = "/tmp/mvdb-bulk-response-XXXXXX"; assert(mkdtemp(path));
  {
    Fixture f(path);
    const auto full = ids(4096);
    // Dimension admission is a range, not a special case for the performance
    // corpus: 15 stays DOM, while 16, 64 and 127 exercise the extra rule.
    for (int dimension : {15, 16, 64, 127}) {
      const std::string name = "v" + std::to_string(dimension);
      f.create(name, dimension, 4096);
      const bool typed = dimension == 15 ? false : bulk_enabled;
      f.check("SELECT * FROM " + name + ";", dimension, full, typed);
      f.metadata(dimension == 15 ? 0 : 1, dimension == 15 ? 0 : 1, dimension == 15 ? 0 : 1);
      f.check("SELECT * FROM " + name + " ORDER BY id LIMIT 4096;", dimension, full, typed);
      f.metadata(dimension == 15 ? 0 : 1, dimension == 15 ? 0 : 1, dimension == 15 ? 0 : 1);
      if (dimension != 16) assert(f.call("DROP TABLE " + name + ";"));
    }
    // 4095/4096 is tested separately for output LIMIT and actual live keys.
    f.check("SELECT * FROM v16 LIMIT 4095;", 16, ids(4095), false);
    f.metadata(1, 0, 0);
    f.check("SELECT * FROM v16 ORDER BY id LIMIT 10;", 16, ids(10), false);
    f.metadata(1, 0, 0);
    f.check("SELECT * FROM v16 LIMIT 0;", 16, {}, false);
    f.metadata(1, 0, 0);
    f.check("SELECT * FROM v16 WHERE id>=0 LIMIT 4096;", 16, full, false);
    f.metadata(1, 1, 0);
    f.check("SELECT * FROM v16 WHERE id=0;", 16, {0}, false);
    f.metadata(1, 1, 0);
    f.check("SELECT id,n,s FROM v16 LIMIT 4096;", 16, full, false, {"id", "n", "s"});
    f.metadata(0, 0, 0);
    f.check("SELECT v FROM v16 LIMIT 4096;", 16, full, bulk_enabled, {"v"});
    f.metadata(1, 1, 1);
    const auto distance = "L2_DISTANCE(v," + zero_vector(16) + ")";
    f.check("SELECT id," + distance + " AS d FROM v16 LIMIT 4096;", 16, full, false, {"id", "d"});
    f.metadata(0, 0, 0);
    // An unprojected vector does not qualify, even when used for sorting.
    auto ranked = full;
    auto squared = [](int64_t key) { double sum=0; for(float x:vector_value(key,16)) sum+=double(x)*double(x); return sum; };
    std::sort(ranked.begin(), ranked.end(), [&](int64_t a, int64_t b) {
      const double x=squared(a), y=squared(b); return x != y ? x < y : a < b;
    });
    f.check("SELECT id FROM v16 ORDER BY " + distance + " LIMIT 4096;", 16, ranked, false, {"id"});
    f.metadata(0, 0, 0);
    auto reverse = full; std::reverse(reverse.begin(), reverse.end());
    f.check("SELECT * FROM v16 ORDER BY id DESC LIMIT 4096;", 16, reverse, bulk_enabled);
    f.metadata(1, 1, 1);

    assert(f.call("BEGIN;")); assert(f.call("DELETE FROM v16 WHERE id=4095;"));
    assert(f.db.table("v16")->rows.size() == 4096 && f.db.table("v16")->pk_pos.size() == 4095);
    f.check("SELECT * FROM v16;", 16, ids(4095), false);
    f.metadata(1, 1, 1);
    f.check("SELECT * FROM v16 LIMIT 4096;", 16, ids(4095), false);
    f.metadata(1, 1, 1);
    assert(f.call("ROLLBACK;"));
    f.check("SELECT * FROM v16;", 16, full, bulk_enabled);
    f.metadata(1, 1, 1);
    assert(f.call("DELETE FROM v16 WHERE id=4095;"));
    f.check("SELECT * FROM v16;", 16, ids(4095), false);
    f.metadata(1, 1, 1);
    assert(f.db.begin(&f.error, &f.code)); f.insert("v16", 4095, 16); assert(f.db.commit(&f.error, &f.code));
    assert(f.db.table("v16")->rows.size() == 4097 && f.db.table("v16")->pk_pos.size() == 4096);
    f.check("SELECT * FROM v16 ORDER BY id LIMIT 4096;", 16, full, bulk_enabled);
    f.metadata(1, 1, 1);

    // The previous >=128 rule must survive: small table, WHERE and LIMIT 1
    // still use typed encoding, while scalar/distance-only projections do not.
    f.create("wide", 128, 2);
    f.check("SELECT * FROM wide WHERE id=1 LIMIT 1;", 128, {1}, true);
    f.metadata(0, 0, 0);
    f.check("SELECT * FROM wide;", 128, ids(2), true);
    f.metadata(0, 0, 0);
    f.check("SELECT id FROM wide LIMIT 1;", 128, {0}, false, {"id"});
    f.metadata(0, 0, 0);
    f.check("SELECT L2_DISTANCE(v," + zero_vector(128) + ") AS d FROM wide LIMIT 1;", 128, {0}, false, {"d"});
    f.metadata(0, 0, 0);

    std::string prefix; prefix.reserve(90000);
    for (size_t i = 0; i < 30000; ++i) prefix += "\xe4\xb8\xad";
    assert(f.call("UPDATE v16 SET s='" + prefix + "' WHERE id=0;"));
    const std::string invalid[] = {std::string("\xff",1),std::string("\xc0\x80",2),
        std::string("\xed\xa0\x80",3),std::string("\xe4\xb8",2),std::string("\xf4\x90\x80\x80",4),std::string("\x80",1)};
    for (const auto& bad : invalid) {
      assert(f.call("UPDATE v16 SET s='" + bad + "' WHERE id=1;"));
      for (const char* suffix : {";", " ORDER BY id LIMIT 4096;"}) {
        f.out = "previous response must be cleared";
        assert(!f.call(std::string("SELECT * FROM v16") + suffix));
        assert(f.out.empty() && f.code != 0 && f.error.find("UTF-8") != std::string::npos);
        assert(f.counts.typed_rows == (bulk_enabled ? 2u : 0u));
        assert(f.counts.dom_rows == (bulk_enabled ? 0u : 2u));
        f.metadata(1, 1, 1);
        f.check("SELECT * FROM v16 WHERE id=4095;", 16, {4095}, false);
        f.metadata(1, 1, 0);
      }
    }
  }
  std::filesystem::remove_all(path);
  std::cout << "PASS bulk typed admission boundaries, independent all-cell oracle, tombstones/rollback/reinsert, wide-rule preservation and 12 UTF8 failures; bulk=" << bulk_enabled << '\n';
}

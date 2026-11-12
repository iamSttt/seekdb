// Build-only workload generation and independent JSONL response verification.
// No database headers or implementation code are used by the oracle.
#include "json.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr uint64_t kSeed = 2026111201ULL;  // independent of evaluation seeds

struct Random {
  uint64_t state = kSeed;
  uint64_t next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
  float coordinate() {
    // CLT approximation to a centred Gaussian, on a finite dyadic grid.
    // The non-integer coordinates exercise quantization residuals.
    int value = -24570;
    for (int i = 0; i < 12; ++i) value += static_cast<int>(next() & 4095);
    return static_cast<float>(value) / 4096.0f;
  }
  std::vector<float> vector(size_t dim) {
    std::vector<float> result(dim);
    for (float& value : result) value = coordinate();
    return result;
  }
};
struct Row { int64_t id, group; std::string tag; std::vector<float> vector; };
struct Dataset { std::string table; size_t dim, queries; std::vector<Row> rows; };

static void require(bool ok, const std::string& message) {
  if (!ok) throw std::runtime_error(message);
}
static std::string quoted(const std::string& text) {
  std::string result = "'";
  for (char ch : text) {
    if (ch == '\'' || ch == '\\') result += ch;
    result += ch;
  }
  return result + "'";
}
static std::string vector_sql(const std::vector<float>& values) {
  std::string text = "[";
  char buffer[64];
  for (size_t i = 0; i < values.size(); ++i) {
    if (i) text += ',';
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), values[i],
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    require(written.ec == std::errc{}, "cannot encode training coordinate");
    text.append(buffer, written.ptr);
  }
  return text + ']';
}

class Phase {
 public:
  Phase(const fs::path& root, int phase)
      : input_(root / ("phase-" + std::to_string(phase) + ".jsonl")),
        expected_(root / ("phase-" + std::to_string(phase) + ".expected.jsonl")) {
    require(input_.good() && expected_.good(), "cannot create workload files");
    expected_ << Json{{"type", "ready"}}.dump() << '\n';
  }
  void sql(const std::string& text, Json response) {
    const int64_t id = ++requests_;
    input_ << Json{{"id", id}, {"op", "execute"}, {"search_mode", "exact"}, {"sql", text}}.dump() << '\n';
    response["id"] = id;
    expected_ << response.dump() << '\n';
  }
  void change(const std::string& text, int64_t affected = 0) {
    sql(text, {{"ok", true}, {"affected_rows", affected}});
  }
  void error(const std::string& text) {
    // Internal expectation marker; it is never sent to the database.
    sql(text, {{"ok", false}, {"expect_error", true}});
  }
  void selected(const std::string& text, Json columns, Json rows) {
    sql(text, {{"ok", true}, {"columns", std::move(columns)}, {"rows", std::move(rows)}});
  }
  int64_t finish() {
    const int64_t id = ++requests_;
    input_ << Json{{"id", id}, {"op", "shutdown"}}.dump() << '\n';
    expected_ << Json{{"id", id}, {"ok", true}}.dump() << '\n';
    input_.flush(); expected_.flush();
    require(input_.good() && expected_.good(), "failed to write workload");
    return requests_;
  }
 private:
  int64_t requests_ = 0;
  std::ofstream input_, expected_;
};

static double squared(const std::vector<float>& a, const std::vector<float>& b) {
  // Inputs are n/4096, |n| <= 24570. Even 512 summed differences have
  // numerator < 2^41 over 2^24, so double operations here are exact.
  require(a.size() == b.size(), "oracle dimension mismatch");
  double result = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    result += diff * diff;
  }
  return result;
}
static void query(Phase& phase, const Dataset& data, Random& random, size_t number) {
  const std::vector<float> q = number % 31 == 0 ? data.rows[64].vector : random.vector(data.dim);
  constexpr size_t limits[]{1, 10, 100};
  const size_t k = limits[number % 3];
  const int filter = number % 5;
  const int64_t group = static_cast<int64_t>(number % 7);
  const int64_t lower = static_cast<int64_t>(data.rows.size() / 4);
  const int64_t upper = static_cast<int64_t>(3 * data.rows.size() / 4);
  const bool empty = number % 71 == 70;
  std::string where;
  if (empty) where = " WHERE g=999";
  else if (filter == 1) where = " WHERE g=" + std::to_string(group);
  else if (filter == 2) where = " WHERE id>=" + std::to_string(lower) + " AND id<" + std::to_string(upper);
  else if (filter == 3) where = " WHERE g>=4";
  struct Neighbor { double distance; const Row* row; };
  std::vector<Neighbor> ranked;
  for (const Row& row : data.rows) {
    if (empty || (filter == 1 && row.group != group) ||
        (filter == 2 && !(row.id >= lower && row.id < upper)) ||
        (filter == 3 && row.group < 4)) continue;
    ranked.push_back({squared(row.vector, q), &row});
  }
  std::sort(ranked.begin(), ranked.end(), [](const Neighbor& a, const Neighbor& b) {
    return a.distance < b.distance || (a.distance == b.distance && a.row->id < b.row->id);
  });
  Json rows = Json::array();
  for (size_t i = 0; i < std::min(k, ranked.size()); ++i)
    rows.push_back(Json::array({ranked[i].row->id, ranked[i].row->group}));
  phase.selected("SELECT id,g FROM " + data.table + where + " ORDER BY L2_DISTANCE(v," +
      quoted(vector_sql(q)) + ") LIMIT " + std::to_string(k), {"id", "g"}, std::move(rows));
}
static void project(Phase& phase, const Dataset& data, size_t row_index) {
  const Row& row = data.rows.at(row_index);
  phase.selected("SELECT id,g,s,v FROM " + data.table + " WHERE id=" + std::to_string(row.id),
      {"id", "g", "s", "v"}, Json::array({Json::array({row.id, row.group, row.tag, row.vector})}));
}
static void populate(Phase& phase, Dataset& data, Random& random, size_t count) {
  phase.change("CREATE TABLE " + data.table + "(id BIGINT PRIMARY KEY,g BIGINT,s VARCHAR(64),v VECTOR(" +
      std::to_string(data.dim) + "))");
  data.rows.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    Row row{static_cast<int64_t>(i + 1), static_cast<int64_t>(i % 7),
        "row-" + std::to_string(i + 1), random.vector(data.dim)};
    if (i % 17 == 0) row.tag += "-行-é";
    if (i % 29 == 0) row.tag += "-'slash\\line\n";
    // Identical pairs make ties meaningful; their identity is generated, not
    // looked up from any evaluation fixture or expected database output.
    if (i > 0 && i % 64 == 0) row.vector = data.rows.back().vector;
    data.rows.push_back(std::move(row));
  }
  for (size_t start = 0; start < count; start += 64) {
    std::string sql = "INSERT INTO " + data.table;
    const bool mapped = (start / 64) % 4 == 3;
    if (mapped) sql += "(v,s,g,id)";
    sql += " VALUES ";
    const size_t end = std::min(start + 64, count);
    for (size_t i = start; i < end; ++i) {
      const Row& row = data.rows[i];
      if (i != start) sql += ',';
      const std::string v = quoted(vector_sql(row.vector));
      sql += mapped ? "(" + v + "," + quoted(row.tag) + "," + std::to_string(row.group) + "," + std::to_string(row.id) + ")"
                    : "(" + std::to_string(row.id) + "," + std::to_string(row.group) + "," + quoted(row.tag) + "," + v + ")";
    }
    phase.change(sql, static_cast<int64_t>(end - start));
  }
}
static void mutations(Phase& phase, Dataset& data, Random& random) {
  const Row first = data.rows[0], second = data.rows[1];
  phase.change("BEGIN");
  data.rows[0].vector = random.vector(data.dim);
  phase.change("UPDATE " + data.table + " SET v=" + quoted(vector_sql(data.rows[0].vector)) + " WHERE id=1", 1);
  data.rows[1].group = 9;
  phase.change("UPDATE " + data.table + " SET g=9 WHERE id=2", 1);
  query(phase, data, random, 1); project(phase, data, 0); project(phase, data, 1);
  phase.change("ROLLBACK");
  data.rows[0] = first; data.rows[1] = second;
  project(phase, data, 0); project(phase, data, 1);
  data.rows[3].vector = random.vector(data.dim);
  phase.change("UPDATE " + data.table + " SET v=" + quoted(vector_sql(data.rows[3].vector)) + " WHERE id=4", 1);
  phase.change("BEGIN");
  data.rows[4].tag = "committed-'text\\-行";
  phase.change("UPDATE " + data.table + " SET s=" + quoted(data.rows[4].tag) + " WHERE id=5", 1);
  phase.change("COMMIT");
  phase.error("INSERT INTO " + data.table + " VALUES (1,0,'duplicate'," + quoted(vector_sql(data.rows[0].vector)) + ")");
  phase.error("INSERT INTO " + data.table + " VALUES (999999,0,'wrong dimension','[1,2,3]')");
  phase.selected("SELECT id FROM " + data.table + " WHERE id=999999", {"id"}, Json::array());
}

static void generate(const fs::path& root) {
  fs::create_directories(root);
  Random random;
  std::vector<Dataset> data{{"train16", 16, 100, {}}, {"train128", 128, 500, {}}, {"train512", 512, 100, {}}};
  Json manifest{{"kind", "build_only_general_PGO_training"}, {"seed", kSeed},
      {"generator", "splitmix64 + sum of twelve 12-bit uniforms / 4096"},
      {"oracle", "independent exact double squared distances; distance then primary-key order"},
      {"datasets", Json::array()}, {"phase_requests", Json::array()},
      {"limitations", "Inputs are deterministic; profile counters may vary with scheduling and startup calibration. No evaluation data is read."}};
  {
    Phase phase(root, 0);
    for (Dataset& table : data) {
      const size_t count = table.dim == 128 ? 10000 : 1024;
      populate(phase, table, random, count);
      const size_t unindexed = table.dim == 128 ? 30 : 10;
      for (size_t i = 0; i < unindexed; ++i) query(phase, table, random, i);
      phase.change("CREATE VECTOR INDEX idx_" + table.table + " ON " + table.table + "(v)");
      for (size_t i = unindexed; i < table.queries; ++i) {
        if (i == table.queries / 2) mutations(phase, table, random);
        query(phase, table, random, i);
      }
      project(phase, table, 3); project(phase, table, 4);
      manifest["datasets"].push_back({{"dimensions", table.dim}, {"rows", count},
          {"main_queries", table.queries}, {"unindexed_queries", unindexed}});
    }
    manifest["phase_requests"].push_back(phase.finish());
  }
  {
    Phase phase(root, 1);
    for (const Dataset& table : data) {
      project(phase, table, 0); project(phase, table, 3); project(phase, table, 4);
      for (size_t i = 0; i < 9; ++i) query(phase, table, random, i);
    }
    // Shutdown with an open transaction must roll back these changes.
    phase.change("BEGIN");
    phase.change("UPDATE train128 SET g=123 WHERE id=1", 1);
    phase.selected("SELECT g FROM train128 WHERE id=1", {"g"}, Json::array({Json::array({123})}));
    manifest["phase_requests"].push_back(phase.finish());
  }
  {
    Phase phase(root, 2);
    for (const Dataset& table : data) {
      project(phase, table, 0);
      for (size_t i = 0; i < 3; ++i) query(phase, table, random, i);
    }
    manifest["phase_requests"].push_back(phase.finish());
  }
  std::ofstream out(root / "manifest.json"); out << manifest.dump(2) << '\n';
  require(out.good(), "cannot write training manifest");
  std::cout << manifest.dump() << '\n';
}

static void verify(const fs::path& expected_path, const fs::path& actual_path) {
  std::ifstream expected(expected_path), actual(actual_path);
  require(expected.good() && actual.good(), "cannot open responses for verification");
  std::string want_line, got_line;
  size_t count = 0;
  while (std::getline(expected, want_line)) {
    ++count;
    require(static_cast<bool>(std::getline(actual, got_line)), "missing response at line " + std::to_string(count));
    const Json want = Json::parse(want_line), got = Json::parse(got_line);
    bool matched = false;
    if (want.value("expect_error", false)) {
      matched = got.is_object() && got.size() == 3 && got.contains("id") && got.at("id") == want.at("id") &&
          got.contains("ok") && got.at("ok") == false && got.contains("error") && got.at("error").is_object() &&
          got.at("error").contains("code") && got.at("error").at("code").is_string() &&
          !got.at("error").at("code").get<std::string>().empty() && got.at("error").contains("message") &&
          got.at("error").at("message").is_string();
    } else matched = got == want;
    require(matched, "response mismatch at line " + std::to_string(count) + ": expected " + want.dump() + ", got " + got.dump());
  }
  require(!std::getline(actual, got_line), "unexpected response after shutdown");
  require(expected.eof() && actual.eof(), "response read failure");
  std::cout << Json{{"status", "passed"}, {"responses", count},
      {"expected_file", expected_path.filename().string()},
      {"actual_file", actual_path.filename().string()},
      {"oracle", "independent exact dyadic distance and complete response comparison"}}.dump() << '\n';
}
int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "generate") generate(argv[2]);
    else if (argc == 4 && std::string(argv[1]) == "verify") verify(argv[2], argv[3]);
    else throw std::runtime_error("usage: train generate DIR | train verify EXPECTED ACTUAL");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "PGO training rejected: " << error.what() << '\n';
    return 1;
  }
}

// In-process diagnostic, never a replacement for the JSONL paired benchmark.
#include "db.h"
#include "exec.h"
#include "sql.h"
#include "json.hpp"
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <time.h>
#include <vector>
#include <unistd.h>
using namespace mvdb;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
double cpu_seconds() {
  timespec value{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value))
    throw std::runtime_error("clock_gettime failed");
  return static_cast<double>(value.tv_sec) + value.tv_nsec * 1e-9;
}
struct Timing {
  double wall = 0, cpu = 0;
  size_t calls = 0;
  Json json() const { return {{"wall_seconds", wall}, {"process_cpu_seconds", cpu}, {"calls", calls}}; }
};
struct Timer {
  Clock::time_point wall = Clock::now();
  double cpu = cpu_seconds();
  void add(Timing& target) const {
    target.cpu += cpu_seconds() - cpu;
    target.wall += std::chrono::duration<double>(Clock::now() - wall).count();
    ++target.calls;
  }
};
struct Scratch {
  std::string path;
  Scratch() {
    const char* temp = std::getenv("TMPDIR");
    std::string pattern = std::string(temp ? temp : "/tmp") + "/mvdb-query-profile-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end()); writable.push_back(0);
    if (!::mkdtemp(writable.data())) throw std::runtime_error("mkdtemp failed");
    path = writable.data();
  }
  ~Scratch() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
std::string vector_sql(std::mt19937& rng, std::normal_distribution<float>& normal) {
  std::string result = "'[";
  for (size_t i = 0; i < 128; ++i) {
    if (i) result += ',';
    char text[64];
    const auto encoded = std::to_chars(text, text + sizeof(text), normal(rng),
        std::chars_format::general, std::numeric_limits<float>::max_digits10);
    if (encoded.ec != std::errc()) throw std::runtime_error("float formatting failed");
    result.append(text, encoded.ptr);
  }
  return result + "]'";
}
int number(const char* value, int lower, int upper) {
  std::string text(value); size_t used = 0;
  const long parsed = std::stol(text, &used);
  if (used != text.size() || parsed < lower || parsed > upper)
    throw std::runtime_error("numeric argument outside diagnostic bounds");
  return static_cast<int>(parsed);
}
}

int main(int argc, char** argv) {
  try {
    int seed = 7319, queries = 1000, repeats = 10;
    std::string responses_path;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help") {
        std::cout << "profile_query [--seed N] [--queries even-N] [--repeats N] [--responses NEW.jsonl]\n"
          "10k x 128d, half BIGINT equality, K=10. JSON stdout; diagnostics stderr.\n"
          "One first pass and N repeat passes; repeats are separate diagnostic observations.\n";
        return 0;
      }
      if (i + 1 == argc) throw std::runtime_error("missing option value");
      const char* value = argv[++i];
      if (arg == "--seed") seed = number(value, 0, 2147483647);
      else if (arg == "--queries") queries = number(value, 2, 10000);
      else if (arg == "--repeats") repeats = number(value, 0, 100);
      else if (arg == "--responses") responses_path = value;
      else throw std::runtime_error("unknown option: " + arg);
    }
    if (queries % 2) throw std::runtime_error("queries must be even");
    if (!responses_path.empty() && std::filesystem::exists(responses_path))
      throw std::runtime_error("responses output already exists");
    Scratch scratch; // Destroyed after db, including on errors.
    Database db; std::string err, out; int code = 0;
    if (!db.open(scratch.path, &err)) throw std::runtime_error(err);
    Column pk; pk.name = "id"; pk.type = Type::BIGINT; pk.is_primary = true;
    Column grp; grp.name = "grp"; grp.type = Type::BIGINT;
    Column vec; vec.name = "v"; vec.type = Type::VECTOR; vec.len = 128;
    if (!db.create_table("t", {pk, grp, vec}, &err, &code)) throw std::runtime_error(err);
    std::mt19937 rng(seed); std::normal_distribution<float> normal;
    if (!db.begin(&err, &code)) throw std::runtime_error(err);
    for (int i = 9999; i >= 0; --i) {
      std::vector<float> vector(128); for (float& value : vector) value = normal(rng);
      if (!db.insert("t", {Value::ofInt(i), Value::ofInt(i % 10), Value::ofVec(vector)}, &err, &code))
        throw std::runtime_error(err);
    }
    if (!db.commit(&err, &code) || !db.create_vector_index("t", "v", "vi", &err, &code))
      throw std::runtime_error(err);
    std::vector<std::string> sql;
    for (int i = 0; i < queries; ++i) {
      sql.push_back("SELECT id,L2_DISTANCE(v," + vector_sql(rng, normal) + ") AS d FROM t" +
          (i % 2 ? " WHERE grp=" + std::to_string((i / 2) % 10) : "") + " ORDER BY d LIMIT 10;");
    }
    std::vector<std::string> expected;
    std::ofstream responses;
    if (!responses_path.empty()) {
      responses.open(responses_path, std::ios::binary);
      if (!responses) throw std::runtime_error("cannot create response output");
    }
    Json passes = Json::array();
    size_t output_bytes = 0;
    for (int pass = 0; pass <= repeats; ++pass) {
      std::vector<Statement> statements; statements.reserve(sql.size());
      Timing parse;
      {
        const Timer started;
        for (const auto& query : sql) {
          Statement statement;
          // Match main.cpp: retain_raw=false. AST owns all values after return.
          if (!parse_sql(query, &statement, &err, &code, false)) throw std::runtime_error(err);
          statements.push_back(std::move(statement));
        }
        started.add(parse);
      }
      parse.calls = sql.size();
      Timing execute[2];
      for (size_t i = 0; i < statements.size(); ++i) {
        {
          const Timer started;
          if (!execute_json(db, statements[i], &out, &err, &code, false)) throw std::runtime_error(err);
          started.add(execute[i % 2]);
        }
        // All validation and response persistence are outside measured spans.
        if (pass == 0) {
          const Json result = Json::parse(out);
          if (result.at("columns") != Json::array({"id", "d"}) || result.at("rows").size() != 10)
            throw std::runtime_error("query response shape mismatch");
          for (const auto& row : result.at("rows")) {
            const int id = row.at(0).get<int>();
            if (id < 0 || id >= 10000 || (i % 2 && id % 10 != static_cast<int>((i / 2) % 10)))
              throw std::runtime_error("query returned an invalid or unfiltered id");
          }
          expected.push_back(out); output_bytes += out.size();
          if (responses.is_open()) responses << out << '\n';
        } else if (out != expected[i]) throw std::runtime_error("repeat response mismatch");
      }
      passes.push_back({{"pass", pass}, {"first_pass", pass == 0}, {"parse", parse.json()},
          {"execute_unfiltered", execute[0].json()}, {"execute_filtered", execute[1].json()}});
    }
    if (responses.is_open()) { responses.close(); if (!responses) throw std::runtime_error("response write failed"); }
    db.close();
    const Json report = {{"kind", "in_process_query_diagnostic"}, {"seed", seed},
        {"rows", 10000}, {"dimension", 128}, {"groups", 10}, {"k", 10}, {"queries_per_pass", queries},
        {"repeat_passes", repeats}, {"search_mode", "exact"}, {"passes", passes},
        {"first_pass_output_bytes", output_bytes}, {"repeat_responses_identical", true},
        {"scope", "Synthetic C++ mt19937 normal-distribution workload; not byte-identical to Python benchmark. "
          "Generation, ingestion, index construction and output validation excluded. SQL parsing uses retain_raw=false. "
          "Execution includes selection, distance, row lookup and JSON encoding; no JSON request envelope or IPC. "
          "Process CPU includes workers. Per-query clock overhead is included; components are not pure CPU attribution. "
          "First pass includes lazy scalar posting construction and possible query-pool calibration. "
          "Response equality is not an independent distance oracle; paired JSONL benchmark supplies sampled oracles. "
          "No formal score; repeat passes must not be pooled as independent seeds."}};
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "profile_query: " << error.what() << '\n';
    return 1;
  }
}

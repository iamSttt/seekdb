// Build this same fixture against old and candidate storage sources.
// It creates real on-disk snapshots (not exposed as a public SQL command).
#include "db.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void check(bool ok, const std::string& error) {
  if (!ok) { std::fprintf(stderr, "fixture failed: %s\n", error.c_str()); std::exit(1); }
}
static std::vector<mvdb::Value> row(int64_t id) {
  return {mvdb::Value::ofInt(id), mvdb::Value::ofInt(id % 2),
          mvdb::Value::ofStr("quoted '[1,2]' utf8 汉字 " + std::string(size_t(17 + id), 'x')),
          mvdb::Value::ofVec({float(id), -0.0f, 0.125f, -3.5f})};
}
int main(int argc, char** argv) {
  if (argc != 3 || (std::string(argv[1]) != "wal" && std::string(argv[1]) != "snapshot")) return 2;
  mvdb::Database db; std::string error; int code = 0;
  check(db.open(argv[2], &error), error);
  std::vector<mvdb::Column> columns(4);
  columns[0].name="id"; columns[0].type=mvdb::Type::BIGINT; columns[0].is_primary=true;
  columns[1].name="tag"; columns[1].type=mvdb::Type::BIGINT;
  columns[2].name="label"; columns[2].type=mvdb::Type::VARCHAR; columns[2].len=4096;
  columns[3].name="v"; columns[3].type=mvdb::Type::VECTOR; columns[3].len=4;
  check(db.create_table("crc_rows", columns, &error, &code), error);
  check(db.begin(&error, &code), error);
  for (int64_t id : {1, 2, 7, 15}) check(db.insert("crc_rows", row(id), &error, &code), error);
  check(db.commit(&error, &code), error);
  check(db.begin(&error, &code), error);
  check(db.insert("crc_rows", row(29), &error, &code), error);
  check(db.rollback(&error, &code), error);
  if (std::string(argv[1]) == "snapshot") check(db.checkpoint(&error), error);
  // Both a snapshot+WAL and pure-WAL fixture contain a later committed row.
  check(db.begin(&error, &code), error);
  check(db.insert("crc_rows", row(40), &error, &code), error);
  check(db.commit(&error, &code), error);
  // A durable-looking but uncommitted tail must never become visible on open.
  check(db.begin(&error, &code), error);
  check(db.insert("crc_rows", row(59), &error, &code), error);
  db.close();
  std::puts("PASS CRC persistence fixture written");
}

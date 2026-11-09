// value.h — core value types for MiniVectorDB
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mvdb {

enum class Type : uint8_t { BIGINT = 1, VARCHAR = 2, VECTOR = 3 };

struct Column {
  std::string name;
  Type type;
  int32_t len = 0;      // VARCHAR max chars, or VECTOR dimension
  bool is_primary = false;
};

// A row is a vector of Values; a VECTOR value is stored as float32 array.
struct Value {
  Type type = Type::BIGINT;
  int64_t i = 0;                 // BIGINT
  std::string s;                 // VARCHAR (UTF-8)
  std::vector<float> v;          // VECTOR

  static Value ofInt(int64_t x) { Value r; r.type = Type::BIGINT; r.i = x; return r; }
  static Value ofStr(std::string x) { Value r; r.type = Type::VARCHAR; r.s = std::move(x); return r; }
  static Value ofVec(std::vector<float> x) { Value r; r.type = Type::VECTOR; r.v = std::move(x); return r; }
};

struct Row {
  int64_t rid = 0;               // internal row id (monotonic, stable)
  std::vector<Value> vals;       // same order as table columns
};

// UTF-8 helpers: count code points, compare by code point order.
size_t utf8_length(const std::string& s);
int utf8_compare(const std::string& a, const std::string& b);

}  // namespace mvdb

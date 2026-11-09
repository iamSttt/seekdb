// sql.h — AST for the MiniVectorDB SQL subset (parser lives in sql.cpp)
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "value.h"

namespace mvdb {

struct Cond {
  enum Op { EQ, NE, LT, LE, GT, GE } op = EQ;
  std::string column;
  Value value;
};

struct SelectItem {
  bool is_distance = false;   // L2_DISTANCE(col, vec)
  std::string column;         // column name (or distance column)
  std::string alias;          // AS alias (may be empty)
  std::vector<float> query;   // for is_distance
};

struct Statement {
  enum Kind {
    EMPTY,
    CREATE_TABLE,
    DROP_TABLE,
    INSERT,
    SELECT,
    UPDATE,
    DELETE_ROWS,
    BEGIN_TXN,
    COMMIT_TXN,
    ROLLBACK_TXN,
    CREATE_INDEX,
    SHOW_TABLES,
    UNKNOWN,
  } kind = EMPTY;

  // CREATE TABLE
  std::string table;
  std::vector<Column> columns;
  std::string if_not_exists;      // raw token when present

  // INSERT
  std::vector<std::vector<Value>> rows;

  // SELECT
  std::vector<SelectItem> items;  // empty => SELECT *
  std::vector<Cond> conds;
  std::string order_by;
  bool order_desc = false;
  int64_t limit = -1;

  // UPDATE
  std::vector<std::pair<std::string, Value>> sets;

  // CREATE VECTOR INDEX
  std::string index_name;
  std::string index_column;

  std::string raw;                // original statement text
};

// Parses one statement. On failure returns false and fills `err` with a
// message; `err_code` receives a MySQL-ish error code (1064 by default).
bool parse_sql(const std::string& sql, Statement* out, std::string* err, int* err_code);

}  // namespace mvdb

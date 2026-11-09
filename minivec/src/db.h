// db.h — catalogue, tables, transactions, persistence (implementation in db.cpp)
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sql.h"
#include "value.h"

namespace mvdb {

struct TableSchema {
  std::string name;
  std::vector<Column> cols;
  int pk_idx = 0;
  int vec_idx = -1;

  int find(const std::string& n) const {
    for (size_t i = 0; i < cols.size(); ++i)
      if (cols[i].name == n) return (int)i;
    return -1;
  }
};

struct Table {
  TableSchema schema;
  std::vector<Row> rows;                       // live rows (append-only; deletes are tombstones)
  std::vector<uint8_t> dead;                   // 1 = deleted
  std::unordered_map<int64_t, size_t> pk_pos;  // pk -> index into rows
  int64_t next_rid = 1;
  bool has_index = false;

  size_t live_count() const;
};

enum class ErrCode : int {
  OK = 0,
  SYNTAX = 1064,
  NO_SUCH_TABLE = 1146,
  NO_SUCH_COLUMN = 1054,
  DUP_KEY = 1062,
  NOT_SUPPORTED = 1235,
  TABLE_EXISTS = 1050,
  BAD_VALUE = 1264,
  TXN = 1568,
};

class Database {
 public:
  Database();
  ~Database();

  bool open(const std::string& data_dir, std::string* err);
  void close();

  // schema
  bool create_table(const std::string& name, const std::vector<Column>& cols, std::string* err, int* code);
  bool drop_table(const std::string& name, std::string* err, int* code);
  bool has_table(const std::string& name) const;
  Table* table(const std::string& name);
  std::vector<std::string> table_names() const;

  // DML (applies immediately; undo recorded when a txn is open)
  bool insert(const std::string& t, std::vector<Value> vals, std::string* err, int* code);
  int64_t update_rows(const std::string& t, const std::vector<Cond>& conds,
                      const std::vector<std::pair<std::string, Value>>& sets, std::string* err, int* code);
  int64_t delete_rows(const std::string& t, const std::vector<Cond>& conds, std::string* err, int* code);

  // scan with predicate pushdown (conds are ANDed); returns live rows only
  void scan(const std::string& t, const std::vector<Cond>& conds,
            const std::function<bool(const Row&)>& cb);

  // transactions
  bool begin(std::string* err, int* code);
  bool commit(std::string* err, int* code);
  bool rollback(std::string* err, int* code);
  bool in_txn() const { return txn_active_; }

  // vector index
  bool create_vector_index(const std::string& t, const std::string& col, const std::string& name,
                           std::string* err, int* code);
  bool has_vector_index(const std::string& t) const;

  // durability
  bool checkpoint(std::string* err);
  uint64_t committed_rows() const { return wal_rows_; }

 private:
  struct Undo;
  void apply_insert(const std::string& t, std::vector<Value> vals, int64_t* pk);
  void append_wal(const std::string& rec);
  bool fsync_wal();
  bool load_snapshot(std::string* err);
  bool replay_wal(std::string* err);

  std::string dir_;
  std::unordered_map<std::string, Table> tables_;
  std::vector<Undo> undo_;
  bool txn_active_ = false;
  FILE* wal_ = nullptr;
  uint64_t wal_rows_ = 0;
  uint64_t wal_bytes_ = 0;
};

bool eval_conds(const Table& t, const Row& r, const std::vector<Cond>& conds);

}  // namespace mvdb

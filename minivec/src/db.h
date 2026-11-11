// db.h — catalogue, tables, transactions, persistence (implementation in db.cpp)
#pragma once
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "flat_index.h"
#include "sql.h"
#include "value.h"

namespace mvdb {

struct PayloadStore;

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
  Table()=default;
  Table(const Table&)=delete;
  Table& operator=(const Table&)=delete;
  Table(Table&&) noexcept=default;
  Table& operator=(Table&&) noexcept=default;
  TableSchema schema;
  std::vector<Row> rows;                       // live rows (append-only; deletes are tombstones)
  std::vector<uint8_t> dead;                   // 1 = deleted
  std::unordered_map<int64_t, size_t> pk_pos;  // pk -> index into rows
  int64_t next_rid = 1;
  bool has_index = false;
  std::shared_ptr<FlatIndex> vector_index;
  // Large row payloads spill to a rebuildable, unlinked file in /data. The
  // primary-key and row-slot metadata remain resident.
  std::string storage_dir;
  std::shared_ptr<PayloadStore> payload_store;
  std::vector<uint64_t> payload_offsets;
  uint64_t payload_bytes = 0;

  size_t live_count() const;
  const Row& read_row(size_t slot, Row& scratch) const;
  void replace_row(size_t slot, Row row);
  void append_row(Row row);
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
  const std::string& data_directory() const { return dir_; }
  // Wrap one modifying SQL statement, including statements inside BEGIN.
  bool begin_statement(std::string* err, int* code);
  bool end_statement(bool success, std::string* err, int* code);

  // vector index
  bool create_vector_index(const std::string& t, const std::string& col, const std::string& name,
                           std::string* err, int* code);
  bool has_vector_index(const std::string& t) const;
  // Non-owning, read-only storage held by the table's physical index. Use only
  // during the current statement without mutations: mutation, rollback,
  // rebuild, dropping the table or closing the database can invalidate it.
  // Unindexed and disk-backed tables return false and retain the scan path.
  bool resident_vector_view(const std::string& t, FlatIndex::ResidentView* view);
  // Same statement-only lifetime as resident_vector_view. Conditions are fully
  // coerced/validated first. An empty residual means equality postings prove
  // the complete predicate; otherwise call it on the selected source slot.
  bool resident_integer_equal_view(const std::string& t,const std::vector<Cond>& conds,
      FlatIndex::ResidentSelection* selection,std::function<bool(uint64_t)>* residual);
  // Pointer lifetime is limited to this callback. Filtering precedes distance
  // computation by the caller; unindexed tables use the ordinary row scan.
  void scan_vectors(const std::string& t, const std::vector<Cond>& conds,
                    const std::function<bool(int64_t,const float*,size_t)>& cb);

  // durability
  bool checkpoint(std::string* err);
  uint64_t committed_rows() const { return wal_rows_; }

 private:
  struct Undo;
  void apply_insert(const std::string& t, std::vector<Value> vals, int64_t* pk);
  bool append_wal(const std::string& rec);
  void undo_to(size_t mark);
  bool truncate_wal(uint64_t bytes, uint64_t lsn, std::string* err, int* code);
  bool fsync_wal();
  bool load_snapshot(std::string* err);
  bool replay_wal(std::string* err);
  void build_vector_index(Table& table);

  std::string dir_;
  std::unordered_map<std::string, Table> tables_;
  std::vector<Undo> undo_;
  bool txn_active_ = false;
  uint64_t txn_wal_start_ = 0, txn_lsn_start_ = 0;
  bool stmt_active_ = false, stmt_implicit_ = false;
  size_t stmt_undo_start_ = 0;
  uint64_t stmt_wal_start_ = 0, stmt_lsn_start_ = 0;
  FILE* wal_ = nullptr;
  uint64_t wal_rows_ = 0;
  uint64_t wal_bytes_ = 0;
};

bool validate_conditions(const Table& t, const std::vector<Cond>& conds,
                         std::string* err, int* code);
bool eval_conds(const Table& t, const Row& r, const std::vector<Cond>& conds);

}  // namespace mvdb

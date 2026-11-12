// db.cpp — MiniVectorDB storage engine: in-memory tables, WAL, snapshot, catalog,
// and single-connection transactions.
//
// ---------------------------------------------------------------------------
// On-disk layout (all integers little-endian, all strings length prefixed)
// ---------------------------------------------------------------------------
//   <dir>/catalog.bin    legacy file, ignored by recovery
//   <dir>/snapshot.bin   full table contents at LSN N (+ N in the header), CRC32 tail
//   <dir>/snapshot.lsn   mirror of N as text (informational; header is authoritative)
//   <dir>/data.log       append-only WAL: [u32 len][u32 crc][payload]
//
// WAL payload:
//   u8 kind, u8 flags, u64 lsn, u32 name_len, name bytes, <body>
//
// Durability model:
//   * Autocommit DML (no BEGIN active) appends its record and fdatasync()s before
//     returning, i.e. a statement is durable before the caller reports success.
//   * Explicit transaction records carry RF_TXN and are appended without fsync;
//     commit() appends a TXN_COMMIT record and fdatasync()s once. Replay re-reads
//     RF_TXN records only after a TXN_COMMIT has been validated, so a
//     rolled back or crashed transaction never reaches the in-memory state.
//   * Replay stops at the first bad/truncated record and then truncates data.log
//     back to the last good record boundary, so later appends stay contiguous.
//   * checkpoint() writes snapshot.bin atomically (tmp+fsync+rename+dir fsync)
//     *before* truncating the WAL; records already covered by the snapshot are
//     skipped on replay using the LSN stored in the snapshot header.
// ---------------------------------------------------------------------------
#include "db.h"
#include "flat_index.h"
#include "quantized_bound.h"

#include <algorithm>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mvdb {

struct PayloadStore {
  int fd = -1;
  uint64_t end = 0;
  ~PayloadStore() { if (fd >= 0) ::close(fd); }
};

struct RowUndoImage {
  Row row;
  bool spilled = false;
  uint64_t offset = 0;
};

namespace {

using TableMap = std::unordered_map<std::string, Table>;

constexpr uint32_t kSnapMagic = 0x4D565331u;  // "MVS1"

constexpr uint32_t kMaxStr = 1u << 26;   // 64 MiB for a single string field
constexpr uint32_t kMaxDim = 1u << 20;   // vector dimension sanity bound
constexpr uint32_t kMaxCols = 4096;      // columns per table
constexpr uint32_t kMaxRec = 1u << 30;   // WAL payload sanity bound

// Once the WAL passes this size outside a transaction it is folded into a fresh
// snapshot and truncated, which bounds both disk usage and restart replay time.
// Nothing else in the engine triggers checkpoint(), so this is what keeps a long
// single-process run from accumulating an unbounded log.
#ifndef MVDB_AUTO_CHECKPOINT_BYTES
#define MVDB_AUTO_CHECKPOINT_BYTES (64ull << 20)
#endif
constexpr uint64_t kAutoCheckpointBytes = MVDB_AUTO_CHECKPOINT_BYTES;
#ifndef MVDB_SPILL_BYTES
#define MVDB_SPILL_BYTES (32ull << 20)
#endif
constexpr uint64_t kSpillBytes = MVDB_SPILL_BYTES;
// Bound additional row images across all tables in one transaction. Metadata
// for INSERT/DELETE and the transaction itself retain their existing accounting.
constexpr uint64_t kUndoImageBytes = 1ull << 20;

// WAL record kinds.
enum RecKind : uint8_t {
  REC_INSERT = 1,
  REC_UPDATE = 2,
  REC_DELETE = 3,
  REC_CREATE_TABLE = 4,
  REC_DROP_TABLE = 5,
  REC_CREATE_INDEX = 6,
  REC_TXN_BEGIN = 16,
  REC_TXN_COMMIT = 17,
};

enum RecFlags : uint8_t { RF_TXN = 1 };  // record belongs to an explicit transaction

// ---------------------------------------------------------------- CRC32 ----
struct Crc32Table {
  uint32_t t[256];
  Crc32Table() {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
  }
};
const Crc32Table kCrcTable;

inline uint32_t crc32_upd(uint32_t crc, const void* p, size_t n) {
  const uint8_t* q = static_cast<const uint8_t*>(p);
  while (n--) crc = kCrcTable.t[(crc ^ *q++) & 0xFFu] ^ (crc >> 8);
  return crc;
}
inline uint32_t crc32_of(const void* p, size_t n) {
  return crc32_upd(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------ byte IO ------
inline void put_u8(std::string& b, uint8_t v) { b.push_back(static_cast<char>(v)); }
inline void put_u16(std::string& b, uint16_t v) {
  b.push_back(static_cast<char>(v & 0xFF));
  b.push_back(static_cast<char>((v >> 8) & 0xFF));
}
inline void put_u32(std::string& b, uint32_t v) {
  put_u16(b, static_cast<uint16_t>(v & 0xFFFF));
  put_u16(b, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}
inline void put_u64(std::string& b, uint64_t v) {
  put_u32(b, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  put_u32(b, static_cast<uint32_t>(v >> 32));
}
inline void put_i64(std::string& b, int64_t v) { put_u64(b, static_cast<uint64_t>(v)); }
inline void put_i32(std::string& b, int32_t v) { put_u32(b, static_cast<uint32_t>(v)); }
inline void put_f32(std::string& b, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  put_u32(b, u);
}
inline void put_str(std::string& b, const std::string& s) {
  put_u32(b, static_cast<uint32_t>(s.size()));
  b.append(s);
}

struct Rd {
  const uint8_t* p;
  const uint8_t* e;
  bool bad = false;

  Rd(const void* data, size_t n)
      : p(static_cast<const uint8_t*>(data)), e(static_cast<const uint8_t*>(data) + n) {}

  size_t left() const { return static_cast<size_t>(e - p); }
  bool need(size_t n) {
    if (left() < n) { bad = true; return false; }
    return true;
  }
  bool read(void* out, size_t n) {
    if (!need(n)) return false;
    if (n) std::memcpy(out, p, n);
    p += n;
    return true;
  }
  uint8_t u8() {
    if (!need(1)) return 0;
    return *p++;
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[0] | (p[1] << 8));
    p += 2;
    return v;
  }
  uint32_t u32() {
    uint32_t lo = u16();
    uint32_t hi = u16();
    return lo | (hi << 16);
  }
  uint64_t u64() {
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
  }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  float f32() {
    uint32_t u = u32();
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
  bool str(std::string* out) {
    uint32_t n = u32();
    if (bad || n > kMaxStr || !need(n)) { bad = true; return false; }
    out->assign(reinterpret_cast<const char*>(p), n);
    p += n;
    return true;
  }
};

// Value / Row / TableSchema serialisation (shared by WAL and snapshot).
void put_value(std::string& b, const Value& v) {
  put_u8(b, static_cast<uint8_t>(v.type));
  switch (v.type) {
    case Type::BIGINT:
      put_i64(b, v.i);
      break;
    case Type::VARCHAR:
      put_str(b, v.s);
      break;
    case Type::VECTOR: {
      put_u32(b, static_cast<uint32_t>(v.v.size()));
      // Bulk copy of the float payload, byte-swapped on big-endian hosts only.
      if (sizeof(float) == 4) {
        static const uint16_t probe = 1;
        const bool little = *reinterpret_cast<const uint8_t*>(&probe) == 1;
        if (little) {
          b.append(reinterpret_cast<const char*>(v.v.data()), v.v.size() * 4);
        } else {
          for (float f : v.v) put_f32(b, f);
        }
      } else {
        for (float f : v.v) put_f32(b, f);
      }
      break;
    }
  }
}

template <class Reader>
bool get_value(Reader& r, Value* v) {
  uint8_t t = r.u8();
  if (r.bad) return false;
  switch (static_cast<Type>(t)) {
    case Type::BIGINT:
      v->type = Type::BIGINT;
      v->i = r.i64();
      return !r.bad;
    case Type::VARCHAR:
      v->type = Type::VARCHAR;
      v->s.clear();
      return r.str(&v->s);
    case Type::VECTOR: {
      v->type = Type::VECTOR;
      uint32_t n = r.u32();
      if (r.bad || n > kMaxDim || !r.need(static_cast<size_t>(n) * 4)) {
        r.bad = true;
        return false;
      }
      v->v.resize(n);
      return r.read(v->v.data(), static_cast<size_t>(n) * 4);
    }
    default:
      r.bad = true;
      return false;
  }
}

void put_row(std::string& b, const Row& row) {
  put_u32(b, static_cast<uint32_t>(row.vals.size()));
  for (const Value& v : row.vals) put_value(b, v);
}

template <class Reader>
bool get_row(Reader& r, Row* row) {
  uint32_t n = r.u32();
  if (r.bad || n > kMaxCols) { r.bad = true; return false; }
  row->vals.clear();
  row->vals.resize(n);
  for (uint32_t i = 0; i < n; ++i)
    if (!get_value(r, &row->vals[i])) return false;
  return true;
}

uint64_t row_heap_bytes(const Row& row) {
  uint64_t n = row.vals.size() * sizeof(Value);
  for (const Value& v : row.vals) n += v.s.size() + v.v.size() * sizeof(float);
  return n;
}

uint64_t undo_image_bytes(const Row& row) {
  // Include capacities (not just sizes), shared ownership and weak registration
  // storage. Counting inline string capacity too is deliberately conservative.
  uint64_t n = sizeof(RowUndoImage) + 2 * sizeof(void*) +
      sizeof(std::weak_ptr<RowUndoImage>) + 2 * sizeof(void*) +
      row.vals.capacity() * sizeof(Value);
  for (const Value& v : row.vals)
    n += v.s.capacity() + 1 + v.v.capacity() * sizeof(float);
  return n;
}

void discard_appended_row(Table& table) {
  if (table.payload_store) table.payload_offsets.pop_back();
  else table.payload_bytes -= row_heap_bytes(table.rows.back());
  table.rows.pop_back();
}

void positional_io(int fd, void* data, size_t size, uint64_t off, bool writing) {
  char* p = static_cast<char*>(data);
  while (size) {
    ssize_t n = writing ? ::pwrite(fd,p,size,static_cast<off_t>(off))
                        : ::pread(fd,p,size,static_cast<off_t>(off));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) throw std::runtime_error("row payload cache I/O failed");
    p += n; size -= static_cast<size_t>(n); off += static_cast<uint64_t>(n);
  }
}

uint64_t store_payload(PayloadStore& store, const Row& row) {
  std::string b;
  put_u32(b,0);
  put_row(b,row);
  if (b.size()-4 > kMaxRec) throw std::runtime_error("row exceeds storage record limit");
  const uint32_t len=static_cast<uint32_t>(b.size()-4);
  std::memcpy(&b[0],&len,4);
  const uint64_t off=store.end;
  positional_io(store.fd,&b[0],b.size(),off,true);
  store.end+=b.size();
  return off;
}

void spill_payloads(Table& table, bool force = false) {
  if (table.payload_store || (!force && table.payload_bytes <= kSpillBytes)) return;
  std::string name=table.storage_dir+"/.payload-XXXXXX";
  std::vector<char> path(name.begin(),name.end()); path.push_back(0);
  auto store=std::make_shared<PayloadStore>();
  store->fd=::mkstemp(path.data());
  if (store->fd < 0) throw std::runtime_error("cannot create row payload cache");
  ::unlink(path.data()); // source of truth is snapshot + WAL, never this cache
  std::vector<uint64_t> offsets;
  offsets.reserve(table.rows.size());
  std::vector<std::pair<std::shared_ptr<RowUndoImage>,uint64_t>> old_images;
  // Write Undo before current rows: failed INSERT cleanup can rewind the store
  // to the last inserted row, and must never overwrite any older Undo offset.
  for (const auto& weak : table.resident_undo) {
    auto image = weak.lock();
    if (image && !image->spilled)
      old_images.emplace_back(image,store_payload(*store,image->row));
  }
  for (const Row& row : table.rows) offsets.push_back(store_payload(*store,row));
  table.payload_offsets=std::move(offsets);
  table.payload_store=std::move(store);
  // Global budget enforcement may spill another table with a live borrowed index.
  // Invalidate its pointers before releasing any resident payload.
  table.vector_index.reset();
  // Only publish migrated images after every write succeeded. On failure the
  // resident table and its Undo remain intact and the temporary file is closed.
  for (auto& image : old_images) {
    image.first->offset = image.second;
    image.first->spilled = true;
    std::vector<Value>().swap(image.first->row.vals);
  }
  table.resident_undo.clear();
  for (Row& row : table.rows) std::vector<Value>().swap(row.vals);
  table.payload_bytes=0;
}

void enforce_payload_budget(TableMap& tables) {
  for (;;) {
    uint64_t resident=0;
    Table* largest=nullptr;
    for (auto& kv : tables) {
      Table& table=kv.second;
      if (table.payload_store) continue;
      resident+=table.payload_bytes;
      if (!largest || table.payload_bytes > largest->payload_bytes) largest=&table;
    }
    if (!largest || resident <= (64ull << 20)) return;
    spill_payloads(*largest, true);
  }
}

void put_schema(std::string& b, const TableSchema& s) {
  put_str(b, s.name);
  put_u32(b, static_cast<uint32_t>(s.cols.size()));
  for (const Column& c : s.cols) {
    put_str(b, c.name);
    put_u8(b, static_cast<uint8_t>(c.type));
    put_i32(b, c.len);
    put_u8(b, c.is_primary ? 1 : 0);
  }
  put_i32(b, s.pk_idx);
  put_i32(b, s.vec_idx);
}

template <class Reader>
bool get_schema(Reader& r, TableSchema* s) {
  if (!r.str(&s->name)) return false;
  uint32_t n = r.u32();
  if (r.bad || n > kMaxCols) { r.bad = true; return false; }
  s->cols.clear();
  s->cols.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    Column& c = s->cols[i];
    if (!r.str(&c.name)) return false;
    uint8_t t = r.u8();
    if (r.bad || t < 1 || t > 3) { r.bad = true; return false; }
    c.type = static_cast<Type>(t);
    c.len = r.i32();
    c.is_primary = r.u8() != 0;
    if (r.bad) return false;
  }
  s->pk_idx = r.i32();
  s->vec_idx = r.i32();
  return !r.bad;
}

bool same_schema(const TableSchema& a, const TableSchema& b) {
  if (a.name != b.name || a.pk_idx != b.pk_idx || a.vec_idx != b.vec_idx) return false;
  if (a.cols.size() != b.cols.size()) return false;
  for (size_t i = 0; i < a.cols.size(); ++i) {
    const Column& x = a.cols[i];
    const Column& y = b.cols[i];
    if (x.name != y.name || x.type != y.type || x.len != y.len || x.is_primary != y.is_primary)
      return false;
  }
  return true;
}

// ------------------------------------------------------------ file utils ---
bool ensure_dir(const std::string& d) {
  if (d.empty()) return false;
  struct stat st;
  if (::stat(d.c_str(), &st) == 0) return S_ISDIR(st.st_mode) != 0;
  size_t i = 0;
  while (i < d.size()) {
    size_t j = d.find('/', i);
    if (j == std::string::npos) j = d.size();
    std::string cur = d.substr(0, j);
    if (!cur.empty() && ::mkdir(cur.c_str(), 0777) != 0 && errno != EEXIST) return false;
    i = j + 1;
  }
  return ::stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) != 0;
}

bool fsync_dir(const std::string& d) {
  int fd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool write_atomic(const std::string& path, const std::string& data, std::string* err) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (err) *err = "open " + tmp + ": " + std::strerror(errno);
    return false;
  }
  size_t off = 0;
  while (off < data.size()) {
    ssize_t w = ::write(fd, data.data() + off, data.size() - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (err) *err = "write " + tmp + ": " + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (w == 0) {
      if (err) *err = "short write " + tmp;
      ::close(fd);
      return false;
    }
    off += static_cast<size_t>(w);
  }
  if (::fsync(fd) != 0) {
    if (err) *err = "fsync " + tmp + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
    return false;
  }
  std::string dir = path.substr(0, path.find_last_of('/'));
  if (!dir.empty() && !fsync_dir(dir)) {
    if (err) *err = "cannot sync directory " + dir;
    return false;
  }
  return true;
}

// ------------------------------------------------------- predicate eval ----
// Conditions resolved once per scan: column index + a coerced literal, so the
// per-row loop does no string lookups.
struct RCond {
  uint32_t col = 0;
  uint8_t op = 0;    // Cond::Op
  uint8_t vt = 0;    // Type of the column
  const Value* lit = nullptr;
};

struct Pred {
  std::vector<RCond> c;
  std::vector<Value> owned;  // coerced literals; reserved so pointers stay valid
  bool never = false;        // a scalar literal cannot be coerced to its column type
};

void set_err(std::string* err, int* code, ErrCode c, const std::string& msg) {
  if (err) *err = msg;
  if (code) *code = static_cast<int>(c);
}

bool parse_i64(const std::string& s, int64_t* out) {
  if (s.empty()) return false;
  size_t i = 0;
  bool neg = false;
  if (s[0] == '+' || s[0] == '-') {
    neg = (s[0] == '-');
    i = 1;
    if (s.size() == 1) return false;
  }
  uint64_t v = 0;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
    uint64_t d = static_cast<uint64_t>(s[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  if (neg) {
    if (v > static_cast<uint64_t>(INT64_MAX) + 1ull) return false;
    *out = static_cast<int64_t>(0ull - v);
  } else {
    if (v > static_cast<uint64_t>(INT64_MAX)) return false;
    *out = static_cast<int64_t>(v);
  }
  return true;
}

bool check_vector_value(const Column& column, const Value& value, std::string* err) {
  if (value.type != Type::VECTOR) {
    if (err) *err = "column '" + column.name + "': expected VECTOR";
    return false;
  }
  if (value.v.size() != static_cast<size_t>(column.len)) {
    if (err)
      *err = "column '" + column.name + "': vector dimension " +
             std::to_string(value.v.size()) + " != " + std::to_string(column.len);
    return false;
  }
  for (float f : value.v) {
    if (!std::isfinite(f)) {
      if (err) *err = "column '" + column.name + "': vector contains NaN/Inf";
      return false;
    }
  }
  return true;
}

// Coerces a validated predicate literal to the column type. Returns false
// when a scalar conversion cannot succeed.
bool coerce_literal(Type ct, const Value& in, Value* out) {
  switch (ct) {
    case Type::BIGINT:
      if (in.type == Type::BIGINT) {
        *out = in;
        return true;
      }
      if (in.type == Type::VARCHAR) {
        int64_t x;
        if (!parse_i64(in.s, &x)) return false;
        *out = Value::ofInt(x);
        return true;
      }
      return false;
    case Type::VARCHAR:
      if (in.type == Type::VARCHAR || (in.type == Type::VECTOR && !in.s.empty())) {
        *out = in;
        out->type = Type::VARCHAR;
        out->v.clear();
        return true;
      }
      if (in.type == Type::BIGINT) {
        *out = Value::ofStr(std::to_string(in.i));
        return true;
      }
      return false;
    case Type::VECTOR:
      *out = in;
      return true;
  }
  return false;
}

bool compile_pred(const Table& t, const std::vector<Cond>& conds, Pred* out,
                  std::string* err = nullptr, int* code = nullptr) {
  if (out) {
    out->c.reserve(conds.size());
    out->owned.reserve(conds.size());  // no reallocation => &owned.back() stays valid
  }
  for (const Cond& cond : conds) {
    const int idx = t.schema.find(cond.column);
    if (idx < 0) {
      set_err(err, code, ErrCode::NO_SUCH_COLUMN,
              "Unknown column '" + cond.column + "' in 'where clause'");
      return false;
    }
    const Column& column = t.schema.cols[static_cast<size_t>(idx)];
    if (column.type == Type::VECTOR && !check_vector_value(column, cond.value, err)) {
      if (code) *code = static_cast<int>(ErrCode::BAD_VALUE);
      return false;
    }
    // Still validate later terms after a scalar comparison can never match.
    if (!out || out->never) continue;
    Value coerced;
    if (!coerce_literal(column.type, cond.value, &coerced)) {
      out->never = true;
      continue;
    }
    out->owned.push_back(std::move(coerced));
    RCond rc;
    rc.col = static_cast<uint32_t>(idx);
    rc.op = static_cast<uint8_t>(cond.op);
    rc.vt = static_cast<uint8_t>(column.type);
    rc.lit = &out->owned.back();
    out->c.push_back(rc);
  }
  return true;
}

inline int cmp_vecf(const std::vector<float>& a, const std::vector<float>& b) {
  size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; ++i) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

inline bool row_matches(const Pred& pr, const Row& row) {
  const size_t ncond = pr.c.size();
  if (ncond == 0) return true;
  const size_t nvals = row.vals.size();
  for (size_t i = 0; i < ncond; ++i) {
    const RCond& c = pr.c[i];
    if (c.col >= nvals) return false;
    const Value& a = row.vals[c.col];
    int cmpv;
    if (c.vt == static_cast<uint8_t>(Type::BIGINT)) {
      const int64_t x = a.i, y = c.lit->i;
      cmpv = (x < y) ? -1 : (x > y ? 1 : 0);
    } else if (c.vt == static_cast<uint8_t>(Type::VARCHAR)) {
      cmpv = utf8_compare(a.s, c.lit->s);
    } else {
      cmpv = cmp_vecf(a.v, c.lit->v);
    }
    bool m;
    switch (static_cast<Cond::Op>(c.op)) {
      case Cond::EQ: m = (cmpv == 0); break;
      case Cond::NE: m = (cmpv != 0); break;
      case Cond::LT: m = (cmpv < 0); break;
      case Cond::LE: m = (cmpv <= 0); break;
      case Cond::GT: m = (cmpv > 0); break;
      case Cond::GE: m = (cmpv >= 0); break;
      default: m = false; break;
    }
    if (!m) return false;
  }
  return true;
}

// Validates/normalises one value against its column definition.
bool check_value(const Column& c, Value* v, std::string* err) {
  if ((v->type == Type::VARCHAR && v->s.size() > kMaxStr) ||
      (v->type == Type::VECTOR && v->v.size() > kMaxDim)) {
    if (err) *err = "value exceeds durable format limit";
    return false;
  }
  switch (c.type) {
    case Type::BIGINT:
      if (v->type != Type::BIGINT) {
        if (v->type == Type::VARCHAR) {
          int64_t x;
          if (!parse_i64(v->s, &x)) {
            if (err) *err = "column '" + c.name + "': cannot convert string to BIGINT";
            return false;
          }
          *v = Value::ofInt(x);
        } else {
          if (err) *err = "column '" + c.name + "': expected BIGINT";
          return false;
        }
      }
      return true;
    case Type::VARCHAR:
      if (v->type == Type::VECTOR && !v->s.empty()) {
        v->type = Type::VARCHAR;
        v->v.clear();
      }
      if (v->type == Type::BIGINT) {
        *v = Value::ofStr(std::to_string(v->i));
      } else if (v->type != Type::VARCHAR) {
        if (err) *err = "column '" + c.name + "': expected VARCHAR";
        return false;
      }
      if (v->s.size() > kMaxStr) {
        if (err) *err = "string exceeds durable format limit";
        return false;
      }
      if (static_cast<int64_t>(utf8_length(v->s)) > static_cast<int64_t>(c.len)) {
        if (err)
          *err = "column '" + c.name + "': value too long (" +
                 std::to_string(utf8_length(v->s)) + " > " + std::to_string(c.len) + " chars)";
        return false;
      }
      return true;
    case Type::VECTOR:
      if (!check_vector_value(c, *v, err)) return false;
      std::string().swap(v->s);
      return true;
  }
  if (err) *err = "unknown column type";
  return false;
}

// Check before constructing or narrowing the record length. UPDATE has the
// largest row-record header, so this bound also covers INSERT and spill rows.
bool row_fits_record(const std::string& table, const std::vector<Value>& vals) {
  uint64_t bytes = 10 + 4 + static_cast<uint64_t>(table.size()) + 8 + 4;
  for (const Value& v : vals) {
    bytes += 1;
    if (v.type == Type::BIGINT) bytes += 8;
    else if (v.type == Type::VARCHAR) bytes += 4 + static_cast<uint64_t>(v.s.size());
    else bytes += 4 + static_cast<uint64_t>(v.v.size()) * 4;
    if (bytes > kMaxRec) return false;
  }
  return bytes <= kMaxRec;
}

void write_all_fd(int fd, const void* p, size_t n, size_t* done) {
  const char* q = static_cast<const char*>(p);
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, q + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (w == 0) break;
    off += static_cast<size_t>(w);
  }
  *done = off;
}

}  // namespace

// ===========================================================================
// Table
// ===========================================================================
size_t Table::live_count() const {
  if (dead.empty()) return rows.size();
  const size_t n = rows.size() < dead.size() ? rows.size() : dead.size();
  size_t live = 0;
  for (size_t i = 0; i < n; ++i) live += (dead[i] == 0);
  return live + (rows.size() > n ? rows.size() - n : 0);
}

const Row& Table::read_row(size_t slot, Row& scratch) const {
  if (!payload_store) return rows[slot];
  const uint64_t off=payload_offsets[slot];
  uint32_t len=0;
  positional_io(payload_store->fd,&len,4,off,false);
  if (len > kMaxRec) throw std::runtime_error("invalid payload cache record");
  std::string b(len,'\0');
  if (len) positional_io(payload_store->fd,&b[0],len,off+4,false);
  Rd rd(b.data(),b.size());
  scratch.rid=rows[slot].rid;
  if (!get_row(rd,&scratch)) throw std::runtime_error("invalid payload cache row");
  return scratch;
}

void Table::append_row(Row row) {
  if (payload_store) {
    const uint64_t before = payload_store->end;
    Row meta;
    meta.rid = row.rid;
    rows.push_back(std::move(meta));
    try {
      payload_offsets.push_back(store_payload(*payload_store, row));
    } catch (...) {
      rows.pop_back();
      payload_store->end = before;
      throw;
    }
  } else {
    const uint64_t bytes = row_heap_bytes(row);
    rows.push_back(std::move(row));
    payload_bytes += bytes;
    try {
      spill_payloads(*this);
    } catch (...) {
      discard_appended_row(*this);
      throw;
    }
  }
  vector_index.reset();
}

void Table::replace_row(size_t slot, Row row) {
  vector_index.reset();
  if (payload_store) {
    payload_offsets[slot]=store_payload(*payload_store,row);
    rows[slot].rid=row.rid;
  } else {
    payload_bytes-=std::min(payload_bytes,row_heap_bytes(rows[slot]));
    payload_bytes+=row_heap_bytes(row);
    rows[slot]=std::move(row);
    spill_payloads(*this);
  }
}

// ===========================================================================
// Undo log
// ===========================================================================
struct Database::Undo {
  enum K : uint8_t { K_INS, K_UPD, K_DEL, K_DROP, K_CREATE, K_INDEX } k = K_INS;
  std::string table;
  int64_t pk = 0;
  size_t slot = 0;
  std::shared_ptr<RowUndoImage> old;  // K_UPD: bounded image, or migrated offset
  uint64_t image_bytes = 0;
  std::unique_ptr<Table> dropped;  // K_DROP: entire table as it was
  bool old_index = false;
  bool old_spilled = false;
  uint64_t old_offset = 0;
};

// ===========================================================================
// Database
// ===========================================================================
Database::Database() = default;

Database::~Database() { close(); }

void Database::close() {
  if (wal_) {
    (void)flush_wal();
    std::fflush(wal_);
    std::fclose(wal_);
    wal_ = nullptr;
  }
  wal_buf_.clear();
  wal_flushed_ = 0;
  tables_.clear();
  undo_.clear();
  undo_image_bytes_ = 0;
  txn_active_ = false;
  stmt_active_ = false;
  stmt_implicit_ = false;
  wal_rows_ = 0;
  wal_bytes_ = 0;
  dir_.clear();
}

bool Database::open(const std::string& data_dir, std::string* err) {
  close();
  if (data_dir.empty()) {
    if (err) *err = "empty data directory";
    return false;
  }
  dir_ = data_dir;
  while (dir_.size() > 1 && dir_.back() == '/') dir_.pop_back();
  if (!ensure_dir(dir_)) {
    if (err) *err = "cannot create data directory '" + dir_ + "': " + std::strerror(errno);
    return false;
  }
  if (!load_snapshot(err)) {
    close();
    return false;
  }
  if (!replay_wal(err)) {
    close();
    return false;
  }
  std::string log = dir_ + "/data.log";
  wal_ = std::fopen(log.c_str(), "ab");  // O_APPEND: every write lands at EOF
  if (wal_) {
    // Reserve blocks past the end without moving it, so appends reuse allocated
    // space and each commit fdatasync stops paying for block allocation. Best
    // effort: st_size - and therefore replay - is unchanged.
    (void)::fallocate(::fileno(wal_), FALLOC_FL_KEEP_SIZE, 0, static_cast<off_t>(8u << 20));
  }
  if (!wal_) {
    if (err) *err = "cannot open " + log + ": " + std::strerror(errno);
    close();
    return false;
  }
  struct stat st;
  wal_bytes_ = (::stat(log.c_str(), &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
  wal_flushed_ = wal_bytes_;
  wal_advised_ = 0;  // page-cache watermark restarts with the file
  if (!fsync_dir(dir_)) {
    if (err) *err = "cannot sync data directory";
    close();
    return false;
  }
  try {
    for (auto& kv : tables_) if (kv.second.has_index) build_vector_index(kv.second);
  } catch (const std::exception& e) {
    if (err) *err=std::string("cannot rebuild vector index: ")+e.what();
    close();
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- schema ---
bool Database::create_table(const std::string& name, const std::vector<Column>& cols,
                            std::string* err, int* code) {
  // Readers reject these bounds; never acknowledge a schema they cannot replay.
  if (name.size() > kMaxStr || cols.size() > kMaxCols || tables_.size() >= 100000) {
    set_err(err, code, ErrCode::BAD_VALUE, "schema exceeds durable format limit");
    return false;
  }
  uint64_t schema_bytes = 10 + 4 + static_cast<uint64_t>(name.size()) + 4 + 8;
  for (const Column& c : cols) {
    schema_bytes += 4 + static_cast<uint64_t>(c.name.size()) + 1 + 4 + 1;
    if (c.name.size() > kMaxStr || schema_bytes > kMaxRec ||
        (c.type == Type::VECTOR && c.len > static_cast<int32_t>(kMaxDim))) {
      set_err(err, code, ErrCode::BAD_VALUE, "schema exceeds durable format limit");
      return false;
    }
  }
  if (name.empty()) {
    set_err(err, code, ErrCode::SYNTAX, "empty table name");
    return false;
  }
  if (tables_.count(name)) {
    set_err(err, code, ErrCode::TABLE_EXISTS, "table '" + name + "' already exists");
    return false;
  }
  if (cols.empty()) {
    set_err(err, code, ErrCode::BAD_VALUE, "table '" + name + "' has no columns");
    return false;
  }
  int pk_count = 0, vec_count = 0;
  int pk_idx = -1, vec_idx = -1;
  for (size_t i = 0; i < cols.size(); ++i) {
    const Column& c = cols[i];
    if (c.name.empty()) {
      set_err(err, code, ErrCode::BAD_VALUE, "empty column name in table '" + name + "'");
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (cols[j].name == c.name) {
        set_err(err, code, ErrCode::BAD_VALUE,
                "duplicate column name '" + c.name + "' in table '" + name + "'");
        return false;
      }
    }
    switch (c.type) {
      case Type::BIGINT:
        break;
      case Type::VARCHAR:
        if (c.len <= 0) {
          set_err(err, code, ErrCode::BAD_VALUE,
                  "column '" + c.name + "': VARCHAR length must be > 0");
          return false;
        }
        break;
      case Type::VECTOR:
        if (c.len <= 0) {
          set_err(err, code, ErrCode::BAD_VALUE,
                  "column '" + c.name + "': VECTOR dimension must be > 0");
          return false;
        }
        ++vec_count;
        vec_idx = static_cast<int>(i);
        break;
      default:
        set_err(err, code, ErrCode::BAD_VALUE, "column '" + c.name + "': unknown type");
        return false;
    }
    if (c.is_primary) {
      ++pk_count;
      pk_idx = static_cast<int>(i);
      if (c.type != Type::BIGINT) {
        set_err(err, code, ErrCode::BAD_VALUE,
                "primary key column '" + c.name + "' must be BIGINT");
        return false;
      }
    }
  }
  if (pk_count == 0) {
    set_err(err, code, ErrCode::BAD_VALUE, "table '" + name + "' has no primary key");
    return false;
  }
  if (pk_count > 1) {
    set_err(err, code, ErrCode::BAD_VALUE, "table '" + name + "' has multiple primary keys");
    return false;
  }
  if (vec_count > 1) {
    set_err(err, code, ErrCode::BAD_VALUE, "table '" + name + "' has more than one VECTOR column");
    return false;
  }

  Table t;
  t.schema.name = name;
  t.storage_dir = dir_;
  t.schema.cols = cols;
  t.schema.pk_idx = pk_idx;
  t.schema.vec_idx = vec_idx;
  t.rows.reserve(1024);
  t.dead.reserve(1024);
  const size_t expect = t.rows.capacity();
  t.pk_pos.reserve(expect / 2 + 16);

  tables_.emplace(name, std::move(t));
  if (txn_active_) {
    Undo u;
    u.k = Undo::K_CREATE;
    u.table = name;
    undo_.push_back(std::move(u));
  }
  // WAL marker so that replay is order-faithful even if the catalog write is lost.
  {
    const TableSchema& s = tables_.find(name)->second.schema;
    std::string pay;
    put_u8(pay, REC_CREATE_TABLE);
    put_u8(pay, txn_active_ ? RF_TXN : 0);
    put_u64(pay, ++wal_rows_);
    put_schema(pay, s);
    uint32_t len = static_cast<uint32_t>(pay.size());
    uint32_t crc = crc32_of(pay.data(), pay.size());
    std::string rec;
    rec.append(reinterpret_cast<const char*>(&len), 4);
    rec.append(reinterpret_cast<const char*>(&crc), 4);
    rec.append(pay);
    if (!append_wal(rec) || (!txn_active_ && !fsync_wal())) {
      tables_.erase(name);
      if (txn_active_) undo_.pop_back();
      set_err(err, code, ErrCode::TXN, "cannot persist CREATE TABLE");
      return false;
    }
  }
  return true;
}

bool Database::drop_table(const std::string& name, std::string* err, int* code) {
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + name + "' does not exist");
    return false;
  }
  it->second.vector_index.reset();
  if (txn_active_) {
    Undo u;
    u.k = Undo::K_DROP;
    u.table = name;
    u.dropped.reset(new Table(std::move(it->second)));
    undo_.push_back(std::move(u));
    tables_.erase(it);
  } else {
    tables_.erase(it);
  }
  std::string rec;
  {
    const uint64_t lsn = ++wal_rows_;
    std::string pay;
    put_u8(pay, REC_DROP_TABLE);
    put_u8(pay, txn_active_ ? RF_TXN : 0);
    put_u64(pay, lsn);
    put_str(pay, name);
    uint32_t len = static_cast<uint32_t>(pay.size());
    uint32_t crc = crc32_of(pay.data(), pay.size());
    rec.append(reinterpret_cast<const char*>(&len), 4);
    rec.append(reinterpret_cast<const char*>(&crc), 4);
    rec.append(pay);
    if (!append_wal(rec) || (!txn_active_ && !fsync_wal())) {
      set_err(err, code, ErrCode::TXN, "cannot persist DROP TABLE");
      return false;
    }
  }
  return true;
}

bool Database::has_table(const std::string& name) const { return tables_.count(name) != 0; }

Table* Database::table(const std::string& name) {
  auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : &it->second;
}

std::vector<std::string> Database::table_names() const {
  std::vector<std::string> v;
  v.reserve(tables_.size());
  for (const auto& kv : tables_) v.push_back(kv.first);
  std::sort(v.begin(), v.end());
  return v;
}

// ------------------------------------------------------------------- DML ---
void Database::apply_insert(const std::string& t, std::vector<Value> vals, int64_t* pk) {
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    if (pk) *pk = 0;
    return;
  }
  Table& tb = it->second;
  int64_t key = 0;
  if (tb.schema.pk_idx >= 0 && static_cast<size_t>(tb.schema.pk_idx) < vals.size())
    key = vals[static_cast<size_t>(tb.schema.pk_idx)].i;

  const size_t slot = tb.rows.size();
  Row row;
  row.rid = tb.next_rid;
  row.vals = std::move(vals);
  tb.append_row(std::move(row));
  try {
    enforce_payload_budget(tables_);
    tb.dead.push_back(0);
    const auto [pos, inserted] = tb.pk_pos.try_emplace(key, slot);
    if (!inserted) {
      if (pos->second < slot) tb.dead[pos->second] = 1;  // replay: last writer wins
      pos->second = slot;
    }
  } catch (...) {
    if (tb.dead.size() > slot) tb.dead.pop_back();
    if (tb.payload_store) tb.payload_store->end = tb.payload_offsets.back();
    discard_appended_row(tb);
    throw;
  }
  ++tb.next_rid;
  if (pk) *pk = key;
}

bool Database::append_wal(const std::string& rec) {
  if (!wal_) return false;
  wal_bytes_ += rec.size();  // logical end: a boundary may still be staged
  wal_buf_.append(rec);
  // Bound the staging buffer so one huge autocommit statement cannot pin memory.
  if (wal_buf_.size() >= (1u << 20)) return flush_wal();
  return true;
}

// Hands staged records to the kernel in a single write. wal_bytes_ is the
// logical end of the log and wal_flushed_ the physical one; the difference is
// exactly what wal_buf_ still holds.
bool Database::flush_wal() {
  if (!wal_) return false;
  if (wal_buf_.empty()) return true;
  size_t done = 0;
  write_all_fd(::fileno(wal_), wal_buf_.data(), wal_buf_.size(), &done);
  wal_flushed_ += done;
  const bool all = done == wal_buf_.size();
  wal_write_failed_ = !all;
  if (all) wal_buf_.clear();
  else wal_buf_.erase(0, done);
  return all;
}

bool Database::fsync_wal() {
  if (!wal_) return false;
  if (!flush_wal()) return false;
  if (std::fflush(wal_) != 0) return false;
  if (::fdatasync(::fileno(wal_)) != 0) return false;
#if !defined(MVDB_DISABLE_PAGE_ADVICE)
  // Durability is already complete: every byte of this log reached the medium.
  // The cached pages therefore hold nothing this session will read again, while
  // the platform memory metric samples the cgroup those pages are charged to.
  // Dropping them keeps the resident charge at the durable material instead of
  // at log bytes already persisted.
  // Only the bytes appended since the previous advice are still cached, so the
  // range walk stays proportional to new log traffic, never to log length.
  // The upper bound is the physical end of the log, never the logical one:
  // wal_bytes_ counts records still staged in wal_buf_, and those bytes are not
  // in the file yet, so advising them away would target the wrong range.
  // wal_flushed_ is exactly what the file holds after the sync above.
  const int fd=::fileno(wal_);
  const uint64_t end=wal_flushed_;
  if(end>wal_advised_) {
    ::posix_fadvise(fd,static_cast<off_t>(wal_advised_),static_cast<off_t>(end-wal_advised_),
                    POSIX_FADV_DONTNEED);
    wal_advised_=end;
  }
#endif
  return true;
}

bool Database::insert(const std::string& t, std::vector<Value> vals, std::string* err,
                      int* code) {
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return false;
  }
  Table& tb = it->second;
  const TableSchema& sc = tb.schema;
  if (vals.size() != sc.cols.size()) {
    set_err(err, code, ErrCode::BAD_VALUE,
            "column count mismatch: expected " + std::to_string(sc.cols.size()) + ", got " +
                std::to_string(vals.size()));
    return false;
  }
  for (size_t i = 0; i < vals.size(); ++i) {
    if (!check_value(sc.cols[i], &vals[i], err)) {
      if (code) *code = static_cast<int>(ErrCode::BAD_VALUE);
      return false;
    }
  }
  const int64_t key = vals[static_cast<size_t>(sc.pk_idx)].i;
  if (!row_fits_record(t, vals)) {
    set_err(err, code, ErrCode::BAD_VALUE, "row exceeds durable record limit");
    return false;
  }
  if (tb.pk_pos.count(key)) {
    set_err(err, code, ErrCode::DUP_KEY,
            "Duplicate entry '" + std::to_string(key) + "' for key 'PRIMARY'");
    return false;
  }

  const uint64_t lsn = wal_rows_ + 1;
  std::string rec;
  {
    // Serialise straight into the framed record: the payload is written once
    // and the length/CRC header is patched in place afterwards.
    rec.reserve(64 + vals.size() * 24 + 8);
    rec.append(8, '\0');
    put_u8(rec, REC_INSERT);
    put_u8(rec, txn_active_ ? RF_TXN : 0);
    put_u64(rec, lsn);
    put_str(rec, t);
    put_u32(rec, static_cast<uint32_t>(vals.size()));
    for (const Value& v : vals) put_value(rec, v);
    const uint32_t len = static_cast<uint32_t>(rec.size() - 8);
    const uint32_t crc = crc32_of(rec.data() + 8, len);
    std::memcpy(&rec[0], &len, 4);
    std::memcpy(&rec[4], &crc, 4);
  }
  // Prepare the rebuildable payload before logging the insert. A cache write
  // can fail even when the authoritative WAL still has space.
  const size_t undo_mark = undo_.size();
  Undo u;
  u.k = Undo::K_INS;
  u.table = t;
  u.pk = key;
  u.slot = tb.rows.size();
  undo_.push_back(std::move(u));
  try {
    apply_insert(t, std::move(vals), nullptr);
  } catch (const std::exception& ex) {
    undo_.pop_back();
    set_err(err, code, ErrCode::TXN, std::string("cannot write insert payload: ") + ex.what());
    return false;
  }

  const uint64_t before = wal_bytes_;
  const bool wrote = append_wal(rec);
  if (!wrote || (!txn_active_ && !fsync_wal())) {
    if (tb.payload_store) tb.payload_store->end = tb.payload_offsets.back();
    undo_to(undo_mark);
    --tb.next_rid;
    // The statement wrapper restores the full statement boundary. Direct
    // callers need the failed insert's own WAL suffix removed here.
    if (!stmt_active_ && wal_bytes_ != before && !truncate_wal(before, wal_rows_, err, code))
      return false;
    set_err(err, code, ErrCode::TXN,
            (wrote && !wal_write_failed_) ? "WAL fdatasync failed" : "WAL write failed");
    return false;
  }
  wal_rows_ = lsn;
  if (!txn_active_) {
    undo_.pop_back();
    if (wal_bytes_ > kAutoCheckpointBytes) {
      std::string ignored;
      checkpoint(&ignored);  // best effort; the row is already durable
    }
  }
  return true;
}

int64_t Database::update_rows(const std::string& t, const std::vector<Cond>& conds,
                              const std::vector<std::pair<std::string, Value>>& sets,
                              std::string* err, int* code) {
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return -1;
  }
  Table& tb = it->second;
  const TableSchema& sc = tb.schema;
  std::vector<int> setcol(sets.size(), -1);
  std::vector<Value> setvals;
  setvals.reserve(sets.size());
  for (size_t k = 0; k < sets.size(); ++k) {
    setcol[k] = sc.find(sets[k].first);
    if (setcol[k] < 0) {
      set_err(err, code, ErrCode::NO_SUCH_COLUMN,
              "Unknown column '" + sets[k].first + "' in 'field list'");
      return -1;
    }
    if (setcol[k] == sc.pk_idx) {
      set_err(err, code, ErrCode::NOT_SUPPORTED, "updating the primary key is not supported");
      return -1;
    }
    Value v=sets[k].second;
    if (!check_value(sc.cols[static_cast<size_t>(setcol[k])],&v,err)) {
      if (code) *code=static_cast<int>(ErrCode::BAD_VALUE);
      return -1;
    }
    setvals.push_back(std::move(v));
  }
  Pred pr;
  if (!compile_pred(tb, conds, &pr, err, code)) return -1;
  if (pr.never) return 0;

  int64_t affected = 0;
  size_t first=0,last=tb.rows.size();
  for (const RCond& cond : pr.c) {
    if (static_cast<int>(cond.col)!=sc.pk_idx || cond.op!=static_cast<uint8_t>(Cond::EQ)) continue;
    auto position=tb.pk_pos.find(cond.lit->i);
    if (position==tb.pk_pos.end()) return 0;
    first=position->second;last=first+1;
    break;
  }
  for (size_t i = first; i < last; ++i) {
    if (tb.dead[i]) continue;
    Row scratch;
    const Row& row = tb.read_row(i,scratch);
    if (!row_matches(pr, row)) continue;

    Row replacement = row;
    auto& nv = replacement.vals;
    for (size_t k = 0; k < sets.size(); ++k) {
      nv[static_cast<size_t>(setcol[k])] = setvals[k];
    }
    const int64_t oldpk = row.vals[static_cast<size_t>(sc.pk_idx)].i;
    if (!row_fits_record(t, nv)) {
      set_err(err, code, ErrCode::BAD_VALUE, "row exceeds durable record limit");
      return -1;
    }
    const int64_t newpk = nv[static_cast<size_t>(sc.pk_idx)].i;
    if (newpk != oldpk) {
      auto dup = tb.pk_pos.find(newpk);
      if (dup != tb.pk_pos.end()) {
        set_err(err, code, ErrCode::DUP_KEY,
                "Duplicate entry '" + std::to_string(newpk) + "' for key 'PRIMARY'");
        return -1;
      }
    }

    // Prepare every potentially allocating Undo operation before logging or
    // changing the row. Small updates keep the table resident; once the global
    // image allowance is exhausted, spill migrates all its active old images.
    Undo pending;
    std::list<std::weak_ptr<RowUndoImage>> pending_registration;
    if (txn_active_) {
      pending.k = Undo::K_UPD;
      pending.table = t;
      pending.pk = oldpk;
      pending.slot = i;
      if (undo_.size() == undo_.capacity())
        undo_.reserve(undo_.capacity() ? undo_.capacity() * 2 : 16);
      if (!tb.payload_store) {
        try {
          const uint64_t remaining = kUndoImageBytes - undo_image_bytes_;
          if (undo_image_bytes(row) <= remaining) {
            pending.old = std::make_shared<RowUndoImage>();
            pending.old->row = row;
            pending.image_bytes = undo_image_bytes(pending.old->row);
          }
          if (!pending.old || pending.image_bytes > remaining) {
            pending.old.reset();
            pending.image_bytes = 0;
            spill_payloads(tb,true);
          } else {
            pending_registration.push_back(pending.old);
          }
        } catch (const std::exception& ex) {
          set_err(err, code, ErrCode::TXN,
                  std::string("cannot prepare update payloads: ") + ex.what());
          return -1;
        }
      }
      if (tb.payload_store) {
        pending.old_spilled = true;
        pending.old_offset = tb.payload_offsets[i];
      }
    }

    // A cache write may fail independently of the WAL. Prepare the bytes
    // without changing the visible offset, before logging this row update.
    uint64_t replacement_offset = 0;
    if (tb.payload_store) {
      try {
        replacement_offset = store_payload(*tb.payload_store, replacement);
      } catch (const std::runtime_error& ex) {
        set_err(err, code, ErrCode::TXN, std::string("cannot write update payload: ") + ex.what());
        return -1;
      }
    }

    std::string rec;
    {
      const uint64_t lsn = ++wal_rows_;
      std::string pay;
      put_u8(pay, REC_UPDATE);
      put_u8(pay, txn_active_ ? RF_TXN : 0);
      put_u64(pay, lsn);
      put_str(pay, t);
      put_i64(pay, oldpk);
      put_u32(pay, static_cast<uint32_t>(nv.size()));
      for (const Value& v : nv) put_value(pay, v);
      uint32_t len = static_cast<uint32_t>(pay.size());
      uint32_t crc = crc32_of(pay.data(), pay.size());
      rec.append(reinterpret_cast<const char*>(&len), 4);
      rec.append(reinterpret_cast<const char*>(&crc), 4);
      rec.append(pay);
    }
    if (!append_wal(rec)) {
      set_err(err, code, ErrCode::TXN, "WAL write failed");
      return -1;
    }
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN,
              wal_write_failed_ ? "WAL write failed" : "WAL fdatasync failed");
      return -1;
    }

    if (txn_active_) {
      undo_image_bytes_ += pending.image_bytes;
      undo_.push_back(std::move(pending));
      tb.resident_undo.splice(tb.resident_undo.end(),pending_registration);
    }
    if (newpk != oldpk) {
      tb.pk_pos.erase(oldpk);
      tb.pk_pos[newpk] = i;
    }
    if (tb.payload_store) {
      tb.payload_offsets[i] = replacement_offset;
      tb.vector_index.reset();
    } else {
      tb.replace_row(i, std::move(replacement));
    }
    ++affected;
  }
  if (!txn_active_ && wal_bytes_ > kAutoCheckpointBytes) {
    std::string ignored;
    checkpoint(&ignored);
  }
  return affected;
}

int64_t Database::delete_rows(const std::string& t, const std::vector<Cond>& conds,
                              std::string* err, int* code) {
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return -1;
  }
  Table& tb = it->second;
  Pred pr;
  if (!compile_pred(tb, conds, &pr, err, code)) return -1;
  if (pr.never) return 0;
  tb.vector_index.reset();

  int64_t affected = 0;
  size_t first=0,last=tb.rows.size();
  for (const RCond& cond : pr.c) {
    if (static_cast<int>(cond.col)!=tb.schema.pk_idx || cond.op!=static_cast<uint8_t>(Cond::EQ)) continue;
    auto position=tb.pk_pos.find(cond.lit->i);
    if (position==tb.pk_pos.end()) return 0;
    first=position->second;last=first+1;
    break;
  }
  for (size_t i = first; i < last; ++i) {
    if (tb.dead[i]) continue;
    Row scratch;
    const Row& row = tb.read_row(i,scratch);
    if (!row_matches(pr, row)) continue;
    const int64_t pk = row.vals[static_cast<size_t>(tb.schema.pk_idx)].i;

    std::string rec;
    {
      const uint64_t lsn = ++wal_rows_;
      std::string pay;
      put_u8(pay, REC_DELETE);
      put_u8(pay, txn_active_ ? RF_TXN : 0);
      put_u64(pay, lsn);
      put_str(pay, t);
      put_i64(pay, pk);
      uint32_t len = static_cast<uint32_t>(pay.size());
      uint32_t crc = crc32_of(pay.data(), pay.size());
      rec.append(reinterpret_cast<const char*>(&len), 4);
      rec.append(reinterpret_cast<const char*>(&crc), 4);
      rec.append(pay);
    }
    if (!append_wal(rec)) {
      set_err(err, code, ErrCode::TXN, "WAL write failed");
      return -1;
    }
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN,
              wal_write_failed_ ? "WAL write failed" : "WAL fdatasync failed");
      return -1;
    }

    if (txn_active_) {
      Undo u;
      u.k = Undo::K_DEL;
      u.table = t;
      u.pk = pk;
      u.slot = i;
      undo_.push_back(std::move(u));
    }
    tb.dead[i] = 1;
    tb.pk_pos.erase(pk);
    ++affected;
  }
  if (!txn_active_ && wal_bytes_ > kAutoCheckpointBytes) {
    std::string ignored;
    checkpoint(&ignored);
  }
  return affected;
}

// ------------------------------------------------------------------ scan ---
void Database::scan(const std::string& t, const std::vector<Cond>& conds,
                    const std::function<bool(const Row&)>& cb) {
  auto it = tables_.find(t);
  if (it == tables_.end()) return;
  const Table& tb = it->second;
  Pred pr;
  if (!compile_pred(tb, conds, &pr) || pr.never) return;

  const uint8_t* dead = tb.dead.data();
  const size_t n = tb.rows.size();

  // Primary key equality: jump straight to the row.
  if (!pr.c.empty()) {
    for (const RCond& c : pr.c) {
      if (static_cast<int>(c.col) == tb.schema.pk_idx && c.op == static_cast<uint8_t>(Cond::EQ)) {
        auto pos = tb.pk_pos.find(c.lit->i);
        if (pos == tb.pk_pos.end()) return;
        const size_t s = pos->second;
        if (s < n && !dead[s]) {
          Row scratch;
          const Row& row=tb.read_row(s,scratch);
          if (row_matches(pr,row)) cb(row);
        }
        return;
      }
    }
  }

  for (size_t i = 0; i < n; ++i) {
    if (dead[i]) continue;
    Row scratch;
    const Row& row=tb.read_row(i,scratch);
    if (!row_matches(pr, row)) continue;
    if (!cb(row)) return;
  }
}

// ------------------------------------------------------------ transactions -
bool Database::begin(std::string* err, int* code) {
  if (txn_active_) {
    set_err(err, code, ErrCode::TXN, "a transaction is already in progress");
    return false;
  }
  txn_wal_start_ = wal_bytes_;
  txn_lsn_start_ = wal_rows_;
  undo_.clear();
  undo_image_bytes_ = 0;
  std::string pay;
  const uint64_t lsn = ++wal_rows_;
  put_u8(pay, REC_TXN_BEGIN);
  put_u8(pay, RF_TXN);
  put_u64(pay, lsn);
  put_str(pay, std::string());
  uint32_t len = static_cast<uint32_t>(pay.size());
  uint32_t crc = crc32_of(pay.data(), pay.size());
  std::string rec;
  rec.append(reinterpret_cast<const char*>(&len), 4);
  rec.append(reinterpret_cast<const char*>(&crc), 4);
  rec.append(pay);
  if (!append_wal(rec)) {
    truncate_wal(txn_wal_start_, txn_lsn_start_, err, code);
    set_err(err, code, ErrCode::TXN, "cannot write transaction begin");
    return false;
  }
  txn_active_ = true;
  return true;
}

bool Database::commit(std::string* err, int* code) {
  if (txn_active_) {
    const uint64_t before_bytes = wal_bytes_, before_lsn = wal_rows_;
    std::string pay;
    const uint64_t lsn = ++wal_rows_;
    put_u8(pay, REC_TXN_COMMIT);
    put_u8(pay, RF_TXN);
    put_u64(pay, lsn);
    put_str(pay, std::string());
    uint32_t len = static_cast<uint32_t>(pay.size());
    uint32_t crc = crc32_of(pay.data(), pay.size());
    std::string rec;
    rec.append(reinterpret_cast<const char*>(&len), 4);
    rec.append(reinterpret_cast<const char*>(&crc), 4);
    rec.append(pay);
    if (!append_wal(rec) || !fsync_wal()) {
      truncate_wal(before_bytes, before_lsn, err, code);
      set_err(err, code, ErrCode::TXN, "cannot persist transaction commit");
      return false;
    }
    undo_.clear();
    undo_image_bytes_ = 0;
    for (auto& table : tables_) table.second.resident_undo.clear();
    undo_.shrink_to_fit();
    txn_active_ = false;
  }
  if (wal_bytes_ > kAutoCheckpointBytes) {
    std::string ignored;
    checkpoint(&ignored);
  }
  return true;
}

void Database::undo_to(size_t mark) {
  for (size_t i = undo_.size(); i-- > mark;) {
    Undo& u = undo_[i];
    undo_image_bytes_ -= u.image_bytes;
    if (u.k == Undo::K_CREATE) {
      tables_.erase(u.table);
      continue;
    }
    if (u.k == Undo::K_DROP) {
      if (u.dropped) {
        Table t = std::move(*u.dropped);
        u.dropped.reset();
        tables_[u.table] = std::move(t);
      }
      continue;
    }
    auto it = tables_.find(u.table);
    if (it == tables_.end()) continue;  // table dropped later in the txn
    Table& tb = it->second;
    tb.vector_index.reset();
    switch (u.k) {
      case Undo::K_INS: {
        auto pos = tb.pk_pos.find(u.pk);
        if (pos != tb.pk_pos.end()) {
          if (pos->second < tb.dead.size()) tb.dead[pos->second] = 1;
          tb.pk_pos.erase(pos);
        }
        // Undo runs in reverse insertion order; reclaim appended payloads too.
        if (!tb.rows.empty() && u.slot == tb.rows.size() - 1) {
          discard_appended_row(tb);
          tb.dead.pop_back();
        }
        break;
      }
      case Undo::K_UPD: {
        if (u.old_spilled && u.slot < tb.rows.size()) {
          tb.payload_offsets[u.slot]=u.old_offset;
          break;
        }
        if (u.slot < tb.rows.size() && u.old) {
          if (u.old->spilled) {
            tb.payload_offsets[u.slot] = u.old->offset;
          } else {
            // A live memory image is registered with the table: every spill
            // converts it first, so this branch cannot require rollback I/O.
            tb.payload_bytes -= row_heap_bytes(tb.rows[u.slot]);
            tb.payload_bytes += row_heap_bytes(u.old->row);
            tb.rows[u.slot] = std::move(u.old->row);
          }
          tb.pk_pos[u.pk] = u.slot;
        }
        break;
      }
      case Undo::K_DEL: {
        if (u.slot < tb.rows.size() && u.slot < tb.dead.size()) {
          tb.dead[u.slot] = 0;
          tb.pk_pos[u.pk] = u.slot;
        }
        break;
      }
      case Undo::K_INDEX:
        tb.has_index = u.old_index;
        break;
      default:
        break;
    }
  }
  undo_.resize(mark);
  for (auto& table : tables_)
    table.second.resident_undo.remove_if(
        [](const std::weak_ptr<RowUndoImage>& image) { return image.expired(); });
}

bool Database::truncate_wal(uint64_t bytes, uint64_t lsn, std::string* err, int* code) {
  if (!wal_) {
    set_err(err, code, ErrCode::TXN, "cannot restore WAL boundary");
    return false;
  }
  if (bytes >= wal_flushed_) {
    // The boundary is at or past the end of the file: only the staged suffix has
    // to go, and the file is left untouched so a boundary that never reached
    // disk stays invisible to the filesystem.
    wal_buf_.resize(static_cast<size_t>(bytes - wal_flushed_));
  } else {
    wal_buf_.clear();
    if (::ftruncate(::fileno(wal_), static_cast<off_t>(bytes)) != 0 ||
        ::fdatasync(::fileno(wal_)) != 0) {
      set_err(err, code, ErrCode::TXN, "cannot restore WAL boundary");
      return false;
    }
    wal_flushed_ = bytes;
  }
  if (wal_advised_ > wal_flushed_) wal_advised_ = wal_flushed_;
  wal_bytes_ = bytes;
  wal_rows_ = lsn;
  return true;
}

bool Database::rollback(std::string* err, int* code) {
  if (!txn_active_) return true;
  if (!truncate_wal(txn_wal_start_, txn_lsn_start_, err, code)) return false;
  undo_to(0);
  txn_active_ = false;
  return true;
}

bool Database::begin_statement(std::string* err, int* code) {
  if (stmt_active_) {
    set_err(err, code, ErrCode::TXN, "statement already active");
    return false;
  }
  stmt_wal_start_ = wal_bytes_;
  stmt_lsn_start_ = wal_rows_;
  stmt_implicit_ = !txn_active_;
  if (stmt_implicit_ && !begin(err, code)) return false;
  stmt_undo_start_ = undo_.size();
  stmt_active_ = true;
  return true;
}

bool Database::end_statement(bool success, std::string* err, int* code) {
  if (!stmt_active_) {
    set_err(err, code, ErrCode::TXN, "no active statement");
    return false;
  }
  if (success) enforce_payload_budget(tables_);
  if (success && stmt_implicit_) success = commit(err, code);
  if (!success) {
    // No committed transaction can contain these records: the boundary precedes
    // this statement and the explicit transaction's previous statements remain.
    if (!truncate_wal(stmt_wal_start_, stmt_lsn_start_, err, code)) {
      stmt_active_ = false;
      return false;
    }
    undo_to(stmt_undo_start_);
    if (stmt_implicit_) txn_active_ = false;
  }
  stmt_active_ = false;
  return success;
}

// ----------------------------------------------------------- vector index --
bool Database::create_vector_index(const std::string& t, const std::string& col,
                                   const std::string& name, std::string* err, int* code) {
  (void)name;  // the schema permits only one vector column/index per table
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return false;
  }
  const int idx = it->second.schema.find(col);
  if (idx < 0) {
    set_err(err, code, ErrCode::NO_SUCH_COLUMN, "Unknown column '" + col + "'");
    return false;
  }
  if (it->second.schema.cols[static_cast<size_t>(idx)].type != Type::VECTOR) {
    set_err(err, code, ErrCode::BAD_VALUE, "column '" + col + "' is not a VECTOR column");
    return false;
  }
  try {
    build_vector_index(it->second);
  } catch (const std::exception& e) {
    set_err(err,code,ErrCode::TXN,std::string("cannot build vector index: ")+e.what());
    return false;
  }
  std::string pay, rec;
  put_u8(pay, REC_CREATE_INDEX);
  put_u8(pay, txn_active_ ? RF_TXN : 0);
  put_u64(pay, ++wal_rows_);
  put_str(pay, t);
  put_u32(rec, static_cast<uint32_t>(pay.size()));
  put_u32(rec, crc32_of(pay.data(), pay.size()));
  rec.append(pay);
  if (!append_wal(rec) || (!txn_active_ && !fsync_wal())) {
    it->second.vector_index.reset();
    set_err(err, code, ErrCode::TXN, "cannot persist index metadata");
    return false;
  }
  if (txn_active_) {
    Undo u;
    u.k = Undo::K_INDEX;
    u.table = t;
    u.old_index = it->second.has_index;
    undo_.push_back(std::move(u));
  }
  it->second.has_index = true;
  return true;
}

bool Database::has_vector_index(const std::string& t) const {
  auto it = tables_.find(t);
  return it != tables_.end() && it->second.has_index;
}

bool Database::resident_vector_view(const std::string& t, FlatIndex::ResidentView* view) {
  if(!view)return false;
  *view=FlatIndex::ResidentView{};
  auto it=tables_.find(t);
  if(it==tables_.end() || !it->second.has_index || it->second.schema.vec_idx<0)return false;
  build_vector_index(it->second);
  return it->second.vector_index->resident_view(view);
}

FlatIndex* Database::resident_vector_index(const std::string& t) {
  auto it=tables_.find(t);
  if(it==tables_.end() || !it->second.has_index || it->second.schema.vec_idx<0)return nullptr;
  build_vector_index(it->second);
  return it->second.vector_index.get();
}

void Database::build_vector_index(Table& table) {
  if (table.vector_index) return;
  if (table.schema.vec_idx < 0) throw std::runtime_error("table has no vector column");
#ifndef MVDB_FLAT_INDEX_MEMORY_BYTES
#define MVDB_FLAT_INDEX_MEMORY_BYTES (16ull << 20)
#endif
  size_t budget=MVDB_FLAT_INDEX_MEMORY_BYTES;
  for (const auto& kv : tables_) {
    if (!kv.second.vector_index) continue;
    const size_t used=kv.second.vector_index->resident_bytes();
    budget-=std::min(budget,used);
  }
  const size_t dimension=static_cast<size_t>(table.schema.cols[table.schema.vec_idx].len);
  const size_t count=table.live_count();
  // A resident index borrows the row payloads instead of copying them, so it can
  // only be built over rows this table holds in memory. A spilled table keeps its
  // payloads in the rebuildable file and gets the disk-backed index, whose
  // records are copies in its own file, exactly as before.
  auto index=std::make_shared<FlatIndex>(dir_,dimension,count,table.payload_store?0:budget);
  auto add_slot=[&](size_t slot) {
    Row scratch;
    const Row& row=table.read_row(slot,scratch);
    const auto& vector=row.vals[table.schema.vec_idx].v;
    index->add(row.vals[table.schema.pk_idx].i,slot,vector.data(),vector.size());
  };
#ifndef MVDB_DISABLE_NORM_ORDER
  struct NormSlot { double norm;size_t slot; };
  constexpr size_t norm_metadata_budget=1u<<20;
  // This changes only traversal order. Keep one bounded temporary metadata
  // array, with no row copies or extra payload reads from spilled tables.
  std::unique_ptr<NormSlot[]> ordered;
  if(!index->disk_backed() && !table.payload_store && count>1 &&
     count<=norm_metadata_budget/sizeof(NormSlot) &&
     count<=(budget-std::min(budget,index->resident_bytes()))/sizeof(NormSlot)) {
    ordered.reset(new(std::nothrow) NormSlot[count]);
    if(ordered) {
      size_t used=0;
      for(size_t slot=0;slot<table.rows.size();++slot) {
        if(table.dead[slot])continue;
        double norm=0;
        for(float value:table.rows[slot].vals[table.schema.vec_idx].v) {
          const double component=value;norm+=component*component;
        }
        if(used==count)throw std::runtime_error("excess flat index norm ordering row");
        ordered[used++]={norm,slot};
      }
      if(used!=count)throw std::runtime_error("incomplete flat index norm ordering");
      std::sort(ordered.get(),ordered.get()+count,[](const NormSlot& a,const NormSlot& b) {
        return a.norm<b.norm || (a.norm==b.norm && a.slot<b.slot);
      });
    }
  }
  if(ordered) {
    for(size_t i=0;i<count;++i)add_slot(ordered[i].slot);
  } else
#endif
  {
    for(size_t slot=0;slot<table.rows.size();++slot)
      if(!table.dead[slot])add_slot(slot);
  }
#ifndef MVDB_DISABLE_NORM_ORDER
  ordered.reset();
#endif
  index->finish();
  table.vector_index=std::move(index);
}

void Database::scan_vectors(const std::string& t,const std::vector<Cond>& conds,
                           const std::function<bool(int64_t,const float*,size_t)>& cb,
                           const QuantizedQueryBound* quantized,const double* threshold) {
  auto it=tables_.find(t);
  if (it==tables_.end() || it->second.schema.vec_idx<0) return;
  Table& table=it->second;
  if (!table.has_index) {
    scan(t,conds,[&](const Row& row) {
      const auto& vector=row.vals[table.schema.vec_idx].v;
      return cb(row.vals[table.schema.pk_idx].i,vector.data(),vector.size());
    });
    return;
  }
  Pred predicate;
  if (!compile_pred(table, conds, &predicate) || predicate.never) return;
  build_vector_index(table);
  // Exact equality subset. When the whole predicate is a single BIGINT equality
  // the posting list is already the complete filter, so the candidate walk
  // needs neither a row materialisation nor a second predicate evaluation: the
  // already-quantised vector storage is read in place. Every candidate still
  // takes the caller's exact distance path, so results are unchanged.
  if(predicate.c.size()==1) {
    const RCond& term=predicate.c.front();
    if(term.op==static_cast<uint8_t>(Cond::EQ) &&
       term.vt==static_cast<uint8_t>(Type::BIGINT)) {
      FlatIndex::ResidentView view;
      if(table.vector_index->resident_view(&view)) {
        const uint32_t* ordinals=nullptr;
        size_t count=0;
        if(table.vector_index->equal_ordinals(term.col,term.lit->i,
            [&](uint64_t slot) {
              Row scratch;
              return table.read_row(static_cast<size_t>(slot),scratch).vals[term.col].i;
            },&ordinals,&count)) {
          // The same certified int8 rejection the unfiltered scan uses: a row
          // proven farther than the current k-th distance never reaches the
          // float bound or the exact kernel.
          const int8_t* codes=view.quantized;
          const size_t dimension=view.dimension;
          const bool reject=codes && threshold && quantized && quantized->enabled();
          for(size_t j=0;j<count;++j) {
            const size_t i=ordinals[j];
            const uint64_t slot=view.slots[i];
            if(slot>=table.rows.size() || (slot<table.dead.size() && table.dead[slot]))
              throw std::runtime_error("stale flat vector index slot");
            if(reject && quantized->definitely_greater(codes+i*dimension,*threshold))continue;
            if(!cb(view.keys[i],view.rows[i],dimension))return;
          }
          return;
        }
      }
    }
  }
  const FlatIndex::Visitor visit=[&](int64_t pk,uint64_t slot,const float* vector,size_t dimension) {
    if (slot>=table.rows.size() || table.dead[static_cast<size_t>(slot)])
      throw std::runtime_error("stale flat vector index slot");
    if (!conds.empty()) {
      Row scratch;
      if (!row_matches(predicate,table.read_row(static_cast<size_t>(slot),scratch))) return true;
    }
    return cb(pk,vector,dimension);
  };
  // Use the already-coerced predicate, never reinterpret SQL literals. The
  // posting list only selects candidates; all conjunction terms are checked
  // by the same row_matches path above. Its lifetime is the physical index's,
  // so every mutation/rebuild also invalidates these scalar postings.
  for(const RCond& term:predicate.c) {
    if(term.op!=static_cast<uint8_t>(Cond::EQ) ||
       term.vt!=static_cast<uint8_t>(Type::BIGINT))continue;
    if(table.vector_index->scan_integer_equal(term.col,term.lit->i,
        [&](uint64_t slot) {
          Row scratch;
          return table.read_row(static_cast<size_t>(slot),scratch).vals[term.col].i;
        },visit))return;
    break;
  }
  table.vector_index->scan(visit);
}

// ------------------------------------------------------------- durability --
namespace {

// Decode the original format without mapping the entire file. The byte
// budget excludes the trailing checksum, which is validated before decoding.
class SnapshotReader {
 public:
  SnapshotReader(FILE* file, size_t size) : file_(file), remaining_(size) {}

  size_t left() const { return remaining_; }

  bool need(size_t n) {
    if (n > remaining_) bad = true;
    return !bad;
  }

  bool read(void* out, size_t n) {
    if (!need(n)) return false;
    auto* dest = static_cast<uint8_t*>(out);
    while (n != 0) {
      if (cursor_ == filled_) {
        cursor_ = 0;
        filled_ = std::min(remaining_, sizeof(buffer_));
        if (std::fread(buffer_, 1, filled_, file_) != filled_) {
          bad = true;
          return false;
        }
      }
      const size_t take = std::min(n, filled_ - cursor_);
      std::memcpy(dest, buffer_ + cursor_, take);
      dest += take;
      cursor_ += take;
      remaining_ -= take;
      n -= take;
    }
    return true;
  }

  uint8_t u8() {
    uint8_t value = 0;
    read(&value, 1);
    return value;
  }

  uint32_t u32() {
    uint8_t bytes[4];
    return read(bytes, sizeof(bytes)) ? Rd(bytes, sizeof(bytes)).u32() : 0;
  }

  uint64_t u64() {
    uint8_t bytes[8];
    return read(bytes, sizeof(bytes)) ? Rd(bytes, sizeof(bytes)).u64() : 0;
  }

  int64_t i64() { return static_cast<int64_t>(u64()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }

  bool str(std::string* out) {
    const uint32_t n = u32();
    if (n > kMaxStr || !need(n)) {
      bad = true;
      return false;
    }
    out->resize(n);
    return read(out->data(), n);
  }

  bool bad = false;

 private:
  FILE* file_;
  size_t remaining_;
  size_t cursor_ = 0;
  size_t filled_ = 0;
  uint8_t buffer_[64 * 1024];
};

}  // namespace

bool Database::load_snapshot(std::string* err) {
  tables_.clear();
  wal_rows_ = 0;

  const std::string path = dir_ + "/snapshot.bin";
  std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!file) {
    if (errno == ENOENT) return true;
    if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
    return false;
  }
  struct stat st{};
  if (::fstat(::fileno(file.get()), &st) != 0) {
    if (err) *err = "cannot determine snapshot size";
    return false;
  }
  if (st.st_size < 20) {
    if (err) *err = "snapshot.bin: truncated";
    return false;
  }
  const size_t payload_size = static_cast<size_t>(st.st_size) - 4;
  // Verify the CRC before allocating tables or creating their spill files.
  {
    uint32_t crc = 0xFFFFFFFFu;
    char buf[262144];
    size_t remaining = payload_size;
    while (remaining != 0) {
      const size_t want = std::min(sizeof(buf), remaining);
      if (std::fread(buf, 1, want, file.get()) != want) {
        if (err) *err = "snapshot.bin: short read";
        return false;
      }
      crc = crc32_upd(crc, buf, want);
      remaining -= want;
    }
    uint8_t tail[4];
    if (std::fread(tail, 1, sizeof(tail), file.get()) != sizeof(tail)) {
      if (err) *err = "snapshot.bin: short read";
      return false;
    }
    if ((crc ^ 0xFFFFFFFFu) != Rd(tail, sizeof(tail)).u32()) {
      if (err) *err = "snapshot.bin: CRC mismatch";
      return false;
    }
  }
  if (::fseeko(file.get(), 0, SEEK_SET) != 0) {
    if (err) *err = "cannot rewind snapshot";
    return false;
  }

  SnapshotReader r(file.get(), payload_size);
  if (r.u32() != kSnapMagic) {
    if (err) *err = "snapshot.bin: bad magic";
    return false;
  }
  const uint64_t lsn = r.u64();
  const uint32_t ntab = r.u32();
  if (r.bad || ntab > 100000) {
    if (err) *err = "snapshot.bin: bad table count";
    return false;
  }
  for (uint32_t ti = 0; ti < ntab; ++ti) {
    Table table;
    table.storage_dir = dir_;
    if (!get_schema(r, &table.schema) || table.schema.name.empty()) {
      if (err) *err = "snapshot.bin: bad schema";
      return false;
    }
    const size_t pk_column = static_cast<size_t>(table.schema.pk_idx);
    if (pk_column >= table.schema.cols.size() ||
        table.schema.cols[pk_column].type != Type::BIGINT) {
      if (err) *err = "snapshot.bin: bad primary key column";
      return false;
    }
    const int64_t next_rid = r.i64();
    table.has_index = r.u8() != 0;
    const uint64_t nrows = r.u64();
    // Each row has a row ID, field count and at least its BIGINT primary key.
    constexpr size_t min_row_bytes = 8 + 4 + 1 + 8;
    if (r.bad || nrows > (1ull << 34) || nrows > r.left() / min_row_bytes) {
      if (err) *err = "snapshot.bin: bad row count";
      return false;
    }
    if (tables_.count(table.schema.name)) {
      if (err) *err = "snapshot.bin: duplicate table";
      return false;
    }
    table.rows.reserve(static_cast<size_t>(nrows));
    table.dead.reserve(static_cast<size_t>(nrows));
    table.pk_pos.reserve(static_cast<size_t>(nrows) / 2 + 16);
    table.next_rid = next_rid > 0 ? next_rid : 1;
    for (uint64_t k = 0; k < nrows; ++k) {
      Row row;
      row.rid = r.i64();
      if (!get_row(r, &row) || row.vals.size() != table.schema.cols.size() ||
          row.vals[pk_column].type != Type::BIGINT || row.rid == INT64_MAX) {
        if (err) *err = "snapshot.bin: bad row";
        return false;
      }
      if (row.rid >= table.next_rid) table.next_rid = row.rid + 1;
      if (!table.pk_pos.emplace(row.vals[pk_column].i, table.rows.size()).second) {
        if (err) *err = "snapshot.bin: duplicate primary key";
        return false;
      }
      table.append_row(std::move(row));
      table.dead.push_back(0);
    }
    const std::string table_name = table.schema.name;
    tables_.emplace(table_name, std::move(table));
    enforce_payload_budget(tables_);
  }
  if (r.bad || r.left() != 0) {
    if (err) *err = "snapshot.bin: unexpected trailing bytes";
    return false;
  }
  wal_rows_ = lsn;
  return true;
}

bool Database::replay_wal(std::string* err) {
  const std::string path = dir_ + "/data.log";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (errno == ENOENT) return true;
    if (err) *err = "cannot read WAL: " + std::string(std::strerror(errno));
    return false;
  }
  struct stat wal_stat;
  if (::fstat(::fileno(f), &wal_stat) != 0 || wal_stat.st_size < 0) {
    std::fclose(f);
    if (err) *err = "cannot determine WAL size";
    return false;
  }
  const auto wal_size = static_cast<uint64_t>(wal_stat.st_size);
  const uint64_t snap_lsn = wal_rows_;

  struct Rec {
    uint8_t kind = 0;
    uint8_t flags = 0;
    uint64_t lsn = 0;
    std::string name;
    TableSchema schema;
    std::vector<Value> vals;
    int64_t pk = 0;
  };

  auto apply_one = [this](Rec& rec) {
    auto it = tables_.find(rec.name);
    switch (rec.kind) {
      case REC_INSERT:
        apply_insert(rec.name, std::move(rec.vals), nullptr);
        break;
      case REC_UPDATE: {
        if (it == tables_.end()) break;
        Table& tb = it->second;
        auto pos = tb.pk_pos.find(rec.pk);
        if (pos == tb.pk_pos.end()) break;
        const size_t s = pos->second;
        if (s >= tb.rows.size()) break;
        const int64_t newpk = (static_cast<size_t>(tb.schema.pk_idx) < rec.vals.size())
                                  ? rec.vals[static_cast<size_t>(tb.schema.pk_idx)].i
                                  : rec.pk;
        if (newpk != rec.pk) {
          tb.pk_pos.erase(pos);
          tb.pk_pos[newpk] = s;
        }
        Row replacement; replacement.rid=tb.rows[s].rid;
        replacement.vals=std::move(rec.vals);
        tb.replace_row(s,std::move(replacement));
        enforce_payload_budget(tables_);
        break;
      }
      case REC_DELETE: {
        if (it == tables_.end()) break;
        Table& tb = it->second;
        tb.vector_index.reset();
        auto pos = tb.pk_pos.find(rec.pk);
        if (pos == tb.pk_pos.end()) break;
        const size_t s = pos->second;
        if (s < tb.dead.size()) tb.dead[s] = 1;
        tb.pk_pos.erase(pos);
        break;
      }
      case REC_CREATE_TABLE: {
        auto ex = tables_.find(rec.schema.name);
        if (ex != tables_.end()) {
          if (same_schema(ex->second.schema, rec.schema)) break;  // already materialised
          tables_.erase(ex);
        }
        Table t;
        t.storage_dir=dir_;
        t.schema = rec.schema;
        tables_.emplace(t.schema.name, std::move(t));
        break;
      }
      case REC_DROP_TABLE:
        tables_.erase(rec.name);
        break;
      case REC_CREATE_INDEX:
        if (it != tables_.end()) it->second.has_index = true;
        break;
      default:
        break;
    }
  };

  auto decode = [](const std::string& pay, Rec* out) {
    Rd r(pay.data(), pay.size());
    Rec& rec = *out;
    rec.kind = r.u8();
    rec.flags = r.u8();
    rec.lsn = r.u64();
    bool ok = !r.bad;
    if (ok) {
      switch (rec.kind) {
        case REC_INSERT:
        case REC_UPDATE:
        case REC_DELETE:
        case REC_CREATE_INDEX:
        case REC_DROP_TABLE: {
          ok = r.str(&rec.name);
          if (ok && (rec.kind == REC_UPDATE || rec.kind == REC_DELETE)) rec.pk = r.i64();
          if (ok && rec.kind == REC_INSERT) {
            uint32_t n = r.u32();
            ok = !r.bad && n <= kMaxCols;
            if (ok) {
              rec.vals.resize(n);
              for (uint32_t k = 0; k < n && ok; ++k) ok = get_value(r, &rec.vals[k]);
            }
          } else if (ok && rec.kind == REC_UPDATE) {
            uint32_t n = r.u32();
            ok = !r.bad && n <= kMaxCols;
            if (ok) {
              rec.vals.resize(n);
              for (uint32_t k = 0; k < n && ok; ++k) ok = get_value(r, &rec.vals[k]);
            }
          }
          ok = ok && !r.bad;
          break;
        }
        case REC_CREATE_TABLE:
          ok = get_schema(r, &rec.schema);
          break;
        case REC_TXN_BEGIN:
        case REC_TXN_COMMIT:
          break;
        default:
          ok = false;
          break;
      }
    }
    return ok;
  };

  uint64_t pending_start = UINT64_MAX; // WAL offset, not a transaction-sized heap buffer
  uint64_t good_end = 0;     // byte offset just past the last valid record
  uint64_t durable_end = 0;  // last replayed commit or snapshot-covered record
  uint64_t durable_lsn = snap_lsn;
  bool torn = false;

  for (;;) {
    uint8_t hdr[8];
    size_t got = std::fread(hdr, 1, 8, f);
    if (got == 0) break;
    if (got != 8) { torn = true; break; }
    uint32_t len = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                   (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    uint32_t crc = static_cast<uint32_t>(hdr[4]) | (static_cast<uint32_t>(hdr[5]) << 8) |
                   (static_cast<uint32_t>(hdr[6]) << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
    // A durable header can outlive its payload. Check the file boundary
    // before using the header to size a buffer.
    if (len == 0 || len > kMaxRec || good_end + sizeof(hdr) + len > wal_size) {
      torn = true;
      break;
    }
    std::string pay;
    pay.resize(len);
    if (std::fread(&pay[0], 1, len, f) != len) { torn = true; break; }
    if (crc32_of(pay.data(), pay.size()) != crc) { torn = true; break; }

    Rec rec;
    bool ok=decode(pay,&rec);
    if (!ok) { torn = true; break; }

    good_end += 8 + static_cast<uint64_t>(len);

    if (rec.lsn <= snap_lsn) {  // already folded into the snapshot
      if (rec.kind == REC_TXN_BEGIN || rec.kind == REC_TXN_COMMIT) pending_start = UINT64_MAX;
      durable_end = good_end;
      continue;
    }

    if (rec.kind == REC_TXN_BEGIN) {
      pending_start = good_end;
    } else if (rec.kind == REC_TXN_COMMIT) {
      // Re-read only committed transactions one record at a time. No writes to
      // authoritative state occur during recovery, so interruption is harmless.
      const uint64_t commit_start=good_end-8-len;
      if (pending_start != UINT64_MAX) {
        if (::fseeko(f,static_cast<off_t>(pending_start),SEEK_SET) != 0) {
          std::fclose(f); if (err) *err="cannot seek transaction WAL"; return false;
        }
        uint64_t pos=pending_start;
        while (pos < commit_start) {
          uint32_t h[2];
          if (std::fread(h,1,8,f) != 8 || h[0] == 0 || h[0] > kMaxRec ||
              pos+8+h[0] > commit_start) {
            std::fclose(f); if (err) *err="cannot reread transaction WAL"; return false;
          }
          std::string body(h[0],'\0');
          Rec item;
          if (std::fread(&body[0],1,body.size(),f) != body.size() ||
              crc32_of(body.data(),body.size()) != h[1] || !decode(body,&item)) {
            std::fclose(f); if (err) *err="invalid transaction WAL reread"; return false;
          }
          if (item.lsn > snap_lsn) apply_one(item);
          pos+=8+h[0];
        }
        if (::fseeko(f,static_cast<off_t>(good_end),SEEK_SET) != 0) {
          std::fclose(f); if (err) *err="cannot resume WAL replay"; return false;
        }
      }
      pending_start = UINT64_MAX;
      durable_end = good_end;
      durable_lsn = std::max(durable_lsn, rec.lsn);
    } else if (!(rec.flags & RF_TXN)) {
      pending_start = UINT64_MAX;
      apply_one(rec);
      durable_end = good_end;
      durable_lsn = std::max(durable_lsn, rec.lsn);
    }
  }
  const bool read_error = std::ferror(f);
  std::fclose(f);
  if (read_error) {
    if (err) *err = "cannot read WAL during recovery";
    return false;
  }

  // Keep the committed prefix. Even valid records from a trailing transaction
  // without a commit are unreachable after recovery and must not accumulate.
  if (torn || durable_end < good_end) {
    int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) { if (err) *err = "cannot open WAL for repair"; return false; }
    const bool repaired = ::ftruncate(fd, static_cast<off_t>(durable_end)) == 0 &&
                          ::fdatasync(fd) == 0;
    ::close(fd);
    if (!repaired) { if (err) *err = "cannot remove uncommitted WAL tail"; return false; }
  }
  wal_rows_ = durable_lsn;
  return true;
}

bool Database::checkpoint(std::string* err) {
  if (!wal_) {
    if (err) *err = "database is not open";
    return false;
  }
  if (txn_active_) {
    // Defer: snapshotting mid-transaction would either bake the uncommitted rows
    // into the snapshot or drop the TXN_BEGIN marker. The next call after COMMIT
    // does the work.
    return true;
  }

  const std::string path = dir_ + "/snapshot.bin";
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    if (err) *err = "cannot create " + tmp + ": " + std::strerror(errno);
    return false;
  }
  auto failed_snapshot = [&]() -> bool {
    std::fclose(f);
    ::unlink(tmp.c_str());
    if (err) *err = "failed writing " + tmp;
    return false;
  };
  std::string buf;
  buf.reserve(1u << 20);
  uint32_t crc = 0xFFFFFFFFu;
  auto flush_out = [&]() -> bool {
    if (buf.empty()) return true;
    bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
    buf.clear();
    return ok;
  };
  bool io_ok = true;
  auto put = [&](const void* p, size_t n) -> bool {
    crc = crc32_upd(crc, p, n);
    buf.append(static_cast<const char*>(p), n);
    return buf.size() < (1u << 20) || flush_out();
  };

  std::string hdr;
  put_u32(hdr, kSnapMagic);
  put_u64(hdr, wal_rows_);
  put_u32(hdr, static_cast<uint32_t>(tables_.size()));
  if (!put(hdr.data(), hdr.size())) return failed_snapshot();
  for (const auto& kv : tables_) {
    const Table& t = kv.second;
    std::string b;
    put_schema(b, t.schema);
    put_i64(b, t.next_rid);
    put_u8(b, t.has_index ? 1 : 0);
    put_u64(b, static_cast<uint64_t>(t.live_count()));
    for (size_t i = 0; i < t.rows.size(); ++i) {
      if (i < t.dead.size() && t.dead[i]) continue;
      put_i64(b, t.rows[i].rid);
      Row scratch;
      put_row(b, t.read_row(i,scratch));
      if (b.size() >= (1u << 19)) {
        if (!put(b.data(), b.size())) return failed_snapshot();
        b.clear();
      }
    }
    if (!put(b.data(), b.size())) return failed_snapshot();
  }
  uint32_t fin = crc ^ 0xFFFFFFFFu;
  buf.append(reinterpret_cast<const char*>(&fin), 4);  // CRC is not part of itself
  io_ok = io_ok && flush_out();
  io_ok = io_ok && (std::fflush(f) == 0);
  io_ok = io_ok && (::fsync(::fileno(f)) == 0);
  if (std::fclose(f) != 0) io_ok = false;
  if (!io_ok) {
    ::unlink(tmp.c_str());
    if (err) *err = "failed writing " + tmp;
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  // Mirror file (informational; the snapshot header is authoritative).
  write_atomic(dir_ + "/snapshot.lsn", std::to_string(wal_rows_) + "\n", nullptr);
  if (!fsync_dir(dir_)) {
    if (err) *err = "cannot sync checkpoint directory";
    return false;
  }

  // The snapshot is durable: truncating the WAL cannot lose anything.
  if (!flush_wal() || std::fflush(wal_) != 0 || ::ftruncate(::fileno(wal_), 0) != 0) {
    if (err) *err = "cannot truncate data.log";
    return false;
  }
  if (::fdatasync(::fileno(wal_)) != 0) {
    if (err) *err = "cannot sync truncated WAL";
    return false;
  }
  wal_bytes_ = 0;
  wal_flushed_ = 0;
  wal_advised_ = 0;  // the log was truncated: no persisted bytes remain
  return true;
}

// ---------------------------------------------------------------- conds ----
bool validate_conditions(const Table& t, const std::vector<Cond>& conds,
                         std::string* err, int* code) {
  return compile_pred(t, conds, nullptr, err, code);
}

bool eval_conds(const Table& t, const Row& r, const std::vector<Cond>& conds) {
  if (conds.empty()) return true;
  Pred pr;
  return compile_pred(t, conds, &pr) && !pr.never && row_matches(pr, r);
}

}  // namespace mvdb

// db.cpp — MiniVectorDB storage engine: in-memory tables, WAL, snapshot, catalog,
// and single-connection transactions.
//
// ---------------------------------------------------------------------------
// On-disk layout (all integers little-endian, all strings length prefixed)
// ---------------------------------------------------------------------------
//   <dir>/catalog.bin    table schemas (+ per table: has_index flag), CRC32 tail
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
//     commit() appends a TXN_COMMIT record and fdatasync()s once. Replay buffers
//     RF_TXN records and only applies them when a TXN_COMMIT arrives, so a
//     rolled back or crashed transaction never reaches the in-memory state.
//   * Replay stops at the first bad/truncated record and then truncates data.log
//     back to the last good record boundary, so later appends stay contiguous.
//   * checkpoint() writes snapshot.bin atomically (tmp+fsync+rename+dir fsync)
//     *before* truncating the WAL; records already covered by the snapshot are
//     skipped on replay using the LSN stored in the snapshot header.
// ---------------------------------------------------------------------------
#include "db.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mvdb {
namespace {

using TableMap = std::unordered_map<std::string, Table>;

constexpr uint32_t kCatMagic = 0x4D564331u;   // "MVC1"
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

// WAL record kinds.
enum RecKind : uint8_t {
  REC_INSERT = 1,
  REC_UPDATE = 2,
  REC_DELETE = 3,
  REC_CREATE_TABLE = 4,
  REC_DROP_TABLE = 5,
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

bool get_value(Rd& r, Value* v) {
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
      std::memcpy(v->v.data(), r.p, static_cast<size_t>(n) * 4);
      r.p += static_cast<size_t>(n) * 4;
      return true;
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

bool get_row(Rd& r, Row* row) {
  uint32_t n = r.u32();
  if (r.bad || n > kMaxCols) { r.bad = true; return false; }
  row->vals.clear();
  row->vals.resize(n);
  for (uint32_t i = 0; i < n; ++i)
    if (!get_value(r, &row->vals[i])) return false;
  return true;
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

bool get_schema(Rd& r, TableSchema* s) {
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

bool read_file(const std::string& path, std::string* out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[262144];
  size_t n;
  out->clear();
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
  bool ok = !std::ferror(f);
  std::fclose(f);
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
  if (!dir.empty()) fsync_dir(dir);
  return true;
}

// ------------------------------------------------------------- catalog -----
std::string encode_catalog(const TableMap& tables) {
  std::string b;
  b.reserve(256 + tables.size() * 128);
  put_u32(b, kCatMagic);
  put_u32(b, static_cast<uint32_t>(tables.size()));
  for (const auto& kv : tables) {
    put_schema(b, kv.second.schema);
    put_u8(b, kv.second.has_index ? 1 : 0);
  }
  put_u32(b, crc32_of(b.data(), b.size()));
  return b;
}

bool decode_catalog(const std::string& blob, TableMap* out, std::string* err) {
  if (blob.size() < 12) {
    *err = "catalog.bin: truncated";
    return false;
  }
  Rd r(blob.data(), blob.size() - 4);
  uint32_t crc_stored;
  std::memcpy(&crc_stored, blob.data() + blob.size() - 4, 4);
  if (crc32_of(blob.data(), blob.size() - 4) != crc_stored) {
    *err = "catalog.bin: CRC mismatch";
    return false;
  }
  if (r.u32() != kCatMagic) {
    *err = "catalog.bin: bad magic";
    return false;
  }
  uint32_t n = r.u32();
  if (r.bad || n > 100000) {
    *err = "catalog.bin: bad table count";
    return false;
  }
  out->clear();
  for (uint32_t i = 0; i < n; ++i) {
    TableSchema s;
    if (!get_schema(r, &s)) {
      *err = "catalog.bin: bad schema";
      return false;
    }
    bool has_index = r.u8() != 0;
    if (r.bad) {
      *err = "catalog.bin: truncated schema";
      return false;
    }
    Table t;
    t.schema = std::move(s);
    t.has_index = has_index;
    out->emplace(t.schema.name, std::move(t));
  }
  return true;
}

bool save_catalog(const std::string& dir, const TableMap& tables, std::string* err) {
  return write_atomic(dir + "/catalog.bin", encode_catalog(tables), err);
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
  bool never = false;        // predicate can never match (unknown column / bad type)
};

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

// Coerces `lit` to the column type. Returns false when the comparison can never
// hold (type mismatch that is not convertible).
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
      if (in.type == Type::VARCHAR) {
        *out = in;
        return true;
      }
      if (in.type == Type::BIGINT) {
        *out = Value::ofStr(std::to_string(in.i));
        return true;
      }
      return false;
    case Type::VECTOR:
      if (in.type != Type::VECTOR) return false;
      *out = in;
      return true;
  }
  return false;
}

void compile_pred(const Table& t, const std::vector<Cond>& conds, Pred* out) {
  out->never = false;
  out->c.clear();
  out->owned.clear();
  if (conds.empty()) return;
  out->c.reserve(conds.size());
  out->owned.reserve(conds.size());  // no reallocation => &owned.back() stays valid
  for (const Cond& cond : conds) {
    int idx = t.schema.find(cond.column);
    if (idx < 0) {
      out->never = true;  // unknown column: nothing can match
      out->c.clear();
      return;
    }
    Type ct = t.schema.cols[static_cast<size_t>(idx)].type;
    Value coerced;
    if (!coerce_literal(ct, cond.value, &coerced)) {
      out->never = true;
      out->c.clear();
      return;
    }
    out->owned.push_back(std::move(coerced));
    RCond rc;
    rc.col = static_cast<uint32_t>(idx);
    rc.op = static_cast<uint8_t>(cond.op);
    rc.vt = static_cast<uint8_t>(ct);
    rc.lit = &out->owned.back();
    out->c.push_back(rc);
  }
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
      if (v->type == Type::BIGINT) {
        *v = Value::ofStr(std::to_string(v->i));
      } else if (v->type != Type::VARCHAR) {
        if (err) *err = "column '" + c.name + "': expected VARCHAR";
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
      if (v->type != Type::VECTOR) {
        if (err) *err = "column '" + c.name + "': expected VECTOR";
        return false;
      }
      if (static_cast<int32_t>(v->v.size()) != c.len) {
        if (err)
          *err = "column '" + c.name + "': vector dimension " +
                 std::to_string(v->v.size()) + " != " + std::to_string(c.len);
        return false;
      }
      for (float f : v->v) {
        if (!std::isfinite(f)) {
          if (err) *err = "column '" + c.name + "': vector contains NaN/Inf";
          return false;
        }
      }
      return true;
  }
  if (err) *err = "unknown column type";
  return false;
}

void set_err(std::string* err, int* code, ErrCode c, const std::string& msg) {
  if (err) *err = msg;
  if (code) *code = static_cast<int>(c);
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

// ===========================================================================
// Undo log
// ===========================================================================
struct Database::Undo {
  enum K : uint8_t { K_INS, K_UPD, K_DEL, K_DROP } k = K_INS;
  std::string table;
  int64_t pk = 0;
  size_t slot = 0;
  std::unique_ptr<Row> old;        // K_UPD: previous row image
  std::unique_ptr<Table> dropped;  // K_DROP: entire table as it was
};

// ===========================================================================
// Database
// ===========================================================================
Database::Database() = default;

Database::~Database() { close(); }

void Database::close() {
  if (wal_) {
    std::fflush(wal_);
    std::fclose(wal_);
    wal_ = nullptr;
  }
  tables_.clear();
  undo_.clear();
  txn_active_ = false;
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
  if (!load_snapshot(err)) {  // also loads the catalog first
    close();
    return false;
  }
  if (!replay_wal(err)) {
    close();
    return false;
  }
  std::string log = dir_ + "/data.log";
  wal_ = std::fopen(log.c_str(), "ab");  // O_APPEND: every write lands at EOF
  if (!wal_) {
    if (err) *err = "cannot open " + log + ": " + std::strerror(errno);
    close();
    return false;
  }
  struct stat st;
  wal_bytes_ = (::stat(log.c_str(), &st) == 0) ? static_cast<uint64_t>(st.st_size) : 0;
  return true;
}

// ---------------------------------------------------------------- schema ---
bool Database::create_table(const std::string& name, const std::vector<Column>& cols,
                            std::string* err, int* code) {
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
  t.schema.cols = cols;
  t.schema.pk_idx = pk_idx;
  t.schema.vec_idx = vec_idx;
  t.rows.reserve(1024);
  t.dead.reserve(1024);
  const size_t expect = t.rows.capacity();
  t.pk_pos.reserve(expect / 2 + 16);

  auto ins = tables_.emplace(name, std::move(t));
  if (!save_catalog(dir_, tables_, err)) {
    tables_.erase(ins.first);
    set_err(err, code, ErrCode::TXN, "cannot persist catalog: " + (err ? *err : ""));
    return false;
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
    append_wal(rec);
  }
  return true;
}

bool Database::drop_table(const std::string& name, std::string* err, int* code) {
  auto it = tables_.find(name);
  if (it == tables_.end()) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + name + "' does not exist");
    return false;
  }
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
  if (!save_catalog(dir_, tables_, err)) {
    set_err(err, code, ErrCode::TXN, "cannot persist catalog");
    return false;
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
    append_wal(rec);
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

  auto prev = tb.pk_pos.find(key);
  if (prev != tb.pk_pos.end()) {
    if (prev->second < tb.dead.size()) tb.dead[prev->second] = 1;  // last writer wins
    tb.pk_pos.erase(prev);
  }
  Row row;
  row.rid = tb.next_rid++;
  row.vals = std::move(vals);
  tb.rows.push_back(std::move(row));
  tb.dead.push_back(0);
  tb.pk_pos[key] = tb.rows.size() - 1;
  if (pk) *pk = key;
}

void Database::append_wal(const std::string& rec) {
  if (!wal_) return;
  size_t done = 0;
  write_all_fd(::fileno(wal_), rec.data(), rec.size(), &done);
  wal_bytes_ += done;
}

bool Database::fsync_wal() {
  if (!wal_) return false;
  if (std::fflush(wal_) != 0) return false;
  // MVDB_FSYNC=0 turns the per-statement fdatasync into a plain write(). Default
  // (unset / anything else) is the durable behaviour: the record is on stable
  // storage before the caller is told the statement succeeded.
  static const bool durable = []() {
    const char* e = std::getenv("MVDB_FSYNC");
    if (!e) return true;
    std::string s(e);
    return !(s == "0" || s == "off" || s == "false" || s == "no");
  }();
  if (!durable) return true;
  return ::fdatasync(::fileno(wal_)) == 0;
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
  if (tb.pk_pos.count(key)) {
    set_err(err, code, ErrCode::DUP_KEY,
            "Duplicate entry '" + std::to_string(key) + "' for key 'PRIMARY'");
    return false;
  }

  // Write-ahead: the record hits the log before the row becomes visible.
  std::string rec;
  {
    const uint64_t lsn = ++wal_rows_;
    std::string pay;
    pay.reserve(64 + vals.size() * 16);
    put_u8(pay, REC_INSERT);
    put_u8(pay, txn_active_ ? RF_TXN : 0);
    put_u64(pay, lsn);
    put_str(pay, t);
    put_u32(pay, static_cast<uint32_t>(vals.size()));
    for (const Value& v : vals) put_value(pay, v);
    uint32_t len = static_cast<uint32_t>(pay.size());
    uint32_t crc = crc32_of(pay.data(), pay.size());
    rec.reserve(pay.size() + 8);
    rec.append(reinterpret_cast<const char*>(&len), 4);
    rec.append(reinterpret_cast<const char*>(&crc), 4);
    rec.append(pay);
  }
  const uint64_t before = wal_bytes_;
  append_wal(rec);
  if (wal_ && wal_bytes_ - before != rec.size()) {
    set_err(err, code, ErrCode::TXN, "WAL write failed");
    return false;
  }
  if (!txn_active_ && !fsync_wal()) {
    set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
    return false;
  }

  int64_t got = 0;
  apply_insert(t, std::move(vals), &got);
  if (txn_active_) {
    Undo u;
    u.k = Undo::K_INS;
    u.table = t;
    u.pk = got;
    undo_.push_back(std::move(u));
  } else if (wal_bytes_ > kAutoCheckpointBytes) {
    std::string ignored;
    checkpoint(&ignored);  // best effort; the row is already durable
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
  for (size_t k = 0; k < sets.size(); ++k) {
    setcol[k] = sc.find(sets[k].first);
    if (setcol[k] < 0) {
      set_err(err, code, ErrCode::NO_SUCH_COLUMN,
              "Unknown column '" + sets[k].first + "' in 'field list'");
      return -1;
    }
  }
  Pred pr;
  compile_pred(tb, conds, &pr);
  if (pr.never) return 0;

  int64_t affected = 0;
  for (size_t i = 0; i < tb.rows.size(); ++i) {
    if (tb.dead[i]) continue;
    Row& row = tb.rows[i];
    if (!row_matches(pr, row)) continue;

    std::vector<Value> nv = row.vals;
    for (size_t k = 0; k < sets.size(); ++k) {
      Value v = sets[k].second;  // may be coerced in place
      if (!check_value(sc.cols[static_cast<size_t>(setcol[k])], &v, err)) {
        if (code) *code = static_cast<int>(ErrCode::BAD_VALUE);
        return -1;
      }
      nv[static_cast<size_t>(setcol[k])] = std::move(v);
    }
    const int64_t oldpk = row.vals[static_cast<size_t>(sc.pk_idx)].i;
    const int64_t newpk = nv[static_cast<size_t>(sc.pk_idx)].i;
    if (newpk != oldpk) {
      auto dup = tb.pk_pos.find(newpk);
      if (dup != tb.pk_pos.end()) {
        set_err(err, code, ErrCode::DUP_KEY,
                "Duplicate entry '" + std::to_string(newpk) + "' for key 'PRIMARY'");
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
    const uint64_t before = wal_bytes_;
    append_wal(rec);
    if (wal_ && wal_bytes_ - before != rec.size()) {
      set_err(err, code, ErrCode::TXN, "WAL write failed");
      return -1;
    }
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
      return -1;
    }

    if (txn_active_) {
      Undo u;
      u.k = Undo::K_UPD;
      u.table = t;
      u.pk = oldpk;
      u.slot = i;
      u.old.reset(new Row(row));
      undo_.push_back(std::move(u));
    }
    if (newpk != oldpk) {
      tb.pk_pos.erase(oldpk);
      tb.pk_pos[newpk] = i;
    }
    row.vals = std::move(nv);
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
  compile_pred(tb, conds, &pr);
  if (pr.never) return 0;

  int64_t affected = 0;
  for (size_t i = 0; i < tb.rows.size(); ++i) {
    if (tb.dead[i]) continue;
    const Row& row = tb.rows[i];
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
    const uint64_t before = wal_bytes_;
    append_wal(rec);
    if (wal_ && wal_bytes_ - before != rec.size()) {
      set_err(err, code, ErrCode::TXN, "WAL write failed");
      return -1;
    }
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
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
  compile_pred(tb, conds, &pr);
  if (pr.never) return;

  const Row* rows = tb.rows.data();
  const uint8_t* dead = tb.dead.data();
  const size_t n = tb.rows.size();

  // Primary key equality: jump straight to the row.
  if (!pr.c.empty()) {
    for (const RCond& c : pr.c) {
      if (static_cast<int>(c.col) == tb.schema.pk_idx && c.op == static_cast<uint8_t>(Cond::EQ)) {
        auto pos = tb.pk_pos.find(c.lit->i);
        if (pos == tb.pk_pos.end()) return;
        const size_t s = pos->second;
        if (s < n && !dead[s] && row_matches(pr, rows[s])) cb(rows[s]);
        return;
      }
    }
  }

  for (size_t i = 0; i < n; ++i) {
    if (dead[i]) continue;
    if (!row_matches(pr, rows[i])) continue;
    if (!cb(rows[i])) return;
  }
}

// ------------------------------------------------------------ transactions -
bool Database::begin(std::string* err, int* code) {
  if (txn_active_) {
    set_err(err, code, ErrCode::TXN, "a transaction is already in progress");
    return false;
  }
  txn_active_ = true;
  undo_.clear();
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
  append_wal(rec);
  return true;
}

bool Database::commit(std::string* err, int* code) {
  if (txn_active_) {
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
    append_wal(rec);
    undo_.clear();
    undo_.shrink_to_fit();
    txn_active_ = false;
  }
  if (!fsync_wal()) {
    set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
    return false;
  }
  return true;
}

bool Database::rollback(std::string* err, int* code) {
  (void)err;
  (void)code;
  if (!txn_active_) return true;  // no-op, like COMMIT outside a transaction

  for (size_t i = undo_.size(); i-- > 0;) {
    Undo& u = undo_[i];
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
    switch (u.k) {
      case Undo::K_INS: {
        auto pos = tb.pk_pos.find(u.pk);
        if (pos != tb.pk_pos.end()) {
          if (pos->second < tb.dead.size()) tb.dead[pos->second] = 1;
          tb.pk_pos.erase(pos);
        }
        break;
      }
      case Undo::K_UPD: {
        if (u.slot < tb.rows.size() && u.old) {
          int64_t cur = u.pk;
          if (static_cast<size_t>(tb.schema.pk_idx) < tb.rows[u.slot].vals.size())
            cur = tb.rows[u.slot].vals[static_cast<size_t>(tb.schema.pk_idx)].i;
          tb.pk_pos.erase(cur);
          tb.rows[u.slot] = std::move(*u.old);
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
      default:
        break;
    }
  }
  undo_.clear();
  undo_.shrink_to_fit();
  txn_active_ = false;
  // DDL effects (catalog.bin) may have been undone; rewrite it from memory.
  std::string ignored;
  save_catalog(dir_, tables_, &ignored);
  // The WAL keeps the aborted transaction's records until the next checkpoint;
  // they are never applied because no TXN_COMMIT record follows them.
  return true;
}

// ----------------------------------------------------------- vector index --
bool Database::create_vector_index(const std::string& t, const std::string& col,
                                   const std::string& name, std::string* err, int* code) {
  (void)name;  // index build is a later phase: only the flag is recorded
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
  it->second.has_index = true;
  if (!save_catalog(dir_, tables_, err)) {
    set_err(err, code, ErrCode::TXN, "cannot persist catalog");
    return false;
  }
  return true;
}

bool Database::has_vector_index(const std::string& t) const {
  auto it = tables_.find(t);
  return it != tables_.end() && it->second.has_index;
}

// ------------------------------------------------------------- durability --
bool Database::load_snapshot(std::string* err) {
  // 1. catalog
  std::string blob;
  if (read_file(dir_ + "/catalog.bin", &blob) && !blob.empty()) {
    if (!decode_catalog(blob, &tables_, err)) {
      tables_.clear();
      return false;
    }
  }
  wal_rows_ = 0;

  // 2. snapshot
  const std::string path = dir_ + "/snapshot.bin";
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return true;  // no checkpoint yet
  if (st.st_size < 20) {
    if (err) *err = "snapshot.bin: truncated";
    return false;
  }
  // Verify the trailing CRC in a streaming pass (the snapshot is then read whole,
  // trading peak memory for a simple parser).
  {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      if (err) *err = "cannot open " + path + ": " + std::strerror(errno);
      return false;
    }
    const size_t limit = static_cast<size_t>(st.st_size) - 4;
    uint32_t crc = 0xFFFFFFFFu;
    char buf[262144];
    size_t got_total = 0;
    while (got_total < limit) {
      size_t want = std::min(sizeof(buf), limit - got_total);
      size_t got = std::fread(buf, 1, want, f);
      if (got == 0) break;
      crc = crc32_upd(crc, buf, got);
      got_total += got;
    }
    uint8_t tail[4] = {0, 0, 0, 0};
    size_t tg = std::fread(tail, 1, 4, f);
    std::fclose(f);
    if (got_total != limit || tg != 4) {
      if (err) *err = "snapshot.bin: short read";
      return false;
    }
    uint32_t stored = static_cast<uint32_t>(tail[0]) | (static_cast<uint32_t>(tail[1]) << 8) |
                      (static_cast<uint32_t>(tail[2]) << 16) |
                      (static_cast<uint32_t>(tail[3]) << 24);
    if ((crc ^ 0xFFFFFFFFu) != stored) {
      if (err) *err = "snapshot.bin: CRC mismatch";
      return false;
    }
  }

  if (!read_file(path, &blob)) {
    if (err) *err = "cannot read " + path;
    return false;
  }
  Rd r(blob.data(), blob.size() - 4);
  if (r.u32() != kSnapMagic) {
    if (err) *err = "snapshot.bin: bad magic";
    return false;
  }
  const uint64_t lsn = r.u64();
  uint32_t ntab = r.u32();
  if (r.bad || ntab > 100000) {
    if (err) *err = "snapshot.bin: bad table count";
    return false;
  }
  for (uint32_t ti = 0; ti < ntab; ++ti) {
    TableSchema sc;
    if (!get_schema(r, &sc)) {
      if (err) *err = "snapshot.bin: bad schema";
      return false;
    }
    const int64_t next_rid = r.i64();
    const bool has_index = r.u8() != 0;
    uint64_t nrows = r.u64();
    if (r.bad || nrows > (1ull << 34)) {
      if (err) *err = "snapshot.bin: bad row count";
      return false;
    }

    auto exists = tables_.find(sc.name);
    const bool compatible = exists != tables_.end() && same_schema(exists->second.schema, sc);
    Table fresh;
    Table* tb = nullptr;
    if (compatible) {
      tb = &exists->second;  // catalog is authoritative (it may be newer than the snapshot)
      tb->rows.clear();
      tb->dead.clear();
      tb->pk_pos.clear();
    } else if (exists != tables_.end()) {
      // Schema changed after the checkpoint: the snapshot rows belong to the old
      // generation. Skip them; WAL replay carries DROP/CREATE/rows for the new one.
      tb = nullptr;
    } else {
      fresh.schema = sc;
      fresh.has_index = has_index;
      tb = &fresh;
    }

    if (tb) {
      tb->rows.reserve(static_cast<size_t>(nrows));
      tb->dead.reserve(static_cast<size_t>(nrows));
      tb->pk_pos.reserve(static_cast<size_t>(nrows) / 2 + 16);
      tb->next_rid = next_rid > 0 ? next_rid : 1;
    }
    for (uint64_t k = 0; k < nrows; ++k) {
      Row row;
      row.rid = r.i64();
      if (!get_row(r, &row)) {
        if (err) *err = "snapshot.bin: bad row";
        return false;
      }
      if (!tb) continue;
      int64_t pk = 0;
      if (static_cast<size_t>(tb->schema.pk_idx) < row.vals.size())
        pk = row.vals[static_cast<size_t>(tb->schema.pk_idx)].i;
      if (row.rid >= tb->next_rid) tb->next_rid = row.rid + 1;
      tb->pk_pos[pk] = tb->rows.size();
      tb->rows.push_back(std::move(row));
      tb->dead.push_back(0);
    }
    if (tb == &fresh && !fresh.schema.name.empty()) {
      tables_[fresh.schema.name] = std::move(fresh);
    }
  }
  wal_rows_ = lsn;  // LSN of everything contained in the snapshot
  return true;
}

bool Database::replay_wal(std::string* err) {
  const std::string path = dir_ + "/data.log";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return true;  // nothing to replay
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
        tb.rows[s].vals = std::move(rec.vals);
        break;
      }
      case REC_DELETE: {
        if (it == tables_.end()) break;
        Table& tb = it->second;
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
        t.schema = rec.schema;
        tables_.emplace(t.schema.name, std::move(t));
        break;
      }
      case REC_DROP_TABLE:
        tables_.erase(rec.name);
        break;
      default:
        break;
    }
  };

  std::vector<Rec> pending;  // records of the open (uncommitted) transaction
  uint64_t good_end = 0;     // byte offset just past the last valid record
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
    if (len == 0 || len > kMaxRec) { torn = true; break; }
    std::string pay;
    pay.resize(len);
    if (std::fread(&pay[0], 1, len, f) != len) { torn = true; break; }
    if (crc32_of(pay.data(), pay.size()) != crc) { torn = true; break; }

    Rd r(pay.data(), pay.size());
    Rec rec;
    rec.kind = r.u8();
    rec.flags = r.u8();
    rec.lsn = r.u64();
    bool ok = !r.bad;
    if (ok) {
      switch (rec.kind) {
        case REC_INSERT:
        case REC_UPDATE:
        case REC_DELETE:
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
    if (!ok) { torn = true; break; }

    good_end += 8 + static_cast<uint64_t>(len);

    if (rec.lsn <= snap_lsn) {  // already folded into the snapshot
      if (rec.kind == REC_TXN_BEGIN || rec.kind == REC_TXN_COMMIT) pending.clear();
      continue;
    }
    if (rec.lsn > wal_rows_) wal_rows_ = rec.lsn;

    if (rec.kind == REC_TXN_BEGIN) {
      pending.clear();
    } else if (rec.kind == REC_TXN_COMMIT) {
      for (Rec& p : pending) apply_one(p);
      pending.clear();
    } else if (rec.flags & RF_TXN) {
      pending.push_back(std::move(rec));
    } else {
      if (!pending.empty()) pending.clear();  // orphaned txn records
      apply_one(rec);
    }
  }
  pending.clear();
  std::fclose(f);

  // Repair: drop the torn tail so future appends stay contiguous.
  if (torn) {
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && static_cast<uint64_t>(st.st_size) > good_end) {
      int fd = ::open(path.c_str(), O_WRONLY);
      if (fd >= 0) {
        if (::ftruncate(fd, static_cast<off_t>(good_end)) == 0) ::fdatasync(fd);
        ::close(fd);
      }
    }
  }
  (void)err;
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
  auto put = [&](const void* p, size_t n) {
    crc = crc32_upd(crc, p, n);
    buf.append(static_cast<const char*>(p), n);
    if (buf.size() >= (1u << 20)) io_ok = io_ok && flush_out();
  };

  std::string hdr;
  put_u32(hdr, kSnapMagic);
  put_u64(hdr, wal_rows_);
  put_u32(hdr, static_cast<uint32_t>(tables_.size()));
  put(hdr.data(), hdr.size());
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
      put_row(b, t.rows[i]);
      if (b.size() >= (1u << 19)) {
        put(b.data(), b.size());
        b.clear();
      }
    }
    put(b.data(), b.size());
  }
  uint32_t fin = crc ^ 0xFFFFFFFFu;
  buf.append(reinterpret_cast<const char*>(&fin), 4);  // CRC is not part of itself
  io_ok = io_ok && flush_out();
  io_ok = io_ok && (std::fflush(f) == 0);
  io_ok = io_ok && (::fsync(::fileno(f)) == 0);
  std::fclose(f);
  if (!io_ok) {
    ::unlink(tmp.c_str());
    if (err) *err = "failed writing " + tmp;
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
    return false;
  }
  // Mirror file (informational; the snapshot header is authoritative).
  write_atomic(dir_ + "/snapshot.lsn", std::to_string(wal_rows_) + "\n", nullptr);
  if (!save_catalog(dir_, tables_, err)) return false;
  fsync_dir(dir_);

  // The snapshot is durable: truncating the WAL cannot lose anything.
  if (std::fflush(wal_) != 0 || ::ftruncate(::fileno(wal_), 0) != 0) {
    if (err) *err = "cannot truncate data.log";
    return false;
  }
  ::fdatasync(::fileno(wal_));
  wal_bytes_ = 0;
  return true;
}

// ---------------------------------------------------------------- conds ----
bool eval_conds(const Table& t, const Row& r, const std::vector<Cond>& conds) {
  if (conds.empty()) return true;
  Pred pr;
  compile_pred(t, conds, &pr);
  if (pr.never) return false;
  return row_matches(pr, r);
}

}  // namespace mvdb

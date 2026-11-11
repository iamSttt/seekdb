// db.cpp — MiniVectorDB storage engine.
//
// The bulk of every table lives on disk; RAM holds only a pk -> record index,
// a one-bit-per-record version bitmap and a handful of counters, so peak RSS
// is bounded by the row count and not by the row size.
//
// ---------------------------------------------------------------------------
// On-disk layout (all integers little-endian; one file triple per table)
// ---------------------------------------------------------------------------
//   <dir>/t_<tag>.dat   fixed-size row records, append-only between checkpoints
//        [0, 64)  header: magic, gen, rec_size, ckpt_rids, ckpt_lsn,
//                 heap_rids, heap_lsn, live_rows, crc32
//        [64,..)  record i at 64 + i*rec_size
//   <dir>/t_<tag>.str   append-only VARCHAR arena; a byte's offset never moves
//   <dir>/t_<tag>.idx   pk -> record image written at checkpoint / clean close
//                        (purely derived: a bad or stale one is discarded and
//                         rebuilt by streaming the .dat)
//   <dir>/catalog.bin   table schemas + has_index flag (unchanged format)
//   <dir>/snapshot.bin  checkpoint metadata {lsn, per-table gen/rows}
//   <dir>/snapshot.lsn  human readable mirror of the checkpoint LSN
//   <dir>/data.log      logical WAL: [u32 len][u32 crc][payload] (unchanged)
//
// Record layout (fixed per table, so a record's offset is rid*rec_size):
//   off 0   u8   kind        0 = live version, 1 = tombstone
//   off 8   i64  pk
//   off 16  column slots in schema order, except the pk column, which aliases
//           the pk field at off 8:
//             BIGINT   i64
//             VARCHAR  u32 str_off, u32 len    (into t_<tag>.str)
//             VECTOR   float32[len]            (raw block)
//
// ---------------------------------------------------------------------------
// Durability model
// ---------------------------------------------------------------------------
//   * The WAL is the only source of truth. It stores logical operations with
//     the same record format as before (INSERT carries the full row, UPDATE the
//     old pk plus the new row, DELETE the pk), so replay can rebuild a heap
//     from nothing.
//   * The heap is *derived* state: after a crash it is trusted only up to the
//     commit watermark in its header (heap_rids/heap_lsn, refreshed at every
//     commit, written without fsync because a stale watermark only costs
//     replay). Everything past the watermark -- a half finished transaction, a
//     superseded version -- is truncated away and rebuilt from the WAL, so an
//     uncommitted row can never come back.
//   * Rows are never rewritten in place: UPDATE appends a new version and
//     clears the old one's bit, DELETE appends a tombstone.
//   * checkpoint() compacts every heap to its live rows, rewrites the .idx and
//     truncates the WAL: tmp file + fsync + rename + directory fsync, with the
//     per-table generation counter making a stale .idx detectable.
//   * ROLLBACK truncates the heap back to the transaction's watermark and
//     restores the index, so an aborted transaction leaves no trace.
//   * MVDB_FSYNC=0 downgrades every fdatasync to a plain write.
//
// ---------------------------------------------------------------------------
// Deliberate deviations from the original sketch, and why
// ---------------------------------------------------------------------------
//   1. VARCHAR bytes live in a side file (t_<tag>.str) instead of inside the
//      .dat. Fixed-size records need a fixed slot, and an append-only arena
//      whose offsets never move means a checkpoint only ever rewrites .dat --
//      .str is never copied, never renamed, and cannot be caught half swapped.
//      The price is that string bytes freed by an UPDATE or DELETE are only
//      reclaimed by DROP TABLE; a table that is rewritten in place forever
//      grows its .str (disk, not RAM) accordingly.
//   2. The map holds pk -> record *id* (12 B: int64 key + u32 rid in two
//      parallel arrays) rather than pk -> byte offset; the offset is
//      rid*rec_size, so this is 25% smaller and just as fast. 2M rows ~= 30 MB.
//   3. t_<tag>.idx caches the map so a reopen of a 1 GB heap is a 24 MB read
//      instead of a 1 GB scan. It is pure cache: a stale generation or a size
//      mismatch silently falls back to rebuilding it by streaming .dat.
//   4. Replay applies only records a committed transaction covers, discovered
//      by a first pass that keeps just the (begin, commit) LSN pairs, so a
//      multi-million-row transaction is never buffered in RAM to be discarded.
//   5. The auto-checkpoint threshold is max(64 MiB, live heap size) rather than
//      a flat 64 MiB, so a bulk load does not rewrite a growing heap every
//      64 MiB (write amplification stays ~O(n) instead of O(n^2/64 MiB)).
// ---------------------------------------------------------------------------
#include "db.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

constexpr uint32_t kCatMagic = 0x4D564331u;   // "MVC1"
constexpr uint32_t kDatMagic = 0x4D564431u;   // "MVD1"
constexpr uint32_t kIdxMagic = 0x4D564931u;   // "MVI1"
constexpr uint32_t kSnapMagic = 0x4D565332u;  // "MVS2"

constexpr uint32_t kMaxStr = 1u << 26;   // 64 MiB for a single string field
constexpr uint32_t kMaxDim = 1u << 20;   // vector dimension sanity bound
constexpr uint32_t kMaxCols = 4096;      // columns per table
constexpr uint32_t kMaxRec = 1u << 30;   // WAL payload sanity bound

constexpr uint64_t kDatHdr = 64;              // .dat header size
constexpr uint32_t kIdxHdr = 32;              // .idx header size
constexpr uint32_t kEmptyRid = 0xFFFFFFFFu;   // "no record"
constexpr size_t kAppendBuf = 1u << 20;       // heap append buffer
constexpr size_t kReadBuf = 1u << 20;         // scan read buffer
constexpr size_t kStrWin = 1u << 18;          // VARCHAR window cache
constexpr uint64_t kMaxRid = 0xFFFFFFFEull;   // rid must stay below kEmptyRid

// The WAL is folded into the heap once it grows past this (or past the size of
// the live data, whichever is larger) -- keeping the write amplification of a
// bulk load bounded instead of rewriting a growing heap every 64 MiB.
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

// Little-endian field access on a fixed byte image.
inline uint32_t g32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}
inline uint64_t g64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
inline void s32(char* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void s64(char* p, uint64_t v) { std::memcpy(p, &v, 8); }

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

// Value / schema serialisation (WAL and the small metadata files).
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
      if (!v.v.empty()) b.append(reinterpret_cast<const char*>(v.v.data()), v.v.size() * 4);
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
      if (n) std::memcpy(v->v.data(), r.p, static_cast<size_t>(n) * 4);
      r.p += static_cast<size_t>(n) * 4;
      return true;
    }
    default:
      r.bad = true;
      return false;
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

// ------------------------------------------------------------- catalog -----
using TableMap = std::unordered_map<std::string, Table>;

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

bool decode_catalog_at(const std::string& blob, TableMap* out, std::string* err) {
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

// MVDB_FSYNC=0 turns every fdatasync into a plain write. Default is durable.
bool durable_mode() {
  static const bool durable = []() {
    const char* e = std::getenv("MVDB_FSYNC");
    if (!e) return true;
    std::string s(e);
    return !(s == "0" || s == "off" || s == "false" || s == "no");
  }();
  return durable;
}

bool pread_all(int fd, void* p, size_t n, uint64_t off) {
  char* q = static_cast<char*>(p);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::pread(fd, q + got, n - got, static_cast<off_t>(off + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (r == 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

bool pwrite_all(int fd, const void* p, size_t n, uint64_t off) {
  const char* q = static_cast<const char*>(p);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::pwrite(fd, q + done, n - done, static_cast<off_t>(off + done));
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (w == 0) return false;
    done += static_cast<size_t>(w);
  }
  return true;
}

bool fdatasync_fd(int fd) {
  if (!durable_mode()) return true;
  return ::fdatasync(fd) == 0;
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
  if (!pwrite_all(fd, data.data(), data.size(), 0)) {
    if (err) *err = "write " + tmp + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (!fdatasync_fd(fd)) {
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

bool save_catalog_at(const std::string& dir, const TableMap& tables, std::string* err) {
  return write_atomic(dir + "/catalog.bin", encode_catalog(tables), err);
}

// A table name turned into a file-name-safe tag. '%' escapes anything that is
// not [A-Za-z0-9.-], so the mapping stays injective; over-long names fall back
// to a hash so the path always fits in a NAME_MAX component.
std::string table_tag(const std::string& name) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(name.size() + 8);
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
        c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  if (out.size() > 80) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "h%08x%08lx", crc32_of(name.data(), name.size()),
                  static_cast<unsigned long>(name.size()));
    out = buf;
  }
  return out;
}

// ------------------------------------------------------- record layout -----
struct RecLayout {
  std::vector<uint32_t> off;  // byte offset of each column's slot
  uint32_t rec_size = 16;
  int32_t pk_idx = 0;
};

void build_layout(const TableSchema& s, RecLayout* lay) {
  lay->off.assign(s.cols.size(), 0);
  lay->pk_idx = s.pk_idx;
  uint32_t cur = 16;
  for (size_t i = 0; i < s.cols.size(); ++i) {
    if (static_cast<int>(i) == s.pk_idx) {
      lay->off[i] = 8;  // aliases the pk field in the record header
      continue;
    }
    uint32_t sz = 0;
    switch (s.cols[i].type) {
      case Type::BIGINT: sz = 8; break;
      case Type::VARCHAR: sz = 8; break;
      case Type::VECTOR: sz = static_cast<uint32_t>(s.cols[i].len) * 4u; break;
    }
    lay->off[i] = cur;
    cur += sz;
  }
  lay->rec_size = (cur + 7u) & ~7u;
}

// ------------------------------------------------------------ pk index ----
// Open addressing, linear probing, no tombstones (backward-shift deletion).
// Two parallel arrays keep the slot at 12 bytes with no padding, which is what
// bounds RSS for a huge table: 2M rows ~= 30 MB.
inline uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

struct PkIndex {
  std::vector<int64_t> keys;
  std::vector<uint32_t> vals;
  uint32_t cap = 0;
  uint64_t occ = 0;

  void clear() {
    keys.clear();
    vals.clear();
    cap = 0;
    occ = 0;
  }

  void reset(uint64_t expect) {
    occ = 0;
    uint64_t need = expect + expect / 4 + 16;
    uint64_t ncap = 1024;
    while (ncap < need) ncap += ncap / 2;
    cap = static_cast<uint32_t>(ncap);
    keys.assign(cap, 0);
    vals.assign(cap, kEmptyRid);
  }

  // Slot that either holds `pk` or is the first free slot on its probe run.
  uint32_t probe(int64_t pk) const {
    uint32_t i = static_cast<uint32_t>(mix64(static_cast<uint64_t>(pk)) % cap);
    for (;;) {
      if (vals[i] == kEmptyRid || keys[i] == pk) return i;
      i = (i + 1 == cap) ? 0 : i + 1;
    }
  }

  bool find(int64_t pk, uint32_t* rid) const {
    if (cap == 0) return false;
    uint32_t i = probe(pk);
    if (vals[i] == kEmptyRid) return false;
    if (rid) *rid = vals[i];
    return true;
  }

  void grow() {
    uint64_t ncap = cap ? static_cast<uint64_t>(cap) + cap / 3 : 1024;
    std::vector<int64_t> nk(static_cast<size_t>(ncap), 0);
    std::vector<uint32_t> nv(static_cast<size_t>(ncap), kEmptyRid);
    for (uint32_t i = 0; i < cap; ++i) {
      if (vals[i] == kEmptyRid) continue;
      uint32_t j = static_cast<uint32_t>(mix64(static_cast<uint64_t>(keys[i])) % ncap);
      while (nv[j] != kEmptyRid) j = (j + 1 == ncap) ? 0 : j + 1;
      nk[j] = keys[i];
      nv[j] = vals[i];
    }
    keys.swap(nk);
    vals.swap(nv);
    cap = static_cast<uint32_t>(ncap);
  }

  // Inserts a key that is known to be absent.
  void put(int64_t pk, uint32_t rid) {
    if (cap == 0) reset(4);
    else if ((occ + 1) * 5 > static_cast<uint64_t>(cap) * 4) grow();
    uint32_t i = probe(pk);
    keys[i] = pk;
    vals[i] = rid;
    ++occ;
  }

  bool del(int64_t pk) {
    if (cap == 0) return false;
    uint32_t i = probe(pk);
    if (vals[i] == kEmptyRid) return false;
    vals[i] = kEmptyRid;
    --occ;
    // Backward-shift the rest of the probe run so no tombstone is needed.
    uint32_t j = i;
    for (;;) {
      j = (j + 1 == cap) ? 0 : j + 1;
      if (vals[j] == kEmptyRid) break;
      uint32_t home = static_cast<uint32_t>(mix64(static_cast<uint64_t>(keys[j])) % cap);
      bool keep = (i <= j) ? (home > i && home <= j) : (home > i || home <= j);
      if (keep) continue;
      keys[i] = keys[j];
      vals[i] = vals[j];
      vals[j] = kEmptyRid;
      i = j;
    }
    return true;
  }
};

}  // namespace

// ===========================================================================
// TableStorage — one table's disk heap, index and counters.
// ===========================================================================
struct TableStorage {
  std::string name;
  std::string base;  // <dir>/t_<tag>
  RecLayout lay;
  int fd_dat = -1;
  int fd_str = -1;
  bool unlink_on_close = false;

  // Header fields.
  uint64_t gen = 0;
  uint64_t ckpt_rids = 0;
  uint64_t ckpt_lsn = 0;
  uint64_t heap_rids = 0;
  uint64_t heap_lsn = 0;

  // Append / arena cursors.
  uint64_t next_rid = 0;     // records present (or buffered) in .dat
  uint64_t flushed_rids = 0; // records actually written to .dat
  uint64_t str_bytes = 0;    // logical end of the VARCHAR arena
  uint64_t str_flushed = 0;  // physical end of the VARCHAR arena

  PkIndex idx;
  std::vector<uint64_t> bits;  // 1 bit per rid: the record is the current version

  std::string dat_buf;
  std::string str_buf;

  // VARCHAR window cache.
  std::vector<char> win;
  uint64_t win_off = 0;
  size_t win_len = 0;

  ~TableStorage() { close_files(); }

  std::string dat_path() const { return base + ".dat"; }
  std::string str_path() const { return base + ".str"; }
  std::string idx_path() const { return base + ".idx"; }

  uint64_t rec_size() const { return lay.rec_size; }
  uint64_t live_rows() const { return idx.occ; }

  // ------------------------------------------------------------- bits -----
  bool bit_get(uint64_t rid) const {
    uint64_t w = rid >> 6;
    if (w >= bits.size()) return false;
    return (bits[static_cast<size_t>(w)] >> (rid & 63)) & 1ull;
  }
  void bit_set(uint64_t rid) {
    uint64_t w = rid >> 6;
    if (w >= bits.size()) bits.resize(static_cast<size_t>(w) + 1, 0);
    bits[static_cast<size_t>(w)] |= (1ull << (rid & 63));
  }
  void bit_clear(uint64_t rid) {
    uint64_t w = rid >> 6;
    if (w < bits.size()) bits[static_cast<size_t>(w)] &= ~(1ull << (rid & 63));
  }

  // ------------------------------------------------------------ files -----
  void close_files() {
    if (fd_dat >= 0) { ::close(fd_dat); fd_dat = -1; }
    if (fd_str >= 0) { ::close(fd_str); fd_str = -1; }
    if (unlink_on_close) {
      ::unlink(dat_path().c_str());
      ::unlink(str_path().c_str());
      ::unlink(idx_path().c_str());
      unlink_on_close = false;
    }
  }

  void encode_header(char* h) const {
    std::memset(h, 0, kDatHdr);
    s32(h + 0, kDatMagic);
    s32(h + 4, static_cast<uint32_t>(gen));
    s32(h + 8, lay.rec_size);
    s32(h + 12, 0);
    s64(h + 16, ckpt_rids);
    s64(h + 24, ckpt_lsn);
    s64(h + 32, heap_rids);
    s64(h + 40, heap_lsn);
    s64(h + 48, live_rows());
    s32(h + 56, crc32_of(h, 56));
    s32(h + 60, 0);
  }

  bool decode_header(const char* h) {
    if (g32(h + 0) != kDatMagic) return false;
    if (g32(h + 56) != crc32_of(h, 56)) return false;
    if (g32(h + 8) != lay.rec_size) return false;
    gen = g32(h + 4);
    ckpt_rids = g64(h + 16);
    ckpt_lsn = g64(h + 24);
    heap_rids = g64(h + 32);
    heap_lsn = g64(h + 40);
    return true;
  }

  bool write_header() {
    char h[kDatHdr];
    encode_header(h);
    return pwrite_all(fd_dat, h, kDatHdr, 0);
  }

  // Refreshes the commit watermark (cheap: 64 bytes, no fsync -- a stale or
  // torn watermark only means replay starts earlier than it could).
  void set_watermark(uint64_t rids, uint64_t lsn) {
    if (rids == heap_rids && lsn == heap_lsn) return;
    heap_rids = rids;
    heap_lsn = lsn;
    write_header();
  }

  bool flush() {
    if (!dat_buf.empty()) {
      if (!pwrite_all(fd_dat, dat_buf.data(), dat_buf.size(),
                      kDatHdr + flushed_rids * rec_size()))
        return false;
      flushed_rids += dat_buf.size() / rec_size();
      dat_buf.clear();
    }
    if (!str_buf.empty()) {
      if (!pwrite_all(fd_str, str_buf.data(), str_buf.size(), str_flushed)) return false;
      str_flushed += str_buf.size();
      str_buf.clear();
    }
    return true;
  }

  // ------------------------------------------------------------ reading ---
  bool read_rec(uint64_t rid, char* out) const {
    return pread_all(fd_dat, out, lay.rec_size, kDatHdr + rid * rec_size());
  }

  bool str_read(uint64_t off, uint32_t len, std::string* out) {
    if (len == 0) {
      out->clear();
      return true;
    }
    if (off >= win_off && off + len <= win_off + win_len) {
      out->assign(win.data() + static_cast<size_t>(off - win_off), len);
      return true;
    }
    if (len > kStrWin) {
      out->resize(len);
      return pread_all(fd_str, &(*out)[0], len, off);
    }
    size_t want = kStrWin;
    size_t got = 0;
    while (got < want) {
      ssize_t r = ::pread(fd_str, win.data() + got, want - got, static_cast<off_t>(off + got));
      if (r <= 0) break;
      got += static_cast<size_t>(r);
    }
    win_off = off;
    win_len = got;
    if (got < len) {
      out->clear();
      return false;
    }
    out->assign(win.data(), len);
    return true;
  }

  void decode(const char* rec, const TableSchema& sc, Row* row, uint64_t rid) {
    row->rid = static_cast<int64_t>(rid);
    row->vals.resize(sc.cols.size());
    const int64_t key = static_cast<int64_t>(g64(rec + 8));
    for (size_t i = 0; i < sc.cols.size(); ++i) {
      Value& v = row->vals[i];
      const Column& c = sc.cols[i];
      v.type = c.type;
      if (static_cast<int>(i) == lay.pk_idx) {
        v.i = key;
        continue;
      }
      const char* q = rec + lay.off[i];
      switch (c.type) {
        case Type::BIGINT:
          v.i = static_cast<int64_t>(g64(q));
          break;
        case Type::VARCHAR: {
          uint32_t o = g32(q), l = g32(q + 4);
          str_read(o, l, &v.s);
          break;
        }
        case Type::VECTOR: {
          size_t n = static_cast<size_t>(c.len);
          v.v.resize(n);
          if (n) std::memcpy(v.v.data(), q, n * 4);
          break;
        }
      }
    }
  }

  // Streams records [from, to) that are the current version, handing the raw
  // bytes to fn. fn returns false to stop.
  void for_each_live(uint64_t from, uint64_t to,
                     const std::function<bool(uint64_t, const char*)>& fn) {
    if (to > next_rid) to = next_rid;
    if (from >= to) return;
    flush();
    size_t bs = kReadBuf;
    if (bs < rec_size()) bs = static_cast<size_t>(rec_size());
    std::vector<char> buf(bs + static_cast<size_t>(rec_size()));
    uint64_t rid = from;
    while (rid < to) {
      uint64_t left = to - rid;
      size_t want = static_cast<size_t>(std::min<uint64_t>(bs, left * rec_size()));
      if (!pread_all(fd_dat, buf.data(), want, kDatHdr + rid * rec_size())) break;
      size_t nrec = want / rec_size();
      const char* p = buf.data();
      for (size_t i = 0; i < nrec; ++i, p += rec_size()) {
        uint64_t r = rid + i;
        if (!bit_get(r)) continue;
        if (!fn(r, p)) return;
      }
      rid += nrec;
    }
  }

  // ------------------------------------------------------------ writing ---
  uint32_t append_raw(const TableSchema& sc, const std::vector<Value>& vals, uint8_t kind) {
    if (next_rid >= kMaxRid) return kEmptyRid;
    uint64_t need = 0;
    for (const Value& v : vals)
      if (v.type == Type::VARCHAR) need += v.s.size();
    if (str_bytes + need > 0xFFFFFFFFull) return kEmptyRid;  // 4 GiB arena limit

    if (dat_buf.size() + rec_size() > kAppendBuf) {
      if (!flush()) return kEmptyRid;
    }
    const uint32_t rid = static_cast<uint32_t>(next_rid);
    const size_t base = dat_buf.size();
    dat_buf.resize(base + rec_size(), 0);
    char* p = &dat_buf[base];
    p[0] = static_cast<char>(kind);
    s64(p + 8, static_cast<uint64_t>(vals[static_cast<size_t>(lay.pk_idx)].i));
    for (size_t i = 0; i < sc.cols.size(); ++i) {
      if (static_cast<int>(i) == lay.pk_idx) continue;
      const Value& v = vals[i];
      char* q = p + lay.off[i];
      switch (sc.cols[i].type) {
        case Type::BIGINT:
          s64(q, static_cast<uint64_t>(v.i));
          break;
        case Type::VARCHAR: {
          const uint32_t o = static_cast<uint32_t>(str_bytes);
          const uint32_t l = static_cast<uint32_t>(v.s.size());
          if (l) str_buf.append(v.s);
          str_bytes += l;
          s32(q, o);
          s32(q + 4, l);
          break;
        }
        case Type::VECTOR:
          if (!v.v.empty()) std::memcpy(q, v.v.data(), v.v.size() * 4);
          break;
      }
    }
    ++next_rid;
    if ((next_rid + 63) / 64 > bits.size()) bits.resize(static_cast<size_t>((next_rid + 63) / 64), 0);
    return rid;
  }

  uint32_t append_tomb(int64_t pk) {
    if (next_rid >= kMaxRid) return kEmptyRid;
    if (dat_buf.size() + rec_size() > kAppendBuf) {
      if (!flush()) return kEmptyRid;
    }
    const uint32_t rid = static_cast<uint32_t>(next_rid);
    const size_t base = dat_buf.size();
    dat_buf.resize(base + rec_size(), 0);
    char* p = &dat_buf[base];
    p[0] = 1;
    s64(p + 8, static_cast<uint64_t>(pk));
    ++next_rid;
    if ((next_rid + 63) / 64 > bits.size()) bits.resize(static_cast<size_t>((next_rid + 63) / 64), 0);
    return rid;
  }

  // Drops [rid, next_rid) and [str_off, str_bytes): the rollback primitive.
  bool truncate_to(uint64_t rid, uint64_t str_off) {
    if (!flush()) return false;
    if (::ftruncate(fd_dat, static_cast<off_t>(kDatHdr + rid * rec_size())) != 0) return false;
    if (::ftruncate(fd_str, static_cast<off_t>(str_off)) != 0) return false;
    next_rid = rid;
    flushed_rids = rid;
    str_bytes = str_off;
    str_flushed = str_off;
    win_off = 0;
    win_len = 0;
    const size_t w = static_cast<size_t>(rid / 64), b = static_cast<size_t>(rid % 64);
    if (b) {
      bits.resize(w + 1, 0);
      bits[w] &= ~(~0ull << b);
    } else {
      bits.resize(w, 0);
    }
    return true;
  }

  // Removes every index entry pointing at rid >= limit (used by rollback).
  void index_drop_above(uint64_t limit) {
    uint64_t removed = 0;
    for (uint32_t i = 0; i < idx.cap; ++i) {
      if (idx.vals[i] == kEmptyRid || idx.vals[i] < limit) continue;
      idx.vals[i] = kEmptyRid;
      --idx.occ;
      ++removed;
    }
    if (removed) {
      // Rebuild the probe chains once instead of per removal.
      PkIndex fresh;
      fresh.reset(idx.occ ? idx.occ : 4);
      for (uint32_t i = 0; i < idx.cap; ++i) {
        if (idx.vals[i] == kEmptyRid) continue;
        const int64_t k = idx.keys[i];
        const uint32_t v = idx.vals[i];
        const uint32_t j = fresh.probe(k);
        fresh.keys[j] = k;
        fresh.vals[j] = v;
        ++fresh.occ;
      }
      idx.keys.swap(fresh.keys);
      idx.vals.swap(fresh.vals);
      idx.cap = fresh.cap;
      idx.occ = fresh.occ;
    }
  }

  // ------------------------------------------------------------- index ----
  void rebuild_index() {
    idx.clear();
    idx.reset(next_rid / 2 + 16);
    bits.assign(static_cast<size_t>((next_rid + 63) / 64), 0);
    const size_t rs = static_cast<size_t>(rec_size());
    size_t bs = kReadBuf;
    if (bs < rs) bs = rs;
    std::vector<char> buf(bs + rs);
    uint64_t rid = 0;
    while (rid < next_rid) {
      const uint64_t left = next_rid - rid;
      const size_t want = static_cast<size_t>(std::min<uint64_t>(bs, left * rec_size()));
      if (!pread_all(fd_dat, buf.data(), want, kDatHdr + rid * rec_size())) break;
      const size_t nrec = want / rs;
      const char* p = buf.data();
      for (size_t i = 0; i < nrec; ++i, p += rs) {
        const int64_t pk = static_cast<int64_t>(g64(p + 8));
        uint32_t prev = 0;
        if (idx.find(pk, &prev)) {
          bit_clear(prev);
          idx.del(pk);
        }
        if (p[0] == 0) {
          idx.put(pk, static_cast<uint32_t>(rid + i));
          bit_set(rid + i);
        }
      }
      rid += nrec;
    }
  }

  bool write_idx(std::string* err) {
    const std::string tmp = idx_path() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      if (err) *err = "open " + tmp + ": " + std::strerror(errno);
      return false;
    }
    char h[kIdxHdr];
    std::memset(h, 0, sizeof(h));
    s32(h + 0, kIdxMagic);
    s32(h + 4, lay.rec_size);
    s64(h + 8, gen);
    s64(h + 16, next_rid);
    s64(h + 24, idx.occ);
    bool ok = pwrite_all(fd, h, sizeof(h), 0);
    std::string buf;
    buf.reserve(1u << 20);
    uint64_t off = kIdxHdr;
    if (ok) {
      for (uint32_t i = 0; i < idx.cap; ++i) {
        if (idx.vals[i] == kEmptyRid) continue;
        char e[16];
        s64(e + 0, static_cast<uint64_t>(idx.keys[i]));
        s32(e + 8, idx.vals[i]);
        s32(e + 12, 0);
        buf.append(e, 16);
        if (buf.size() >= (1u << 20)) {
          if (!pwrite_all(fd, buf.data(), buf.size(), off)) { ok = false; break; }
          off += buf.size();
          buf.clear();
        }
      }
    }
    if (ok && !buf.empty()) {
      if (!pwrite_all(fd, buf.data(), buf.size(), off)) ok = false;
      else off += buf.size();
    }
    ok = ok && fdatasync_fd(fd);
    ::close(fd);
    if (!ok) {
      ::unlink(tmp.c_str());
      if (err) *err = "failed writing " + tmp;
      return false;
    }
    if (::rename(tmp.c_str(), idx_path().c_str()) != 0) {
      if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
      return false;
    }
    return true;
  }

  // Returns false when the cached index cannot be trusted; the caller then
  // rebuilds it by streaming the heap.
  bool load_idx() {
    FILE* f = std::fopen(idx_path().c_str(), "rb");
    if (!f) return false;
    char h[kIdxHdr];
    if (std::fread(h, 1, sizeof(h), f) != sizeof(h)) {
      std::fclose(f);
      return false;
    }
    const bool ok = g32(h + 0) == kIdxMagic && g32(h + 4) == lay.rec_size && g64(h + 8) == gen &&
                    g64(h + 16) == next_rid;
    if (!ok) {
      std::fclose(f);
      return false;
    }
    const uint64_t rows = g64(h + 24);
    idx.clear();
    idx.reset(rows + 8);
    bits.assign(static_cast<size_t>((next_rid + 63) / 64), 0);
    std::string buf;
    buf.resize(1u << 20);
    uint64_t seen = 0;
    for (;;) {
      size_t got = std::fread(&buf[0], 1, buf.size(), f);
      if (got == 0) break;
      size_t n = got / 16;
      for (size_t i = 0; i < n; ++i) {
        const char* e = buf.data() + i * 16;
        const int64_t pk = static_cast<int64_t>(g64(e + 0));
        const uint32_t rid = g32(e + 8);
        if (rid >= next_rid) continue;
        idx.put(pk, rid);
        bit_set(rid);
        ++seen;
      }
      if (got % 16) break;
    }
    std::fclose(f);
    if (seen != rows) return false;
    return true;
  }

  // -------------------------------------------------------------- open ----
  bool open_files(std::string* err) {
    // A checkpoint that died between "write tmp" and "rename" leaves these
    // behind; nothing can be mid-flight while we are opening.
    ::unlink((dat_path() + ".tmp").c_str());
    ::unlink((idx_path() + ".tmp").c_str());
    fd_dat = ::open(dat_path().c_str(), O_RDWR | O_CREAT, 0644);
    fd_str = ::open(str_path().c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_dat < 0 || fd_str < 0) {
      if (err) *err = "cannot open " + dat_path() + ": " + std::strerror(errno);
      return false;
    }
    win.assign(kStrWin, 0);
    struct stat st;
    if (::fstat(fd_dat, &st) != 0) {
      if (err) *err = "stat " + dat_path();
      return false;
    }
    bool hdr_ok = false;
    if (st.st_size >= static_cast<off_t>(kDatHdr)) {
      char h[kDatHdr];
      hdr_ok = pread_all(fd_dat, h, kDatHdr, 0) && decode_header(h);
    }
    if (!hdr_ok) {
      gen = 0;
      ckpt_rids = ckpt_lsn = heap_rids = heap_lsn = 0;
      if (::ftruncate(fd_dat, static_cast<off_t>(kDatHdr)) != 0) {
        if (err) *err = "truncate " + dat_path();
        return false;
      }
      if (!write_header()) {
        if (err) *err = "write " + dat_path();
        return false;
      }
      if (!fdatasync_fd(fd_dat)) {
        if (err) *err = "fsync " + dat_path();
        return false;
      }
    }
    if (::fstat(fd_dat, &st) != 0) return false;
    const uint64_t body = static_cast<uint64_t>(st.st_size - static_cast<off_t>(kDatHdr));
    uint64_t dat_rows = body / rec_size();
    if (body % rec_size()) {  // torn append
      if (::ftruncate(fd_dat, static_cast<off_t>(kDatHdr + dat_rows * rec_size())) != 0) return false;
    }
    if (heap_rids > dat_rows) {
      // Records the header claimed are not on disk: fall back to the last
      // point we know was fsynced (the checkpoint) and replay from there.
      heap_rids = ckpt_rids <= dat_rows ? ckpt_rids : dat_rows;
      heap_lsn = ckpt_lsn;
      if (::ftruncate(fd_dat, static_cast<off_t>(kDatHdr + heap_rids * rec_size())) != 0) return false;
    } else if (dat_rows > heap_rids) {
      // Past the commit watermark: an uncommitted transaction or versions
      // that were appended but never acknowledged. Drop them; replay rebuilds.
      if (::ftruncate(fd_dat, static_cast<off_t>(kDatHdr + heap_rids * rec_size())) != 0) return false;
    }
    next_rid = heap_rids;
    flushed_rids = heap_rids;
    if (::fstat(fd_str, &st) != 0) return false;
    str_bytes = static_cast<uint64_t>(st.st_size);
    str_flushed = str_bytes;
    if (!load_idx()) rebuild_index();
    return true;
  }

  // ---------------------------------------------------------- compaction --
  bool compact(uint64_t lsn, std::string* err) {
    if (!flush()) {
      if (err) *err = "flush " + dat_path();
      return false;
    }
    const uint64_t out_rows = idx.occ;
    const std::string tmp = dat_path() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      if (err) *err = "open " + tmp + ": " + std::strerror(errno);
      return false;
    }
    const uint64_t new_gen = gen + 1;
    char h[kDatHdr];
    std::memset(h, 0, sizeof(h));
    s32(h + 0, kDatMagic);
    s32(h + 4, static_cast<uint32_t>(new_gen));
    s32(h + 8, lay.rec_size);
    s64(h + 16, out_rows);
    s64(h + 24, lsn);
    s64(h + 32, out_rows);
    s64(h + 40, lsn);
    s64(h + 48, out_rows);
    s32(h + 56, crc32_of(h, 56));
    bool ok = pwrite_all(fd, h, sizeof(h), 0);
    std::string out;
    out.reserve(1u << 20);
    uint64_t w_off = kDatHdr;
    uint64_t new_rid = 0;
    if (ok) {
      const size_t rs = static_cast<size_t>(rec_size());
      std::vector<char> buf;
      buf.resize(kReadBuf + rs);
      uint64_t rid = 0;
      while (rid < next_rid && ok) {
        const uint64_t left = next_rid - rid;
        const size_t want = static_cast<size_t>(std::min<uint64_t>(kReadBuf, left * rec_size()));
        if (!pread_all(fd_dat, buf.data(), want, kDatHdr + rid * rec_size())) { ok = false; break; }
        const size_t nrec = want / rec_size();
        const char* p = buf.data();
        for (size_t i = 0; i < nrec; ++i, p += rs) {
          if (!bit_get(rid + i)) continue;
          out.append(p, rs);
          // The pk index is renumbered in place: repointing vals[] cannot
          // disturb the probe chains, which depend on keys[] only. This keeps
          // compaction free of a second index-sized allocation.
          const int64_t pk = static_cast<int64_t>(g64(p + 8));
          const uint32_t slot = idx.probe(pk);
          if (idx.vals[slot] != kEmptyRid) idx.vals[slot] = static_cast<uint32_t>(new_rid);
          ++new_rid;
          if (out.size() >= (1u << 20)) {
            if (!pwrite_all(fd, out.data(), out.size(), w_off)) { ok = false; break; }
            w_off += out.size();
            out.clear();
          }
        }
        rid += nrec;
      }
    }
    if (ok && !out.empty()) {
      if (!pwrite_all(fd, out.data(), out.size(), w_off)) ok = false;
    }
    ok = ok && fdatasync_fd(fd);
    ::close(fd);
    if (!ok) {
      ::unlink(tmp.c_str());
      if (err) *err = "failed writing " + tmp;
      return false;
    }
    if (::rename(tmp.c_str(), dat_path().c_str()) != 0) {
      if (err) *err = "rename " + tmp + ": " + std::strerror(errno);
      return false;
    }
    // The rename replaced the inode underneath us: reopen, so every later
    // append and read lands in the compacted file instead of the old inode.
    ::close(fd_dat);
    fd_dat = ::open(dat_path().c_str(), O_RDWR, 0644);
    if (fd_dat < 0) {
      if (err) *err = "reopen " + dat_path() + ": " + std::strerror(errno);
      return false;
    }
    gen = new_gen;
    ckpt_rids = heap_rids = next_rid = flushed_rids = out_rows;
    ckpt_lsn = heap_lsn = lsn;
    bits.assign(static_cast<size_t>((out_rows + 63) / 64), 0);
    for (uint64_t i = 0; i < out_rows; ++i) bit_set(i);
    dat_buf.clear();
    return write_idx(err);
  }

  // Flush + make everything durable + refresh the watermark and cached index.
  bool seal(uint64_t lsn, std::string* err) {
    if (!flush()) {
      if (err) *err = "flush " + dat_path();
      return false;
    }
    if (!fdatasync_fd(fd_dat) || !fdatasync_fd(fd_str)) return true;  // best effort
    heap_rids = next_rid;
    heap_lsn = lsn;
    write_header();
    fdatasync_fd(fd_dat);
    write_idx(nullptr);
    (void)err;
    return true;
  }
};

namespace {

// ===========================================================================
// Predicates (unchanged semantics)
// ===========================================================================
struct RCond {
  uint32_t col = 0;
  uint8_t op = 0;  // Cond::Op
  uint8_t vt = 0;  // Type of the column
  const Value* lit = nullptr;
};

struct Pred {
  std::vector<RCond> c;
  std::vector<Value> owned;  // coerced literals; reserved so pointers stay valid
  bool never = false;
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
      out->never = true;
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

// Appends `vals` as the current version of its pk (INSERT semantics: the key
// must be absent, or the previous version is superseded -- which is what replay
// needs to be idempotent).
bool store_row(Table& tb, const std::vector<Value>& vals, bool replace) {
  TableStorage& st = *tb.store;
  const int64_t key = vals[static_cast<size_t>(tb.schema.pk_idx)].i;
  uint32_t prev = 0;
  if (st.idx.find(key, &prev)) {
    if (!replace) return false;
    st.bit_clear(prev);
    st.idx.del(key);
  }
  const uint32_t rid = st.append_raw(tb.schema, vals, 0);
  if (rid == kEmptyRid) return false;
  st.bit_set(rid);
  st.idx.put(key, rid);
  tb.next_rid = static_cast<int64_t>(st.next_rid) + 1;
  return true;
}

bool store_tomb(Table& tb, int64_t pk) {
  TableStorage& st = *tb.store;
  uint32_t prev = 0;
  if (!st.idx.find(pk, &prev)) return false;
  const uint32_t rid = st.append_tomb(pk);
  if (rid == kEmptyRid) return false;
  st.bit_clear(prev);
  st.idx.del(pk);
  tb.next_rid = static_cast<int64_t>(st.next_rid) + 1;
  return true;
}

}  // namespace

// ===========================================================================
// Table
// ===========================================================================
size_t Table::live_count() const {
  if (store) return static_cast<size_t>(store->live_rows());
  if (dead.empty()) return rows.size();
  const size_t n = rows.size() < dead.size() ? rows.size() : dead.size();
  size_t live = 0;
  for (size_t i = 0; i < n; ++i) live += (dead[i] == 0);
  return live + (rows.size() > n ? rows.size() - n : 0);
}

// ===========================================================================
// Undo log / transaction watermarks
// ===========================================================================
struct Database::Undo {
  enum K : uint8_t { K_UPD, K_DEL, K_DROP } k = K_UPD;
  std::string table;
  int64_t pk = 0;      // pk before the change
  int64_t newpk = 0;   // K_UPD: pk after the change
  uint32_t old_rid = kEmptyRid;
  std::unique_ptr<Table> dropped;  // K_DROP
};

struct Database::TxnMark {
  std::string table;
  uint64_t rid = 0;
  uint64_t str = 0;
  uint64_t lsn = 0;
};

// ===========================================================================
// Database
// ===========================================================================
Database::Database() = default;

Database::~Database() { close(); }

void Database::close() {
  if (wal_) {
    if (txn_active_) {
      std::string e;
      int c = 0;
      rollback(&e, &c);  // an uncommitted transaction never survives a close
    }
    const uint64_t lsn = wal_rows_;
    flush_wal();
    for (auto& kv : tables_)
      if (kv.second.store) kv.second.store->seal(lsn, nullptr);
    std::fflush(wal_);
    std::fclose(wal_);
    wal_ = nullptr;
  }
  tables_.clear();
  undo_.clear();
  marks_.clear();
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
  ckpt_base_ = kAutoCheckpointBytes;
  if (const char* e = std::getenv("MVDB_CKPT_BYTES")) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(e, &end, 10);
    if (end && *end == '\0' && v >= 4096) ckpt_base_ = static_cast<uint64_t>(v);
  }
  if (!ensure_dir(dir_)) {
    if (err) *err = "cannot create data directory '" + dir_ + "': " + std::strerror(errno);
    return false;
  }
  if (!load_catalog(err)) {
    close();
    return false;
  }
  for (auto& kv : tables_)
    if (!open_table(kv.second, err)) {
      close();
      return false;
    }
  if (!replay_wal(err)) {
    close();
    return false;
  }
  const std::string log = dir_ + "/data.log";
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

bool Database::open_table(Table& tb, std::string* err) {
  tb.store.reset(new TableStorage());
  tb.store->name = tb.schema.name;
  tb.store->base = dir_ + "/t_" + table_tag(tb.schema.name);
  build_layout(tb.schema, &tb.store->lay);
  return tb.store->open_files(err);
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

  // A table name that was used before must not inherit stale heap files.
  const std::string base = dir_ + "/t_" + table_tag(name);
  for (const char* ext : {".dat", ".str", ".idx", ".dat.tmp", ".idx.tmp"})
    ::unlink((base + ext).c_str());

  Table t;
  t.schema.name = name;
  t.schema.cols = cols;
  t.schema.pk_idx = pk_idx;
  t.schema.vec_idx = vec_idx;
  t.next_rid = 1;

  auto ins = tables_.emplace(name, std::move(t));
  Table& tb = ins.first->second;
  if (!open_table(tb, err)) {
    tables_.erase(ins.first);
    set_err(err, code, ErrCode::TXN, "cannot create storage: " + (err ? *err : ""));
    return false;
  }
  if (!save_catalog_at(dir_, tables_, err)) {
    tables_.erase(ins.first);
    set_err(err, code, ErrCode::TXN, "cannot persist catalog: " + (err ? *err : ""));
    return false;
  }
  // WAL marker so that replay is order-faithful even if the catalog write is lost.
  {
    const TableSchema& s = tb.schema;
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
    if (it->second.store) it->second.store->unlink_on_close = true;
    tables_.erase(it);
  }
  if (!save_catalog_at(dir_, tables_, err)) {
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
void Database::apply_insert(const std::string& t, const std::vector<Value>& vals, int64_t* pk) {
  auto it = tables_.find(t);
  if (it == tables_.end()) {
    if (pk) *pk = 0;
    return;
  }
  Table& tb = it->second;
  const int64_t key = vals.size() > static_cast<size_t>(tb.schema.pk_idx)
                          ? vals[static_cast<size_t>(tb.schema.pk_idx)].i
                          : 0;
  store_row(tb, vals, true);
  if (pk) *pk = key;
}

void Database::append_wal(const std::string& rec) {
  if (!wal_) return;
  wal_out_.append(rec);
  wal_bytes_ += rec.size();  // logical size: pending bytes count for the limit
  if (wal_out_.size() >= (256u << 10)) flush_wal();
}

void Database::flush_wal() {
  if (!wal_ || wal_out_.empty()) return;
  size_t done = 0;
  write_all_fd(::fileno(wal_), wal_out_.data(), wal_out_.size(), &done);
  if (done != wal_out_.size()) wal_err_ = true;
  wal_out_.clear();
}

bool Database::fsync_wal() {
  if (!wal_) return false;
  flush_wal();
  if (wal_err_) return false;
  if (std::fflush(wal_) != 0) return false;
  if (!durable_mode()) return true;
  return ::fdatasync(::fileno(wal_)) == 0;
}

void Database::mark_table(const std::string& name) {
  if (!txn_active_) return;
  for (const TxnMark& m : marks_)
    if (m.table == name) return;
  auto it = tables_.find(name);
  if (it == tables_.end() || !it->second.store) return;
  TxnMark m;
  m.table = name;
  m.rid = it->second.store->next_rid;
  m.str = it->second.store->str_bytes;
  m.lsn = wal_rows_;
  marks_.push_back(std::move(m));
}

void Database::note_commit() {
  // Every table the transaction touched is flushed and its commit watermark
  // advanced, so a crash right after COMMIT finds a heap that already holds
  // those rows and has nothing to replay.
  if (!marks_.empty()) {
    for (const TxnMark& m : marks_) {
      auto it = tables_.find(m.table);
      if (it == tables_.end() || !it->second.store) continue;
      it->second.store->flush();
      it->second.store->set_watermark(it->second.store->next_rid, wal_rows_);
    }
    return;
  }
  for (auto& kv : tables_) {
    TableStorage* st = kv.second.store.get();
    if (!st || st->next_rid == st->heap_rids) continue;
    st->flush();
    st->set_watermark(st->next_rid, wal_rows_);
  }
}

uint64_t Database::wal_limit() const {
  uint64_t data = 0;
  for (const auto& kv : tables_)
    if (kv.second.store) data += kv.second.store->next_rid * kv.second.store->rec_size();
  return std::max(ckpt_base_, data);
}

bool Database::insert(const std::string& t, std::vector<Value> vals, std::string* err,
                      int* code) {
  auto it = tables_.find(t);
  if (it == tables_.end() || !it->second.store) {
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
  if (tb.store->idx.find(key, nullptr)) {
    set_err(err, code, ErrCode::DUP_KEY,
            "Duplicate entry '" + std::to_string(key) + "' for key 'PRIMARY'");
    return false;
  }

  // Write-ahead: the record hits the log before the row becomes visible.
  {
    const uint64_t lsn = ++wal_rows_;
    std::string& pay = wal_pay_;
    pay.clear();
    pay.reserve(64 + vals.size() * 16);
    put_u8(pay, REC_INSERT);
    put_u8(pay, txn_active_ ? RF_TXN : 0);
    put_u64(pay, lsn);
    put_str(pay, t);
    put_u32(pay, static_cast<uint32_t>(vals.size()));
    for (const Value& v : vals) put_value(pay, v);
    uint32_t len = static_cast<uint32_t>(pay.size());
    uint32_t crc = crc32_of(pay.data(), pay.size());
    wal_rec_.clear();
    wal_rec_.reserve(pay.size() + 8);
    wal_rec_.append(reinterpret_cast<const char*>(&len), 4);
    wal_rec_.append(reinterpret_cast<const char*>(&crc), 4);
    wal_rec_.append(pay);
  }
  if (txn_active_) mark_table(t);
  append_wal(wal_rec_);
  if (!txn_active_ && !fsync_wal()) {
    set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
    return false;
  }

  if (!store_row(tb, vals, false)) {
    set_err(err, code, ErrCode::TXN, "row store failed");
    return false;
  }
  if (!txn_active_) {
    tb.store->flush();
    tb.store->set_watermark(tb.store->next_rid, wal_rows_);
    if (wal_bytes_ > wal_limit()) {
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
  if (it == tables_.end() || !it->second.store) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return -1;
  }
  Table& tb = it->second;
  const TableSchema& sc = tb.schema;
  TableStorage& st = *tb.store;
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
  int64_t rc = 0;
  const uint64_t n0 = st.next_rid;
  Row scratch;
  scratch.vals.resize(sc.cols.size());
  std::vector<Value> nv;

  // "UPDATE ... WHERE pk = x" is one random read through the offset map, not a
  // scan. pk_eq: -1 = no pk equality in the predicate, 0 = absent, 1 = present.
  int pk_eq = -1;
  uint32_t one = 0;
  for (const RCond& c : pr.c) {
    if (static_cast<int>(c.col) != sc.pk_idx || c.op != static_cast<uint8_t>(Cond::EQ)) continue;
    pk_eq = st.idx.find(c.lit->i, &one) ? 1 : 0;
    break;
  }

  auto body = [&](uint64_t rid, const char* raw) -> bool {
    st.decode(raw, sc, &scratch, rid);
    if (!row_matches(pr, scratch)) return true;

    nv = scratch.vals;
    for (size_t k = 0; k < sets.size(); ++k) {
      Value v = sets[k].second;  // may be coerced in place
      if (!check_value(sc.cols[static_cast<size_t>(setcol[k])], &v, err)) {
        if (code) *code = static_cast<int>(ErrCode::BAD_VALUE);
        rc = -1;
        return false;
      }
      nv[static_cast<size_t>(setcol[k])] = std::move(v);
    }
    const int64_t oldpk = scratch.vals[static_cast<size_t>(sc.pk_idx)].i;
    const int64_t newpk = nv[static_cast<size_t>(sc.pk_idx)].i;
    if (newpk != oldpk && st.idx.find(newpk, nullptr)) {
      set_err(err, code, ErrCode::DUP_KEY,
              "Duplicate entry '" + std::to_string(newpk) + "' for key 'PRIMARY'");
      rc = -1;
      return false;
    }

    {
      std::string& pay = wal_pay_;
      pay.clear();
      const uint64_t lsn = ++wal_rows_;
      put_u8(pay, REC_UPDATE);
      put_u8(pay, txn_active_ ? RF_TXN : 0);
      put_u64(pay, lsn);
      put_str(pay, t);
      put_i64(pay, oldpk);
      put_u32(pay, static_cast<uint32_t>(nv.size()));
      for (const Value& v : nv) put_value(pay, v);
      uint32_t len = static_cast<uint32_t>(pay.size());
      uint32_t crc = crc32_of(pay.data(), pay.size());
      wal_rec_.resize(8);
      std::memcpy(&wal_rec_[0], &len, 4);
      std::memcpy(&wal_rec_[4], &crc, 4);
      wal_rec_.append(pay);
    }
    if (txn_active_) mark_table(t);
    append_wal(wal_rec_);
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
      rc = -1;
      return false;
    }
    if (txn_active_) {
      Undo u;
      u.k = Undo::K_UPD;
      u.table = t;
      u.pk = oldpk;
      u.newpk = newpk;
      u.old_rid = static_cast<uint32_t>(rid);
      undo_.push_back(std::move(u));
    }
    const uint32_t new_rid = st.append_raw(sc, nv, 0);
    if (new_rid == kEmptyRid) {
      set_err(err, code, ErrCode::TXN, "row store failed");
      rc = -1;
      return false;
    }
    st.bit_clear(static_cast<uint32_t>(rid));
    st.idx.del(oldpk);
    st.idx.put(newpk, new_rid);
    st.bit_set(new_rid);
    tb.next_rid = static_cast<int64_t>(st.next_rid) + 1;
    ++affected;
    return true;
  };
  if (pk_eq < 0) {
    st.for_each_live(0, n0, body);
  } else if (pk_eq == 1) {
    st.flush();
    std::vector<char> rec0(static_cast<size_t>(st.rec_size()));
    if (st.read_rec(one, rec0.data()) && st.bit_get(one)) body(one, rec0.data());
  }
  if (rc != 0) return rc;
  if (!txn_active_) {
    st.flush();
    st.set_watermark(st.next_rid, wal_rows_);
    if (wal_bytes_ > wal_limit()) {
      std::string ignored;
      checkpoint(&ignored);
    }
  }
  return affected;
}

int64_t Database::delete_rows(const std::string& t, const std::vector<Cond>& conds,
                              std::string* err, int* code) {
  auto it = tables_.find(t);
  if (it == tables_.end() || !it->second.store) {
    set_err(err, code, ErrCode::NO_SUCH_TABLE, "table '" + t + "' does not exist");
    return -1;
  }
  Table& tb = it->second;
  TableStorage& st = *tb.store;
  Pred pr;
  compile_pred(tb, conds, &pr);
  if (pr.never) return 0;

  int64_t affected = 0;
  int64_t rc = 0;
  const uint64_t n0 = st.next_rid;
  Row scratch;
  scratch.vals.resize(tb.schema.cols.size());

  // Same shortcut as UPDATE: a pk equality condition is a single random read.
  int pk_eq = -1;
  uint32_t one = 0;
  for (const RCond& c : pr.c) {
    if (static_cast<int>(c.col) != tb.schema.pk_idx ||
        c.op != static_cast<uint8_t>(Cond::EQ))
      continue;
    pk_eq = st.idx.find(c.lit->i, &one) ? 1 : 0;
    break;
  }

  auto body = [&](uint64_t rid, const char* raw) -> bool {
    st.decode(raw, tb.schema, &scratch, rid);
    if (!row_matches(pr, scratch)) return true;
    const int64_t pk = scratch.vals[static_cast<size_t>(tb.schema.pk_idx)].i;

    {
      std::string& pay = wal_pay_;
      pay.clear();
      const uint64_t lsn = ++wal_rows_;
      put_u8(pay, REC_DELETE);
      put_u8(pay, txn_active_ ? RF_TXN : 0);
      put_u64(pay, lsn);
      put_str(pay, t);
      put_i64(pay, pk);
      uint32_t len = static_cast<uint32_t>(pay.size());
      uint32_t crc = crc32_of(pay.data(), pay.size());
      wal_rec_.resize(8);
      std::memcpy(&wal_rec_[0], &len, 4);
      std::memcpy(&wal_rec_[4], &crc, 4);
      wal_rec_.append(pay);
    }
    if (txn_active_) mark_table(t);
    append_wal(wal_rec_);
    if (!txn_active_ && !fsync_wal()) {
      set_err(err, code, ErrCode::TXN, "WAL fdatasync failed");
      rc = -1;
      return false;
    }
    if (txn_active_) {
      Undo u;
      u.k = Undo::K_DEL;
      u.table = t;
      u.pk = pk;
      u.old_rid = static_cast<uint32_t>(rid);
      undo_.push_back(std::move(u));
    }
    st.bit_clear(static_cast<uint32_t>(rid));
    st.idx.del(pk);
    st.append_tomb(pk);
    tb.next_rid = static_cast<int64_t>(st.next_rid) + 1;
    ++affected;
    return true;
  };
  if (pk_eq < 0) {
    st.for_each_live(0, n0, body);
  } else if (pk_eq == 1) {
    st.flush();
    std::vector<char> rec0(static_cast<size_t>(st.rec_size()));
    if (st.read_rec(one, rec0.data()) && st.bit_get(one)) body(one, rec0.data());
  }
  if (rc != 0) return rc;
  if (!txn_active_) {
    st.flush();
    st.set_watermark(st.next_rid, wal_rows_);
    if (wal_bytes_ > wal_limit()) {
      std::string ignored;
      checkpoint(&ignored);
    }
  }
  return affected;
}

// ------------------------------------------------------------------ scan ---
void Database::scan(const std::string& t, const std::vector<Cond>& conds,
                    const std::function<bool(const Row&)>& cb) {
  auto it = tables_.find(t);
  if (it == tables_.end() || !it->second.store) return;
  Table& tb = it->second;
  TableStorage& st = *tb.store;
  Pred pr;
  compile_pred(tb, conds, &pr);
  if (pr.never) return;

  Row scratch;
  scratch.vals.resize(tb.schema.cols.size());

  // Primary key equality: one random read instead of a scan.
  for (const RCond& c : pr.c) {
    if (static_cast<int>(c.col) == tb.schema.pk_idx && c.op == static_cast<uint8_t>(Cond::EQ)) {
      uint32_t rid = 0;
      if (!st.idx.find(c.lit->i, &rid)) return;
      st.flush();
      std::vector<char> rec(static_cast<size_t>(st.rec_size()));
      if (!st.read_rec(rid, rec.data())) return;
      st.decode(rec.data(), tb.schema, &scratch, rid);
      if (row_matches(pr, scratch)) cb(scratch);
      return;
    }
  }

  st.for_each_live(0, st.next_rid, [&](uint64_t rid, const char* raw) {
    st.decode(raw, tb.schema, &scratch, rid);
    if (!row_matches(pr, scratch)) return true;
    return cb(scratch);
  });
}

// ------------------------------------------------------------ transactions -
bool Database::begin(std::string* err, int* code) {
  if (txn_active_) {
    set_err(err, code, ErrCode::TXN, "a transaction is already in progress");
    return false;
  }
  txn_active_ = true;
  undo_.clear();
  marks_.clear();
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
    for (Undo& u : undo_)
      if (u.k == Undo::K_DROP && u.dropped && u.dropped->store) u.dropped->store->unlink_on_close = true;
    undo_.clear();
    undo_.shrink_to_fit();
    txn_active_ = false;
    note_commit();  // flush the touched heaps, advance their commit watermark
    marks_.clear();
    if (wal_bytes_ > wal_limit()) {
      std::string ignored;
      checkpoint(&ignored);
    }
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
    if (it == tables_.end() || !it->second.store) continue;  // table dropped later
    TableStorage& st = *it->second.store;
    if (u.k == Undo::K_UPD) {
      st.idx.del(u.newpk);
      st.idx.put(u.pk, u.old_rid);
      st.bit_set(u.old_rid);
    } else if (u.k == Undo::K_DEL) {
      st.idx.put(u.pk, u.old_rid);
      st.bit_set(u.old_rid);
    }
  }
  // Everything appended by the transaction is dropped: the heap files go back
  // to the watermarks taken at the transaction's first write.
  for (const TxnMark& m : marks_) {
    auto it = tables_.find(m.table);
    if (it == tables_.end() || !it->second.store) continue;
    TableStorage& st = *it->second.store;
    st.truncate_to(m.rid, m.str);
    st.index_drop_above(m.rid);
    // Undo entries above the watermark were restored above and point below it.
    st.heap_rids = st.next_rid;
    st.heap_lsn = m.lsn;
    st.write_header();
    it->second.next_rid = static_cast<int64_t>(st.next_rid) + 1;
  }
  undo_.clear();
  undo_.shrink_to_fit();
  marks_.clear();
  txn_active_ = false;
  // DDL effects (catalog.bin) may have been undone; rewrite it from memory.
  std::string ignored;
  save_catalog_at(dir_, tables_, &ignored);
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
  if (!save_catalog_at(dir_, tables_, err)) {
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
bool Database::load_catalog(std::string* err) {
  std::string blob;
  if (read_file(dir_ + "/catalog.bin", &blob) && !blob.empty()) {
    if (!decode_catalog_at(blob, &tables_, err)) {
      tables_.clear();
      return false;
    }
  }
  wal_rows_ = 0;
  // snapshot.bin only carries the checkpoint LSN now; the per-table LSN lives
  // in each .dat header.
  std::string meta;
  if (read_file(dir_ + "/snapshot.bin", &meta) && meta.size() >= 16) {
    Rd r(meta.data(), meta.size() - 4);
    uint32_t stored = 0;
    std::memcpy(&stored, meta.data() + meta.size() - 4, 4);
    if (crc32_of(meta.data(), meta.size() - 4) == stored && r.u32() == kSnapMagic) {
      const uint64_t lsn = r.u64();
      if (lsn > wal_rows_) wal_rows_ = lsn;
    }
  }
  return true;
}

bool Database::replay_wal(std::string* err) {
  (void)err;
  const std::string path = dir_ + "/data.log";
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return true;  // nothing to replay
  uint64_t max_lsn = wal_rows_;
  uint64_t good_end = 0;   // byte offset just past the last valid record
  bool torn = false;

  // ---- pass 1: which explicit transactions actually committed? -------------
  // Only the (begin, commit) LSN pairs are kept, so a multi-million-row
  // transaction never has to be buffered in RAM to be discarded on a crash.
  struct Range {
    uint64_t begin, end;
  };
  std::vector<Range> committed;
  {
    std::string pay;
    for (;;) {
      uint8_t hdr[8];
      size_t got = std::fread(hdr, 1, 8, f);
      if (got == 0) break;
      if (got != 8) break;
      uint32_t len = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                     (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
      uint32_t crc = static_cast<uint32_t>(hdr[4]) | (static_cast<uint32_t>(hdr[5]) << 8) |
                     (static_cast<uint32_t>(hdr[6]) << 16) | (static_cast<uint32_t>(hdr[7]) << 24);
      if (len == 0 || len > kMaxRec) { torn = true; break; }
      pay.resize(len);
      if (std::fread(&pay[0], 1, len, f) != len) { torn = true; break; }
      if (crc32_of(pay.data(), pay.size()) != crc) { torn = true; break; }
      if (len < 10) { torn = true; break; }
      good_end += 8 + static_cast<uint64_t>(len);
      Rd r(pay.data(), pay.size());
      const uint8_t kind = r.u8();
      const uint8_t flags = r.u8();
      const uint64_t lsn = r.u64();
      if (lsn > max_lsn) max_lsn = lsn;
      if ((flags & RF_TXN) && kind == REC_TXN_BEGIN) {
        committed.push_back(Range{lsn, 0});
      } else if ((flags & RF_TXN) && kind == REC_TXN_COMMIT) {
        if (!committed.empty() && committed.back().end == 0) committed.back().end = lsn;
      } else if (!(flags & RF_TXN) && !committed.empty() && committed.back().end == 0) {
        committed.pop_back();  // orphaned begin
      }
    }
    // Drop unterminated transactions.
    while (!committed.empty() && committed.back().end == 0) committed.pop_back();
  }

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
  if (max_lsn > wal_rows_) wal_rows_ = max_lsn;

  // Every heap may already hold every committed row (the commit watermark is
  // written ahead of the WAL being truncated). Then nothing has to be
  // re-appended and the second pass can be skipped entirely.
  bool need_apply = false;
  for (auto& kv : tables_) {
    if (!kv.second.store) continue;
    if (kv.second.store->heap_lsn < max_lsn) { need_apply = true; break; }
  }
  if (!need_apply) {
    std::fclose(f);
    return true;
  }

  // ---- pass 2: apply the committed records --------------------------------
  std::fseek(f, 0, SEEK_SET);
  size_t rpos = 0;
  std::string pay;
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
    pay.resize(len);
    if (std::fread(&pay[0], 1, len, f) != len) { torn = true; break; }
    if (crc32_of(pay.data(), pay.size()) != crc) { torn = true; break; }

    Rd r(pay.data(), pay.size());
    const uint8_t kind = r.u8();
    const uint8_t flags = r.u8();
    const uint64_t lsn = r.u64();
    bool ok = !r.bad;
    std::string name;
    std::vector<Value> vals;
    TableSchema schema;
    int64_t pk = 0;
    if (ok) {
      switch (kind) {
        case REC_INSERT:
        case REC_UPDATE:
        case REC_DELETE:
        case REC_DROP_TABLE: {
          ok = r.str(&name);
          if (ok && (kind == REC_UPDATE || kind == REC_DELETE)) pk = r.i64();
          if (ok && (kind == REC_INSERT || kind == REC_UPDATE)) {
            uint32_t n = r.u32();
            ok = !r.bad && n <= kMaxCols;
            if (ok) {
              vals.resize(n);
              for (uint32_t k = 0; k < n && ok; ++k) ok = get_value(r, &vals[k]);
            }
          }
          ok = ok && !r.bad;
          break;
        }
        case REC_CREATE_TABLE:
          ok = get_schema(r, &schema);
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
    if (lsn > max_lsn) max_lsn = lsn;

    if (flags & RF_TXN) {
      while (rpos < committed.size() && committed[rpos].end < lsn) ++rpos;
      const bool inside = rpos < committed.size() && lsn > committed[rpos].begin &&
                          lsn < committed[rpos].end;
      if (!inside) continue;  // aborted / crashed transaction
    }

    switch (kind) {
      case REC_INSERT: {
        auto it = tables_.find(name);
        if (it == tables_.end() || !it->second.store) break;
        if (lsn <= it->second.store->heap_lsn) break;  // already in the heap
        apply_insert(name, vals, nullptr);
        break;
      }
      case REC_UPDATE: {
        auto it = tables_.find(name);
        if (it == tables_.end() || !it->second.store) break;
        Table& tb = it->second;
        if (lsn <= tb.store->heap_lsn) break;
        uint32_t rid = 0;
        if (!tb.store->idx.find(pk, &rid)) break;
        store_row(tb, vals, true);
        break;
      }
      case REC_DELETE: {
        auto it = tables_.find(name);
        if (it == tables_.end() || !it->second.store) break;
        if (lsn <= it->second.store->heap_lsn) break;
        store_tomb(it->second, pk);
        break;
      }
      case REC_CREATE_TABLE: {
        auto ex = tables_.find(schema.name);
        if (ex != tables_.end()) {
          if (same_schema(ex->second.schema, schema)) break;  // already materialised
          ex->second.store.reset();  // close + unlink the old generation
          tables_.erase(ex);
        }
        Table t;
        t.schema = schema;
        t.next_rid = 1;
        auto ins = tables_.emplace(t.schema.name, std::move(t));
        std::string e2;
        open_table(ins.first->second, &e2);
        break;
      }
      case REC_DROP_TABLE: {
        auto ex = tables_.find(name);
        if (ex != tables_.end()) {
          if (ex->second.store) ex->second.store->unlink_on_close = true;
          tables_.erase(ex);
        }
        break;
      }
      default:
        break;
    }
  }
  std::fclose(f);
  if (max_lsn > wal_rows_) wal_rows_ = max_lsn;
  return true;
}

bool Database::checkpoint_table(Table& tb, uint64_t lsn, std::string* err) {
  if (!tb.store) return true;
  return tb.store->compact(lsn, err);
}

bool Database::save_checkpoint_meta(std::string* err) {
  std::string b;
  put_u32(b, kSnapMagic);
  put_u64(b, wal_rows_);
  put_u32(b, static_cast<uint32_t>(tables_.size()));
  for (const auto& kv : tables_) {
    const Table& t = kv.second;
    put_str(b, t.schema.name);
    put_u32(b, t.store ? static_cast<uint32_t>(t.store->gen) : 0);
    put_u64(b, t.store ? t.store->next_rid : 0);
    put_u64(b, t.store ? t.store->live_rows() : 0);
  }
  put_u32(b, crc32_of(b.data(), b.size()));
  if (!write_atomic(dir_ + "/snapshot.bin", b, err)) return false;
  return write_atomic(dir_ + "/snapshot.lsn", std::to_string(wal_rows_) + "\n", err);
}

bool Database::checkpoint(std::string* err) {
  if (!wal_) {
    if (err) *err = "database is not open";
    return false;
  }
  if (txn_active_) {
    // Defer: compacting mid-transaction would bake uncommitted rows into the
    // heap. The next call after COMMIT does the work.
    return true;
  }
  const uint64_t lsn = wal_rows_;
  flush_wal();
  if (wal_err_) {
    if (err) *err = "WAL write failed";
    return false;
  }
  for (auto& kv : tables_)
    if (!checkpoint_table(kv.second, lsn, err)) return false;
  if (!save_catalog_at(dir_, tables_, err)) return false;
  if (!save_checkpoint_meta(err)) return false;
  fsync_dir(dir_);

  // The heaps are durable: truncating the WAL cannot lose anything.
  if (std::fflush(wal_) != 0 || ::ftruncate(::fileno(wal_), 0) != 0) {
    if (err) *err = "cannot truncate data.log";
    return false;
  }
  if (durable_mode()) ::fdatasync(::fileno(wal_));
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

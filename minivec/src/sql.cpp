// sql.cpp — hand-written recursive-descent parser for the MiniVectorDB SQL subset.
//
// Supported grammar (keywords case-insensitive):
//   CREATE TABLE [IF NOT EXISTS] t ( id BIGINT PRIMARY KEY, name VARCHAR(20),
//                                    emb VECTOR(4) [, ...] );
//   DROP TABLE [IF EXISTS] t;
//   INSERT INTO t [(c1,c2,...)] VALUES (v,...) [, (v,...)]*;
//   SELECT * | item [, item]* FROM t
//          [WHERE cond [AND cond]*] [ORDER BY col|alias [ASC|DESC]] [LIMIT n];
//   UPDATE t SET c = v [, c = v]* [WHERE ...];
//   DELETE FROM t [WHERE ...];
//   CREATE VECTOR INDEX idx ON t (emb);
//   SHOW TABLES; BEGIN; START TRANSACTION; COMMIT; ROLLBACK;
//
// Errors: 1064 = syntax error, 1235 = recognised construct that MiniVectorDB
// does not implement (OR, JOIN, GROUP BY, aggregates, subqueries, ALTER,
// savepoints, ...). Nothing in here ever crashes: every failure path throws a
// ParseError that parse_sql converts into (false, message, code).
#include "sql.h"
#include "short_decimal.h"
#include "parse_pool.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mvdb {
namespace {

// ------------------------------------------------------------------ errors --

struct ParseError {
  int code;
  std::string msg;
  ParseError(int c, std::string m) : code(c), msg(std::move(m)) {}
};

[[noreturn]] void throw_err(int code, std::string msg) {
  throw ParseError(code, std::move(msg));
}

// ------------------------------------------------------------- char helpers --

bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool is_alpha(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_ident_start(unsigned char c) {
  return is_alpha(c) || c == '_' || c >= 0x80;  // permit UTF-8 identifiers
}
bool is_ident_char(unsigned char c) {
  return is_ident_start(c) || is_digit(c) || c == '$';
}

std::string to_upper(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
  return r;
}

// ------------------------------------------------------------------ tokens --

enum TokKind { TK_END, TK_IDENT, TK_INT, TK_NUM, TK_STR, TK_VEC, TK_PUNCT };

struct Tok {
  TokKind kind = TK_END;
  std::string text;  // IDENT: name, STR: decoded text, VEC: body, PUNCT: operator
  bool quoted = false;
  size_t pos = 0;
};

void tokenize(const std::string& s, std::vector<Tok>* out) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (is_space(c)) {
      ++i;
      continue;
    }
    // comments (the caller strips them, but parse_sql may be called directly)
    if (c == '-' && i + 1 < n && s[i + 1] == '-') {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '#') {
      while (i < n && s[i] != '\n') ++i;
      continue;
    }
    if (c == '/' && i + 1 < n && s[i + 1] == '*') {
      size_t end = s.find("*/", i + 2);
      if (end == std::string::npos) throw_err(1064, "unterminated block comment");
      i = end + 2;
      continue;
    }
    const size_t start = i;

    // Fast path: a quoted literal with no escape sequence and no doubled quote
    // is copied in one pass instead of being rebuilt byte by byte.
    if (c == '\'' || c == '"') {
      const char q = (char)c;
      // memchr hands the scan to the vectorised libc search instead of walking
      // the literal one byte at a time.
      const char* hit = static_cast<const char*>(
          std::memchr(s.data() + i + 1, q, n - i - 1));
      if (hit != nullptr) {
        const size_t j = static_cast<size_t>(hit - s.data());
        bool simple = true;
        if (std::memchr(s.data() + i + 1, '\\', j - i - 1) != nullptr) simple = false;
        else if (j + 1 < n && s[j + 1] == q) simple = false;
        if (simple) {
          Tok t;
          t.kind = TK_STR;
          t.text.assign(s, i + 1, j - i - 1);
          t.pos = start;
          out->push_back(std::move(t));
          i = j + 1;
          continue;
        }
      }
    }


    // --- quoted string -----------------------------------------------------
    if (c == '\'' || c == '"') {
      const char q = (char)c;
      std::string v;
      ++i;
      bool closed = false;
      while (i < n) {
        char ch = s[i];
        if (ch == '\\' && i + 1 < n) {
          char e = s[i + 1];
          i += 2;
          switch (e) {
            case 'n': v.push_back('\n'); break;
            case 't': v.push_back('\t'); break;
            case 'r': v.push_back('\r'); break;
            case '0': v.push_back('\0'); break;
            case 'b': v.push_back('\b'); break;
            case 'Z': v.push_back('\x1a'); break;
            case '\\': v.push_back('\\'); break;
            case '\'': v.push_back('\''); break;
            case '"': v.push_back('"'); break;
            case '%': v.push_back('%'); break;
            case '_': v.push_back('_'); break;
            default: v.push_back(e); break;
          }
          continue;
        }
        if (ch == q) {
          if (i + 1 < n && s[i + 1] == q) {  // '' escape
            v.push_back(q);
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        v.push_back(ch);
        ++i;
      }
      if (!closed) throw_err(1064, "unterminated string literal");
      Tok t;
      t.kind = TK_STR;
      t.text = std::move(v);
      t.pos = start;
      out->push_back(std::move(t));
      continue;
    }

    // --- backtick identifier ----------------------------------------------
    if (c == '`') {
      std::string v;
      ++i;
      bool closed = false;
      while (i < n) {
        char ch = s[i];
        if (ch == '`') {
          if (i + 1 < n && s[i + 1] == '`') {
            v.push_back('`');
            i += 2;
            continue;
          }
          ++i;
          closed = true;
          break;
        }
        v.push_back(ch);
        ++i;
      }
      if (!closed) throw_err(1064, "unterminated quoted identifier");
      Tok t;
      t.kind = TK_IDENT;
      t.text = std::move(v);
      t.quoted = true;
      t.pos = start;
      out->push_back(std::move(t));
      continue;
    }

    // --- bare vector literal: [1, 2, -3.5] ---------------------------------
    if (c == '[') {
      int depth = 0;
      size_t j = i;
      bool closed = false;
      for (; j < n; ++j) {
        if (s[j] == '[') ++depth;
        else if (s[j] == ']') {
          if (--depth == 0) {
            closed = true;
            break;
          }
        }
      }
      if (!closed) throw_err(1064, "unterminated vector literal '['");
      Tok t;
      t.kind = TK_VEC;
      t.text = s.substr(i + 1, j - i - 1);
      t.pos = start;
      out->push_back(std::move(t));
      i = j + 1;
      continue;
    }

    // --- number ------------------------------------------------------------
    if (is_digit(c) || (c == '.' && i + 1 < n && is_digit((unsigned char)s[i + 1]))) {
      bool is_num = false;
      while (i < n && is_digit((unsigned char)s[i])) ++i;
      if (i < n && s[i] == '.') {
        is_num = true;
        ++i;
        while (i < n && is_digit((unsigned char)s[i])) ++i;
      }
      if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        size_t k = i + 1;
        if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
        if (k < n && is_digit((unsigned char)s[k])) {
          while (k < n && is_digit((unsigned char)s[k])) ++k;
          i = k;
          is_num = true;
        }
      }
      Tok t;
      t.kind = is_num ? TK_NUM : TK_INT;
      t.text = s.substr(start, i - start);
      t.pos = start;
      out->push_back(std::move(t));
      continue;
    }

    // --- identifier --------------------------------------------------------
    if (is_ident_start(c)) {
      while (i < n && is_ident_char((unsigned char)s[i])) ++i;
      Tok t;
      t.kind = TK_IDENT;
      t.text = s.substr(start, i - start);
      t.pos = start;
      out->push_back(std::move(t));
      continue;
    }

    // --- punctuation -------------------------------------------------------
    if (i + 1 < n) {
      const char a = s[i], b = s[i + 1];
      bool two = false;
      std::string op;
      if ((a == '<' && (b == '=' || b == '>')) || (a == '>' && b == '=') ||
          (a == '!' && b == '=') ||
          (a == '|' && b == '|') || (a == '&' && b == '&')) {
        op.push_back(a);
        op.push_back(b);
        two = true;
      }
      if (two) {
        Tok t;
        t.kind = TK_PUNCT;
        t.text = std::move(op);
        t.pos = start;
        out->push_back(std::move(t));
        i += 2;
        continue;
      }
    }
    if (std::strchr("(),.=<>*+-/;", (int)c) != nullptr && c < 0x80) {
      Tok t;
      t.kind = TK_PUNCT;
      t.text = std::string(1, (char)c);
      t.pos = start;
      out->push_back(std::move(t));
      ++i;
      continue;
    }
    throw_err(1064, std::string("unexpected character '") + (char)c + "' in SQL");
  }
  Tok end;
  end.kind = TK_END;
  end.pos = n;
  out->push_back(std::move(end));
}

// ------------------------------------------------------------ value helpers --

// Parses "1, 2.5, -3e2" into floats. Strict: rejects empty elements, trailing
// commas, garbage tokens and non-finite values.
bool parse_float_list(std::string_view body, std::vector<float>* out) {
  out->clear();
  // One allocation for the usual 128-coordinate case instead of log2 growth.
  if (out->capacity() == 0) out->reserve(body.size() / 8 + 4);
  const size_t n = body.size();
  size_t i = 0;
  const short_decimal::Context context(body.size() >= 128);
  if(context.enabled()) {
    while (i < n) {
      while (i < n && is_space((unsigned char)body[i])) ++i;
      if (i >= n) break;
      const char* begin = body.data() + i;
      const auto fast = context.parse(begin, body.data() + n);
      if(!fast.accepted())break; // i is unchanged at the first non-space byte.
      const char* endp=fast.ptr;
      const float stored=fast.value;
      out->push_back(stored);
      i += (size_t)(endp - begin);
      while (i < n && is_space((unsigned char)body[i])) ++i;
      if (i < n) {
        if (body[i] != ',') return false;
        ++i;
        size_t probe = i;
        while (probe < n && is_space((unsigned char)body[probe])) ++probe;
        if (probe >= n) return false;  // trailing comma
      }
    }
  }
  while (i < n) {
    while (i < n && is_space((unsigned char)body[i])) ++i;
    if (i >= n) break;
    const char* begin = body.data() + i;
    const char* endp = nullptr;
    // Parse at storage precision to avoid rounding first to double and then
    // rounding again at a float32 midpoint. Subnormals and zero are finite.
    float stored = 0;
    const auto parsed = std::from_chars(begin, body.data() + n, stored, std::chars_format::general);
    if (parsed.ec == std::errc{} &&
        (parsed.ptr == body.data() + n || *parsed.ptr == ',' || is_space((unsigned char)*parsed.ptr))) {
      endp = parsed.ptr;
    } else {
      // Keep strtof's leading plus, hexadecimal and underflow-to-zero forms.
      // In particular, from_chars may successfully consume only the 0 in 0x1.
      char* compatible_end = nullptr;
      stored = std::strtof(begin, &compatible_end);
      endp = compatible_end;
    }
    if (endp == begin || !std::isfinite(stored)) return false;
    out->push_back(stored);
    i += (size_t)(endp - begin);
    while (i < n && is_space((unsigned char)body[i])) ++i;
    if (i < n) {
      if (body[i] != ',') return false;
      ++i;
      size_t probe = i;
      while (probe < n && is_space((unsigned char)body[probe])) ++probe;
      if (probe >= n) return false;  // trailing comma
    }
  }
  return !out->empty();
}

// A quoted string that looks like '[1.0, 2, -3.5]' becomes a VECTOR value;
// anything else stays a VARCHAR.
Value value_from_string(const std::string& raw) {
  // Trim and bracket stripping stay views: the byte range is examined in place.
  size_t b = 0, e = raw.size();
  while (b < e && is_space((unsigned char)raw[b])) ++b;
  while (e > b && is_space((unsigned char)raw[e - 1])) --e;
  if (e - b >= 2 && raw[b] == '[' && raw[e - 1] == ']') {
    std::vector<float> v;
    if (parse_float_list(std::string_view(raw.data() + b + 1, e - b - 2), &v)) {
      Value result = Value::ofVec(std::move(v));
      result.s = raw;  // retain quoted spelling for a VARCHAR destination
      return result;
    }
  }
  return Value::ofStr(raw);
}

// Keywords that may never be used as a bare table/column name.
bool is_name_reserved(const std::string& up) {
  static const char* const kWords[] = {
      "SELECT", "FROM", "WHERE", "GROUP", "ORDER", "LIMIT", "HAVING", "INSERT", "INTO",
      "VALUES",     "UPDATE", "SET",      "DELETE", "CREATE", "DROP",   "TABLE",  "INDEX",
      "JOIN",       "ON",     "UNION",    "AND",    "OR",     "NOT",    "NULL",   "PRIMARY",
      "KEY",        "AS",     "BY",       "ASC",    "DESC",   "BEGIN",  "COMMIT", "ROLLBACK",
      "ALTER",      "SHOW",   "IF",       "EXISTS", "DISTINCT", "CONSTRAINT", "UNIQUE",
      "DEFAULT",    "IS",     "LIKE",     "IN",     "BETWEEN", "OFFSET", "FOR",   "USING",
      "OUTER",      "L2_DISTANCE", "L2_DIST", "TRUE", "FALSE", "START", "EXCEPT",
      "INTERSECT",  "TEMPORARY", "VIEW",   "DATABASE", "SCHEMA", "CASCADE",
      "RESTRICT",   "CHECK",   "FOREIGN",  "REFERENCES", "COLLATE", "UNSIGNED",
      "ZEROFILL",   "AUTO_INCREMENT", "PARTITION", "TRUNCATE", "GRANT", "REVOKE"};
  for (const char* w : kWords)
    if (up == w) return true;
  return false;
}

// Words that end a table alias / start the next clause.
bool is_clause_word(const std::string& up) {
  if (is_name_reserved(up)) return true;
  static const char* const kExtra[] = {
      "VECTOR", "VARCHAR", "BIGINT", "INT", "INTEGER", "FULL", "CROSS", "NATURAL",
      "INNER",  "LEFT",    "RIGHT",  "OUTER", "WORK", "TRANSACTION", "JOIN", "STRAIGHT_JOIN",
      "WINDOW", "HAVING",  "FOR",    "INTO", "TO",   "CHAIN",  "STATUS"};
  for (const char* w : kExtra)
    if (up == w) return true;
  return false;
}

// ------------------------------------------------------------------ parser --

// Rows below this count are parsed inline: the pool hand-off costs more than it
// saves (one wake-up plus one join is ~10 us, a literal is ~5 us).
constexpr size_t kParallelRowThreshold = 16;

// One deferred literal conversion. `tok` points into the token vector (immutable
// during a parse) and `dest` at its slot in the destination row, so a worker
// needs no parser state; `near_tok` is the token index syntax_error() must quote
// if the conversion fails, which is exactly what the sequential parser used.
struct LiteralJob {
  const Tok* tok = nullptr;
  Value* dest = nullptr;
  size_t near_tok = 0;
  bool neg = false;
};

class Parser {
 public:
  Parser(const std::string& sql, std::vector<Tok> toks)
      : sql_(sql), t_(std::move(toks)), i_(0) {}

  void run(Statement* st);

 private:
  // token access
  const Tok& cur() const { return t_[i_]; }
  void adv() {
    if (i_ + 1 < t_.size()) ++i_;
  }
  bool at_end() const { return cur().kind == TK_END; }
  bool is_kw(const char* kw) const {
    return cur().kind == TK_IDENT && !cur().quoted && to_upper(cur().text) == kw;
  }
  bool accept_kw(const char* kw) {
    if (!is_kw(kw)) return false;
    adv();
    return true;
  }
  bool is_punct(const char* p) const { return cur().kind == TK_PUNCT && cur().text == p; }
  bool accept_punct(const char* p) {
    if (!is_punct(p)) return false;
    adv();
    return true;
  }
  // errors
  [[noreturn]] void syntax_error(const std::string& what);
  [[noreturn]] void syntax_error_at(size_t at, const std::string& what) const;
  [[noreturn]] void not_supported(const std::string& feature);
  void expect_kw(const char* kw, const char* ctx);
  void expect_punct(const char* p, const char* ctx);

  // primitives
  std::string ident(const char* ctx);
  int64_t int_literal(const char* ctx);
  int64_t decode_int(const std::string& digits, bool neg, size_t near_tok) const;
  LiteralJob scan_value(const char* ctx);
  void materialize_into(const LiteralJob& job, Value* dest) const;
  Value parse_value(const char* ctx);
  std::vector<float> parse_query_vector();

  // statements
  void parse_create(Statement* st);
  void parse_create_table(Statement* st);
  void parse_create_index(Statement* st);
  void parse_drop(Statement* st);
  void parse_insert(Statement* st);
  void parse_select(Statement* st);
  void parse_update(Statement* st);
  void parse_delete(Statement* st);
  void parse_show(Statement* st);

  // pieces
  Column parse_column_def();
  SelectItem parse_select_item();
  Cond parse_cond();
  void parse_where(Statement* st);
  void parse_cond_group(Statement* st, int depth);

  const std::string& sql_;
  std::vector<Tok> t_;
  size_t i_;
};

// --- error helpers ---------------------------------------------------------

void Parser::syntax_error(const std::string& what) { syntax_error_at(i_, what); }

void Parser::syntax_error_at(size_t at, const std::string& what) const {
  const Tok& tok = t_[at < t_.size() ? at : t_.size() - 1];
  std::string near;
  if (tok.kind == TK_END) {
    near = "end of the statement";
  } else {
    size_t len = 32;
    if (tok.pos + len > sql_.size()) len = sql_.size() - tok.pos;
    near = "'" + sql_.substr(tok.pos, len) + "'";
  }
  std::string msg = "You have an error in your SQL syntax";
  if (!what.empty()) msg += ": " + what;
  msg += " near " + near;
  throw_err(1064, msg);
}

void Parser::not_supported(const std::string& feature) {
  throw_err(1235, feature + " is not supported by MiniVectorDB");
}

void Parser::expect_kw(const char* kw, const char* ctx) {
  if (accept_kw(kw)) return;
  syntax_error(std::string("expected '") + kw + "' in " + ctx);
}

void Parser::expect_punct(const char* p, const char* ctx) {
  if (accept_punct(p)) return;
  syntax_error(std::string("expected '") + p + "' in " + ctx);
}

// --- primitives ------------------------------------------------------------

std::string Parser::ident(const char* ctx) {
  if (cur().kind != TK_IDENT) syntax_error(std::string("expected ") + ctx);
  if (!cur().quoted && is_name_reserved(to_upper(cur().text)))
    syntax_error(std::string("expected ") + ctx + ", found keyword '" + cur().text + "'");
  std::string r = cur().text;
  adv();
  return r;
}

int64_t Parser::decode_int(const std::string& digits, bool neg, size_t near_tok) const {
  errno = 0;
  char* endp = nullptr;
  const unsigned long long u = std::strtoull(digits.c_str(), &endp, 10);
  if (endp == digits.c_str() || (endp && *endp != '\0') || errno == ERANGE)
    syntax_error_at(near_tok, "invalid integer literal '" + digits + "'");
  const unsigned long long kMax = 9223372036854775807ULL;
  if (!neg) {
    if (u > kMax) syntax_error_at(near_tok, "integer literal '" + digits + "' is out of range");
    return (int64_t)u;
  }
  if (u > kMax + 1ULL) syntax_error_at(near_tok, "integer literal '-" + digits + "' is out of range");
  if (u == kMax + 1ULL) return (-9223372036854775807LL - 1);
  return -(int64_t)u;
}

int64_t Parser::int_literal(const char* ctx) {
  if (cur().kind != TK_INT) syntax_error(std::string("expected an integer literal for ") + ctx);
  // strtoull needs a stable string; copy before advancing.
  std::string digits = cur().text;
  adv();
  return decode_int(digits, false, i_);
}

// Half one of parse_value: consumes the sign and classifies the literal, keeping
// every error branch and its order exactly where it was. The conversion itself is
// left to materialize_into(), which may run on another thread.
LiteralJob Parser::scan_value(const char* ctx) {
  LiteralJob job;
  if (is_punct("-")) {
    adv();
    job.neg = true;
  } else if (is_punct("+")) {
    adv();
  }
  // Token order mirrors the sequential parser: a pending sign only allows an
  // integer, so it must fail before a quoted string or a bare vector is accepted.
  if (cur().kind == TK_INT ||
      (!job.neg && (cur().kind == TK_STR || cur().kind == TK_VEC))) {
    job.tok = &cur();
    adv();
    // Errors raised by the conversion quote this token, as syntax_error() would
    // have done after adv() in the sequential parser.
    job.near_tok = i_;
    return job;
  }
  if (job.neg) syntax_error("expected a number after '-'");
  if (cur().kind == TK_NUM)
    syntax_error("floating point literal '" + cur().text +
                 "' is not a valid value; use an integer, a quoted string or '[..]'");
  if (is_kw("NULL")) syntax_error("NULL is not supported; supply a concrete value");
  if (is_kw("TRUE") || is_kw("FALSE")) syntax_error("boolean literals are not supported");
  if (is_kw("DEFAULT")) not_supported("DEFAULT in a value list");
  if (is_punct("(")) not_supported("subqueries and parenthesised expressions in " + std::string(ctx));
  if (cur().kind == TK_IDENT)
    not_supported("column references / expressions in " + std::string(ctx));
  syntax_error(std::string("expected a literal value in ") + ctx);
}

// Half two of parse_value: pure literal -> Value conversion. Const, so it can run
// on a worker thread against the shared parser as long as `dest` is not shared.
void Parser::materialize_into(const LiteralJob& job, Value* dest) const {
  const Tok& tok = *job.tok;
  if (tok.kind == TK_INT) {
    *dest = Value::ofInt(decode_int(tok.text, job.neg, job.near_tok));
  } else if (tok.kind == TK_STR) {
    *dest = value_from_string(tok.text);
  } else {
    std::vector<float> v;
    if (!parse_float_list(std::string_view(tok.text), &v))
      syntax_error_at(job.near_tok, "invalid vector literal '[" + tok.text + "]'");
    *dest = Value::ofVec(std::move(v));
  }
}

Value Parser::parse_value(const char* ctx) {
  Value value;
  materialize_into(scan_value(ctx), &value);
  return value;
}

std::vector<float> Parser::parse_query_vector() {
  if (cur().kind == TK_VEC) {
    const std::string& body = cur().text;
    adv();
    std::vector<float> v;
    if (!parse_float_list(std::string_view(body), &v)) syntax_error("invalid vector literal '[" + body + "]'");
    return v;
  }
  if (cur().kind == TK_STR) {
    const std::string& s = cur().text;
    adv();
    size_t b = 0, e = s.size();
    while (b < e && is_space((unsigned char)s[b])) ++b;
    while (e > b && is_space((unsigned char)s[e - 1])) --e;
    if (e - b >= 2 && s[b] == '[' && s[e - 1] == ']') {
      std::vector<float> v;
      if (parse_float_list(std::string_view(s.data() + b + 1, e - b - 2), &v)) return v;
    }
    syntax_error("invalid vector literal '" + s + "'");
  }
  syntax_error("expected a vector literal such as '[1.0,2.0]'");
}

// --- statement dispatch ----------------------------------------------------

void Parser::run(Statement* st) {
  while (is_punct(";")) adv();  // tolerate stray separators when called directly
  if (cur().kind == TK_END) {
    st->kind = Statement::EMPTY;
    return;
  }
  if (cur().kind != TK_IDENT || cur().quoted) syntax_error("expected a SQL statement");
  const std::string w = to_upper(cur().text);
  adv();

  if (w == "CREATE") {
    parse_create(st);
  } else if (w == "DROP") {
    parse_drop(st);
  } else if (w == "INSERT") {
    parse_insert(st);
  } else if (w == "SELECT") {
    parse_select(st);
  } else if (w == "UPDATE") {
    parse_update(st);
  } else if (w == "DELETE") {
    parse_delete(st);
  } else if (w == "SHOW") {
    parse_show(st);
  } else if (w == "BEGIN") {
    accept_kw("WORK") || accept_kw("TRANSACTION");
    st->kind = Statement::BEGIN_TXN;
  } else if (w == "START") {
    expect_kw("TRANSACTION", "START");
    accept_kw("WORK");
    st->kind = Statement::BEGIN_TXN;
  } else if (w == "COMMIT") {
    if (!accept_kw("WORK")) accept_kw("TRANSACTION");
    st->kind = Statement::COMMIT_TXN;
    if (is_kw("AND") || is_kw("CHAIN")) not_supported("COMMIT AND CHAIN");
  } else if (w == "ROLLBACK") {
    if (!accept_kw("WORK")) accept_kw("TRANSACTION");
    st->kind = Statement::ROLLBACK_TXN;
    if (is_kw("TO")) not_supported("ROLLBACK TO SAVEPOINT");
    if (is_kw("AND") || is_kw("CHAIN")) not_supported("ROLLBACK AND CHAIN");
  } else if (w == "SAVEPOINT" || w == "RELEASE") {
    not_supported("savepoints / nested transactions");
  } else if (w == "ALTER") {
    not_supported("ALTER TABLE");
  } else if (w == "TRUNCATE") {
    not_supported("TRUNCATE TABLE");
  } else if (w == "USE") {
    not_supported("USE <database>");
  } else if (w == "SET") {
    not_supported("the SET statement (only SET inside UPDATE is supported)");
  } else if (w == "EXPLAIN" || w == "DESCRIBE" || w == "DESC") {
    not_supported("EXPLAIN / DESCRIBE");
  } else if (w == "GRANT" || w == "REVOKE" || w == "FLUSH") {
    not_supported("statement '" + w + "'");
  } else {
    not_supported("statement '" + w + "'");
  }

  while (is_punct(";")) adv();
  if (!at_end())
    syntax_error("unexpected trailing input after the statement");
}

// --- CREATE ----------------------------------------------------------------

void Parser::parse_create(Statement* st) {
  if (accept_kw("TABLE")) {
    st->kind = Statement::CREATE_TABLE;
    parse_create_table(st);
    return;
  }
  if (accept_kw("VECTOR")) {
    expect_kw("INDEX", "CREATE VECTOR");
    st->kind = Statement::CREATE_INDEX;
    parse_create_index(st);
    return;
  }
  if (is_kw("UNIQUE")) not_supported("UNIQUE index");
  if (is_kw("INDEX")) not_supported("non-vector index (only CREATE VECTOR INDEX)");
  if (is_kw("TEMPORARY")) not_supported("CREATE TEMPORARY TABLE");
  if (is_kw("DATABASE") || is_kw("SCHEMA")) not_supported("CREATE DATABASE / SCHEMA");
  if (is_kw("VIEW")) not_supported("CREATE VIEW");
  if (is_kw("USER") || is_kw("ROLE")) not_supported("CREATE USER / ROLE");
  syntax_error("expected TABLE or VECTOR INDEX after CREATE");
}

void Parser::parse_create_table(Statement* st) {
  if (accept_kw("IF")) {
    expect_kw("NOT", "IF");
    expect_kw("EXISTS", "IF NOT");
    st->if_not_exists = "IF NOT EXISTS";
  }
  st->table = ident("a table name");
  expect_punct("(", "CREATE TABLE");

  int primaries = 0;
  bool table_primary = false;
  std::string primary_column;
  for (;;) {
    if (is_punct(")")) syntax_error("a table needs at least one column");
    if (accept_kw("PRIMARY")) {
      if (table_primary) syntax_error("duplicate PRIMARY KEY clause");
      table_primary = true;
      expect_kw("KEY", "PRIMARY KEY");
      expect_punct("(", "PRIMARY KEY");
      primary_column = ident("the PRIMARY KEY column");
      if (is_punct(",")) not_supported("composite PRIMARY KEY");
      expect_punct(")", "PRIMARY KEY");
    } else {
      st->columns.push_back(parse_column_def());
    }
    if (!accept_punct(",")) break;
    if (is_punct(")")) syntax_error("trailing ',' before ')' in the column list");
  }
  expect_punct(")", "CREATE TABLE");

  if (table_primary) {
    bool found = false;
    for (Column& column : st->columns) {
      if (column.name != primary_column) continue;
      if (column.is_primary) syntax_error("duplicate PRIMARY KEY declaration");
      column.is_primary = true;
      found = true;
    }
    if (!found) throw_err(1072, "Key column '" + primary_column + "' doesn't exist in table");
  }

  for (const Column& c : st->columns)
    if (c.is_primary) ++primaries;
  if (primaries > 1) syntax_error("only one PRIMARY KEY column is supported");
  if (st->columns.empty()) syntax_error("a table needs at least one column");
}

Column Parser::parse_column_def() {
  if (is_kw("PRIMARY") || is_kw("UNIQUE") || is_kw("KEY") || is_kw("INDEX") ||
      is_kw("CONSTRAINT") || is_kw("CHECK") || is_kw("FOREIGN"))
    not_supported("table-level constraints (declare PRIMARY KEY inline on a column)");

  Column c;
  c.name = ident("a column name");
  c.is_primary = false;
  c.len = 0;

  if (is_kw("BIGINT") || is_kw("INT") || is_kw("INTEGER")) {
    adv();
    c.type = Type::BIGINT;
    if (is_punct("(")) syntax_error(std::string("type '") + c.name + "' takes no length argument");
  } else if (is_kw("VARCHAR")) {
    adv();
    c.type = Type::VARCHAR;
    expect_punct("(", "VARCHAR");
    const int64_t len = int_literal("the VARCHAR length");
    if (is_punct(",")) syntax_error("VARCHAR takes exactly one length argument");
    expect_punct(")", "VARCHAR");
    if (len <= 0 || len > 65535)
      syntax_error("invalid VARCHAR length " + std::to_string(len) + " (expected 1..65535)");
    c.len = (int32_t)len;
  } else if (is_kw("VECTOR")) {
    adv();
    c.type = Type::VECTOR;
    expect_punct("(", "VECTOR");
    const int64_t dim = int_literal("the VECTOR dimension");
    if (is_punct(",")) syntax_error("VECTOR takes exactly one dimension argument");
    expect_punct(")", "VECTOR");
    if (dim <= 0 || dim > 65536)
      syntax_error("invalid VECTOR dimension " + std::to_string(dim) + " (expected 1..65536)");
    c.len = (int32_t)dim;
  } else if (cur().kind == TK_IDENT) {
    syntax_error("unknown column type '" + cur().text +
                 "' (expected BIGINT, INT, VARCHAR(n) or VECTOR(n))");
  } else {
    syntax_error("expected a column type after '" + c.name + "'");
  }

  for (;;) {
    if (is_kw("PRIMARY")) {
      adv();
      expect_kw("KEY", "PRIMARY KEY");
      if (c.is_primary) syntax_error("duplicate PRIMARY KEY on column '" + c.name + "'");
      if (is_punct("(")) not_supported("table-level PRIMARY KEY (col)");
      c.is_primary = true;
      continue;
    }
    if (is_kw("NOT")) {
      adv();
      expect_kw("NULL", "NOT NULL");
      continue;
    }
    if (is_kw("NULL")) {
      not_supported("nullable columns");
    }
    if (is_kw("AUTO_INCREMENT")) {
      not_supported("AUTO_INCREMENT");
    }
    if (is_kw("UNIQUE") || is_kw("DEFAULT") || is_kw("COMMENT") || is_kw("CHECK") ||
        is_kw("REFERENCES") || is_kw("COLLATE") || is_kw("UNSIGNED") || is_kw("ZEROFILL") ||
        is_kw("KEY") || is_kw("INDEX") || is_kw("GENERATED")) {
      not_supported("column constraint '" + to_upper(cur().text) + "'");
    }
    break;
  }
  return c;
}

void Parser::parse_create_index(Statement* st) {
  st->index_name = ident("an index name");
  expect_kw("ON", "CREATE VECTOR INDEX");
  st->table = ident("a table name");
  expect_punct("(", "CREATE VECTOR INDEX");
  st->index_column = ident("a column name");
  expect_punct(")", "CREATE VECTOR INDEX");
  if (is_punct(",")) not_supported("multi-column vector index");
}

// --- DROP ------------------------------------------------------------------

void Parser::parse_drop(Statement* st) {
  if (accept_kw("TABLE")) {
    st->kind = Statement::DROP_TABLE;
    if (accept_kw("IF")) {
      expect_kw("EXISTS", "IF");
      st->if_not_exists = "IF EXISTS";
    }
    st->table = ident("a table name");
    if (is_kw("CASCADE") || is_kw("RESTRICT")) adv();
    return;
  }
  if (is_kw("INDEX")) not_supported("DROP INDEX");
  if (is_kw("VIEW")) not_supported("DROP VIEW");
  if (is_kw("DATABASE") || is_kw("SCHEMA")) not_supported("DROP DATABASE / SCHEMA");
  syntax_error("expected TABLE after DROP");
}

// --- INSERT ----------------------------------------------------------------

void Parser::parse_insert(Statement* st) {
  st->kind = Statement::INSERT;
  if (is_kw("LOW_PRIORITY") || is_kw("DELAYED") || is_kw("HIGH_PRIORITY"))
    not_supported("INSERT modifiers");
  if (is_kw("IGNORE")) not_supported("INSERT IGNORE");
  accept_kw("INTO");
  st->table = ident("a table name");

  // Optional explicit column list: kept in Statement::columns (name only) so
  // the executor can map values by name; order in `rows` stays as written.
  if (is_punct("(")) {
    adv();
    for (;;) {
      Column c;
      c.name = ident("a column name");
      c.type = Type::BIGINT;
      c.len = 0;
      c.is_primary = false;
      st->columns.push_back(c);
      if (!accept_punct(",")) break;
    }
    expect_punct(")", "the INSERT column list");
  }

  if (!accept_kw("VALUES")) {
    if (accept_kw("VALUE")) {
      // 'VALUE' is an accepted synonym of 'VALUES'
    } else if (is_kw("SELECT") || is_kw("SET")) {
      not_supported("INSERT ... SELECT / INSERT ... SET");
    } else {
      syntax_error("expected VALUES after the target table");
    }
  }

  // Rows are independent, so from kParallelRowThreshold on the literal
  // conversions are recorded and run on the parse pool once the structural scan
  // is done. Ordering stays sequential: jobs are appended in parse order, every
  // job precedes the token where the scan stopped, and the pool rethrows the
  // earliest part's failure -- so the reported error is the one a one-thread
  // parse would report, at the same token.
  std::vector<LiteralJob> jobs;
  bool bound = true;  // every recorded job already names its destination slot
  auto drain = [&]() {
    if (jobs.empty()) return;
    // Swapped out first, so the list stays empty even if a literal throws: the
    // failure that was raised is already the sequential-order one.
    std::vector<LiteralJob> pending;
    pending.swap(jobs);
    ParsePool& pool = ParsePool::instance();
    if (st->rows.size() >= kParallelRowThreshold && pool.parts() > 1 && bound) {
      pool.run(pending.size(),
               [this, &pending](std::size_t i) { materialize_into(pending[i], pending[i].dest); });
    } else {
      // Unbound jobs belong to a row the scan never finished (the statement is
      // already failing), so only their errors matter: keep one scratch slot and
      // stay on this thread.
      Value scratch;
      for (LiteralJob& job : pending)
        materialize_into(job, job.dest != nullptr ? job.dest : &scratch);
    }
    bound = true;
  };
  try {
    for (;;) {
      expect_punct("(", "VALUES");
      std::vector<Value> row;
      const size_t first_job = jobs.size();
      bound = false;
      for (;;) {
        jobs.push_back(scan_value("the VALUES list"));
        if (!accept_punct(",")) break;
      }
      expect_punct(")", "VALUES");
      // Every destination slot exists before a worker can write to it; moving the
      // row into st->rows keeps its buffer, so the pointers stay valid.
      row.resize(jobs.size() - first_job);
      for (size_t k = first_job; k < jobs.size(); ++k) jobs[k].dest = &row[k - first_job];
      bound = true;
      st->rows.push_back(std::move(row));
      if (!accept_punct(",")) break;
    }
    drain();
  } catch (const ParseError&) {
    drain();  // a literal that precedes the structural failure reports first
    throw;
  }
  if (st->rows.empty()) syntax_error("VALUES needs at least one row");
  if (!st->columns.empty()) {
    for (const auto& r : st->rows)
      if (r.size() != st->columns.size())
        syntax_error("each VALUES row must supply exactly " +
                     std::to_string(st->columns.size()) + " value(s)");
  }
}

// --- SELECT ----------------------------------------------------------------

SelectItem Parser::parse_select_item() {
  SelectItem it;
  if (is_punct("*"))
    not_supported("'*' mixed with explicit select items (use SELECT * on its own)");
  if (is_punct("("))
    not_supported("subqueries or parenthesised expressions in the select list");
  if (cur().kind != TK_IDENT) syntax_error("expected a column name in the select list");

  if (cur().quoted) {
    it.column = cur().text;
    adv();
  } else {
    const std::string up = to_upper(cur().text);
    if (up == "L2_DISTANCE" || up == "L2_DIST") {
      adv();
      expect_punct("(", "L2_DISTANCE");
      it.is_distance = true;
      it.column = ident("a column name");
      expect_punct(",", "L2_DISTANCE");
      it.query = parse_query_vector();
      expect_punct(")", "L2_DISTANCE");
    } else if (is_name_reserved(up)) {
      syntax_error("expected a column name in the select list, found keyword '" + cur().text + "'");
    } else {
      it.column = cur().text;
      adv();
    }
  }
  if (accept_kw("AS")) {
    it.alias = ident("an alias");
  } else if (cur().kind == TK_IDENT &&
             (cur().quoted || !is_clause_word(to_upper(cur().text)))) {
    it.alias = ident("an alias");
  }
  if (is_punct("(")) not_supported("function calls other than L2_DISTANCE");
  return it;
}

void Parser::parse_select(Statement* st) {
  st->kind = Statement::SELECT;
  if (accept_kw("DISTINCT")) not_supported("SELECT DISTINCT");
  accept_kw("ALL");

  if (is_punct("*")) {
    adv();
    if (is_punct(",")) not_supported("'*' mixed with explicit select items");
    // items left empty => SELECT *
  } else {
    for (;;) {
      st->items.push_back(parse_select_item());
      if (!accept_punct(",")) break;
    }
  }

  expect_kw("FROM", "SELECT");
  if (is_punct("(")) not_supported("subqueries in FROM");
  st->table = ident("a table name");

  // table alias, or a JOIN we must reject explicitly
  if (is_kw("JOIN") || is_kw("INNER") || is_kw("LEFT") || is_kw("RIGHT") || is_kw("FULL") ||
      is_kw("CROSS") || is_kw("NATURAL") || is_kw("OUTER") || is_kw("STRAIGHT_JOIN") ||
      is_kw("ON") || is_kw("USING"))
    not_supported("JOIN");
  if (is_punct(",")) not_supported("selecting from multiple tables (comma join)");
  if (cur().kind == TK_IDENT) {
    const std::string up = cur().quoted ? std::string() : to_upper(cur().text);
    if (cur().quoted || !is_clause_word(up)) adv();  // optional alias, unused
  }

  bool seen_where = false, seen_order = false, seen_limit = false;
  for (;;) {
    if (is_kw("WHERE")) {
      if (seen_where || seen_order || seen_limit) syntax_error("misplaced WHERE clause");
      seen_where = true;
      adv();
      parse_where(st);
      continue;
    }
    if (is_kw("ORDER")) {
      if (seen_order || seen_limit) syntax_error("misplaced ORDER BY clause");
      seen_order = true;
      adv();
      expect_kw("BY", "ORDER BY");
      st->order_expression = parse_select_item();
      if (!st->order_expression.alias.empty()) syntax_error("alias in ORDER BY");
      st->order_is_expression = st->order_expression.is_distance;
      st->order_by = st->order_expression.column;
      if (accept_kw("ASC")) {
      } else if (accept_kw("DESC")) {
        st->order_desc = true;
      }
      if (is_punct(",")) not_supported("multiple ORDER BY keys");
      continue;
    }
    if (is_kw("LIMIT")) {
      if (seen_limit) syntax_error("duplicate LIMIT clause");
      seen_limit = true;
      adv();
      st->limit = int_literal("LIMIT");
      if (is_punct(",")) not_supported("LIMIT offset, count");
      if (is_kw("OFFSET")) not_supported("LIMIT ... OFFSET");
      continue;
    }
    if (is_kw("GROUP")) not_supported("GROUP BY");
    if (is_kw("HAVING")) not_supported("HAVING");
    if (is_kw("WINDOW")) not_supported("WINDOW");
    if (is_kw("UNION") || is_kw("EXCEPT") || is_kw("INTERSECT")) not_supported("UNION / EXCEPT / INTERSECT");
    if (is_kw("FOR")) not_supported("FOR UPDATE / FOR SHARE");
    if (is_kw("PROCEDURE") || is_kw("INTO")) not_supported("SELECT ... INTO / PROCEDURE");
    break;
  }
}

// --- UPDATE / DELETE -------------------------------------------------------

void Parser::parse_update(Statement* st) {
  st->kind = Statement::UPDATE;
  if (is_kw("LOW_PRIORITY") || is_kw("IGNORE")) not_supported("UPDATE modifiers");
  st->table = ident("a table name");
  expect_kw("SET", "UPDATE");
  for (;;) {
    std::string col = ident("a column name");
    expect_punct("=", "SET");
    Value v = parse_value("SET");
    st->sets.emplace_back(std::move(col), std::move(v));
    if (!accept_punct(",")) break;
  }
  if (accept_kw("WHERE")) parse_where(st);
  if (is_kw("ORDER")) not_supported("ORDER BY in UPDATE");
  if (is_kw("LIMIT")) not_supported("LIMIT in UPDATE");
}

void Parser::parse_delete(Statement* st) {
  st->kind = Statement::DELETE_ROWS;
  expect_kw("FROM", "DELETE");
  st->table = ident("a table name");
  if (is_punct(",")) not_supported("multi-table DELETE");
  if (cur().kind == TK_IDENT && !is_clause_word(cur().quoted ? std::string() : to_upper(cur().text)))
    not_supported("DELETE with a table alias");
  if (accept_kw("WHERE")) parse_where(st);
  if (is_kw("ORDER")) not_supported("ORDER BY in DELETE");
  if (is_kw("LIMIT")) not_supported("LIMIT in DELETE");
}

// --- SHOW ------------------------------------------------------------------

void Parser::parse_show(Statement* st) {
  if (accept_kw("TABLES")) {
    st->kind = Statement::SHOW_TABLES;
    if (is_kw("FROM") || is_kw("IN") || is_kw("LIKE") || is_kw("WHERE"))
      not_supported("SHOW TABLES filters");
    return;
  }
  if (is_kw("COLUMNS") || is_kw("FIELDS")) not_supported("SHOW COLUMNS");
  if (is_kw("INDEX") || is_kw("INDEXES") || is_kw("KEYS")) not_supported("SHOW INDEX");
  if (is_kw("DATABASES") || is_kw("SCHEMAS")) not_supported("SHOW DATABASES");
  if (is_kw("STATUS") || is_kw("VARIABLES")) not_supported("SHOW STATUS / VARIABLES");
  syntax_error("expected TABLES after SHOW");
}

// --- WHERE -----------------------------------------------------------------

Cond Parser::parse_cond() {
  Cond c;
  c.column = ident("a column name in WHERE");
  if (cur().kind == TK_PUNCT) {
    const std::string& p = cur().text;
    if (p == "=") c.op = Cond::EQ;
    else if (p == "<>") c.op = Cond::NE;
    else if (p == "!=") c.op = Cond::NE;
    else if (p == "<") c.op = Cond::LT;
    else if (p == "<=") c.op = Cond::LE;
    else if (p == ">") c.op = Cond::GT;
    else if (p == ">=") c.op = Cond::GE;
    else syntax_error("expected a comparison operator, found '" + p + "'");
    adv();
  } else if (is_kw("IS")) {
    not_supported("IS NULL / IS NOT NULL");
  } else if (is_kw("LIKE")) {
    not_supported("LIKE");
  } else if (is_kw("IN")) {
    not_supported("IN (...)");
  } else if (is_kw("BETWEEN")) {
    not_supported("BETWEEN");
  } else if (is_kw("NOT")) {
    not_supported("NOT / NOT IN / NOT LIKE");
  } else {
    syntax_error("expected a comparison operator after '" + c.column + "'");
  }
  c.value = parse_value("WHERE");
  return c;
}

void Parser::parse_cond_group(Statement* st, int depth) {
  if (depth > 16) syntax_error("conditions are nested too deeply");
  for (;;) {
    if (is_punct("(")) {
      adv();
      parse_cond_group(st, depth + 1);
      expect_punct(")", "a parenthesised condition group");
    } else {
      st->conds.push_back(parse_cond());
    }
    if (accept_kw("AND") || accept_punct("&&")) continue;
    if (is_kw("OR") || is_punct("||")) not_supported("OR (only AND is supported in WHERE)");
    if (is_kw("NOT")) not_supported("NOT");
    break;
  }
}

void Parser::parse_where(Statement* st) { parse_cond_group(st, 0); }

}  // namespace

// ---------------------------------------------------------------- parse_sql --

bool parse_sql(const std::string& sql, Statement* out, std::string* err, int* err_code) {
  Statement local_stmt;
  std::string local_err;
  int local_code = 0;
  Statement* st = out ? out : &local_stmt;
  std::string* e = err ? err : &local_err;
  int* code = err_code ? err_code : &local_code;

  *st = Statement();
  st->raw = sql;
  e->clear();
  *code = 0;

  try {
    std::vector<Tok> toks;
    tokenize(sql, &toks);
    Parser p(sql, std::move(toks));
    p.run(st);
    return true;
  } catch (const ParseError& pe) {
    *e = pe.msg;
    *code = pe.code;
    st->kind = Statement::UNKNOWN;
    return false;
  } catch (const std::bad_alloc&) {
    *e = "out of memory while parsing the statement";
    *code = 1064;
    st->kind = Statement::UNKNOWN;
    return false;
  } catch (...) {
    *e = "unexpected parser failure";
    *code = 1064;
    st->kind = Statement::UNKNOWN;
    return false;
  }
}

}  // namespace mvdb

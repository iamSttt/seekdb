// main.cpp — stdin/stdout protocol loop for MiniVectorDB
//
// Official JSONL transport is the default; --protocol sql explicitly enables
// the legacy SQL console used by local regressions. Both use read(2), avoiding
// buffered reads that might wait for EOF while an interactive peer is waiting.
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

#include "db.h"
#include "exec.h"
#include "sql.h"
#include "../third_party/json.hpp"

using namespace mvdb;
static volatile sig_atomic_t stopping = 0;
static void stop_signal(int) { stopping = 1; }

class StatementScanner {
 public:
  // The cursor and lexical state survive read boundaries. Leave a trailing
  // byte pending only when a second byte can change its meaning.
  size_t next_end(const std::string& buf) {
    for (; cursor_ < buf.size(); ++cursor_) {
      const char c = buf[cursor_];
      const bool has_next = cursor_ + 1 < buf.size();
      const char next = has_next ? buf[cursor_ + 1] : '\0';
      switch (state_) {
        case State::LineComment:
          if (c == '\n') state_ = State::Sql;
          break;
        case State::BlockComment:
          if (c == '*') {
            if (!has_next) return std::string::npos;
            if (next == '/') {
              state_ = State::Sql;
              ++cursor_;
            }
          }
          break;
        case State::Quoted:
          if (c == '\\' && quote_ != '`') {
            if (!has_next) return std::string::npos;
            ++cursor_;
          } else if (c == quote_) {
            if (!has_next) return std::string::npos;
            if (next == quote_) ++cursor_;
            else state_ = State::Sql;
          }
          break;
        case State::Vector:
          // Match tokenize(): a bare vector consumes balanced brackets;
          // quotes, comments and semicolons inside it are ordinary data.
          // Invalid contents are rejected by the parser after the frame ends.
          if (c == '[') ++vector_depth_;
          else if (c == ']' && --vector_depth_ == 0) state_ = State::Sql;
          break;
        case State::Sql:
          if ((c == '-' || c == '/') && !has_next) return std::string::npos;
          if (c == '-' && next == '-') {
            state_ = State::LineComment;
            ++cursor_;
          } else if (c == '#') {
            state_ = State::LineComment;
          } else if (c == '/' && next == '*') {
            state_ = State::BlockComment;
            ++cursor_;
          } else if (c == '\'' || c == '"' || c == '`') {
            state_ = State::Quoted;
            quote_ = c;
          } else if (c == '[') {
            state_ = State::Vector;
            vector_depth_ = 1;
          } else if (c == ';') {
            return cursor_++;
          }
          break;
      }
    }
    return std::string::npos;
  }

  void discard_prefix(size_t count) { cursor_ -= count; }

 private:
  enum class State { Sql, Quoted, Vector, LineComment, BlockComment };
  State state_ = State::Sql;
  size_t cursor_ = 0;
  size_t vector_depth_ = 0;
  char quote_ = '\0';
};

static bool has_token(const std::string& s) {
  for (char c : s)
    if (!std::isspace((unsigned char)c)) return true;
  return false;
}

static void run_statement(Database& db, const std::string& text) {
  if (!has_token(text)) return;
  Statement st;
  std::string perr, out;
  int code = 0;
  if (!parse_sql(text, &st, &perr, &code)) {
    std::printf("ERROR %d (42000): %s\n", code ? code : 1064, perr.c_str());
    std::fflush(stdout);
    return;
  }
  if (st.kind == Statement::EMPTY) return;
  if (execute(db, st, &out, &perr, &code, [](const std::string& text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
      })) {
    std::fwrite(out.data(), 1, out.size(), stdout);
  } else {
    std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
  }
  std::fflush(stdout);
}

using Json = nlohmann::json;

// Fast path for the request envelope. Every official request is a flat object of
// scalar members, so validating that shape directly avoids building a whole DOM
// (and a duplicate-key std::set) for each request. Anything outside the shape —
// nesting, backslash escapes, unknown keys, out-of-range integers — falls back
// to the nlohmann parse below, which stays the reference implementation.
// True when [p,q) holds a byte below 0x20: JSON forbids a raw control byte inside
// a string. Eight bytes per step; a byte with the high bit set reads as "maybe",
// which only sends the envelope back to nlohmann, so this never accepts input the
// strict path rejects.
static bool has_control_byte(const char* p, const char* q) {
  while (p + 8 <= q) {
    uint64_t w = 0;
    std::memcpy(&w, p, 8);
    w &= 0x7f7f7f7f7f7f7f7full;
    if (((w - 0x2020202020202020ull) & ~w & 0x8080808080808080ull) != 0) return true;
    p += 8;
  }
  while (p < q) {
    if (static_cast<unsigned char>(*p) < 0x20) return true;
    ++p;
  }
  return false;
}

static bool scan_simple_json_string(const std::string& t, size_t* p, std::string* out) {
  size_t i = *p;
  if (i >= t.size() || t[i] != '"') return false;
  ++i;
  // A 250 KB SQL string dominates an ingest request, so the span is found with a
  // vectorised search and then proven escape-free and control-byte-free instead
  // of being walked one byte at a time.
  const char* base = t.data();
  const char* hit = static_cast<const char*>(std::memchr(base + i, '"', t.size() - i));
  if (hit == nullptr) return false;
  const size_t close = static_cast<size_t>(hit - base);
  if (std::memchr(base + i, '\\', close - i) != nullptr) return false;
  if (has_control_byte(base + i, base + close)) return false;
  if (out) out->assign(base + i, close - i);
  *p = close + 1;
  return true;
}

static bool fast_envelope(const std::string& t, Json* out) {
  static const char* const kKeys[4] = {"id", "op", "sql", "search_mode"};
  size_t p = 0;
  const size_t n = t.size();
  auto skip_ws = [&]() {
    while (p < n && (t[p] == ' ' || t[p] == '\t' || t[p] == '\n' || t[p] == '\r')) ++p;
  };
  skip_ws();
  if (p >= n || t[p] != '{') return false;
  ++p;
  Json obj = Json::object();
  bool seen[4] = {false, false, false, false};
  bool first = true;
  for (;;) {
    skip_ws();
    if (p < n && t[p] == '}' && first) { ++p; break; }
    if (p >= n) return false;
    std::string key;
    if (!scan_simple_json_string(t, &p, &key)) return false;
    int slot = -1;
    for (int k = 0; k < 4; ++k)
      if (key == kKeys[k]) slot = k;
    if (slot < 0) return false;
    if (seen[slot]) throw std::runtime_error("duplicate JSON object key");
    seen[slot] = true;
    skip_ws();
    if (p >= n || t[p] != ':') return false;
    ++p;
    skip_ws();
    if (p >= n) return false;
    const char c = t[p];
    if (c == '"') {
      std::string v;
      if (!scan_simple_json_string(t, &p, &v)) return false;
      obj[kKeys[slot]] = std::move(v);
    } else if (c == 't' || c == 'f' || c == 'n') {
      if (t.compare(p, 4, "true") == 0) { obj[kKeys[slot]] = true; p += 4; }
      else if (t.compare(p, 5, "false") == 0) { obj[kKeys[slot]] = false; p += 5; }
      else if (t.compare(p, 4, "null") == 0) { obj[kKeys[slot]] = nullptr; p += 4; }
      else return false;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
      const size_t b = p;
      const bool neg = (c == '-');
      if (neg) ++p;
      if (p >= n) return false;
      if (t[p] == '0') { ++p; }
      else if (t[p] >= '1' && t[p] <= '9') { while (p < n && t[p] >= '0' && t[p] <= '9') ++p; }
      else return false;
      if (p < n && t[p] == '.') {
        ++p;
        if (p >= n || t[p] < '0' || t[p] > '9') return false;
        while (p < n && t[p] >= '0' && t[p] <= '9') ++p;
      }
      if (p < n && (t[p] == 'e' || t[p] == 'E')) {
        ++p;
        if (p < n && (t[p] == '+' || t[p] == '-')) ++p;
        if (p >= n || t[p] < '0' || t[p] > '9') return false;
        while (p < n && t[p] >= '0' && t[p] <= '9') ++p;
      }
      // The id value is only tested for presence; other members are strings.
      bool integral = true;
      uint64_t acc = 0;
      for (size_t k = (neg ? b + 1 : b); k < p; ++k) {
        if (t[k] < '0' || t[k] > '9') { integral = false; break; }
        if (acc > 1844674407370955161ull) { integral = false; break; }
        acc = acc * 10 + static_cast<uint64_t>(t[k] - '0');
      }
      if (!integral) obj[kKeys[slot]] = nullptr;
      else if (neg) obj[kKeys[slot]] = -static_cast<int64_t>(acc);
      else obj[kKeys[slot]] = acc;
    } else {
      return false;  // arrays/objects/other tokens: let nlohmann handle it
    }
    first = false;
    skip_ws();
    if (p < n && t[p] == ',') { ++p; continue; }
    if (p < n && t[p] == '}') { ++p; break; }
    return false;
  }
  skip_ws();
  if (p != n) return false;
  *out = std::move(obj);
  return true;
}

static Json strict_json(const std::string& text) {
  {
    Json fast;
    if (fast_envelope(text, &fast)) return fast;
  }
  std::vector<std::set<std::string>> keys;
  return Json::parse(text, [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    else if (event == Json::parse_event_t::object_end) keys.pop_back();
    else if (event == Json::parse_event_t::key &&
             !keys.back().insert(value.get<std::string>()).second)
      throw std::runtime_error("duplicate JSON object key");
    return true;
  });
}

static void emit_json_text(const std::string& encoded) {
  if (std::fwrite(encoded.data(),1,encoded.size(),stdout)!=encoded.size() ||
      std::fputc('\n',stdout)==EOF || std::fflush(stdout)!=0)
    throw std::runtime_error("cannot write protocol response");
}

// Diagnostics may contain a parser-truncated UTF-8 suffix. Replace only in
// envelopes; typed SQL data is serialized strictly by execute_json.
static void emit_json(const Json& message) {
  emit_json_text(message.dump(-1,' ',false,Json::error_handler_t::replace));
}

// Called only after strict_json validated the entire request. Retain the raw
// top-level id value, including integers wider than uint64, instead of
// round-tripping through the JSON library's uint64/double representation.
static std::string raw_request_id(const std::string& text) {
  auto skip_space=[&](size_t& p) {
    while (p<text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
  };
  auto string_end=[&](size_t p) {
    ++p;
    while (p<text.size()) {
      if (text[p]=='\\') p+=2;
      else if (text[p++]=='"') return p;
    }
    throw std::runtime_error("unterminated JSON string");
  };
  size_t p=0; skip_space(p); ++p;
  for (;;) {
    skip_space(p);
    if (p>=text.size() || text[p]=='}') return "null";
    const size_t key_start=p;
    p=string_end(p);
    const std::string key=Json::parse(text.substr(key_start,p-key_start)).get<std::string>();
    skip_space(p); ++p; skip_space(p);
    const size_t value_start=p;
    size_t depth=0;
    while (p<text.size()) {
      const char c=text[p];
      if (c=='"') { p=string_end(p); continue; }
      if (c=='[' || c=='{') ++depth;
      else if (c==']' || c=='}') {
        if (!depth) break;
        --depth;
      } else if (c==',' && !depth) break;
      ++p;
    }
    if (key=="id") return text.substr(value_start,p-value_start);
    if (p>=text.size() || text[p]=='}') return "null";
    ++p;
  }
}

// One physical input line is one request. Parse the complete SQL before doing
// any work: parse_sql rejects multiple statements and trailing non-SQL input.
static bool json_request(Database& db, const std::string& line, int* exit_code) {
  Json response;
  std::string raw_id="null";
  std::string success_result;
  bool shutdown=false;
  try {
    const Json request=strict_json(line);
    if (!request.is_object()) throw std::runtime_error("request must be a JSON object");
    if (request.contains("id")) raw_id=raw_request_id(line);
    else throw std::runtime_error("missing request id");
    if (!request.contains("op") || !request.at("op").is_string())
      throw std::runtime_error("op must be a string");
    const std::string op=request.at("op").get<std::string>();
    std::string error, result; int code=0;
    if (op=="shutdown") {
      shutdown=true;
      if (db.in_txn() && !db.rollback(&error,&code)) {
        *exit_code=1;
        response={{"error",{{"code",std::to_string(code ? code : 1105)},{"message",error}}},{"ok",false}};
      } else {
        db.close();
        response={{"ok",true}};
      }
    } else if (op=="execute") {
      if (!request.contains("sql") || !request.at("sql").is_string())
        throw std::runtime_error("sql must be a string");
      const Json mode=request.value("search_mode",Json("exact"));
      if (!mode.is_string() || (mode!="exact" && mode!="ann"))
        throw std::runtime_error("search_mode must be exact or ann");
      // The mode selects the candidate set of an unfiltered vector search: a
      // recall-bounded coarse partition for "ann", always-exact for "exact".
      const bool ann_mode = (mode == "ann");
      Statement statement;
      const std::string& sql=request.at("sql").get_ref<const std::string&>();
      if (!parse_sql(sql,&statement,&error,&code) ||
          !execute_json(db,statement,&result,&error,&code,ann_mode)) {
        response={{"ok",false},{"error",{{"code",std::to_string(code ? code : 1105)},{"message",error}}}};
      } else {
        if (result.size()<2 || result.front()!='{' || result.back()!='}')
          throw std::runtime_error("executor returned non-object result");
        success_result=std::move(result);
      }
    } else throw std::runtime_error("unsupported operation");
  } catch (const std::exception& ex) {
    response={{"ok",false},{"error",{{"code","invalid_request"},{"message",ex.what()}}}};
    if (shutdown) *exit_code=1;
  }
  if (!success_result.empty()) {
    // execute_json guarantees a compact typed object without id/ok. Add the
    // envelope directly instead of building a second DOM for large row sets.
    const std::string fields="\"id\":"+raw_id+",\"ok\":true"+
                             (success_result.size()>2 ? "," : "");
    success_result.insert(1,fields);
    emit_json_text(success_result);
  } else {
    std::string encoded=response.dump(-1,' ',false,Json::error_handler_t::replace);
    encoded.insert(1,"\"id\":"+raw_id+(encoded.size()>2 ? "," : ""));
    emit_json_text(encoded);
  }
  return shutdown;
}

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  std::string mode = "exact";
  const char* protocol_env=std::getenv("MVDB_PROTOCOL");
  std::string protocol=protocol_env ? protocol_env : "jsonl";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
    else if (std::strncmp(argv[i], "--data=", 7) == 0) data_dir = argv[i] + 7;
    else if (std::strcmp(argv[i], "--mode") == 0 && i+1<argc) mode=argv[++i];
    else if (std::strncmp(argv[i], "--mode=", 7) == 0) mode = argv[i] + 7;
    else if (std::strcmp(argv[i], "--protocol") == 0 && i+1<argc) protocol=argv[++i];
    else if (std::strncmp(argv[i], "--protocol=", 11) == 0) protocol=argv[i]+11;
    else { std::fprintf(stderr,"unknown or incomplete option: %s\n",argv[i]);return 2; }
  }

  if(mode!="exact" && mode!="ann") {std::fprintf(stderr,"mode must be exact or ann\n");return 2;}
  if(protocol!="jsonl" && protocol!="sql") {std::fprintf(stderr,"protocol must be jsonl or sql\n");return 2;}
  // The default mode is exact; JSONL also validates each request's mode.
  struct sigaction action {};
  action.sa_handler = stop_signal;
  sigemptyset(&action.sa_mask);
  sigaction(SIGTERM, &action, nullptr);
  sigaction(SIGINT, &action, nullptr);

  Database db;
  std::string err;
  if (!db.open(data_dir, &err)) {
    std::fprintf(stderr, "open failed: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "minivec ready data=%s\n", data_dir.c_str());
  std::fflush(stderr);

  if (protocol=="jsonl") {
    int status=0;
    try {
      emit_json({{"type","ready"}});
      std::string pending; char input[65536]; bool shutdown=false;
      while (!stopping && !shutdown) {
        const ssize_t count=::read(STDIN_FILENO,input,sizeof(input));
        if (count<0) { if (errno==EINTR) continue; throw std::runtime_error("cannot read request"); }
        if (count==0) break;
        pending.append(input,static_cast<size_t>(count));
        size_t begin=0;
        for (size_t end=pending.find('\n',begin); end!=std::string::npos;
             end=pending.find('\n',begin)) {
          shutdown=json_request(db,pending.substr(begin,end-begin),&status);
          begin=end+1;
          if (shutdown) break;
        }
        if (begin) pending.erase(0,begin);
      }
      if (!shutdown && !pending.empty()) {
        // A truncated JSONL frame is never executed at EOF.
        emit_json({{"id",nullptr},{"ok",false},{"error",{{"code","invalid_request"},{"message","unterminated JSONL request"}}}});
      }
    } catch (const std::exception& ex) {
      std::fprintf(stderr,"protocol failure: %s\n",ex.what()); status=1;
    }
    if (db.in_txn() && !db.rollback(&err,nullptr)) {
      std::fprintf(stderr,"shutdown rollback failed: %s\n",err.c_str()); status=1;
    }
    db.close();
    return status;
  }

  std::string buf;
  StatementScanner scanner;
  char chunk[65536];
  while (!stopping) {
    ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // EOF
    buf.append(chunk, (size_t)n);

    size_t begin = 0;
    for (size_t end = scanner.next_end(buf); end != std::string::npos;
         end = scanner.next_end(buf)) {
      run_statement(db, buf.substr(begin, end - begin));
      begin = end + 1;
    }
    // Compact once per input chunk, not once per statement in a bulk feed.
    if (begin) {
      buf.erase(0, begin);
      scanner.discard_prefix(begin);
    }
  }

  // Newlines can occur inside statements; never execute a pipe-read prefix.
  // EOF accepts one final SQL statement, not a sequence of guessed fragments.
  if (!stopping && has_token(buf)) run_statement(db, buf);
  if (db.in_txn() && !db.rollback(&err, nullptr)) {
    std::fprintf(stderr, "shutdown rollback failed: %s\n", err.c_str());
    db.close();
    return 1;
  }
  db.close();
  return 0;
}

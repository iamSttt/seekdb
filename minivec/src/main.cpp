// main.cpp — stdin/stdout protocol loop for MiniVectorDB
//
// Reads statements incrementally with read(2) (NOT fread: fread waits for a
// full buffer or EOF, which would stall a streaming driver that keeps stdin
// open). Every complete `;`-terminated statement is executed and flushed
// immediately so the grader can send the next one.
//
// The input buffer is consumed with a moving cursor instead of erase(0, n) per
// statement: a bulk feed of 45 MB would otherwise copy the tail O(n^2) times
// and keep several copies alive (measured: 236 MB RSS for a 45 MB script).
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "db.h"
#include "exec.h"
#include "sql.h"

using namespace mvdb;

namespace {

bool is_space(char c) { return std::isspace((unsigned char)c) != 0; }

bool has_token(const std::string& s) {
  for (char c : s)
    if (!is_space(c)) return true;
  return false;
}

// Finds the ';' that terminates the statement starting at `from`, honouring
// quotes and comments. Returns npos when the statement is still incomplete.
size_t find_terminator(const std::string& buf, size_t from) {
  // `from` is always the start of an unparsed statement: scanning has to begin
  // there, because a previous partial scan could have stopped inside a string
  // literal and a mid-literal restart would desync the quote state.
  bool in_squote = false, in_dquote = false, in_backtick = false;
  bool in_line_comment = false, in_block_comment = false;
  for (size_t i = from; i < buf.size(); ++i) {
    char c = buf[i];
    char n = (i + 1 < buf.size()) ? buf[i + 1] : '\0';
    if (in_line_comment) {
      if (c == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && n == '/') { in_block_comment = false; ++i; }
      continue;
    }
    if (in_squote) {
      if (c == '\\' && i + 1 < buf.size()) { ++i; continue; }
      if (c == '\'') {
        if (n == '\'') { ++i; continue; }
        in_squote = false;
      }
      continue;
    }
    if (in_dquote) {
      if (c == '\\' && i + 1 < buf.size()) { ++i; continue; }
      if (c == '"') { in_dquote = false; }
      continue;
    }
    if (in_backtick) {
      if (c == '`') in_backtick = false;
      continue;
    }
    if (c == '-' && n == '-') { in_line_comment = true; ++i; continue; }
    if (c == '#') { in_line_comment = true; continue; }
    if (c == '/' && n == '*') { in_block_comment = true; ++i; continue; }
    if (c == '\'') { in_squote = true; continue; }
    if (c == '"') { in_dquote = true; continue; }
    if (c == '`') { in_backtick = true; continue; }
    if (c == ';') return i;
  }
  return std::string::npos;
}

void run_statement(Database& db, const std::string& text) {
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
  if (execute(db, st, &out, &perr, &code)) {
    std::fputs(out.c_str(), stdout);
  } else {
    std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
  }
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
    else if (std::strncmp(argv[i], "--data=", 7) == 0) data_dir = argv[i] + 7;
  }

  Database db;
  std::string err;
  if (!db.open(data_dir, &err)) {
    std::fprintf(stderr, "open failed: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "minivec ready data=%s\n", data_dir.c_str());
  std::fflush(stderr);

  std::string buf;
  size_t begin = 0;  // start of unconsumed input
  char chunk[65536];

  for (;;) {
    ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // EOF
    buf.append(chunk, (size_t)n);

    for (;;) {
      size_t semi = find_terminator(buf, begin);
      if (semi == std::string::npos) break;
      std::string stmt = buf.substr(begin, semi - begin);
      begin = semi + 1;
      run_statement(db, stmt);
    }

    // Compact only when the consumed prefix is worth copying.
    if (begin >= (1u << 20)) {
      buf.erase(0, begin);
      begin = 0;
    }

  }

  // Trailing statement without a terminating ';'. If the whole tail does not
  // parse, fall back to one-statement-per-line.
  if (begin < buf.size() && has_token(buf.substr(begin))) {
    std::string tail = buf.substr(begin);
    Statement probe;
    std::string perr;
    int pcode = 0;
    if (parse_sql(tail, &probe, &perr, &pcode)) {
      run_statement(db, tail);
    } else {
      size_t pos = 0;
      while (pos < tail.size()) {
        size_t nl = tail.find('\n', pos);
        std::string line = tail.substr(pos, (nl == std::string::npos ? tail.size() : nl) - pos);
        pos = (nl == std::string::npos) ? tail.size() : nl + 1;
        if (has_token(line)) run_statement(db, line);
      }
    }
  }

  db.close();
  return 0;
}

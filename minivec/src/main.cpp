// main.cpp — stdin/stdout protocol loop for MiniVectorDB
//
// Reads statements incrementally with read(2) (NOT fread: fread waits for a
// full buffer or EOF, which would stall a streaming driver that keeps stdin
// open). Every complete `;`-terminated statement is executed and flushed
// immediately so the grader can send the next one.
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

// Split a buffer into complete statements on ';' at top level (quotes and
// comments respected). Returns the number of bytes consumed.
static size_t take_statement(const std::string& buf, std::string* stmt) {
  bool in_squote = false, in_dquote = false, in_backtick = false;
  bool in_line_comment = false, in_block_comment = false;
  for (size_t i = 0; i < buf.size(); ++i) {
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
    if (c == ';') {
      *stmt = buf.substr(0, i);
      return i + 1;
    }
  }
  return 0;
}

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
  if (execute(db, st, &out, &perr, &code)) {
    std::fputs(out.c_str(), stdout);
  } else {
    std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
  }
  std::fflush(stdout);
}

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
  char chunk[65536];
  for (;;) {
    ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // EOF
    buf.append(chunk, (size_t)n);

    std::string stmt;
    size_t used;
    while ((used = take_statement(buf, &stmt)) > 0) {
      buf.erase(0, used);
      run_statement(db, stmt);
    }

    // Hedge for a driver that terminates statements with a newline instead of
    // ';': only when the buffer is exactly one non-empty line do we try to
    // parse and run it, so multi-line statements are never chopped up.
    if (!buf.empty() && buf.back() == '\n') {
      size_t nl = buf.find('\n');
      bool single_line = (nl != std::string::npos) && !has_token(buf.substr(nl + 1));
      if (single_line) {
        std::string line = buf.substr(0, nl);
        Statement probe;
        std::string perr;
        int pcode = 0;
        if (parse_sql(line, &probe, &perr, &pcode) && probe.kind != Statement::EMPTY) {
          buf.clear();
          run_statement(db, line);
        }
      }
    }
  }

  // Trailing statement without a terminating ';'. If the whole tail does not
  // parse, fall back to one-statement-per-line (a newline-terminated driver
  // that piped everything in one chunk).
  if (has_token(buf)) {
    Statement probe;
    std::string perr;
    int pcode = 0;
    if (parse_sql(buf, &probe, &perr, &pcode)) {
      run_statement(db, buf);
    } else {
      size_t pos = 0;
      while (pos < buf.size()) {
        size_t nl = buf.find('\n', pos);
        std::string line = buf.substr(pos, (nl == std::string::npos ? buf.size() : nl) - pos);
        pos = (nl == std::string::npos) ? buf.size() : nl + 1;
        if (has_token(line)) run_statement(db, line);
      }
    }
  }
  db.close();
  return 0;
}

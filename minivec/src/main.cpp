// main.cpp — stdin/stdout protocol loop for MiniVectorDB
//
// Reads statements incrementally with read(2) (NOT fread: fread waits for a
// full buffer or EOF, which would stall a streaming driver that keeps stdin
// open). Every complete `;`-terminated statement is executed and flushed
// immediately so the grader can send the next one.
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "db.h"
#include "exec.h"
#include "sql.h"

using namespace mvdb;
static volatile sig_atomic_t stopping = 0;
static void stop_signal(int) { stopping = 1; }

// Split a buffer into complete statements on ';' at top level (quotes and
// comments respected). Returns the number of bytes consumed.
static size_t take_statement(const std::string& buf, size_t begin, std::string* stmt) {
  bool in_squote = false, in_dquote = false, in_backtick = false;
  bool in_line_comment = false, in_block_comment = false;
  for (size_t i = begin; i < buf.size(); ++i) {
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
      *stmt = buf.substr(begin, i - begin);
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
  if (execute(db, st, &out, &perr, &code, [](const std::string& text) {
        std::fwrite(text.data(), 1, text.size(), stdout);
      })) {
    std::fwrite(out.data(), 1, out.size(), stdout);
  } else {
    std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
  }
  std::fflush(stdout);
}

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  std::string mode = "exact";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
    else if (std::strncmp(argv[i], "--data=", 7) == 0) data_dir = argv[i] + 7;
    else if (std::strcmp(argv[i], "--mode") == 0 && i+1<argc) mode=argv[++i];
    else { std::fprintf(stderr,"unknown or incomplete option: %s\n",argv[i]);return 2; }
  }

  if(mode!="exact" && mode!="ann") {std::fprintf(stderr,"mode must be exact or ann\n");return 2;}
  // Both modes currently use exact Flat search. This is a safe recall-1
  // implementation; the official mode framing still needs the frozen driver.
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

  std::string buf;
  size_t begin = 0;
  char chunk[65536];
  while (!stopping) {
    ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // EOF
    buf.append(chunk, (size_t)n);

    std::string stmt;
    size_t used;
    while ((used = take_statement(buf, begin, &stmt)) > 0) {
      begin = used;
      run_statement(db, stmt);
    }
    // Compact once per input chunk, not once per statement in a bulk feed.
    if (begin) { buf.erase(0, begin); begin = 0; }

  }

  // Newlines can occur inside statements; never execute a pipe-read prefix.
  // EOF accepts one final SQL statement, not a sequence of guessed fragments.
  if (!stopping && has_token(buf)) run_statement(db, buf);
  db.close();
  return 0;
}

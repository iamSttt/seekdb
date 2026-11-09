// main.cpp — stdin/stdout protocol loop for MiniVectorDB
//
// Reads statements incrementally with read(2) (NOT fread: fread waits for a
// full buffer or EOF, which would stall a streaming driver that keeps stdin
// open). Every complete `;`-terminated statement is executed and flushed
// immediately so the grader can send the next one.
#include <cerrno>
#include <cctype>
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
  enum class State { Sql, Quoted, LineComment, BlockComment };
  State state_ = State::Sql;
  size_t cursor_ = 0;
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

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  std::string mode = "exact";
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
    else if (std::strncmp(argv[i], "--data=", 7) == 0) data_dir = argv[i] + 7;
    else if (std::strcmp(argv[i], "--mode") == 0 && i+1<argc) mode=argv[++i];
    else if (std::strncmp(argv[i], "--mode=", 7) == 0) mode = argv[i] + 7;
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
  db.close();
  return 0;
}

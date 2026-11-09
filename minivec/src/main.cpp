// main.cpp — stdin/stdout protocol loop for MiniVectorDB
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  std::fprintf(stderr, "minivec ready, data=%s\n", data_dir.c_str());
  std::fflush(stderr);

  std::string buf, out;
  char chunk[65536];
  for (;;) {
    size_t n = std::fread(chunk, 1, sizeof(chunk), stdin);
    if (n > 0) buf.append(chunk, n);
    if (n == 0 && !std::feof(stdin)) break;

    std::string stmt;
    size_t used;
    while ((used = take_statement(buf, &stmt)) > 0) {
      buf.erase(0, used);
      // skip pure whitespace/comment statements
      bool has_token = false;
      for (char c : stmt) if (!std::isspace((unsigned char)c)) { has_token = true; break; }
      if (!has_token) continue;

      out.clear();
      Statement st;
      std::string perr;
      int code = 0;
      if (!parse_sql(stmt, &st, &perr, &code)) {
        std::printf("ERROR %d (42000): %s\n", code ? code : 1064, perr.c_str());
        std::fflush(stdout);
        continue;
      }
      if (st.kind == Statement::EMPTY) continue;
      if (execute(db, st, &out, &perr, &code)) {
        std::fputs(out.c_str(), stdout);
      } else {
        std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
      }
      std::fflush(stdout);
    }
    if (n == 0) break;  // EOF: flush remaining (no trailing ';')
  }

  // Execute any trailing statement without a semicolon.
  bool has_token = false;
  for (char c : buf) if (!std::isspace((unsigned char)c)) { has_token = true; break; }
  if (has_token) {
    Statement st;
    std::string perr;
    int code = 0;
    if (parse_sql(buf, &st, &perr, &code)) {
      out.clear();
      if (execute(db, st, &out, &perr, &code)) std::fputs(out.c_str(), stdout);
      else std::printf("ERROR %d (HY000): %s\n", code ? code : 1105, perr.c_str());
    } else {
      std::printf("ERROR %d (42000): %s\n", code ? code : 1064, perr.c_str());
    }
    std::fflush(stdout);
  }

  db.close();
  return 0;
}

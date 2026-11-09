// exec.h — statement execution and MySQL-style result rendering
#pragma once
#include <string>

#include "db.h"
#include "sql.h"

namespace mvdb {

// Executes one parsed statement. On success appends the client-visible text to
// `out` (already newline terminated). On failure returns false, sets *code and
// *err.
using OutputSink = std::function<void(const std::string&)>;
bool execute(Database& db, const Statement& st, std::string* out, std::string* err, int* code,
             const OutputSink& sink = {});

// Renders a float the way MySQL does (shortest round-trip, 6 digits default).
std::string format_double(double v);

}  // namespace mvdb

#pragma once
// Test-only evidence of the row encoder actually entered, including an encoder
// that subsequently rejects invalid UTF-8. Timing binaries omit this macro.
#ifdef MVDB_JSON_RESPONSE_DIAGNOSTICS
#include <cstdint>
namespace mvdb {
struct JsonResponseDiagnostics {
  uint64_t typed_rows = 0, dom_rows = 0;
};
inline thread_local JsonResponseDiagnostics* json_response_diag_current = nullptr;
}
#define MVDB_JSON_RESPONSE_COUNT(field) do { \
  if (mvdb::json_response_diag_current) ++mvdb::json_response_diag_current->field; \
} while (false)
#else
#define MVDB_JSON_RESPONSE_COUNT(field) ((void)0)
#endif

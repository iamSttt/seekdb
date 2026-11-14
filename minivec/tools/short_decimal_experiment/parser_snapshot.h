#pragma once
#include "short_decimal.h"
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <vector>
namespace short_decimal_test {
struct Counters { uint64_t accepted=0,midpoint=0,environment=0,syntax=0,bypassed=0; };
bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
inline bool baseline_parse(std::string_view body, std::vector<float>* out) {
  out->clear();
  // One allocation for the usual 128-coordinate case instead of log2 growth.
  if (out->capacity() == 0) out->reserve(body.size() / 8 + 4);
  const size_t n = body.size();
  size_t i = 0;
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
template<bool Count=false>
bool candidate_parse(std::string_view body, std::vector<float>* out, Counters* counts=nullptr) {
  const short_decimal::Context context(body.size() >= 128);
  bool try_short=context.enabled();
  out->clear();
  // One allocation for the usual 128-coordinate case instead of log2 growth.
  if (out->capacity() == 0) out->reserve(body.size() / 8 + 4);
  const size_t n = body.size();
  size_t i = 0;
  while (i < n) {
    while (i < n && is_space((unsigned char)body[i])) ++i;
    if (i >= n) break;
    const char* begin = body.data() + i;
    const char* endp = nullptr;
    // Parse at storage precision to avoid rounding first to double and then
    // rounding again at a float32 midpoint. Subnormals and zero are finite.
    float stored = 0;
    if(try_short) {
      const auto fast = context.parse(begin, body.data() + n);
      if constexpr (Count) {
        if(fast.reason==short_decimal::Reason::accepted)++counts->accepted;
        else if(fast.reason==short_decimal::Reason::midpoint)++counts->midpoint;
        else if(fast.reason==short_decimal::Reason::environment)++counts->environment;
        else ++counts->syntax;
      }
      if(fast.accepted()) { stored=fast.value; endp=fast.ptr; }
      else try_short=false; // First decline disables attempts for this vector.
    } else if constexpr (Count) ++counts->bypassed;
    if(!endp) {
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
} // namespace short_decimal_test

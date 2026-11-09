// value.cpp — UTF-8 helpers: code point count and code point order compare.
//
// Decoding is deliberately tolerant: a malformed byte (bad lead byte, truncated
// sequence, overlong form, surrogate, > U+10FFFF) is treated as a single code
// point whose value is the raw byte, and the scanner advances by one byte. That
// keeps utf8_length()/utf8_compare() total functions on arbitrary bytes coming
// out of string literals, instead of erroring or reading out of bounds.
#include "value.h"

namespace mvdb {
namespace {

// Decodes one code point starting at p (avail bytes are readable).
// Returns the code point, writes the number of bytes consumed (1..4) to *adv.
inline uint32_t decode_cp(const uint8_t* p, size_t avail, int* adv) {
  const uint8_t b0 = p[0];
  if (b0 < 0x80) {
    *adv = 1;
    return b0;
  }
  if ((b0 & 0xE0) == 0xC0) {
    if (avail >= 2 && (p[1] & 0xC0) == 0x80) {
      uint32_t cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
      if (cp >= 0x80) {  // reject overlong
        *adv = 2;
        return cp;
      }
    }
  } else if ((b0 & 0xF0) == 0xE0) {
    if (avail >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
      uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
                    (uint32_t)(p[2] & 0x3F);
      if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) {  // overlong / surrogate
        *adv = 3;
        return cp;
      }
    }
  } else if ((b0 & 0xF8) == 0xF0) {
    if (avail >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
      uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                    ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
      if (cp >= 0x10000 && cp <= 0x10FFFF) {
        *adv = 4;
        return cp;
      }
    }
  }
  *adv = 1;
  return b0;  // invalid: one code point per raw byte
}

}  // namespace

size_t utf8_length(const std::string& s) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data());
  const size_t n = s.size();
  size_t i = 0, cnt = 0;
  while (i < n) {
    int adv = 1;
    decode_cp(p + i, n - i, &adv);
    i += (size_t)adv;
    ++cnt;
  }
  return cnt;
}

int utf8_compare(const std::string& a, const std::string& b) {
  const uint8_t* pa = reinterpret_cast<const uint8_t*>(a.data());
  const uint8_t* pb = reinterpret_cast<const uint8_t*>(b.data());
  const size_t na = a.size(), nb = b.size();
  size_t i = 0, j = 0;
  while (i < na && j < nb) {
    // ASCII fast path: covers the common case without per-byte decoding.
    if (pa[i] < 0x80 && pb[j] < 0x80) {
      if (pa[i] != pb[j]) return pa[i] < pb[j] ? -1 : 1;
      ++i;
      ++j;
      continue;
    }
    int ad = 1, bd = 1;
    uint32_t ca = decode_cp(pa + i, na - i, &ad);
    uint32_t cb = decode_cp(pb + j, nb - j, &bd);
    if (ca != cb) return ca < cb ? -1 : 1;
    i += (size_t)ad;
    j += (size_t)bd;
  }
  if (i < na) return 1;
  if (j < nb) return -1;
  return 0;
}

}  // namespace mvdb

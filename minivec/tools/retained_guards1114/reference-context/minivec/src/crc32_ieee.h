// IEEE CRC32 update; durable format uses the reflected 0xEDB88320 polynomial.
#pragma once
#include <cstddef>
#include <cstdint>

namespace mvdb { namespace storage_crc {

struct Tables {
  uint32_t t[8][256]{};
  constexpr Tables() {
    for (unsigned i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
      t[0][i] = crc;
    }
    for (unsigned slice = 1; slice < 8; ++slice)
      for (unsigned i = 0; i < 256; ++i) {
        const uint32_t crc = t[slice - 1][i];
        t[slice][i] = t[0][crc & 0xffu] ^ (crc >> 8);
      }
  }
};

inline constexpr Tables tables{};

// Accept/return the unfinalized accumulator, exactly as the previous update.
// Byte loads avoid alignment, aliasing and host-endian assumptions; the loop
// reads eight bytes only after proving that the whole block is available.
inline uint32_t update(uint32_t crc, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  while (size >= 8) {
    const uint32_t first = uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    const uint32_t mixed = crc ^ first;
    crc = tables.t[7][mixed & 0xffu] ^ tables.t[6][(mixed >> 8) & 0xffu] ^
          tables.t[5][(mixed >> 16) & 0xffu] ^ tables.t[4][mixed >> 24] ^
          tables.t[3][p[4]] ^ tables.t[2][p[5]] ^
          tables.t[1][p[6]] ^ tables.t[0][p[7]];
    p += 8;
    size -= 8;
  }
  while (size--) crc = tables.t[0][(crc ^ *p++) & 0xffu] ^ (crc >> 8);
  return crc;
}

inline uint32_t checksum(const void* data, size_t size) {
  return update(0xffffffffu, data, size) ^ 0xffffffffu;
}

}}  // namespace mvdb::storage_crc

// Independent bitwise oracle, streaming states, durable framing and read bounds.
#include "crc32_ieee.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef __linux__
#include <sys/mman.h>
#include <unistd.h>
#endif

static size_t checks = 0;
static void require(bool ok, const char* what) {
  ++checks;
  if (!ok) { std::fprintf(stderr, "FAIL %s\n", what); std::exit(1); }
}
static uint32_t oracle(uint32_t state, const uint8_t* bytes, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    state ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      state = (state >> 1) ^ ((state & 1u) ? 0xedb88320u : 0u);
  }
  return state;
}
static uint32_t next(uint32_t& state) {
  state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state;
}
static uint32_t load32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static void put32(std::vector<uint8_t>& b, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) b.push_back(uint8_t(value >> shift));
}
static bool frame_valid(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 8) return false;
  const uint32_t size = load32(bytes.data());
  return size && size == bytes.size() - 8 &&
         load32(bytes.data() + 4) == mvdb::storage_crc::checksum(bytes.data() + 8, size);
}
int main() {
  using mvdb::storage_crc::update;
  using mvdb::storage_crc::checksum;
  require(checksum("123456789", 9) == 0xcbf43926u, "IEEE known vector");
  require(checksum(nullptr, 0) == 0, "empty checksum");
  for (uint32_t state : {0u, 0xffffffffu, 0x12345678u})
    require(update(state, nullptr, 0) == state, "empty update preserves state");
  uint32_t random = 0x7319abcdu;
  std::vector<uint8_t> bytes((1u << 20) + 64);
  for (auto& value : bytes) value = uint8_t(next(random));
  // Every alignment within one cache line and every short length / block tail.
  for (size_t offset = 0; offset < 64; ++offset) {
    for (size_t n = 0; n <= 513; ++n) {
      const uint32_t state = next(random);
      require(update(state, bytes.data() + offset, n) == oracle(state, bytes.data() + offset, n),
              "all offsets and short lengths match bitwise oracle");
    }
    for (size_t n : {size_t(1023), size_t(1024), size_t(1025), size_t(4095), size_t(4096), size_t(4097)})
      require(update(0xffffffffu, bytes.data() + offset, n) == oracle(0xffffffffu, bytes.data() + offset, n),
              "larger boundaries match bitwise oracle");
  }
  for (size_t n : {size_t(1), size_t(7), size_t(8), size_t(9), size_t(511), size_t(512), size_t(513), size_t(1u << 20)}) {
    const uint32_t want = oracle(0xffffffffu, bytes.data(), n);
    uint32_t state = 0xffffffffu;
    for (size_t at = 0; at < n;) {
      const size_t chunk = std::min(n - at, size_t(1 + next(random) % 255));
      state = update(state, bytes.data() + at, chunk); at += chunk;
    }
    require(state == want, "random chunk boundaries preserve incremental result");
    for (size_t split = 0; split <= std::min(n, size_t(64)); ++split)
      require(update(update(0xffffffffu, bytes.data(), split), bytes.data() + split, n - split) == want,
              "explicit split preserves incremental result");
  }
  // Model the exact WAL [len][crc][payload] framing and the snapshot CRC tail.
  // The old-format checksum comes from the independent oracle, not this table.
  const size_t payload = 563;
  std::vector<uint8_t> frame;
  put32(frame, uint32_t(payload));
  put32(frame, oracle(0xffffffffu, bytes.data(), payload) ^ 0xffffffffu);
  frame.insert(frame.end(), bytes.begin(), bytes.begin() + payload);
  require(frame_valid(frame), "old-format framed record accepted");
  for (size_t index = 0; index < frame.size(); ++index) {
    auto corrupt = frame; corrupt[index] ^= 1u;
    require(!frame_valid(corrupt), "one-bit frame corruption rejected");
  }
  for (size_t n = 0; n < frame.size(); ++n)
    require(!frame_valid(std::vector<uint8_t>(frame.begin(), frame.begin() + n)), "all truncated frames rejected");
  std::vector<uint8_t> snapshot(bytes.begin(), bytes.begin() + payload);
  put32(snapshot, oracle(0xffffffffu, bytes.data(), payload) ^ 0xffffffffu);
  require(checksum(snapshot.data(), snapshot.size() - 4) == load32(snapshot.data() + payload), "old snapshot CRC tail accepted");
#ifdef __linux__
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  void* memory = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED && page >= 513, "guard page mapping");
  auto* end = static_cast<uint8_t*>(memory) + page;
  std::memcpy(end - 513, bytes.data(), 513);
  require(mprotect(end, page, PROT_NONE) == 0, "guard page protection");
  for (size_t n = 0; n <= 513; ++n)
    require(update(0xffffffffu, end - n, n) == oracle(0xffffffffu, end - n, n), "no overread across guard page");
  require(munmap(memory, page * 2) == 0, "guard page cleanup");
#endif
  std::printf("PASS crc32 IEEE slicing-by-8 %zu checks\n", checks);
}

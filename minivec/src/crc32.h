#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace mvdb {
namespace crc32_detail {
#if !defined(MVDB_DISABLE_CRC_SLICING) && defined(__BYTE_ORDER__) && \
    defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
inline constexpr bool slicing_by_8=true;
inline constexpr size_t slices=8;
#else
// Unknown/big endian targets retain the original byte-at-a-time algorithm.
inline constexpr bool slicing_by_8=false;
inline constexpr size_t slices=1;
#endif

struct Tables {
  uint32_t t[slices][256]{};
  constexpr Tables() {
    for(uint32_t i=0;i<256;++i) {
      uint32_t crc=i;
      for(unsigned bit=0;bit<8;++bit)
        crc=(crc&1u) ? (crc>>1)^0xEDB88320u : crc>>1;
      t[0][i]=crc;
    }
    for(size_t slice=1;slice<slices;++slice)
      for(size_t i=0;i<256;++i) {
        const uint32_t previous=t[slice-1][i];
        t[slice][i]=(previous>>8)^t[0][previous&0xffu];
      }
  }
};
inline constexpr Tables tables{};
} // namespace crc32_detail

// Reflected IEEE CRC32 running state. No initial/final XOR is applied here:
// callers may split a message at any byte and continue with the returned state.
// A null pointer is permitted only when n==0, in which case crc is unchanged.
inline uint32_t crc32_upd(uint32_t crc,const void* data,size_t n) {
  const auto* p=static_cast<const uint8_t*>(data);
  const auto& t=crc32_detail::tables.t;
#if !defined(MVDB_DISABLE_CRC_SLICING) && defined(__BYTE_ORDER__) && \
    defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  while(n>=8) {
    uint32_t first,second;
    // memcpy permits arbitrary byte alignment and does not violate aliasing.
    // On verified little-endian targets the low bits are the earliest bytes.
    std::memcpy(&first,p,sizeof(first));
    std::memcpy(&second,p+4,sizeof(second));
    first^=crc;
    crc=t[7][first&0xffu]^t[6][(first>>8)&0xffu]^
        t[5][(first>>16)&0xffu]^t[4][first>>24]^
        t[3][second&0xffu]^t[2][(second>>8)&0xffu]^
        t[1][(second>>16)&0xffu]^t[0][second>>24];
    p+=8;n-=8;
  }
#endif
  while(n--)crc=t[0][(crc^*p++)&0xffu]^(crc>>8);
  return crc;
}

inline uint32_t crc32_of(const void* data,size_t n) {
  return crc32_upd(0xffffffffu,data,n)^0xffffffffu;
}
} // namespace mvdb

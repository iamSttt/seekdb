#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
namespace micro {
struct Data{size_t dimension=0,count=512;std::vector<float>a,b;uint64_t hash=1469598103934665603ull;};
inline uint64_t next(uint64_t& state){state+=0x9e3779b97f4a7c15ull;uint64_t z=state;z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;return z^(z>>31);}
inline float full(uint64_t& state){const uint64_t x=next(state);const uint32_t bits=uint32_t(x>>63)<<31 | uint32_t(124+(x>>24)%5)<<23 | uint32_t(x&0x7fffff);float f;std::memcpy(&f,&bits,4);return f;}
inline Data data(const std::string& name){
  Data d;d.dimension=name=="fullmantissa512"?512:128;
  if(name!="fullmantissa128" && name!="fullmantissa512" && name!="grid128")throw std::runtime_error("unknown fixed case");
  const bool grid=name=="grid128";uint64_t state=0x714cc63bb4f10829ull ^ d.dimension ^ (grid?0xa5ull:0);
  d.a.resize(d.count*d.dimension);d.b.resize(d.a.size());
  for(size_t i=0;i<d.a.size();++i){
    if(grid){d.a[i]=float(int(next(state)%49141)-24570)/4096;d.b[i]=float(int(next(state)%49141)-24570)/4096;}
    else {d.a[i]=full(state);d.b[i]=full(state);}
  }
  for(const auto* values:{&d.a,&d.b})for(float value:*values){uint32_t raw;std::memcpy(&raw,&value,4);for(unsigned i=0;i<4;++i){d.hash^=(raw>>(8*i))&255;d.hash*=1099511628211ull;}}
  return d;
}
}

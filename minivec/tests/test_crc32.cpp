// make test_crc32 test_crc32_byte; ./test_crc32 --bench; ./test_crc32_byte --bench
#include "crc32.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace mvdb;
static uint64_t random_state=0x4c87a39f062bd15eull;
static uint32_t random_word() {
  random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;
  return static_cast<uint32_t>(random_state);
}
static void require(bool ok,const char* message) {
  if(!ok){std::cerr<<"FAIL "<<message<<'\n';std::exit(1);}
}
// Independent bit-serial definition: no lookup tables or slicing recurrence.
static uint32_t bit_oracle(uint32_t crc,const uint8_t* data,size_t n) {
  for(size_t i=0;i<n;++i) {
    crc^=data[i];
    for(unsigned bit=0;bit<8;++bit) {
      const uint32_t low=crc&1u;
      crc>>=1;
      if(low)crc^=0xedb88320u;
    }
  }
  return crc;
}
static size_t checked=0;
static void check_case(const uint8_t* data,size_t n,uint32_t seed) {
  const uint32_t expected=bit_oracle(seed,data,n);
  require(crc32_upd(seed,data,n)==expected,"one-shot update differs from bit oracle");
  require(crc32_of(data,n)==(bit_oracle(0xffffffffu,data,n)^0xffffffffu),
          "initial/final XOR differs from standard IEEE CRC32");
  const size_t splits[]={0,1,7,8,9,n/2,n?n-1:0,n};
  for(size_t split:splits)if(split<=n) {
    const uint32_t first=crc32_upd(seed,data,split);
    require(crc32_upd(first,data+split,n-split)==expected,"two-chunk update differs");
  }
  if(n<=17)for(size_t split=0;split<=n;++split)
    require(crc32_upd(crc32_upd(seed,data,split),data+split,n-split)==expected,
            "exhaustive short split differs");
  uint32_t streamed=seed;
  for(size_t position=0;position<n;) {
    require(crc32_upd(streamed,data+position,0)==streamed,"empty chunk changes state");
    const size_t count=std::min<size_t>(n-position,1+random_word()%257);
    streamed=crc32_upd(streamed,data+position,count);position+=count;
  }
  require(streamed==expected,"random multichunk update differs");
  ++checked;
}
static void tests() {
  require(crc32_upd(0x12345678u,nullptr,0)==0x12345678u,"null empty update");
  require(crc32_of(nullptr,0)==0,"empty standard checksum");
  const char known[]="123456789";
  require(crc32_of(known,9)==0xcbf43926u,"123456789 known IEEE checksum");
  require(crc32_upd(0xffffffffu,known,9)==0x340bc6d9u,"known raw running state");
  require((crc32_upd(crc32_upd(0xffffffffu,known,4),known+4,5)^0xffffffffu)
          ==0xcbf43926u,"known incremental checksum");
  std::vector<uint8_t> data((1u<<20)+64);
  for(auto& byte:data)byte=static_cast<uint8_t>(random_word());
  for(size_t n:{size_t(0),size_t(1),size_t(2),size_t(7),size_t(8),size_t(9),
               size_t(15),size_t(16),size_t(17),size_t(31),size_t(32),size_t(63),
               size_t(64),size_t(127),size_t(128),size_t(129),size_t(255),size_t(256),
               size_t(257),size_t(4095),size_t(4096),size_t(4097),size_t(65535),
               size_t(65536),size_t(65537),size_t(1u<<20)}) {
    const size_t offsets=n<=257?32:n<65535?8:3;
    for(size_t offset=0;offset<offsets;++offset)
      for(uint32_t seed:{uint32_t(0),uint32_t(0xffffffffu),uint32_t(0x81234567u)})
        check_case(data.data()+offset,n,seed);
  }
  for(unsigned trial=0;trial<512;++trial) {
    const size_t n=random_word()%32769,offset=random_word()%32;
    check_case(data.data()+offset,n,random_word());
  }
  // Uniform patterns exercise all-zero, all-one, and long runs at block edges.
  for(uint8_t byte:{uint8_t(0),uint8_t(0xff),uint8_t(0x80)}) {
    std::fill(data.begin(),data.end(),byte);
    check_case(data.data()+1,65537,0xffffffffu);
  }
  std::cout<<"PASS IEEE CRC32 bit oracle, known vector, arbitrary seeds/alignment/chunks; cases="
           <<checked<<" mode="<<(crc32_detail::slicing_by_8?"slicing8":"byte")<<'\n';
}
static volatile uint32_t benchmark_sink=0;
static void benchmark() {
  std::vector<uint8_t> data((1u<<20)+64);
  for(auto& byte:data)byte=static_cast<uint8_t>(random_word());
  for(size_t n:{size_t(64),size_t(512),size_t(4096),size_t(1u<<20)}) {
    const size_t rounds=(64u<<20)/n;
    uint32_t state=0xffffffffu;
    const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<rounds;++i)state=crc32_upd(state,data.data()+1,n);
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    benchmark_sink=state; // Data-dependent state prevents hoisting repeated calls.
    std::cout<<"crc32_microbench mode="<<(crc32_detail::slicing_by_8?"slicing8":"byte")
      <<" buffer_bytes="<<n<<" alignment_offset=1 processed_bytes="<<rounds*n
      <<" seconds="<<std::setprecision(9)<<seconds<<" MiB_per_second="
      <<(double(rounds)*n/(1u<<20)/seconds)<<" checksum="<<std::hex<<state<<std::dec<<'\n';
  }
}
int main(int argc,char** argv) {
  tests();
  if(argc==2 && std::string(argv[1])=="--bench")benchmark();
  else if(argc!=1){std::cerr<<"usage: test_crc32 [--bench]\n";return 2;}
}

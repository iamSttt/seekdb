#include "compensated_distance.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace mvdb::compensated_draft;
static size_t checks=0,certificates=0,boundaries=0,coordinates=0,shapes=0;
static void require(bool ok,const char* why) {
  ++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}
}
static uint64_t bits(double d){uint64_t b;std::memcpy(&b,&d,8);return b;}
static float value(uint32_t b){float f;std::memcpy(&f,&b,4);return f;}
static uint64_t read_unsigned(FILE* f,size_t n) {
  uint64_t out=0;for(size_t i=0;i<n;++i) {const int c=std::fgetc(f);require(c!=EOF,"fixture truncated");out|=uint64_t(c)<<(i*8);}return out;
}
static void fixtures(const char* path) {
  FILE* f=std::fopen(path,"rb");require(f,"fixture open");char magic[8];
  require(std::fread(magic,1,8,f)==8 && !std::memcmp(magic,"MVDCMP1\0",8),"fixture magic");
  const size_t count=read_unsigned(f,4);require(count>1000,"fixture count");
  for(size_t c=0;c<count;++c) {
    const size_t n=read_unsigned(f,4);const uint64_t expected=read_unsigned(f,8);const bool guard=read_unsigned(f,4);
    std::vector<float>a(n?n:1),b(n?n:1);
    for(size_t i=0;i<n;++i)a[i]=value(static_cast<uint32_t>(read_unsigned(f,4)));
    for(size_t i=0;i<n;++i)b[i]=value(static_cast<uint32_t>(read_unsigned(f,4)));
    const Attempt r=attempt(a.data(),b.data(),n);
    if(r.outcome==Outcome::certified) {++certificates;require(guard,"accepted outside input proof domain");require(bits(r.value)==expected,"certificate differs from integer oracle");}
    else if(r.outcome==Outcome::boundary) {++boundaries;require(guard,"boundary outside input proof domain");}
    else if(r.outcome==Outcome::coordinate) {++coordinates;require(!guard,"coordinate guard too restrictive against fixture");}
    else if(r.outcome==Outcome::shape) {++shapes;require(!guard,"shape guard differs");}
    else require(false,"supported normal environment unexpectedly unavailable");
    require(bits(mvdb::distance_exact(a.data(),b.data(),n))==expected,"reference exact disagrees with independent oracle");
    require(bits(mvdb::distance_scalar(a.data(),b.data(),n))==expected,"original scalar disagrees with independent oracle");
    require(bits(distance(a.data(),b.data(),n))==expected,"wrapper differs from integer oracle");
  }
  require(std::fgetc(f)==EOF,"trailing fixture bytes");std::fclose(f);
  require(certificates>100 && boundaries>0 && coordinates>0 && shapes>0,"all decision paths exercised");
}
static void environments() {
  std::vector<float>a(16,.75f),b(16,.25f);
  const unsigned original_mx=_mm_getcsr();unsigned short original_cw;
  __asm__ __volatile__("fnstcw %0":"=m"(original_cw));
  for(unsigned mask:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}) {
    _mm_setcsr(original_mx|mask);const unsigned before=_mm_getcsr();
    require(attempt(a.data(),b.data(),16).outcome==Outcome::environment,"MXCSR guard");
    require(bits(distance(a.data(),b.data(),16))==bits(mvdb::distance_scalar(a.data(),b.data(),16)),"MXCSR fallback");
    require((_mm_getcsr()&~0x3fu)==(before&~0x3fu),"MXCSR controls changed");_mm_setcsr(original_mx);
  }
  for(unsigned mode:{0u,0x0200u,0x0700u,0x0b00u,0x0f00u}) {
    const unsigned short changed=static_cast<unsigned short>((original_cw&~0x0f00u)|mode);
    __asm__ __volatile__("fldcw %0"::"m"(changed));
    require(attempt(a.data(),b.data(),16).outcome==Outcome::environment,"x87 precision/rounding guard");
    require(bits(distance(a.data(),b.data(),16))==bits(mvdb::distance_scalar(a.data(),b.data(),16)),"x87 fallback");
    unsigned short after;__asm__ __volatile__("fnstcw %0":"=m"(after));require(after==changed,"x87 controls changed");
    __asm__ __volatile__("fldcw %0"::"m"(original_cw));
  }
  // Reject each unmasked exception before any candidate arithmetic. Clear all
  // sticky bits first, and restore the masked environment before assertions.
  // Never invoke the scalar fallback while an exception is unmasked.
  for(unsigned bit=7;bit<=12;++bit) {
    __asm__ __volatile__("fnclex" ::: "memory");
    _mm_setcsr((original_mx&~0x3fu)&~(1u<<bit));
    const Outcome outcome=attempt(a.data(),b.data(),16).outcome;
    _mm_setcsr(original_mx);
    require(outcome==Outcome::environment,"unmasked MXCSR exception rejected");
  }
  for(unsigned bit=0;bit<6;++bit) {
    __asm__ __volatile__("fnclex" ::: "memory");
    _mm_setcsr(original_mx&~0x3fu);
    const unsigned short changed=static_cast<unsigned short>(original_cw&~(1u<<bit));
    __asm__ __volatile__("fldcw %0"::"m"(changed));
    const Outcome outcome=attempt(a.data(),b.data(),16).outcome;
    __asm__ __volatile__("fldcw %0"::"m"(original_cw));
    _mm_setcsr(original_mx);
    require(outcome==Outcome::environment,"unmasked x87 exception rejected");
  }
  for(uint32_t special:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u}) {
    a[15]=value(special);require(attempt(a.data(),b.data(),16).outcome==Outcome::coordinate,"nonfinite guard");
    const double x=distance(a.data(),b.data(),16),y=mvdb::distance_scalar(a.data(),b.data(),16);
    require((std::isnan(x)&&std::isnan(y))||bits(x)==bits(y),"nonfinite fallback");
  }
  _mm_setcsr(original_mx);__asm__ __volatile__("fldcw %0"::"m"(original_cw));
}
int main(int argc,char** argv) {
  require(argc==2,"usage: test_compensated_distance oracle.bin");
  if(!available()){std::puts("compensated_distance=UNSUPPORTED");return 77;}
  require(environment_admitted(),"initial full-precision normal FP environment");
  fixtures(argv[1]);environments();
  std::printf("certified=%zu boundary_fallback=%zu coordinate_fallback=%zu shape_fallback=%zu\n",certificates,boundaries,coordinates,shapes);
  std::printf("compensated_distance_checks=%zu\ncompensated_distance=PASS\n",checks);
}

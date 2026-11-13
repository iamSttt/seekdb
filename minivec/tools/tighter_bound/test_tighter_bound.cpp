#include "comparison.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static size_t checks=0,cases=0,midpoints=0,rescue_fixtures=0;
static void need(bool ok,const char* why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
static uint64_t read_uint(FILE* f,size_t n){uint64_t out=0;for(size_t i=0;i<n;++i){int c=std::fgetc(f);need(c!=EOF,"fixture truncated");out|=uint64_t(c)<<(8*i);}return out;}
static void fixtures(const char* path){
  FILE* f=std::fopen(path,"rb");need(f,"fixture open");char magic[8];
  need(std::fread(magic,1,8,f)==8 && !std::memcmp(magic,"MVTLB1\0\0",8),"fixture magic");
  const size_t count=read_uint(f,4);need(count>6000,"fixture count");
  for(size_t j=0;j<count;++j){
    const size_t n=read_uint(f,4);const uint64_t expected=read_uint(f,8);const unsigned kind=read_uint(f,4);
    need(kind<=2 && n<=262144,"fixture shape/kind");
    std::vector<float>a(n?n:1),b(n?n:1);
    for(size_t i=0;i<n;++i)a[i]=comparison::value(static_cast<uint32_t>(read_uint(f,4)));
    for(size_t i=0;i<n;++i)b[i]=comparison::value(static_cast<uint32_t>(read_uint(f,4)));
    need(comparison::bits(retained_mvdb::distance_exact(a.data(),b.data(),n))==expected,"original exact vs independent integer oracle");
    need(comparison::bits(comparison::old_distance(a.data(),b.data(),n))==expected,"original scalar vs independent integer oracle");
    need(comparison::bits(comparison::new_distance(a.data(),b.data(),n))==expected,"candidate vs independent integer oracle");
    ++cases;midpoints+=kind==1;rescue_fixtures+=kind==2;
  }
  need(std::fgetc(f)==EOF,"trailing fixture data");std::fclose(f);
  need(midpoints>100 && rescue_fixtures==10,"boundary fixture inventory");
}
static void environments(){
  const unsigned saved_mx=_mm_getcsr();unsigned short saved_cw;
  __asm__ __volatile__("fnstcw %0":"=m"(saved_cw));
  std::vector<float>a(128,0),b(128,0);
  a[0]=1;a[1]=a[2]=0x1p-27f;a[3]=a[4]=0x1p-29f;
  auto compare_legacy=[&](){
    need(!mvdb::distance_detail::tighter_long_double_environment(),"unsupported guard accepted");
    need(comparison::bits(comparison::old_distance(a.data(),b.data(),a.size()))==
         comparison::bits(comparison::new_distance(a.data(),b.data(),a.size())),"unsupported state changed legacy result");
  };
  for(unsigned mode:{0u,0x0200u,0x0700u,0x0b00u,0x0f00u}){
    const unsigned short changed=static_cast<unsigned short>((saved_cw&~0x0f00u)|mode);
    __asm__ __volatile__("fldcw %0"::"m"(changed):"memory");
    compare_legacy();unsigned short after;__asm__ __volatile__("fnstcw %0":"=m"(after));
    __asm__ __volatile__("fldcw %0"::"m"(saved_cw):"memory");
    need(after==changed,"x87 controls modified");
  }
  for(unsigned mode:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}){
    const unsigned changed=(saved_mx&~((3u<<13)|(1u<<6)|(1u<<15)))|mode;
    _mm_setcsr(changed);compare_legacy();const unsigned after=_mm_getcsr();_mm_setcsr(saved_mx);
    need((after&~0x3fu)==(changed&~0x3fu),"MXCSR controls modified");
  }
  // With a trap enabled test only the predicate, never scalar arithmetic.
  for(unsigned bit=0;bit<6;++bit){
    __asm__ __volatile__("fnclex":::"memory");
    const unsigned short changed=static_cast<unsigned short>(saved_cw&~(1u<<bit));
    __asm__ __volatile__("fldcw %0"::"m"(changed):"memory");
    const bool allowed=mvdb::distance_detail::tighter_long_double_environment();
    __asm__ __volatile__("fldcw %0"::"m"(saved_cw):"memory");need(!allowed,"unmasked x87 guard");
  }
  for(unsigned bit=7;bit<=12;++bit){
    _mm_setcsr((saved_mx&~0x3fu)&~(1u<<bit));
    const bool allowed=mvdb::distance_detail::tighter_long_double_environment();_mm_setcsr(saved_mx);
    need(!allowed,"unmasked SSE guard");
  }
  // Sticky status is not a rejection reason and must not be cleared by guard.
  _mm_setcsr(saved_mx|0x3fu);const unsigned sticky_before=_mm_getcsr();
  const bool allowed=mvdb::distance_detail::tighter_long_double_environment();
  const unsigned sticky_after=_mm_getcsr();_mm_setcsr(saved_mx);
  need(allowed && sticky_before==sticky_after,"guard altered/rejected sticky status");
  for(uint32_t special:{0x7f800000u,0xff800000u,0x7fc00001u,0x7f800001u}){
    a[0]=comparison::value(special);
    const double x=comparison::old_distance(a.data(),b.data(),a.size()),y=comparison::new_distance(a.data(),b.data(),a.size());
    need((std::isnan(x)&&std::isnan(y))||comparison::bits(x)==comparison::bits(y),"nonfinite legacy behavior changed");
  }
  _mm_setcsr(saved_mx);__asm__ __volatile__("fldcw %0"::"m"(saved_cw):"memory");
}
int main(int argc,char** argv){
  need(argc==2,"usage: test_tighter_bound oracle.bin");
  if(!mvdb::distance_detail::tighter_long_double_environment()){std::puts("tighter_bound=UNSUPPORTED");return 77;}
  fixtures(argv[1]);environments();
  std::printf("tighter_bound_cases=%zu midpoint_fixtures=%zu rescue_fixtures=%zu\n",cases,midpoints,rescue_fixtures);
  std::printf("tighter_bound_checks=%zu\ntighter_bound=PASS\n",checks);
}

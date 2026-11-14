#include "parser_snapshot.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace short_decimal_test;
static size_t checks=0,oracle_cases=0,semantic_cases=0;
static void need(bool b,const char* what) {
  ++checks;if(!b)throw std::runtime_error(what);
}
static uint32_t bits(float x){uint32_t b;std::memcpy(&b,&x,4);return b;}
static void same(const std::string& s) {
  std::vector<float> a,b;
  const bool ao=baseline_parse(s,&a),bo=candidate_parse(s,&b);
  need(ao==bo,"parser acceptance differs");need(a.size()==b.size(),"partial output size differs");
  for(size_t i=0;i<a.size();++i)need(bits(a[i])==bits(b[i]),"parsed float bits differ");
  ++semantic_cases;
}
int main(int argc,char** argv)try {
  need(argc==2,"usage: test-short-decimal oracle.tsv");
  need(short_decimal::Context::supported(),"native fast-path environment unavailable");
  std::ifstream in(argv[1]);need(bool(in),"oracle open");
  std::string line;
  Counters counts;const short_decimal::Context token_context;
  size_t high_uint64_accepted=0,high_uint64_guarded=0;
  while(std::getline(in,line)) {
    auto tab=line.find('\t');need(tab!=std::string::npos,"oracle record");
    const std::string s=line.substr(0,tab);
    const uint32_t expected=static_cast<uint32_t>(std::stoul(line.substr(tab+1),nullptr,16));
    std::vector<float> value;
    const bool ok=candidate_parse(s,&value);
    const auto fast=token_context.parse(s.data(),s.data()+s.size());
    uint64_t decimal_m=0;unsigned decimal_digits=0;bool short_spelling=true;
    for(size_t i=s[0]=='-'?1:0;i<s.size();++i) {
      if(s[i]=='.')continue;
      if(s[i]<'0' || s[i]>'9' || decimal_digits==19){short_spelling=false;break;}
      decimal_m=decimal_m*10+unsigned(s[i]-'0');++decimal_digits;
    }
    if(short_spelling && decimal_m>=(uint64_t(1)<<63)) {
      high_uint64_accepted+=fast.accepted();
      high_uint64_guarded+=fast.reason==short_decimal::Reason::midpoint;
    }
    if(fast.reason==short_decimal::Reason::accepted) {
      ++counts.accepted;need(bits(fast.value)==expected,"direct fast result differs from rational oracle");
      need(fast.ptr==s.data()+s.size(),"direct oracle token consumed partial range");
    } else if(fast.reason==short_decimal::Reason::midpoint)++counts.midpoint;
    else if(fast.reason==short_decimal::Reason::environment)++counts.environment;
    else ++counts.syntax;
    const bool finite=(expected&0x7f800000)!=0x7f800000;
    if(ok!=finite){std::cerr<<"oracle acceptance token="<<s<<'\n';need(false,"oracle acceptance");}
    if(finite) {
      if(value.size()!=1 || bits(value[0])!=expected) {
        std::cerr<<"oracle mismatch token="<<s<<" expected="<<std::hex<<expected
                 <<" got="<<(value.empty()?0:bits(value[0]))<<std::dec<<'\n';
        need(false,"integer-rational oracle bits");
      }
    }
    same(s);++oracle_cases;
  }
  need(oracle_cases>50000,"oracle corpus unexpectedly small");
  need(counts.accepted>10000 && counts.midpoint>0 && counts.syntax>0,"path coverage");
  need(high_uint64_accepted>0 && high_uint64_guarded>0,"uint64 high-bit conversion coverage");
  const std::vector<std::string> tokens={"", " ","-","+",".","1.",".1","-.1","+1",
    "-0","-0.0","00.0","0000000000000000","0x1","-0x1.8p+2","1e3","1e-45",
    "-1e-1000","1e1000","INF","-infinity","nan","nan(12)","1junk","1.2.3","1e",
    "1_0","1\t","1\v","1\f",std::string("1\0",2),std::string("\xc2\xa0"),"16777217",
    "999999999999999","9999999999999999","0.000000000000001","16384.0009765625",
    "37.692861557006836","-37.692861557006836","927745.5312499999999","-927745.5312499999999",
    "9223372036854775807","9223372036854775808","9223372036854775809",
    "9999999999999999999","10000000000000000000","0.000000000000000001",
    "0.0000000000000000001"};
  for(const auto& t:tokens) {
    same(t);same("  "+t+" \t");same("1,"+t);same(t+",2");same("1,"+t+",2");
    same(t+",");same(t+",,");
  }
  Counters vector_counts;
  // Exercise the >=128-byte gate and first-decline limiter in the full parser,
  // including a decline after many successes and after previous valid values.
  for(unsigned dim:{1u,16u,128u,768u})for(unsigned at:{0u,1u,15u,63u,127u,767u}) {
    if(at>=dim)continue;
    for(const auto& t:tokens) {
      std::string body;
      for(unsigned j=0;j<dim;++j){if(j)body+=',';body+=j==at?t:"0.123456";}
      same(body);std::vector<float> scratch;
      const uint64_t before=vector_counts.accepted;
      candidate_parse<true>(body,&scratch,&vector_counts);
      need(scratch.size()>=vector_counts.accepted-before,"prefix count exceeds output");
      vector_counts.bypassed+=scratch.size()-(vector_counts.accepted-before);
    }
  }
  need(vector_counts.accepted>0 && vector_counts.midpoint>0 && vector_counts.syntax>0 && vector_counts.bypassed>0,
       "full-vector activation/first-decline coverage");
  // Arbitrary bytes and partial-result behavior, with a physical NUL terminator
  // just as std::string-based vector spellings provide to the old strtof path.
  std::mt19937_64 rng(0xdec1a1114);
  const std::string alphabet="0123456789-+.eExXpPnNaAiIfFtTyY(), \t\r\n\f\v_";
  for(unsigned i=0;i<30000;++i) {
    std::string s;
    const unsigned n=unsigned(rng()%65);
    for(unsigned j=0;j<n;++j)s+=alphabet[rng()%alphabet.size()];
    if(i%7==0)s.insert(s.begin()+s.size()/2,'\0');
    same(s);
  }
  // Context failure never consumes input; successful pointers stop before a
  // delimiter. This matters when callers continue parsing a longer vector.
  const short_decimal::Context context;
  for(const char* s:{"12.5,2","-0\t,2","1e3,2","1x","16777217,2"}) {
    const auto r=context.parse(s,s+std::strlen(s));
    if(r.accepted())need(*r.ptr==',' || short_decimal::space(*r.ptr),"accepted ptr boundary");
    else need(r.ptr==s,"failed fast path changed ptr");
  }
  const char nonnul[]={'1','2','3','4','e','6'};
  const auto bounded=context.parse(nonnul,nonnul+3);
  need(bounded.accepted() && bounded.ptr==nonnul+3 && bits(bounded.value)==bits(123.f),
       "fast parser ignored explicit non-NUL end");
#if defined(__linux__)
  const long page=sysconf(_SC_PAGESIZE);need(page>0,"page size");
  void* mapping=mmap(nullptr,size_t(page)*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  need(mapping!=MAP_FAILED,"guard page map");
  char* end=static_cast<char*>(mapping)+page;
  need(mprotect(end,size_t(page),PROT_NONE)==0,"guard page protect");
  for(const char* s:{"0","-","-0","0.",".","1.25","1e3","16777217",
                     "9999999999999999999","10000000000000000000","1 ","1,"}) {
    const size_t n=std::strlen(s);std::memcpy(end-n,s,n);
    const auto r=context.parse(end-n,end);
    need(r.ptr>=end-n && r.ptr<=end,"guard-page parse ptr");
  }
  need(!context.parse(end,end).accepted(),"empty inaccessible range");
  need(munmap(mapping,size_t(page)*2)==0,"guard page unmap");
#endif
  const int original_round=std::fegetround();
  for(int mode:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
    need(std::fesetround(mode)==0,"set alternate rounding");
    need(!short_decimal::Context().enabled(),"alternate rounding must fall back");
    for(const auto& t:tokens)same(t);
  }
  need(std::fesetround(original_round)==0,"restore rounding");
#if defined(__x86_64__) && defined(__SSE2__)
  const unsigned saved=_mm_getcsr();
  for(unsigned flag:{1u<<6,1u<<15,1u<<13,2u<<13,3u<<13}) {
    _mm_setcsr(saved|flag);need(!short_decimal::Context().enabled(),"MXCSR environment must fall back");
    for(const auto& t:tokens)same(t);
    _mm_setcsr(saved);
  }
  for(unsigned mask=7;mask<=12;++mask) {
    _mm_setcsr((saved&~(1u<<mask))&~63u);
    need(!short_decimal::Context().enabled(),"each unmasked exception must fall back");
    _mm_setcsr(saved);
  }
#endif
  need(short_decimal::Context().enabled(),"environment not restored");
  std::cout<<"{\"status\":\"PASS\",\"oracle_cases\":"<<oracle_cases
    <<",\"semantic_cases\":"<<semantic_cases<<",\"checks\":"<<checks
    <<",\"direct_token_accepted\":"<<counts.accepted<<",\"direct_token_midpoint_fallback\":"<<counts.midpoint
    <<",\"direct_token_syntax_fallback\":"<<counts.syntax
    <<",\"high_uint64_accepted\":"<<high_uint64_accepted<<",\"high_uint64_guarded\":"<<high_uint64_guarded
    <<",\"vector_accepted\":"<<vector_counts.accepted<<",\"vector_midpoint_fallback\":"<<vector_counts.midpoint
    <<",\"vector_syntax_fallback\":"<<vector_counts.syntax<<",\"vector_bypassed\":"<<vector_counts.bypassed<<"}\n";
  return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}

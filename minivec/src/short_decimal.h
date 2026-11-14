#pragma once
// Work-only experiment. Does not change SQL grammar or the original fallback.
#include <cfenv>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <limits>
#if defined(__x86_64__) && defined(__SSE2__)
#include <xmmintrin.h>
#endif

namespace short_decimal {
inline bool space(unsigned char c) {
  return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f' || c=='\v';
}
enum class Reason { accepted, environment, syntax_or_size, midpoint };
struct Result {
  Reason reason;
  const char* ptr;
  float value;
  bool accepted() const { return reason==Reason::accepted; }
};

// Construct once per vector. The caller must not change the FP environment
// during that vector's parse. Unsupported builds/environments use old parsing.
class Context {
 public:
  explicit Context(bool attempt=true): enabled_(attempt && supported()) {}
  bool enabled() const { return enabled_; }
  static bool supported() {
#if defined(__x86_64__) && defined(__SSE2__) && FLT_EVAL_METHOD == 0 && !defined(__FAST_MATH__)
    if(!std::numeric_limits<float>::is_iec559 ||
       !std::numeric_limits<double>::is_iec559 || sizeof(float)!=4 || sizeof(double)!=8)
      return false;
    if(std::fegetround()!=FE_TONEAREST)return false;
    const unsigned csr=_mm_getcsr();
    return (csr & ((3u<<13)|(1u<<15)|(1u<<6)))==0 &&
           (csr & (63u<<7))==(63u<<7);
#else
    return false;
#endif
  }
  Result parse(const char* begin,const char* end) const {
    if(!enabled_)return {Reason::environment,begin,0};
    const char* p=begin;
    bool negative=false;
    if(p!=end && *p=='-'){negative=true;++p;}
    // A deliberately strict subset: at least one integer digit, and at least
    // one fractional digit when a dot is present. .1/1./+1 all fall back.
    if(p==end || *p<'0' || *p>'9')return {Reason::syntax_or_size,begin,0};
    uint64_t mantissa=0;
    unsigned digits=0,fraction=0;
    while(p!=end && *p>='0' && *p<='9') {
      if(digits==19)return {Reason::syntax_or_size,begin,0};
      mantissa=mantissa*10+static_cast<unsigned>(*p-'0');++digits;++p;
    }
    if(p!=end && *p=='.') {
      ++p;
      if(p==end || *p<'0' || *p>'9')return {Reason::syntax_or_size,begin,0};
      while(p!=end && *p>='0' && *p<='9') {
        if(digits==19)return {Reason::syntax_or_size,begin,0};
        mantissa=mantissa*10+static_cast<unsigned>(*p-'0');
        ++digits;++fraction;++p;
      }
    }
    // Do not return the prefix of an exponent/hex/garbage token. Preserve the
    // old from_chars + strtof path and its error/partial-pointer behavior.
    if(p!=end && *p!=',' && !space(static_cast<unsigned char>(*p)))
      return {Reason::syntax_or_size,begin,0};
    static constexpr double powers[]={1.,10.,100.,1000.,10000.,100000.,1000000.,
      10000000.,100000000.,1000000000.,10000000000.,100000000000.,
      1000000000000.,10000000000000.,100000000000000.,1000000000000000.,
      10000000000000000.,100000000000000000.,1000000000000000000.};
    static_assert(sizeof powers/sizeof powers[0]==19,"powers 10^0 through 10^18");
    // 10^fraction is exactly represented; mantissa->double and division each
    // round once to nearest. Their combined error is <3 ulp64(d). Explicit
    // division is essential: reciprocal multiplication has no proof.
    const double d=static_cast<double>(mantissa)/powers[fraction];
    uint64_t bits=0;std::memcpy(&bits,&d,sizeof bits);
    // All nonzero magnitudes are float32 normal and finite. Reject every d
    // within four binary64 grid positions of a binary32 rounding boundary.
    // Checking only equality is insufficient once integer conversion rounds.
    const uint64_t low=bits & ((uint64_t(1)<<29)-1);
    constexpr uint64_t midpoint=uint64_t(1)<<28;
    if(low>=midpoint-4 && low<=midpoint+4)
      return {Reason::midpoint,begin,0};
    float value=static_cast<float>(d);
    if(negative)value=-value; // Includes negative zero.
    return {Reason::accepted,p,value};
  }
 private:
  bool enabled_;
};
} // namespace short_decimal

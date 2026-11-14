// Extracted from the actually tested be9dde5 legacy cap-equivalence case.
// Original functions and independent uint64 oracle retained verbatim.
#include "quantized_bound.h"
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>
using namespace mvdb;
static void require(bool condition,const char* message) {
  if(!condition){std::cerr<<"FAIL "<<message<<"\n";std::exit(1);}
}
// Independent wide scalar oracle: no SIMD, horizontal reduction, cached cap or
// floating-point approximation is shared with the implementation under test.
static uint64_t integer_oracle(const int8_t* a,const int8_t* b,size_t n) {
  uint64_t sum=0;
  for(size_t i=0;i<n;++i) {
    const int64_t difference=int64_t(a[i])-int64_t(b[i]);
    sum+=static_cast<uint64_t>(difference*difference);
  }
  return sum;
}
// Preserve the prior floating-cap decision exactly, including its performance
// fallback. nextafter and the wide scalar distance are independent of the new
// integer cap and production bit-neighbor helpers.
static bool legacy_reject(uint64_t distance,size_t n,double step,double threshold) {
  if(!std::isfinite(threshold) || threshold<0)return false;
  const auto up=[](double value) {
    return std::isfinite(value)
        ? std::nextafter(value,std::numeric_limits<double>::infinity()) : value;
  };
  const double root_n=up(std::sqrt(static_cast<double>(n)));
  const double tu=up(threshold);
  if(!std::isfinite(tu))return false;
  const double scaled=up(tu/(step*step));
  if(!std::isfinite(scaled))return false;
  const double root=up(std::sqrt(scaled));
  if(!std::isfinite(root) || root<4*root_n)return false;
  const double sum=up(root+root_n);
  const double cap=up(sum*sum);
  return std::isfinite(cap) && static_cast<double>(distance)>cap;
}
static void check_integer_cap_equivalence() {
  for(size_t n:{size_t(16),size_t(128),size_t(65536)})
    for(int exponent:{-100,0,100}) {
      const double step=std::ldexp(1.0,exponent);
      std::vector<float> query(n,static_cast<float>(-127*step));
      std::vector<int8_t> row(n,127),coded_query(n,-127);
      const uint64_t integer=integer_oracle(row.data(),coded_query.data(),n);
      const QuantizedQueryBound bound(query.data(),n,step);
      const auto check=[&](double threshold) {
        const bool expected=bound.enabled() && legacy_reject(integer,n,step,threshold);
        // Repeat to exercise both cache miss and the validated cached path.
        for(int repeat=0;repeat<2;++repeat)
          require(bound.definitely_greater(row.data(),threshold)==expected,
                  "integer cap differs from legacy floating cap");
      };
      for(double target:{double(integer)-1,double(integer),double(integer)+1,
                         2147483647.0,2147483648.0,2147483649.0,
                         4294967294.0,4294967295.0,4294967296.0}) {
        const double radius=std::sqrt(target)-std::sqrt(static_cast<double>(n));
        const double threshold=(radius*radius)*(step*step);
        // Rounding can put the cap immediately above/below an integer. Sweep
        // adjacent representable thresholds on both sides of that boundary.
        double lower=threshold,upper=threshold;
        for(int i=0;i<24;++i) {
          check(lower);check(upper);
          check(std::numeric_limits<double>::quiet_NaN());
          check(std::numeric_limits<double>::infinity());check(-1);
          check(lower); // invalid input must not corrupt a reusable cached cap
          lower=std::nextafter(lower,0.0);
          upper=std::nextafter(upper,std::numeric_limits<double>::infinity());
        }
      }
      check(0);check(-0.0);check(std::numeric_limits<double>::max());
    }
}
int main() {
  check_integer_cap_equivalence();
  std::cout << "PASS isolated integer-cap legacy decision equivalence, thresholds and cache transitions\n";
}

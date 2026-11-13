// g++ -O3 -std=c++17 -pthread -Isrc tests/test_cached_quantizer.cpp src/flat_index.cpp -o test_cached_quantizer
#include "flat_index.h"
#include "quantized_bound.h"
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <unistd.h>

using namespace mvdb;
namespace {
size_t checks=0;
void require(bool condition,const char* message) {
  ++checks;
  if(!condition) {std::fprintf(stderr,"FAIL cached quantizer: %s (check=%zu)\n",message,checks);std::exit(1);}
}
float from_bits(uint32_t bits) {float value;std::memcpy(&value,&bits,sizeof(value));return value;}
uint32_t float_bits(float value) {uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));return bits;}
uint64_t double_bits(double value) {uint64_t bits;std::memcpy(&bits,&value,sizeof(bits));return bits;}
double power(int exponent) {
  const uint64_t bits=exponent>=-1022 ? uint64_t(exponent+1023)<<52 : uint64_t(1)<<(exponent+1074);
  double value;std::memcpy(&value,&bits,sizeof(value));return value;
}

// Independent dyadic oracle: no floating multiply/divide, reciprocal, or call
// to either production encoder. Success/range and half-away rounding use only
// the source significand and the difference of the two binary exponents.
bool oracle(float value,double step,int8_t* result) {
  const uint64_t sb=double_bits(step),fraction=sb&0x000fffffffffffffull;
  const unsigned se=unsigned((sb>>52)&2047);
  if((sb>>63) || se==2047)return false;
  int k=0;
  if(se) {if(fraction)return false;k=int(se)-1023;}
  else {
    if(!fraction || (fraction&(fraction-1)))return false;
    unsigned bit=0;for(uint64_t f=fraction;f>1;f>>=1)++bit;
    k=int(bit)-1074;
  }
  const uint32_t fb=float_bits(value),frac=fb&0x007fffffu;
  const unsigned fe=(fb>>23)&255;
  if(fe==255)return false;
  const uint32_t mantissa=fe ? (0x00800000u|frac) : frac;
  if(!mantissa) {*result=0;return true;}
  const int shift=(fe ? int(fe)-150 : -149)-k;
  uint32_t rounded=0;
  if(shift>=0) {
    if(shift>6)return false;
    const uint64_t scaled=uint64_t(mantissa)<<unsigned(shift);
    if(scaled>127)return false;
    rounded=uint32_t(scaled);
  } else {
    const unsigned divisor_bits=unsigned(-shift);
    if(divisor_bits<24 && uint64_t(mantissa)>(uint64_t(127)<<divisor_bits))return false;
    if(divisor_bits<25)
      rounded=(mantissa+(uint32_t(1)<<(divisor_bits-1)))>>divisor_bits;
  }
  *result=static_cast<int8_t>((fb>>31) ? -int(rounded) : int(rounded));
  return true;
}

struct Environment {
  std::fenv_t saved;
#ifdef MVDB_SIMD_BOUND_X86
  unsigned csr;
#endif
  Environment() {
    require(std::fegetenv(&saved)==0,"save FP environment");
#ifdef MVDB_SIMD_BOUND_X86
    csr=_mm_getcsr();
#endif
    require(std::fesetround(FE_TONEAREST)==0,"set nearest rounding");
#ifdef MVDB_SIMD_BOUND_X86
    // Mask exceptions for comparisons while leaving all six status bits clear.
    _mm_setcsr((_mm_getcsr()|0x1f80u)&~0xe07fu);
#endif
    require(quantized_detail::normal_fp_environment(),"test requires supported normal FP environment");
  }
  ~Environment() {
    std::fesetenv(&saved);
#ifdef MVDB_SIMD_BOUND_X86
    _mm_setcsr(csr);
#endif
  }
};
// Keep each measured production operation across a real call boundary so the
// optimizer cannot fold constant inputs into or move arithmetic through the
// exception-state sampling calls. These wrappers belong only to this test.
#if defined(__GNUC__)
__attribute__((noinline,noclone))
#endif
bool scalar_call(float value,double step,int8_t* out) {
  return quantize_coordinate(value,step,out);
}
#if defined(__GNUC__)
__attribute__((noinline,noclone))
#endif
bool batch_call(const float* values,size_t count,double step,int8_t* out) {
  return quantize_coordinates(values,count,step,out);
}
struct Flags {int portable;unsigned native;};
void clear_flags() {
  std::feclearexcept(FE_ALL_EXCEPT);
#ifdef MVDB_SIMD_BOUND_X86
  _mm_setcsr(_mm_getcsr()&~0x3fu);
#endif
}
Flags flags() {
  Flags result{std::fetestexcept(FE_ALL_EXCEPT),0};
#ifdef MVDB_SIMD_BOUND_X86
  result.native=_mm_getcsr()&0x3fu; // Includes the denormal-operand flag.
#endif
  return result;
}
void same_flags(Flags a,Flags b) {
  require(a.portable==b.portable,"portable FP exception flags changed");
  require(a.native==b.native,"MXCSR exception flags changed");
}
void check_one(float value,double step) {
  int8_t expected=91,old[3]={91,91,91},current[3]={91,91,91};
  const bool expected_ok=oracle(value,step,&expected);
  clear_flags();const bool old_ok=scalar_call(value,step,&old[1]);const Flags old_flags=flags();
  clear_flags();const bool current_ok=batch_call(&value,1,step,&current[1]);const Flags current_flags=flags();
  if(old_ok!=expected_ok || current_ok!=expected_ok || (expected_ok && (old[1]!=expected || current[1]!=expected)))
    std::fprintf(stderr,"value_bits=%08x step_bits=%016llx oracle=%d/%d scalar=%d/%d batch=%d/%d\n",
        float_bits(value),(unsigned long long)double_bits(step),int(expected_ok),int(expected),
        int(old_ok),int(old[1]),int(current_ok),int(current[1]));
  require(old_ok==expected_ok && current_ok==expected_ok,"independent oracle success mismatch");
  require(!expected_ok || (old[1]==expected && current[1]==expected),"independent oracle code mismatch");
  require(std::memcmp(old,current,sizeof(old))==0,"code/failure write/canary mismatch");
  require(current[0]==91 && current[2]==91,"single-coordinate output canary changed");
  same_flags(old_flags,current_flags);
}
void check_row(const std::vector<float>& values,double step) {
  std::vector<int8_t> old(values.size()+2,91),current(old),expected(old);
  bool expected_ok=true;
  for(size_t i=0;i<values.size();++i)
    if(!oracle(values[i],step,&expected[i+1])) {expected_ok=false;break;}
  bool old_ok=true;
  clear_flags();
  for(size_t i=0;i<values.size();++i)
    if(!scalar_call(values[i],step,&old[i+1])) {old_ok=false;break;}
  const Flags old_flags=flags();
  clear_flags();const bool current_ok=batch_call(values.data(),values.size(),step,current.data()+1);
  const Flags current_flags=flags();
  require(old_ok==expected_ok && current_ok==expected_ok,"row success mismatch");
  require(old==expected && current==expected,"row prefix/canary/oracle mismatch");
  same_flags(old_flags,current_flags);
}

void numeric_matrix() {
  const uint32_t canonical[]={0,0x80000000u,1,0x80000001u,0x007fffffu,0x807fffffu,
    0x00800000u,0x80800000u,0x3effffffu,0x3f000000u,0x3f000001u,0xbf000000u,
    0x3f800000u,0xbf800000u,0x42fdffffu,0x42fe0000u,0x42fe0001u,0xc2fe0000u,
    0x7f7fffffu,0xff7fffffu,0x7f800000u,0xff800000u,0x7fc00001u,0xffc00001u,
    0x7f800001u,0xff800001u};
  // Every representable positive double power of two, including every
  // subnormal power. This is not an exhaustive enumeration of 2^32 floats.
  for(int exponent=-1074;exponent<=1023;++exponent)
    for(uint32_t bits:canonical)check_one(from_bits(bits),power(exponent));
  const int exponents[]={-1074,-1024,-1023,-1022,-1021,-155,-149,-128,-126,-7,-1,0,1,7,121,122,126,1021,1022,1023};
  for(unsigned exponent=0;exponent<256;++exponent)
    for(uint32_t fraction:{0u,1u,0x00400000u,0x007fffffu})
      for(uint32_t sign:{0u,0x80000000u})
        for(int step:exponents)check_one(from_bits(sign|(exponent<<23)|fraction),power(step));
  uint32_t state=0x9173a6d5u;
  auto next=[&] {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
  for(unsigned i=0;i<65536;++i) {
    const float value=from_bits(next());
    const int exponent=(i&1) ? -155+int(next()%278) : -1074+int(next()%2098);
    check_one(value,power(exponent));
  }
  for(int exponent:{-155,-149,-126,-7,-1,0,1,7,121,122})
    for(int cell=0;cell<=127;++cell) {
      const float center=std::ldexp(float(cell)+0.5f,exponent);
      for(float value:{std::nextafter(center,-std::numeric_limits<float>::infinity()),center,
                       std::nextafter(center,std::numeric_limits<float>::infinity())}) {
        check_one(value,power(exponent));check_one(-value,power(exponent));
      }
    }
  for(double step:{0.0,-0.0,-1.0,0.75,1.5,std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    for(float value:{0.0f,1.0f,-1.0f,std::numeric_limits<float>::infinity()})check_one(value,step);
}

void rows_and_environment() {
  for(float bad:{128.0f,-128.0f,std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::quiet_NaN(),from_bits(0x7f800001u)})
    for(size_t at=0;at<12;++at) {
      std::vector<float> values(12,0.5f);values[at]=bad;check_row(values,1);
    }
  for(int exponent:{-1074,-1024,-1023,-1022,-155,0,122,1022,1023}) {
    check_row({0.0f,-0.0f,1.0f,-1.0f},power(exponent));
    check_row({std::numeric_limits<float>::quiet_NaN(),0.0f},power(exponent));
  }
  float value=1;int8_t out=91;
  require(!batch_call(nullptr,1,1,&out) && out==91,"null input");
  require(!batch_call(&value,1,1,nullptr),"null output");
  require(batch_call(&value,0,1,&out) && out==91,"empty valid span");
  const auto disabled=[&] {
    int8_t old=91,current=91;
    clear_flags();const bool a=scalar_call(value,1,&old);const Flags af=flags();
    clear_flags();const bool b=batch_call(&value,1,1,&current);const Flags bf=flags();
    require(!a && !b && old==91 && current==91,"non-default FP must reject before writes");
    same_flags(af,bf);
  };
#ifdef MVDB_SIMD_BOUND_X86
  const unsigned normal=_mm_getcsr();
  for(unsigned changed:{1u<<6,1u<<15,(1u<<6)|(1u<<15),1u<<13,2u<<13,3u<<13}) {
    _mm_setcsr((normal&~0x3fu)|changed);disabled();_mm_setcsr(normal);
  }
#endif
  for(int rounding:{FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}) {
    require(std::fesetround(rounding)==0,"set nondefault rounding");disabled();
    require(std::fesetround(FE_TONEAREST)==0,"restore nearest rounding");
  }
  require(quantized_detail::normal_fp_environment(),"FP controls restored");
}

struct Directory {
  char path[64]="/tmp/mvdb-cached-quantizer-XXXXXX";
  Directory() {require(::mkdtemp(path)!=nullptr,"create index test directory");}
  ~Directory() {::rmdir(path);}
};
void physical_index(size_t dimension,size_t count,size_t budget,unsigned distribution,bool expected_codes,bool expected_local=false) {
  Directory directory;
  std::vector<float> values(count*dimension);
  for(size_t i=0;i<values.size();++i) {
    if(distribution==0)values[i]=float(int((i*13)%251)-125)*0.125f;
    else if(distribution==1)values[i]=(i&1) ? -0.0f : 0.0f;
    else if(distribution==2)values[i]=(i&1) ? -std::numeric_limits<float>::denorm_min() : std::numeric_limits<float>::denorm_min();
    else if(distribution==3)values[i]=(i&1) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    else values[i]=float(int((i*13)%251)-125)*(i<dimension*1024 ? 0.000125f : 1024.0f);
  }
  FlatIndex index(directory.path,dimension,count,budget);
  require(index.resident_bytes()<=budget,"pre-build index budget");
  for(size_t i=0;i<count;++i)index.add(int64_t(500-i),i*3,values.data()+i*dimension,dimension);
  index.finish();index.finish();
  require(index.resident_bytes()<=budget,"finished index budget");
  FlatIndex::ResidentView view;
  const bool resident=index.resident_view(&view);
  if(!resident) {require(!expected_codes,"disk path cannot expose codes");return;}
#ifdef MVDB_DISABLE_QUANTIZED_BOUND
  expected_codes=false;
#endif
  require(bool(view.quantized)==expected_codes,"code budget admission unchanged");
  require(view.count==count && view.dimension==dimension,"index shape unchanged");
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
  require(view.quantization_scales.present()==expected_local,"local-scale admission must be exercised as specified");
#else
  (void)expected_local;
  require(!view.quantization_scales.present(),"disabled local scales must stay absent");
#endif
  for(size_t i=0;i<count;++i) {
    require(view.keys[i]==int64_t(500-i) && view.slots[i]==i*3,"key/slot order unchanged");
    require(std::memcmp(view.rows[i],values.data()+i*dimension,dimension*sizeof(float))==0,"float payload bits unchanged");
  }
  if(!expected_codes) {require(view.quantization_step==0,"no-code step disabled");return;}
  double max_abs=0;for(float value:values)max_abs=std::max(max_abs,std::fabs(double(value)));
  if(!view.quantization_scales.present())
    require(double_bits(view.quantization_step)==double_bits(quantization_step(max_abs)),"global step bits unchanged");
  for(size_t i=0;i<count;++i) {
    const auto& scales=view.quantization_scales;
    const double step=scales.present() ? scales.steps[scales.block_ids[i/scales.block_rows]] : view.quantization_step;
    for(size_t d=0;d<dimension;++d) {
      int8_t reference=91,integer=91;
      require(scalar_call(values[i*dimension+d],step,&reference),"old scalar must encode generated index step");
      require(oracle(values[i*dimension+d],step,&integer),"independent index oracle success");
      require(view.quantized[i*dimension+d]==reference && reference==integer,"actual FlatIndex code mismatch");
    }
  }
}
void indexes() {
  for(unsigned distribution=0;distribution<4;++distribution) {
    physical_index(16,3,1u<<20,distribution,true);
    physical_index(128,19,1u<<20,distribution,true);
  }
  physical_index(17,33,1u<<20,0,true);
  physical_index(128,600,16u<<20,0,true);
  // Four of five 256-row blocks have much smaller magnitude. This admits
  // local scales under both the original and selective historical macros.
  physical_index(128,1280,16u<<20,4,true,true);
  physical_index(15,3,1u<<20,0,false);
  physical_index(65536,1,1u<<20,0,true);
  physical_index(65537,1,1u<<20,0,false);
  const size_t metadata=3*(sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t));
  const size_t posting=3*(2*sizeof(int64_t)+sizeof(uint32_t)),codes=3*16;
  physical_index(16,3,metadata+posting+codes-1,0,false);
  physical_index(16,3,metadata+posting+codes,0,true);
  physical_index(128,3,0,0,false);
  physical_index(128,0,4096,0,false);
}
} // namespace

int main() {
  ::alarm(120);
  Environment environment;
  numeric_matrix();rows_and_environment();indexes();
  std::puts("cached_quantizer=PASS");
  std::printf("cached_quantizer_checks=%zu\n",checks);
}

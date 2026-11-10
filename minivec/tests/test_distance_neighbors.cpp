#include "distance.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
using namespace mvdb;
static uint64_t bits(double v){uint64_t x;std::memcpy(&x,&v,8);return x;}
static float from_bits(uint32_t x){float v;std::memcpy(&v,&x,4);return v;}
static uint64_t state=0x83ae4c210f927b65ull;
static uint64_t random_word(){state^=state<<13;state^=state>>7;state^=state<<17;return state;}
static void require(bool ok,const char* why){if(!ok){std::cerr<<"FAIL "<<why<<"\n";std::exit(1);}}
static double old_distance_scalar(const float* a,const float* b,size_t n) {
#if LDBL_MANT_DIG >= 64
  // Extended precision usually determines the correctly rounded double
  // without integer accumulation. Certify its entire error interval; ties
  // and uncertain cases still use the exact finite-float32 calculation.
  if(n<=uint64_t(1)<<32) {
    long double s0=0,s1=0,s2=0,s3=0;
    size_t i=0;
    for(;i+4<=n;i+=4) {
      const long double d0=static_cast<long double>(a[i])-b[i];
      const long double d1=static_cast<long double>(a[i+1])-b[i+1];
      const long double d2=static_cast<long double>(a[i+2])-b[i+2];
      const long double d3=static_cast<long double>(a[i+3])-b[i+3];
      s0+=d0*d0;s1+=d1*d1;s2+=d2*d2;s3+=d3*d3;
    }
    for(;i<n;++i){const long double d=static_cast<long double>(a[i])-b[i];s0+=d*d;}
    const long double sum=(s0+s1)+(s2+s3);
    const double result=static_cast<double>(sum);
    if(sum>0 && std::isfinite(result)) {
      // Each term has three rounding-error factors (subtraction twice,
      // then multiplication), plus at most n/4+3 lane additions and two
      // reduction additions. With n<=2^32 and at least 64 mantissa bits,
      // this deliberately loose bound also covers rounding of the bound
      // itself, and conversion from relative exact to computed-sum error.
      const long double error=sum*(static_cast<long double>(n/4)+8)*
        (2*std::numeric_limits<long double>::epsilon());
      const long double lower=(static_cast<long double>(result)+std::nextafter(result,0.0))*0.5L;
      const long double upper=(static_cast<long double>(result)+
        std::nextafter(result,std::numeric_limits<double>::infinity()))*0.5L;
      if(sum-lower>error && upper-sum>error)return result;
    }
  }
#endif
  return distance_exact(a,b,n);
}

static void check_neighbor(double value){
  const auto n=distance_detail::positive_neighbors(value);
  require(bits(n.lower)==bits(std::nextafter(value,0.0)),"lower neighbor");
  require(bits(n.upper)==bits(std::nextafter(value,std::numeric_limits<double>::infinity())),"upper neighbor");
}
static void tests(){
  for(double value:{0.0,-0.0,std::numeric_limits<double>::denorm_min(),
      std::numeric_limits<double>::min(),1.0,2.0,std::numeric_limits<double>::max()})
    check_neighbor(value);
  for(int exponent=-1074;exponent<=1023;exponent+=17){
    const double value=std::ldexp(1.0,exponent);
    check_neighbor(value);check_neighbor(std::nextafter(value,0.0));
    check_neighbor(std::nextafter(value,std::numeric_limits<double>::infinity()));
  }
  for(size_t i=0;i<50000;++i){
    const uint64_t u=random_word()&0x7fffffffffffffffull;
    if((u>>52)==2047)continue;
    double value;std::memcpy(&value,&u,8);check_neighbor(value);
  }
  const size_t dims[]={0,1,2,3,4,7,31,128,129,511};
  for(size_t trial=0;trial<7200;++trial){
    size_t n=dims[(trial/6)%10];
    std::vector<float> a(n),b(n);
    for(size_t j=0;j<n;++j){
      const uint32_t x=static_cast<uint32_t>(random_word());
      const uint32_t y=static_cast<uint32_t>(random_word());
      switch(trial%6){
        case 0:
          a[j]=from_bits((x&0x807fffff)|((x%255)<<23));
          b[j]=from_bits((y&0x807fffff)|((y%255)<<23));break;
        case 1:
          a[j]=from_bits((x&0x807fffff)|((x%254+1)<<23));
          b[j]=std::nextafter(a[j],0.0f);break;
        case 2:
          a[j]=static_cast<float>(static_cast<int32_t>(x%20001)-10000)/8192;
          b[j]=static_cast<float>(static_cast<int32_t>(y%20001)-10000)/8192;break;
        case 3:
          a[j]=from_bits(x&0x807fffff);b[j]=from_bits(y&0x807fffff);break;
        case 4:
          a[j]=from_bits((x&0x807fffff)|((x%255)<<23));b[j]=a[j];break;
        default:
          a[j]=(j%2)?std::ldexp(1.0f,-140):std::ldexp(1.0f,120);
          b[j]=(j%3)?-a[j]:a[j];break;
      }
    }
    const double exact=distance_exact(a.data(),b.data(),n);
    require(bits(distance_scalar(a.data(),b.data(),n))==bits(exact),"new scalar vs exact");
    require(bits(old_distance_scalar(a.data(),b.data(),n))==bits(exact),"old scalar vs exact");
  }
  // Exactly halfway between binary64 values, then immediately above and below.
  for(int exponent=-100;exponent<=100;exponent+=10){
    std::vector<float> a={std::ldexp(1.0f,exponent),std::ldexp(1.0f,exponent-27),
                         std::ldexp(1.0f,exponent-27),std::ldexp(1.0f,exponent-40)};
    const float zero[4]={};
    for(size_t n:{size_t(2),size_t(3),size_t(4)})
      require(bits(distance_scalar(a.data(),zero,n))==bits(distance_exact(a.data(),zero,n)),"rounding midpoint");
  }
  std::cout<<"PASS neighbors: boundaries + 50000 random; distances: 7200 cases + 63 midpoint cases\n";
}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static double old_call(const float* a,const float* b,size_t n){return old_distance_scalar(a,b,n);}
#if defined(__GNUC__)
__attribute__((noinline))
#endif
static double new_call(const float* a,const float* b,size_t n){return distance_scalar(a,b,n);}
static volatile double checksum=0;
static double timed(double(*fn)(const float*,const float*,size_t),
                    const std::vector<float>& data,const std::vector<float>& query){
  const auto start=std::chrono::steady_clock::now();
  double total=0;
  for(size_t pass=0;pass<32;++pass)
    for(size_t row=0;row<2048;++row)
      total+=fn(data.data()+row*128,query.data(),128);
  checksum=total;
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
static void bench(){
  std::vector<float> data(2048*128),query(128);
  for(auto& v:data)v=static_cast<float>(static_cast<int32_t>(random_word()%20001)-10000)/10000;
  for(auto& v:query)v=static_cast<float>(static_cast<int32_t>(random_word()%20001)-10000)/10000;
  timed(old_call,data,query);timed(new_call,data,query);
  std::vector<double> old_ms,new_ms;
  for(int round=0;round<5;++round){
    double o,n;
    if(round%2){n=timed(new_call,data,query);o=timed(old_call,data,query);}
    else{o=timed(old_call,data,query);n=timed(new_call,data,query);}
    old_ms.push_back(o);new_ms.push_back(n);
    std::cout<<"round="<<round+1<<" old_ms="<<o<<" new_ms="<<n<<"\n";
  }
  std::sort(old_ms.begin(),old_ms.end());std::sort(new_ms.begin(),new_ms.end());
  std::cout<<"median_old_ms="<<old_ms[2]<<" median_new_ms="<<new_ms[2]
           <<" speedup="<<old_ms[2]/new_ms[2]<<" checksum="<<std::setprecision(17)<<checksum<<"\n";
}
int main(int argc,char** argv){
  tests();
  if(argc==2 && std::string(argv[1])=="--bench")bench();
  else if(argc!=1){std::cerr<<"usage: test_distance_neighbors [--bench]\n";return 2;}
}


#include "parser_snapshot.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace short_decimal_test;
static volatile uint64_t sink=0;
struct Timing { double wall,cpu;uint64_t digest; };
static double cpu_time() {
  timespec ts{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&ts))throw std::runtime_error("thread clock");
  return double(ts.tv_sec)+double(ts.tv_nsec)*1e-9;
}
static uint32_t bits(float x){uint32_t b;std::memcpy(&b,&x,4);return b;}
static uint64_t mix(uint64_t h,uint64_t x){return (h^x)*1099511628211ull;}
template<bool Candidate> static uint64_t round(const std::vector<std::string>& bodies,unsigned dimension) {
  uint64_t h=1469598103934665603ull;
  for(const auto& body:bodies) {
    // Fresh output allocation mirrors value_from_string/parse_query_vector.
    std::vector<float> out;
    const bool ok=Candidate?candidate_parse(body,&out):baseline_parse(body,&out);
    if(!ok || out.size()!=dimension)throw std::runtime_error("timed parse failed");
    h=mix(h,bits(out.front()));h=mix(h,bits(out.back()));h=mix(h,out.size());
  }
  return h;
}
template<bool Candidate> static Timing timed(const std::vector<std::string>& bodies,unsigned dimension,unsigned repeats) {
  const double c0=cpu_time();const auto t0=std::chrono::steady_clock::now();
  uint64_t h=1469598103934665603ull;
  for(unsigned i=0;i<repeats;++i)h=mix(h,round<Candidate>(bodies,dimension));
  const auto t1=std::chrono::steady_clock::now();const double c1=cpu_time();
  sink=h;return {std::chrono::duration<double>(t1-t0).count(),c1-c0,h};
}
static double median(std::vector<double> v) {
  std::sort(v.begin(),v.end());return v.size()%2?v[v.size()/2]:(v[v.size()/2-1]+v[v.size()/2])/2;
}
static std::string fixed6(int n) {
  const bool neg=n<0;if(neg)n=-n;
  std::string frac=std::to_string(n%1000000);frac.insert(0,6-frac.size(),'0');
  return (neg?"-":"")+std::to_string(n/1000000)+'.'+frac;
}
struct Fixture {std::string name;unsigned dimension;std::vector<std::string> bodies;};
static Fixture fixture(unsigned kind,unsigned dimension) {
  static const char* names[]={"short_fixed6","mixed_25pct_exponent","all_exponent",
                              "all_17digit_decimal","float32_as_double_repr17","midpoint_fallback"};
  Fixture result{std::string(names[kind])+"_d"+std::to_string(dimension),dimension,{}};
  std::mt19937 rng(7319+kind);
  for(unsigned row=0;row<128;++row) {
    std::string body;
    for(unsigned j=0;j<dimension;++j) {
      if(j)body+=", ";
      const int n=int(rng()%2000001)-1000000;
      if(kind==2 || (kind==1 && j%4==3))body+=std::to_string(n)+"e-6";
      else if(kind==3)body+=fixed6(n)+"1234567890";
      else if(kind==4) {
        const float f=static_cast<float>(n)/1000000.f;
        std::ostringstream text;text<<std::setprecision(17)<<static_cast<double>(f);body+=text.str();
      }
      else if(kind==5)body+=std::to_string(16777217+2*(rng()%10000));
      else body+=fixed6(n);
    }
    result.bodies.push_back(std::move(body));
  }
  return result;
}
int main(int argc,char** argv)try {
  unsigned pairs=8;double target_ms=25;
  for(int i=1;i<argc;++i) {
    const std::string arg=argv[i];
    if(arg=="--pairs" && i+1<argc)pairs=unsigned(std::stoul(argv[++i]));
    else if(arg=="--target-ms" && i+1<argc)target_ms=std::stod(argv[++i]);
    else throw std::runtime_error("usage: bench-short-decimal [--pairs 8] [--target-ms 25]");
  }
  if(pairs<2 || pairs>100 || !(target_ms>=1 && target_ms<=1000))throw std::runtime_error("bounds");
  if(!short_decimal::Context::supported())throw std::runtime_error("fast environment unavailable");
  std::cout<<std::setprecision(12);
  std::cout<<"{\"type\":\"scope\",\"scope\":\"single-thread parser-only; fresh vector allocation; unchanged fallback; not DB score\",\"pairs\":"
           <<pairs<<",\"target_ms\":"<<target_ms<<",\"short_screen\":"<<(pairs<8?"true":"false")<<"}\n";
  for(unsigned dimension:{1u,16u,128u,768u})for(unsigned kind=0;kind<6;++kind) {
    const auto f=fixture(kind,dimension);Counters counts;
    for(const auto& s:f.bodies) {
      std::vector<float> a,b;
      if(!baseline_parse(s,&a) || !candidate_parse<true>(s,&b,&counts) || a.size()!=b.size())
        throw std::runtime_error("untimed parser mismatch");
      for(size_t i=0;i<a.size();++i)if(bits(a[i])!=bits(b[i]))throw std::runtime_error("untimed bits mismatch");
    }
    std::cout<<"{\"type\":\"untimed_counts\",\"fixture\":\""<<f.name
      <<"\",\"vectors\":128,\"dimension\":"<<dimension<<",\"accepted\":"<<counts.accepted
      <<",\"midpoint_fallback\":"<<counts.midpoint<<",\"syntax_fallback\":"<<counts.syntax
      <<",\"bypassed\":"<<counts.bypassed<<"}\n";
    std::vector<double> old_pilot,new_pilot;
    for(unsigned i=0;i<3;++i) {
      Timing a,b;if(i%2){b=timed<true>(f.bodies,dimension,1);a=timed<false>(f.bodies,dimension,1);}
      else {a=timed<false>(f.bodies,dimension,1);b=timed<true>(f.bodies,dimension,1);}
      if(a.digest!=b.digest)throw std::runtime_error("pilot digest");
      old_pilot.push_back(a.wall);new_pilot.push_back(b.wall);
    }
    const double short_pilot=std::min(median(old_pilot),median(new_pilot));
    const unsigned repeats=std::max(1u,std::min(4096u,unsigned(std::ceil(target_ms*.001/short_pilot))));
    std::cout<<"{\"type\":\"calibration\",\"fixture\":\""<<f.name<<"\",\"repeats\":"<<repeats<<"}\n";
    std::vector<double> walls,cpus;
    for(unsigned i=0;i<pairs;++i) {
      Timing a,b;if(i%2){b=timed<true>(f.bodies,dimension,repeats);a=timed<false>(f.bodies,dimension,repeats);}
      else {a=timed<false>(f.bodies,dimension,repeats);b=timed<true>(f.bodies,dimension,repeats);}
      if(a.digest!=b.digest)throw std::runtime_error("timed digest");
      walls.push_back(a.wall/b.wall);cpus.push_back(a.cpu/b.cpu);
      std::cout<<"{\"type\":\"raw_pair\",\"fixture\":\""<<f.name<<"\",\"pair\":"<<i
        <<",\"order\":\""<<(i%2?"BA":"AB")<<"\",\"baseline_wall\":"<<a.wall
        <<",\"candidate_wall\":"<<b.wall<<",\"baseline_cpu\":"<<a.cpu<<",\"candidate_cpu\":"<<b.cpu
        <<",\"baseline_over_candidate_wall\":"<<walls.back()<<",\"baseline_over_candidate_cpu\":"<<cpus.back()<<"}\n";
    }
    std::cout<<"{\"type\":\"summary\",\"fixture\":\""<<f.name
      <<"\",\"median_baseline_over_candidate_wall\":"<<median(walls)
      <<",\"median_baseline_over_candidate_cpu\":"<<median(cpus)<<"}\n";
  }
  std::cout<<"{\"type\":\"complete\",\"status\":\"PASS\"}\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}

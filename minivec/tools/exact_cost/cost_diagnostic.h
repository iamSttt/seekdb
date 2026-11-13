#pragma once
#ifndef MVDB_COST_DIAGNOSTIC
#error This header is only for an explicitly instrumented diagnostic executable
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>

// Deliberately separate from production. Main-thread-only counters; the runner
// and this constructor both require the existing parser/query one-thread knobs.
// Timers are inclusive and nested. Never sum their times as exclusive CPU time.
namespace mvdb_cost {
enum Kind { Scalar, LongDouble, Integer, QuantBound, FloatBound, DoubleBound, Parse, Crc, Kinds };
inline constexpr const char* names[Kinds]={"scalar_distance","long_double_certificate","integer_exact",
  "int8_bound","float_bound_context","double_bound","sql_parse","crc_update"};
enum Event { Certified, ScalarFallback, ExactNonfinite, QuantInvalid, QuantCapUnusable,
  QuantKernel, QuantReject, FloatInvalid, FloatKernel, FloatReject, DoubleReject, Events };
inline constexpr const char* event_names[Events]={"long_double_certified","scalar_to_integer",
  "integer_nonfinite","int8_invalid_or_disabled","int8_cap_unusable","int8_kernel_calls",
  "int8_rejected","float_invalid_or_disabled","float_kernel_calls","float_rejected","double_rejected"};
struct Metric { uint64_t calls=0, units=0, samples=0, sample_cpu_ns=0; };
struct Data { std::array<Metric,Kinds> metrics{}; std::array<uint64_t,Events> events{}; };
inline thread_local Data* current=nullptr;
inline thread_local std::array<uint64_t,Kinds> serial{};
inline uint64_t sequence=0;
inline uint64_t now() {
  timespec t{};
  if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)!=0)std::abort();
  return uint64_t(t.tv_sec)*1000000000ULL+uint64_t(t.tv_nsec);
}
// Scalar is a call counter only, avoiding a synchronized outer timer around
// the long-double certificate timer on the same invocation.
inline unsigned stride(Kind k) { return k==Scalar?0u:(k==Parse?1u:(k==Crc?16u:256u)); }
inline void event(Event k) { if(current)++current->events[k]; }
class Call {
 public:
  explicit Call(Kind k,uint64_t units=0):metric_(current?&current->metrics[k]:nullptr) {
    if(!metric_)return;
    ++metric_->calls;metric_->units+=units;
    const unsigned interval=stride(k);
    if(interval && (++serial[k])%interval==0){++metric_->samples;start_=now();}
  }
  ~Call(){if(start_)metric_->sample_cpu_ns+=now()-start_;}
 private: Metric* metric_;uint64_t start_=0;
};
class Request {
 public:
  Request():previous_(current),sequence_(++sequence) {
    for(const char* key:{"MVDB_QUERY_THREADS","MVDB_PARSE_THREADS"}) {
      const char* v=std::getenv(key);
      if(!v || std::strcmp(v,"1")) {std::fputs("cost diagnostic requires both thread knobs=1\n",stderr);std::abort();}
    }
    current=&data_;start_=now();
  }
  ~Request(){
    const uint64_t elapsed=now()-start_;current=previous_;
    std::fprintf(stderr,"MVDB_COST_DIAG {\"sequence\":%llu,\"thread_cpu_ns\":%llu,\"metrics\":{",ull(sequence_),ull(elapsed));
    for(unsigned i=0;i<Kinds;++i){const auto& m=data_.metrics[i];
      std::fprintf(stderr,"%s\"%s\":{\"calls\":%llu,\"units\":%llu,\"samples\":%llu,\"sample_cpu_ns\":%llu,\"sample_stride\":%u}",
        i?",":"",names[i],ull(m.calls),ull(m.units),ull(m.samples),ull(m.sample_cpu_ns),stride(static_cast<Kind>(i)));}
    std::fputs("},\"events\":{",stderr);
    for(unsigned i=0;i<Events;++i)std::fprintf(stderr,"%s\"%s\":%llu",i?",":"",event_names[i],ull(data_.events[i]));
    std::fputs("}}\n",stderr);
  }
 private:
  static unsigned long long ull(uint64_t v){return static_cast<unsigned long long>(v);}
  Data* previous_;uint64_t sequence_,start_;Data data_;
};
}

#include "comparison.h"
#include "micro_data.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/resource.h>
#include <utility>

static uint64_t clock_ns(clockid_t kind) {
  timespec t{}; if(clock_gettime(kind,&t))std::abort();
  return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;
}
static uint64_t work(const micro::Data& d,bool newer,size_t repetitions) {
  auto fn=newer?comparison::new_distance:comparison::old_distance;
  uint64_t sum=0;
  for(size_t r=0;r<repetitions;++r)for(size_t i=0;i<d.count;++i)
    sum+=comparison::bits(fn(d.a.data()+i*d.dimension,d.b.data()+i*d.dimension,d.dimension));
  return sum;
}
struct Measurement { uint64_t wall,cpu,checksum; long minor,major,voluntary,involuntary; };
static Measurement measure(const micro::Data& d,bool newer) {
  rusage before{},after{};if(getrusage(RUSAGE_THREAD,&before))std::abort();
  const uint64_t wall=clock_ns(CLOCK_MONOTONIC_RAW),cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
  const uint64_t checksum=work(d,newer,64);
  const uint64_t cpu_end=clock_ns(CLOCK_THREAD_CPUTIME_ID),wall_end=clock_ns(CLOCK_MONOTONIC_RAW);
  if(getrusage(RUSAGE_THREAD,&after))std::abort();
  return {wall_end-wall,cpu_end-cpu,checksum,after.ru_minflt-before.ru_minflt,
          after.ru_majflt-before.ru_majflt,after.ru_nvcsw-before.ru_nvcsw,after.ru_nivcsw-before.ru_nivcsw};
}
int main(int argc,char** argv) {
  if(argc!=3)return 2;
  if(!mvdb::compensated_draft::available() || !mvdb::compensated_draft::environment_admitted()) {
    std::puts("{\"status\":\"unsupported\"}");return 77;
  }
  const std::string name=argv[1],order=argv[2];if(order!="AB" && order!="BA")return 2;
  const auto d=micro::data(name);
  // Untimed per-row identity check is stronger than a timed aggregate checksum.
  for(size_t i=0;i<d.count;++i) {
    const float* a=d.a.data()+i*d.dimension;const float* b=d.b.data()+i*d.dimension;
    const uint64_t exact=comparison::bits(mvdb::distance_exact(a,b,d.dimension));
    if(comparison::bits(comparison::old_distance(a,b,d.dimension))!=exact ||
       comparison::bits(comparison::new_distance(a,b,d.dimension))!=exact)return 3;
  }
  if(work(d,false,1)!=work(d,true,1))return 4;
  Measurement old{},candidate{};
  if(order=="AB") {old=measure(d,false);candidate=measure(d,true);}
  else {candidate=measure(d,true);old=measure(d,false);}
  if(old.checksum!=candidate.checksum)return 5;
  std::printf("{\"scenario\":\"%s\",\"order\":\"%s\",\"dimension\":%zu,\"rows\":%zu,\"repetitions\":64,\"dataset_fnv64\":\"%016llx\",\"counter_instrumented\":false,\"whole_call\":true,\"untimed_row_oracle_checks\":%zu,\"measurements\":[",name.c_str(),order.c_str(),d.dimension,d.count,(unsigned long long)d.hash,d.count);
  unsigned i=0;for(auto entry:{std::make_pair("old",old),std::make_pair("candidate",candidate)}) {
    const auto& m=entry.second;
    std::printf("%s{\"role\":\"%s\",\"wall_ns\":%llu,\"thread_cpu_ns\":%llu,\"checksum\":\"%016llx\",\"minor_faults\":%ld,\"major_faults\":%ld,\"voluntary_switches\":%ld,\"involuntary_switches\":%ld}",i++?",":"",entry.first,(unsigned long long)m.wall,(unsigned long long)m.cpu,(unsigned long long)m.checksum,m.minor,m.major,m.voluntary,m.involuntary);
  }
  std::puts("]}");
}

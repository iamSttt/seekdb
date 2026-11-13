#include "shadow_comparison.h"
#include "micro_data.h"
#include <cstdio>
static void reset(){
  retained_mvdb::distance_detail::diagnostic_calls=retained_mvdb::distance_detail::diagnostic_old_cert=retained_mvdb::distance_detail::diagnostic_tight_rescue=retained_mvdb::distance_detail::diagnostic_integer=0;
  mvdb::distance_detail::diagnostic_calls=mvdb::distance_detail::diagnostic_old_cert=mvdb::distance_detail::diagnostic_tight_rescue=mvdb::distance_detail::diagnostic_integer=0;
}
static void emit(const char* label,size_t expected){
  namespace old=retained_mvdb::distance_detail;namespace now=mvdb::distance_detail;
  if(old::diagnostic_calls!=expected || now::diagnostic_calls!=expected ||
     old::diagnostic_old_cert+old::diagnostic_integer!=expected || old::diagnostic_tight_rescue!=0 ||
     now::diagnostic_old_cert+now::diagnostic_tight_rescue+now::diagnostic_integer!=expected ||
     old::diagnostic_old_cert!=now::diagnostic_old_cert ||
     old::diagnostic_integer!=now::diagnostic_tight_rescue+now::diagnostic_integer)std::abort();
  std::printf("{\"scenario\":\"%s\",\"scope\":\"counter-only shadow; not timed\",\"old\":{\"calls\":%llu,\"old_cert\":%llu,\"tight_rescue\":%llu,\"integer\":%llu},\"candidate\":{\"calls\":%llu,\"old_cert\":%llu,\"tight_rescue\":%llu,\"integer\":%llu}}\n",label,
    (unsigned long long)old::diagnostic_calls,(unsigned long long)old::diagnostic_old_cert,(unsigned long long)old::diagnostic_tight_rescue,(unsigned long long)old::diagnostic_integer,
    (unsigned long long)now::diagnostic_calls,(unsigned long long)now::diagnostic_old_cert,(unsigned long long)now::diagnostic_tight_rescue,(unsigned long long)now::diagnostic_integer);
}
int main(){
  if(!mvdb::distance_detail::tighter_long_double_environment())return 77;
  for(const char* name:{"fullmantissa128","fullmantissa512","grid128"}){
    reset();const auto data=micro::data(name);
    for(size_t i=0;i<data.count;++i)if(comparison::bits(comparison::old_distance(data.a.data()+i*data.dimension,data.b.data()+i*data.dimension,data.dimension))!=
                                  comparison::bits(comparison::new_distance(data.a.data()+i*data.dimension,data.b.data()+i*data.dimension,data.dimension)))return 3;
    emit(name,data.count);
    if(std::string(name)=="grid128" && retained_mvdb::distance_detail::diagnostic_integer)return 4;
  }
  reset();std::vector<float>a(128,0),b(128,0);a[0]=1;a[1]=a[2]=0x1p-27f;a[3]=a[4]=0x1p-29f;
  if(comparison::bits(comparison::old_distance(a.data(),b.data(),a.size()))!=comparison::bits(comparison::new_distance(a.data(),b.data(),a.size())))return 5;
  emit("analytic_midpoint_rescue_not_timed",1);
  if(retained_mvdb::distance_detail::diagnostic_integer!=1 || mvdb::distance_detail::diagnostic_tight_rescue!=1)return 6;
}

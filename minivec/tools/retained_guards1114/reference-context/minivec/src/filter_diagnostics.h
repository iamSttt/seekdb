#pragma once
// Diagnostic-only counters. They are deliberately compiled out of timing
// binaries. Run with MVDB_QUERY_THREADS=1: no cross-worker aggregation exists.
#ifdef MVDB_FILTER_DIAGNOSTICS
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
namespace mvdb {
struct FilterDiagnostics {
  uint64_t scan_calls=0,row_fetches=0,payload_fetches=0,payload_bytes=0;
  uint64_t vector_visits=0,exact_distances=0,sample_rows=0;
  uint64_t vector_read_requested_bytes=0,vector_pread_calls=0,vector_pread_returned_bytes=0,vector_records=0;
  uint64_t payload_pread_calls=0,payload_pread_returned_bytes=0;
  std::array<uint64_t,16> predicate_checks{},posting_lookups{},posting_hits{};
  std::array<uint64_t,16> posting_build_attempts{},posting_builds{},posting_evictions{};
  std::array<uint64_t,16> plan_choices{},estimate_cache_hits{};
};
inline thread_local FilterDiagnostics* filter_diag_current=nullptr;
inline uint64_t filter_diag_sequence=0;
class FilterDiagnosticScope {
 public:
  explicit FilterDiagnosticScope(bool active,size_t conditions,int statement_kind)
      :active_(active),conditions_(conditions),statement_kind_(statement_kind),previous_(filter_diag_current) {
    if(active_) {sequence_=++filter_diag_sequence;filter_diag_current=&data_;}
  }
  ~FilterDiagnosticScope() {
    if(!active_)return;
    filter_diag_current=previous_;
    std::fprintf(stderr,"MVDB_FILTER_DIAG {\"sequence\":%llu,\"conditions\":%zu,\"statement_kind\":%d,"
      "\"scan_calls\":%llu,\"row_fetches\":%llu,\"payload_fetches\":%llu,\"payload_bytes\":%llu,"
      "\"vector_visits\":%llu,\"exact_distances\":%llu,\"sample_rows\":%llu,"
      "\"vector_read_requested_bytes\":%llu,\"vector_pread_calls\":%llu,"
      "\"vector_pread_returned_bytes\":%llu,\"vector_records\":%llu,"
      "\"payload_pread_calls\":%llu,\"payload_pread_returned_bytes\":%llu",
      ull(sequence_),conditions_,statement_kind_,ull(data_.scan_calls),ull(data_.row_fetches),
      ull(data_.payload_fetches),ull(data_.payload_bytes),ull(data_.vector_visits),
      ull(data_.exact_distances),ull(data_.sample_rows),
      ull(data_.vector_read_requested_bytes),ull(data_.vector_pread_calls),
      ull(data_.vector_pread_returned_bytes),ull(data_.vector_records),
      ull(data_.payload_pread_calls),ull(data_.payload_pread_returned_bytes));
    emit("predicate_checks",data_.predicate_checks);
    emit("posting_lookups",data_.posting_lookups);emit("posting_hits",data_.posting_hits);
    emit("posting_build_attempts",data_.posting_build_attempts);emit("posting_builds",data_.posting_builds);
    emit("posting_evictions",data_.posting_evictions);emit("plan_choices",data_.plan_choices);
    emit("estimate_cache_hits",data_.estimate_cache_hits);
    std::fputs("}\n",stderr);
  }
 private:
  static unsigned long long ull(uint64_t n) {return static_cast<unsigned long long>(n);}
  static void emit(const char* name,const std::array<uint64_t,16>& counts) {
    std::fprintf(stderr,",\"%s\":[",name);
    for(size_t i=0;i<counts.size();++i)std::fprintf(stderr,"%s%llu",i?",":"",ull(counts[i]));
    std::fputc(']',stderr);
  }
  bool active_;size_t conditions_;int statement_kind_;uint64_t sequence_=0;
  FilterDiagnostics* previous_;FilterDiagnostics data_;
};
}
#define MVDB_FILTER_COUNT(field) do {if(mvdb::filter_diag_current)++mvdb::filter_diag_current->field;}while(false)
#define MVDB_FILTER_BYTES(field,n) do {if(mvdb::filter_diag_current)mvdb::filter_diag_current->field+=(n);}while(false)
#define MVDB_FILTER_COLUMN(field,column) do {if(mvdb::filter_diag_current) {const size_t fd_col=(column);++mvdb::filter_diag_current->field[fd_col<16?fd_col:15];}}while(false)
#else
#define MVDB_FILTER_COUNT(field) ((void)0)
#define MVDB_FILTER_BYTES(field,n) ((void)0)
#define MVDB_FILTER_COLUMN(field,column) ((void)0)
#endif

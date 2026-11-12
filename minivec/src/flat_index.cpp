#include "flat_index.h"
#include "distance.h"
#include "quantized_bound.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <unistd.h>

namespace mvdb {
namespace {
// Approximate-search width, as a *fraction* of the table rather than a number of
// partitions: the partition count already scales with the row count, so a fixed
// fraction keeps one recall band as the table grows and does not depend on the
// dimension. 100 means "no probe", i.e. the exact scan, and is the default.
//
// The default is the exact scan because the official workload is i.i.d. Gaussian,
// which has no cluster structure: the measured recall/width curve needs >=60% of
// the table scanned to hold Recall@10 at 0.97, and at that width the scattered
// candidate gather costs more than the rows it skips (see the ann report). An
// operator who knows the data is clustered opts in with the two variables here;
// MVDB_ANN_OFF always forces the exact scan regardless of the width.
size_t ann_probe_percent() {
  static const size_t percent=[] {
    const char* s=std::getenv("MVDB_ANN_PROBE_PERCENT");
    if(!s||!*s)return size_t(100);
    const long v=std::strtol(s,nullptr,10);
    return v>0&&v<100 ? static_cast<size_t>(v) : size_t(100);
  }();
  return percent;
}
bool ann_disabled() {
  static const bool off=[] {
    const char* s=std::getenv("MVDB_ANN_OFF");
    return s&&*s&&*s!='0';
  }();
  return off;
}
size_t ann_default_probes(size_t clusters) {
  size_t probes=(clusters*ann_probe_percent()+99)/100;
  if(probes<1)probes=1;
  if(probes>clusters)probes=clusters;
  return probes;
}
void transfer(int fd,void* data,size_t bytes,uint64_t offset,bool write) {
  char* p=static_cast<char*>(data);
  while(bytes) {
    const ssize_t n=write ? ::pwrite(fd,p,bytes,static_cast<off_t>(offset))
                          : ::pread(fd,p,bytes,static_cast<off_t>(offset));
    if(n<0 && errno==EINTR)continue;
    if(n<=0)throw std::runtime_error("flat vector index I/O failed");
    bytes-=static_cast<size_t>(n);p+=n;offset+=static_cast<uint64_t>(n);
  }
}
}
FlatIndex::FlatIndex(const std::string& dir,size_t dimension,size_t count,size_t budget)
    :dimension_(dimension),expected_(count) {
  if(!dimension || dimension>(std::numeric_limits<size_t>::max()-16)/sizeof(float))
    throw std::runtime_error("invalid flat index dimension");
  const size_t stride=dimension*sizeof(float)+16;
  if(count>static_cast<uint64_t>(std::numeric_limits<int64_t>::max())/stride)
    throw std::runtime_error("flat index exceeds file offset range");
  // Resident vectors are borrowed: only these three arrays belong to the
  // index. The float payload remains charged to the table's payload budget.
  constexpr size_t metadata_stride=sizeof(const float*)+sizeof(int64_t)+sizeof(uint64_t);
  bool resident=count<=budget/metadata_stride;
  size_t available=budget;
  if(resident) {
    rows_.reserve(count);
    keys_.reserve(count);slots_.reserve(count);
    // Account capacities, not sizes. A standard-library implementation may
    // reserve more than requested; fall back without retaining those arrays.
    const auto charge=[&](size_t capacity,size_t width) {
      if(capacity>available/width)return false;
      available-=capacity*width;return true;
    };
    resident=charge(rows_.capacity(),sizeof(const float*)) &&
             charge(keys_.capacity(),sizeof(int64_t)) &&
             charge(slots_.capacity(),sizeof(uint64_t));
    if(!resident) {
      std::vector<const float*>().swap(rows_);
      std::vector<int64_t>().swap(keys_);
      std::vector<uint64_t>().swap(slots_);
    }
  }
  if(resident) {
    // Reserve accounting room up front so lazy construction cannot exceed the
    // combined resident-index budget, including the sorted-value scratch array.
    // Ordinals are uint32_t; larger indexes retain the sequential scan path.
    constexpr size_t entry=2*sizeof(int64_t)+sizeof(uint32_t);
    // Preserve the former posting row coverage (1 MiB / 16-byte entry). Its
    // additional construction scratch is charged to the unchanged global
    // budget instead of reducing cache eligibility for 52k--65k-row tables.
    constexpr size_t posting_rows=(1u<<20)/16;
    if(count<=std::numeric_limits<uint32_t>::max() &&
       count<=posting_rows && count<=available/entry)
      posting_budget_=count*entry;
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
    // Reserve accounting room before finish() allocates the optional codes.
    // Metadata admission no longer bounds count*dimension: divide first.
    if(dimension>=16 && dimension<=65536 &&
       count<=std::min<size_t>(4u<<20,available-posting_budget_)/dimension)
      quantized_budget_=count*dimension;
#endif
    // Room for the coarse partition, capped so one index never claims more than
    // 2 MiB for it whatever the budget says.
    const size_t left=available-posting_budget_-quantized_budget_;
    ann_budget_=std::min<size_t>(2u<<20,left);
#ifndef MVDB_DISABLE_QUANTIZED_DOT
    // Never displace postings, codes, or the existing ANN reservation. The
    // optional exact dot kernel gets only the remainder of the same budget.
    if(quantized_budget_ && dimension>=32 && mvdb::quantized_dot_available() &&
       count<=(left-ann_budget_)/sizeof(uint32_t))
      quantized_norm_budget_=count*sizeof(uint32_t);
#endif
  } else {
    std::string path=dir+"/.flat-XXXXXX";
    std::vector<char> name(path.begin(),path.end());name.push_back(0);
    fd_=::mkstemp(name.data());
    if(fd_<0)throw std::runtime_error("cannot create flat vector index file");
    ::unlink(name.data());
  }
}
FlatIndex::~FlatIndex(){if(fd_>=0)::close(fd_);}
void FlatIndex::add(int64_t pk,uint64_t slot,const float* values,size_t dimension) {
  if(ready_ || build_failed_ || count_>=expected_ || dimension!=dimension_ || !values)
    throw std::runtime_error("invalid flat index build row");
  if(fd_<0) {
    // Borrow, never copy: the table's row storage owns this payload and rebuilds
    // the index whenever that storage changes.
    keys_.push_back(pk);slots_.push_back(slot);rows_.push_back(values);
  } else {
    // Float-aligned storage: the first four float-sized words contain raw key
    // metadata and are accessed only via memcpy, never as floating point values.
    const size_t words=dimension_+4,stride=words*sizeof(float);
    if(build_block_.empty()) {
      const size_t records=std::min(expected_,std::max<size_t>(1,(256u<<10)/stride));
      build_block_.resize(records*words);
    }
    float* record=build_block_.data()+build_used_;
    std::memcpy(record,&pk,sizeof(pk));
    std::memcpy(record+2,&slot,sizeof(slot));
    std::memcpy(record+4,values,dimension_*sizeof(float));
    build_used_+=words;
    if(build_used_==build_block_.size())flush_build_block();
  }
  ++count_;
}
void FlatIndex::flush_build_block() {
  if(!build_used_)return;
  const size_t bytes=build_used_*sizeof(float);
  try {
    transfer(fd_,build_block_.data(),bytes,build_offset_,true);
  } catch(...) {
    // A partial write may already have reached the file. Never expose or reuse
    // this failed build; its unlinked file is removed by the destructor.
    build_failed_=true;throw;
  }
  build_offset_+=bytes;build_used_=0;
}
void FlatIndex::finish() {
  if(build_failed_)throw std::runtime_error("flat vector index build failed");
  if(count_!=expected_)throw std::runtime_error("incomplete flat vector index build");
  if(ready_)return;
  if(fd_>=0) {
    flush_build_block();
    std::vector<float>().swap(build_block_);
  }
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  if(quantized_budget_) {
    double max_abs=0;
    bool ok=true;
    for(size_t i=0;i<count_;++i) {
      const float* row=row_at(i);
      for(size_t d=0;d<dimension_;++d) {
        const double magnitude=std::fabs(static_cast<double>(row[d]));
        if(!std::isfinite(magnitude)) {max_abs=magnitude;ok=false;break;}
        max_abs=std::max(max_abs,magnitude);
      }
      if(!ok)break;
    }
    const double step=mvdb::quantization_step(max_abs);
    if(step>0 && std::isfinite(step)) {
      quantized_.reset(new(std::nothrow) int8_t[quantized_budget_]);
      if(quantized_) {
        quantization_step_=step;
#ifndef MVDB_DISABLE_QUANTIZED_DOT
        if(quantized_norm_budget_) {
          quantized_norms_.reset(new(std::nothrow) uint32_t[count_]);
          if(!quantized_norms_)quantized_norm_budget_=0;
        }
#endif
        // Do not reorder: codes have exactly the rows_/keys_ traversal order.
        size_t code=0;
        for(size_t i=0;i<count_ && quantization_step_;++i) {
          const float* row=row_at(i);
#ifndef MVDB_DISABLE_QUANTIZED_DOT
          uint32_t norm=0;
#endif
          for(size_t d=0;d<dimension_;++d) {
            if(!mvdb::quantize_coordinate(row[d],step,&quantized_[code++])) {
              quantization_step_=0;break;
            }
#ifndef MVDB_DISABLE_QUANTIZED_DOT
            if(quantized_norms_) {
              const int value=quantized_[code-1];norm+=static_cast<uint32_t>(value*value);
            }
#endif
          }
#ifndef MVDB_DISABLE_QUANTIZED_DOT
          if(quantized_norms_)quantized_norms_[i]=norm;
#endif
        }
      }
    }
    if(!quantization_step_) {
      quantized_.reset();quantized_budget_=0;
      quantized_norms_.reset();quantized_norm_budget_=0;
    }
  }
#endif
  build_partition();
  ready_=true;
}

// One k-means level over the borrowed float32 payloads. Membership is refined
// twice from an evenly strided seed set: enough to make the partitions coherent
// without a build cost that a rebuild-on-write table cannot pay repeatedly.
void FlatIndex::build_partition() {
  // CREATE VECTOR INDEX builds the physical index eagerly, so the partition is
  // only built when an operator has opted into approximate search: the default
  // configuration must not pay for a structure the exact scan does not use.
  if(ann_probe_percent()>=100)return;
  if(fd_>=0 || count_<512 || count_>std::numeric_limits<uint32_t>::max() ||
     dimension_<8 || dimension_>4096)return;
  size_t clusters=8;
  while(clusters<256 && clusters*clusters<count_)clusters*=2;
  if(count_<clusters*8)return;
  const size_t floats=clusters*dimension_;
  // Bound both training and grouping peaks, including temporary arrays.
  // Divide before multiplying count so very large metadata-only indexes are safe.
  if(count_>ann_budget_/(2*sizeof(uint32_t)))return;
  const size_t training=floats*(sizeof(float)+sizeof(double))+
                        count_*sizeof(uint32_t)+clusters*sizeof(uint32_t);
  const size_t grouping=floats*sizeof(float)+count_*(2*sizeof(uint32_t))+
                        (2*clusters+1)*sizeof(uint32_t);
  if(std::max(training,grouping)>ann_budget_)return;
  std::vector<float> centroids(floats);
  for(size_t c=0;c<clusters;++c) {
    const size_t src=(c*count_)/clusters;
    std::copy(row_at(src),row_at(src)+dimension_,
              centroids.begin()+static_cast<std::ptrdiff_t>(c*dimension_));
  }
  std::vector<uint32_t> assignment(count_,0);
  std::vector<double> sums;
  std::vector<uint32_t> sizes;
  for(int iteration=0;iteration<2;++iteration) {
    for(size_t i=0;i<count_;++i) {
      const float* row=row_at(i);
      size_t best=0;
      double best_distance=distance_squared(row,centroids.data(),dimension_);
      for(size_t c=1;c<clusters;++c) {
        const double d=distance_squared(row,centroids.data()+c*dimension_,dimension_);
        if(d<best_distance) {best_distance=d;best=c;}
      }
      assignment[i]=static_cast<uint32_t>(best);
    }
    if(iteration)break;
    sums.assign(clusters*dimension_,0.0);
    sizes.assign(clusters,0u);
    for(size_t i=0;i<count_;++i) {
      const float* row=row_at(i);
      double* sum=sums.data()+static_cast<size_t>(assignment[i])*dimension_;
      for(size_t d=0;d<dimension_;++d)sum[d]+=row[d];
      ++sizes[assignment[i]];
    }
  }
  std::vector<double>().swap(sums);
  std::vector<uint32_t>().swap(sizes);
  // Counting sort: members of one partition become contiguous, so a probe is a
  // few block copies.
  std::vector<uint32_t> offsets(clusters+1,0);
  for(size_t i=0;i<count_;++i)++offsets[assignment[i]+1];
  for(size_t c=0;c<clusters;++c)offsets[c+1]+=offsets[c];
  std::vector<uint32_t> members(count_);
  std::vector<uint32_t> cursor(offsets.begin(),offsets.end()-1);
  for(size_t i=0;i<count_;++i)members[cursor[assignment[i]]++]=static_cast<uint32_t>(i);
  ann_centroids_.swap(centroids);
  ann_offsets_.swap(offsets);
  ann_members_.swap(members);
}

bool FlatIndex::ann_probe(const float* query,size_t probes,
    std::vector<uint32_t>* buffered,const uint32_t** ordinals,size_t* count) const {
  if(!query||!buffered||!ordinals||!count)return false;
  if(ann_disabled())return false;
  const size_t clusters=ann_clusters();
  if(clusters<2||count_==0)return false;
  size_t width=probes ? probes : ann_default_probes(clusters);
  if(width<1)width=1;
  // A probe that would cover every partition cannot beat the exact scan.
  if(width>=clusters)return false;
  std::vector<std::pair<double,uint32_t>> ranked;
  ranked.reserve(clusters);
  for(size_t c=0;c<clusters;++c)
    ranked.emplace_back(distance_squared(query,ann_centroids_.data()+c*dimension_,dimension_),
                        static_cast<uint32_t>(c));
  std::partial_sort(ranked.begin(),ranked.begin()+static_cast<std::ptrdiff_t>(width),ranked.end());
  buffered->clear();
  for(size_t j=0;j<width;++j) {
    const size_t c=ranked[j].second;
    buffered->insert(buffered->end(),ann_members_.begin()+ann_offsets_[c],
                     ann_members_.begin()+ann_offsets_[c+1]);
  }
  if(buffered->empty())return false;
  *ordinals=buffered->data();
  *count=buffered->size();
  return true;
}
bool FlatIndex::resident_view(ResidentView* view) const {
  if(!view)return false;
  *view=ResidentView{};
  if(!ready_ || fd_>=0)return false;
  view->rows=rows_.data();view->keys=keys_.data();view->slots=slots_.data();
  view->dimension=dimension_;view->count=count_;
  view->quantized=quantized_.get();view->quantization_step=quantization_step_;
  view->quantized_norms=quantized_norms_.get();
  return true;
}
size_t FlatIndex::resident_bytes() const {
  return rows_.capacity()*sizeof(const float*)+keys_.capacity()*sizeof(int64_t)+
         slots_.capacity()*sizeof(uint64_t)+posting_budget_+quantized_budget_+quantized_norm_budget_+
         ann_centroids_.capacity()*sizeof(float)+ann_members_.capacity()*sizeof(uint32_t)+
         ann_offsets_.capacity()*sizeof(uint32_t);
}
bool FlatIndex::equal_ordinals(size_t column,int64_t value,
    const std::function<int64_t(uint64_t)>& read_value,
    const uint32_t** ordinals,size_t* count) {
  if(!ready_)throw std::runtime_error("flat vector index is not ready");
  if(!ordinals||!count)return false;
  if(fd_>=0 || (count_ && !posting_budget_))return false;
  if(!postings_ready_ || posting_column_!=column) {
    // Mark invalid first: an exception in read_value must not leave an older
    // column's ready flag attached to partially replaced entries.
    postings_ready_=false;
    posting_values_.assign(count_,0);
    posting_ordinals_.resize(count_);
    for(size_t i=0;i<count_;++i) {
      posting_values_[i]=read_value(slots_[i]);
      posting_ordinals_[i]=static_cast<uint32_t>(i);
    }
    // Sort ordinals by value, ties in index order, so one equal-value run is a
    // contiguous span and the candidate order stays reproducible.
    std::sort(posting_ordinals_.begin(),posting_ordinals_.end(),[&](uint32_t a,uint32_t b) {
      return posting_values_[a]<posting_values_[b] ||
             (posting_values_[a]==posting_values_[b] && a<b);
    });
    std::vector<int64_t> sorted(count_);
    for(size_t i=0;i<count_;++i)sorted[i]=posting_values_[posting_ordinals_[i]];
    posting_values_.swap(sorted);
    posting_column_=column;postings_ready_=true;
  }
  const auto begin=posting_values_.begin();
  const auto first=std::lower_bound(begin,posting_values_.end(),value);
  const auto last=std::upper_bound(first,posting_values_.end(),value);
  const size_t offset=static_cast<size_t>(first-begin), length=static_cast<size_t>(last-first);
  *ordinals=posting_ordinals_.data()+offset;
  *count=length;
  return true;
}

bool FlatIndex::scan_integer_equal(size_t column,int64_t value,
    const std::function<int64_t(uint64_t)>& read_value,const Visitor& visitor) {
  const uint32_t* ordinals=nullptr;
  size_t count=0;
  if(!equal_ordinals(column,value,read_value,&ordinals,&count))return false;
  for(size_t j=0;j<count;++j) {
    const size_t i=ordinals[j];
    if(!visitor(keys_[i],slots_[i],row_at(i),dimension_))break;
  }
  return true;
}
void FlatIndex::scan(const Visitor& visitor) const {
  if(!ready_)throw std::runtime_error("flat vector index is not ready");
  if(fd_<0) {
    for(size_t i=0;i<count_;++i)
      if(!visitor(keys_[i],slots_[i],row_at(i),dimension_))return;
    return;
  }
  const size_t words=dimension_+4,stride=words*sizeof(float);
  const size_t per_block=std::max<size_t>(1,(1u<<20)/stride);
  std::vector<float> block(std::min(count_,per_block)*words);
  for(size_t first=0;first<count_;) {
    const size_t count=std::min(per_block,count_-first);
    transfer(fd_,block.data(),count*stride,static_cast<uint64_t>(first)*stride,false);
    for(size_t i=0;i<count;++i) {
      const float* record=block.data()+i*words;
      int64_t pk;uint64_t slot;
      std::memcpy(&pk,record,sizeof(pk));std::memcpy(&slot,record+2,sizeof(slot));
      if(!visitor(pk,slot,record+4,dimension_))return;
    }
    first+=count;
  }
}
}

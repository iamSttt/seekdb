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
#if defined(MVDB_ENABLE_BLOCK_QUANT_SCALE) && !defined(MVDB_DISABLE_QUANTIZED_BOUND)
    // Preserve posting, codes and ANN allowances. Local scale metadata/cache
    // may claim only the original remaining budget, never an extra quota.
    const size_t blocks=count ? (count-1)/QuantizedScaleView::block_rows+1 : 0;
    const size_t minimum_cache=QuantizedQueryBound::scale_cache_bytes(dimension,2);
    const size_t metadata=blocks+sizeof(quantization_steps_);
    const size_t spare=left-ann_budget_;
    if(quantized_budget_ && blocks>1 && minimum_cache &&
       minimum_cache<=QuantizedScaleView::query_cache_limit && metadata<=spare &&
       2*minimum_cache<=spare-metadata)
      quantization_scale_budget_=std::min(spare,metadata+2*QuantizedScaleView::query_cache_limit);
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
#ifdef MVDB_ENABLE_SPILL_FILTER
size_t FlatIndex::configure_spill_integers(const size_t* columns,size_t count,size_t budget) {
  if(ready_ || build_failed_ || count_ || spill_integer_count_)
    throw std::runtime_error("flat scalar cache must precede index construction");
  if(fd_<0 || !expected_ || !count || !columns)return 0;
  count=std::min(count,spill_integers_.size());
  if(count==2 && columns[0]==columns[1])
    throw std::runtime_error("duplicate flat scalar cache column");
  spill_integer_budget_=std::min<size_t>(budget,2u<<20);
  size_t available=spill_integer_budget_;
  for(size_t i=0;i<count;++i) {
    if(expected_>available/sizeof(int64_t))break;
    const size_t bytes=expected_*sizeof(int64_t);
    std::unique_ptr<uint8_t[]> values(new(std::nothrow) uint8_t[bytes]);
    if(!values)break;
    auto& column=spill_integers_[i];
    column.column=columns[i];column.bytes=bytes;column.values=std::move(values);
    available-=bytes;++spill_integer_count_;
  }
  return spill_integer_count_;
}

void FlatIndex::compress_spill_integers() {
  for(size_t i=0;i<spill_integer_count_;++i) {
    SpillInteger& column=spill_integers_[i];
    // Unsigned subtraction is the exact nonnegative difference even across
    // INT64_MIN/MAX. No signed subtraction, negation, or endpoint increment.
    const uint64_t span=uint64_t(column.maximum)-uint64_t(column.minimum);
    const size_t width=span==0 ? 0 : span<=UINT8_MAX ? 1 : span<=UINT16_MAX ? 2 :
                       span<=UINT32_MAX ? 4 : 8;
    if(width==8)continue;
    const size_t bytes=count_*width,retained=resident_bytes();
    if(retained>spill_integer_budget_ || bytes>spill_integer_budget_-retained)continue;
    std::unique_ptr<uint8_t[]> packed;
    if(bytes) {
      packed.reset(new(std::nothrow) uint8_t[bytes]);
      if(!packed)continue; // The already complete raw column remains usable.
      for(size_t ordinal=0;ordinal<count_;++ordinal) {
        int64_t value;
        std::memcpy(&value,column.values.get()+ordinal*sizeof(value),sizeof(value));
        const uint64_t delta=uint64_t(value)-uint64_t(column.minimum);
        uint8_t* dest=packed.get()+ordinal*width;
        if(width==1)*dest=static_cast<uint8_t>(delta);
        else if(width==2) {const uint16_t v=static_cast<uint16_t>(delta);std::memcpy(dest,&v,sizeof(v));}
        else {const uint32_t v=static_cast<uint32_t>(delta);std::memcpy(dest,&v,sizeof(v));}
      }
    }
    column.values=std::move(packed);column.width=width;column.bytes=bytes;
  }
}

bool FlatIndex::has_spill_integer(size_t column) const {
  if(!ready_ || fd_<0)return false;
  for(size_t i=0;i<spill_integer_count_;++i)if(spill_integers_[i].column==column)return true;
  return false;
}

bool FlatIndex::spill_integer_value(size_t ordinal,size_t column,int64_t* value) const {
  if(!value || !ready_ || fd_<0 || ordinal>=count_)return false;
  for(size_t i=0;i<spill_integer_count_;++i) {
    const SpillInteger& stored=spill_integers_[i];
    if(stored.column!=column)continue;
    if(stored.width==0) {*value=stored.minimum;return true;}
    const uint8_t* source=stored.values.get()+ordinal*stored.width;
    if(stored.width==8) {std::memcpy(value,source,sizeof(*value));return true;}
    uint64_t delta=0;
    if(stored.width==1)delta=*source;
    else if(stored.width==2) {uint16_t v;std::memcpy(&v,source,sizeof(v));delta=v;}
    else {uint32_t v;std::memcpy(&v,source,sizeof(v));delta=v;}
    const uint64_t bits=uint64_t(stored.minimum)+delta;
    std::memcpy(value,&bits,sizeof(*value));return true;
  }
  return false;
}

void FlatIndex::scan_filtered(const MetadataPredicate& predicate,const Visitor& visitor) const {
  if(!ready_)throw std::runtime_error("flat vector index is not ready");
  if(fd_<0) {
    for(size_t i=0;i<count_;++i)
      if(predicate(i,keys_[i],slots_[i]) && !visitor(keys_[i],slots_[i],row_at(i),dimension_))return;
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
      if(predicate(first+i,pk,slot) && !visitor(pk,slot,record+4,dimension_))return;
    }
    first+=count;
  }
}
#endif
void FlatIndex::add(int64_t pk,uint64_t slot,const float* values,size_t dimension
#ifdef MVDB_ENABLE_SPILL_FILTER
    ,const int64_t* integer_values,size_t integer_count
#endif
    ) {
  if(ready_ || build_failed_ || count_>=expected_ || dimension!=dimension_ || !values)
    throw std::runtime_error("invalid flat index build row");
#ifdef MVDB_ENABLE_SPILL_FILTER
  if(integer_count!=spill_integer_count_ || (integer_count && !integer_values))
    throw std::runtime_error("invalid flat index scalar build row");
  for(size_t i=0;i<integer_count;++i) {
    SpillInteger& column=spill_integers_[i];
    const int64_t value=integer_values[i];
    std::memcpy(column.values.get()+count_*sizeof(value),&value,sizeof(value));
    if(!count_)column.minimum=column.maximum=value;
    else {column.minimum=std::min(column.minimum,value);column.maximum=std::max(column.maximum,value);}
  }
#endif
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
#ifdef MVDB_ENABLE_SPILL_FILTER
    compress_spill_integers();
#endif
  }
#ifndef MVDB_DISABLE_QUANTIZED_BOUND
  if(quantized_budget_) {
    double max_abs=0;
    bool ok=true;
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
    const size_t blocks=count_ ? (count_-1)/QuantizedScaleView::block_rows+1 : 0;
    if(quantization_scale_budget_)
      quantization_block_ids_.reset(new(std::nothrow) uint8_t[blocks]);
    bool local=bool(quantization_block_ids_);
    double block_max=0;
    quantization_step_count_=0;
    const auto abandon_local=[&] {
      quantization_block_ids_.reset();quantization_step_count_=0;
      quantization_scale_budget_=0;quantization_query_cache_bytes_=0;local=false;
    };
#endif
    for(size_t i=0;i<count_;++i) {
      const float* row=row_at(i);
      for(size_t d=0;d<dimension_;++d) {
        const double magnitude=std::fabs(static_cast<double>(row[d]));
        if(!std::isfinite(magnitude)) {max_abs=magnitude;ok=false;break;}
        max_abs=std::max(max_abs,magnitude);
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
        if(local)block_max=std::max(block_max,magnitude);
#endif
      }
      if(!ok)break;
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
      if(local && ((i+1)%QuantizedScaleView::block_rows==0 || i+1==count_)) {
        const double step=mvdb::quantization_step(block_max);
        size_t id=0;
        while(id<quantization_step_count_ && quantization_steps_[id]!=step)++id;
        if(!step || id>=QuantizedScaleView::max_steps)abandon_local();
        else {
          if(id==quantization_step_count_)quantization_steps_[quantization_step_count_++]=step;
          quantization_block_ids_[i/QuantizedScaleView::block_rows]=static_cast<uint8_t>(id);
        }
        block_max=0;
      }
#endif
    }
    const double step=mvdb::quantization_step(max_abs);
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
    const size_t cache=QuantizedQueryBound::scale_cache_bytes(dimension_,quantization_step_count_);
    const size_t metadata=blocks+sizeof(quantization_steps_);
    bool finer=false;
    for(size_t s=0;s<quantization_step_count_;++s)finer=finer || quantization_steps_[s]<step;
    if(!local || !ok || !finer || !cache || cache>QuantizedScaleView::query_cache_limit ||
       metadata>quantization_scale_budget_ || 2*cache>quantization_scale_budget_-metadata)
      abandon_local();
    else {
      quantization_query_cache_bytes_=cache;
      quantization_scale_budget_=metadata+2*cache;
    }
#endif
    if(ok && step>0 && std::isfinite(step)) {
      quantized_.reset(new(std::nothrow) int8_t[quantized_budget_]);
      if(quantized_) {
        // Do not reorder: codes have exactly the rows_/keys_ traversal order.
        const auto encode_rows=[&](bool use_local) {
          size_t code=0;
          for(size_t i=0;i<count_;++i) {
            double row_step=step;
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
            if(use_local)row_step=quantization_steps_[quantization_block_ids_[i/QuantizedScaleView::block_rows]];
#else
            (void)use_local;
#endif
            const float* row=row_at(i);
            for(size_t d=0;d<dimension_;++d)
              if(!mvdb::quantize_coordinate(row[d],row_step,&quantized_[code++]))return false;
          }
          return true;
        };
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
        if(local && !encode_rows(true))abandon_local();
        if(!local && !encode_rows(false))ok=false;
        // Zero deliberately disables any old consumer that ignores scale IDs.
        if(ok)quantization_step_=local ? 0 : step;
#else
        if(encode_rows(false))quantization_step_=step;
#endif
      }
    }
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
    if(!ok || !quantized_ || (!local && !quantization_step_)) {
      abandon_local();quantized_.reset();quantized_budget_=0;quantization_step_=0;
    }
#else
    if(!quantization_step_) {
      quantized_.reset();quantized_budget_=0;
    }
#endif
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
  view->quantization_scales={};
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
  if(quantization_block_ids_ && quantized_) {
    auto& scales=view->quantization_scales;
    scales.block_ids=quantization_block_ids_.get();scales.steps=quantization_steps_.data();
    scales.block_count=(count_-1)/QuantizedScaleView::block_rows+1;
    scales.row_count=count_;scales.step_count=quantization_step_count_;
    scales.query_cache_bytes=quantization_query_cache_bytes_;
  }
#endif
  return true;
}
size_t FlatIndex::resident_bytes() const {
  size_t bytes=rows_.capacity()*sizeof(const float*)+keys_.capacity()*sizeof(int64_t)+
         slots_.capacity()*sizeof(uint64_t)+posting_budget_+quantized_budget_+
         ann_centroids_.capacity()*sizeof(float)+ann_members_.capacity()*sizeof(uint32_t)+
         ann_offsets_.capacity()*sizeof(uint32_t);
#ifdef MVDB_ENABLE_SPILL_FILTER
  for(size_t i=0;i<spill_integer_count_;++i)bytes+=spill_integers_[i].bytes;
#endif
#ifdef MVDB_ENABLE_BLOCK_QUANT_SCALE
  bytes+=quantization_scale_budget_;
#endif
  return bytes;
}
bool FlatIndex::equal_ordinals(size_t column,int64_t value,
    const std::function<int64_t(uint64_t)>& read_value,
    const uint32_t** ordinals,size_t* count) {
#ifdef MVDB_DISABLE_MULTI_POSTING
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
#else
  IntegerRange range;
  range.has_lower=range.has_upper=true;range.lower=range.upper=value;
  return integer_ordinals(column,range,read_value,ordinals,count);
#endif
}

#ifndef MVDB_DISABLE_MULTI_POSTING
FlatIndex::IntegerPosting* FlatIndex::integer_posting(size_t column,
    const std::function<int64_t(uint64_t)>& read_value) {
  // This counter only orders two cache entries; wrap loses recency, not data.
  if(++posting_clock_==0) {
    for(auto& posting:postings_)posting.last_used=0;
    posting_clock_=1;
  }
  for(auto& posting:postings_)if(posting.ready && posting.column==column) {
    posting.last_used=posting_clock_;return &posting;
  }
  const auto evict_oldest=[&]() {
    IntegerPosting* victim=nullptr;
    for(auto& posting:postings_)if(posting.ready &&
        (!victim || posting.last_used<victim->last_used))victim=&posting;
    if(!victim)return false;
    *victim=IntegerPosting{};return true;
  };
  const auto make_room=[&](size_t extra) {
    if(extra>posting_budget_)return false;
    for(;;) {
      size_t used=0;
      for(const auto& posting:postings_)used+=posting.bytes;
      if(used<=posting_budget_-extra)return true;
      if(!evict_oldest())return false;
    }
  };
  bool full=true;
  for(const auto& posting:postings_)if(!posting.ready)full=false;
  if(full)evict_oldest();
  // 8N unsorted values + 4N ordinals, followed by at most 8N output.
  // The existing 20N reservation covers retained columns AND construction.
  // Exact-length arrays avoid implementation-dependent vector overcapacity.
  const size_t scratch=count_*(sizeof(int64_t)+sizeof(uint32_t));
  if(!make_room(scratch))return nullptr;
  std::unique_ptr<int64_t[]> unsorted(new(std::nothrow) int64_t[count_]);
  std::unique_ptr<uint32_t[]> ordinals(new(std::nothrow) uint32_t[count_]);
  if(!unsorted || !ordinals)return nullptr;
  for(size_t i=0;i<count_;++i) {
    unsorted[i]=read_value(slots_[i]);ordinals[i]=static_cast<uint32_t>(i);
  }
  std::sort(ordinals.get(),ordinals.get()+count_,[&](uint32_t a,uint32_t b) {
    return unsorted[a]<unsorted[b] || (unsorted[a]==unsorted[b] && a<b);
  });
  size_t groups=1;
  for(size_t i=1;i<count_;++i)
    if(unsorted[ordinals[i]]!=unsorted[ordinals[i-1]])++groups;
  const bool compressed=groups*(sizeof(int64_t)+sizeof(uint32_t))<count_*sizeof(int64_t);
  const size_t values=compressed ? groups : count_;
  const size_t output=values*sizeof(int64_t)+(compressed ? groups*sizeof(uint32_t) : 0);
  if(!make_room(scratch+output))return nullptr;
  IntegerPosting built;
  built.values.reset(new(std::nothrow) int64_t[values]);
  if(compressed)built.starts.reset(new(std::nothrow) uint32_t[groups]);
  if(!built.values || (compressed && !built.starts))return nullptr;
  size_t group=0;
  for(size_t i=0;i<count_;++i) {
    const int64_t value=unsorted[ordinals[i]];
    if(!compressed)built.values[i]=value;
    else if(i==0 || value!=unsorted[ordinals[i-1]]) {
      built.values[group]=value;built.starts[group++]=static_cast<uint32_t>(i);
    }
  }
  built.ordinals=std::move(ordinals);built.value_count=values;
  built.bytes=count_*sizeof(uint32_t)+output;
  built.column=column;built.last_used=posting_clock_;built.ready=true;
  for(auto& posting:postings_)if(!posting.ready) {
    posting=std::move(built);return &posting;
  }
  return nullptr; // Defensive: a free slot was reserved before construction.
}

std::pair<size_t,size_t> FlatIndex::posting_span(const IntegerPosting& posting,
    const IntegerRange& range) const {
  const int64_t* begin=posting.values.get();
  const int64_t* end=begin+posting.value_count;
  const int64_t* first=range.has_lower ?
      (range.lower_inclusive ? std::lower_bound(begin,end,range.lower) :
                               std::upper_bound(begin,end,range.lower)) : begin;
  const int64_t* last=range.has_upper ?
      (range.upper_inclusive ? std::upper_bound(begin,end,range.upper) :
                               std::lower_bound(begin,end,range.upper)) : end;
  const auto offset=[&](const int64_t* value) {
    const size_t i=static_cast<size_t>(value-begin);
    return posting.starts ? (i==posting.value_count ? count_ : size_t(posting.starts[i])) : i;
  };
  const size_t left=offset(first),right=offset(last);
  return {left,std::max(left,right)};
}
#endif

bool FlatIndex::integer_ordinals(size_t column,const IntegerRange& range,
    const std::function<int64_t(uint64_t)>& read_value,
    const uint32_t** ordinals,size_t* count) {
#ifdef MVDB_DISABLE_MULTI_POSTING
  (void)column;(void)range;(void)read_value;(void)ordinals;(void)count;
  return false;
#else
  if(!ready_)throw std::runtime_error("flat vector index is not ready");
  if(!ordinals || !count)return false;
  *ordinals=nullptr;*count=0;
  if(fd_>=0 || (count_ && !posting_budget_))return false;
  if(!count_)return true;
  IntegerPosting* posting=integer_posting(column,read_value);
  if(!posting)return false; // Optional allocation failure retains exact scan.
  const auto span=posting_span(*posting,range);
  *ordinals=posting->ordinals.get()+span.first;*count=span.second-span.first;
  return true;
#endif
}

bool FlatIndex::cached_integer_count(size_t column,const IntegerRange& range,size_t* count) const {
#ifdef MVDB_DISABLE_MULTI_POSTING
  (void)column;(void)range;(void)count;return false;
#else
  if(!count || !ready_ || fd_>=0)return false;
  if(!count_) {*count=0;return true;}
  for(const auto& posting:postings_)if(posting.ready && posting.column==column) {
    const auto span=posting_span(posting,range);
    *count=span.second-span.first;return true;
  }
  return false;
#endif
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

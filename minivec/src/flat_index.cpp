#include "flat_index.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unistd.h>

namespace mvdb {
namespace {
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
  if(count<=budget/stride) {
    vectors_.reserve(count*dimension);
    keys_.reserve(count);slots_.reserve(count);
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
  if(ready_ || count_>=expected_ || dimension!=dimension_)
    throw std::runtime_error("invalid flat index build row");
  if(fd_<0) {
    keys_.push_back(pk);slots_.push_back(slot);
    vectors_.insert(vectors_.end(),values,values+dimension_);
  } else {
    // Float-aligned storage: the first four float-sized words contain raw key
    // metadata and are accessed only via memcpy, never as floating point values.
    std::vector<float> record(dimension_+4);
    std::memcpy(record.data(),&pk,sizeof(pk));
    std::memcpy(record.data()+2,&slot,sizeof(slot));
    std::memcpy(record.data()+4,values,dimension_*sizeof(float));
    const size_t bytes=record.size()*sizeof(float);
    transfer(fd_,record.data(),bytes,static_cast<uint64_t>(count_)*bytes,true);
  }
  ++count_;
}
void FlatIndex::finish() {
  if(count_!=expected_)throw std::runtime_error("incomplete flat vector index build");
  ready_=true;
}
size_t FlatIndex::resident_bytes() const {
  return vectors_.capacity()*sizeof(float)+keys_.capacity()*sizeof(int64_t)+slots_.capacity()*sizeof(uint64_t);
}
void FlatIndex::scan(const Visitor& visitor) const {
  if(!ready_)throw std::runtime_error("flat vector index is not ready");
  if(fd_<0) {
    for(size_t i=0;i<count_;++i)
      if(!visitor(keys_[i],slots_[i],vectors_.data()+i*dimension_,dimension_))return;
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

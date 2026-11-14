#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace mvdb {
struct SortRow {
  int64_t pk = 0, integer = 0;
  double distance = 0;
  std::string text, payload;
};

// Bounded sorted runs in one /data temporary file. A run cursor keeps just
// one output row resident; payload text never accumulates for the whole table.
class ResultSorter {
  using Less = std::function<bool(const SortRow&, const SortRow&)>;
  struct Run { uint64_t offset, count; };
  Less less_;
  std::string dir_;
  std::vector<SortRow> buffer_;
  std::vector<Run> runs_;
  uint64_t bytes_ = 0, end_ = 0, widest_ = 1;
  int fd_ = -1;
  static constexpr size_t budget = 8u << 20;
  int temporary_file() {
    std::string path=dir_+"/.sort-XXXXXX";
    std::vector<char> name(path.begin(),path.end());name.push_back(0);
    int fd=::mkstemp(name.data());
    if(fd<0)throw std::runtime_error("cannot create external result sort file");
    ::unlink(name.data());return fd;
  }
  void io(void* data, size_t n, uint64_t at, bool write, int target=-1) {
    if(target<0)target=fd_;
    char* p = static_cast<char*>(data);
    while (n) {
      ssize_t k = write ? ::pwrite(target,p,n,at) : ::pread(target,p,n,at);
      if (k < 0 && errno == EINTR) continue;
      if (k <= 0) throw std::runtime_error("external result sort I/O failed");
      n -= k; p += k; at += k;
    }
  }
  template<class T> static void put(std::string& b, T v) {
    b.append(reinterpret_cast<const char*>(&v),sizeof(v));
  }
  static void putstr(std::string& b, const std::string& s) {
    put<uint64_t>(b,s.size()); b.append(s);
  }
  template<class T> static T get(const std::string& b, size_t& p) {
    if (p > b.size() || sizeof(T)>b.size()-p) throw std::runtime_error("bad sort record");
    T v; std::memcpy(&v,b.data()+p,sizeof(v)); p+=sizeof(v); return v;
  }
  static std::string getstr(const std::string& b, size_t& p) {
    auto n=get<uint64_t>(b,p);
    if (p>b.size() || n>b.size()-p) throw std::runtime_error("bad sort string");
    auto s=b.substr(p,n); p+=n; return s;
  }
  SortRow read(Run& run) {
    uint64_t n; io(&n,sizeof(n),run.offset,false); run.offset+=sizeof(n);
    if (n > (1ull<<30)) throw std::runtime_error("oversized sort record");
    std::string b(n,'\0'); io(&b[0],n,run.offset,false); run.offset+=n; --run.count;
    size_t at=0; SortRow r;
    r.pk=get<int64_t>(b,at); r.integer=get<int64_t>(b,at); r.distance=get<double>(b,at);
    r.text=getstr(b,at); r.payload=getstr(b,at); return r;
  }
  void write_row(const SortRow& r,int target,uint64_t& offset) {
    std::string b; put(b,r.pk);put(b,r.integer);put(b,r.distance);putstr(b,r.text);putstr(b,r.payload);
    uint64_t n=b.size();io(&n,sizeof(n),offset,true,target);offset+=sizeof(n);
    io(&b[0],b.size(),offset,true,target);offset+=b.size();
  }
  template<class Emit> void merge(size_t first,size_t last,size_t limit,Emit output) {
    struct Cursor {SortRow row;size_t run;};
    auto worse=[&](const Cursor& a,const Cursor& b){return less_(b.row,a.row);};
    std::priority_queue<Cursor,std::vector<Cursor>,decltype(worse)> heap(worse);
    for(size_t i=first;i<last;++i)if(runs_[i].count)heap.push({read(runs_[i]),i});
    for(size_t count=0;!heap.empty() && count<limit;++count) {
      Cursor c=heap.top();heap.pop();output(c.row);
      if(runs_[c.run].count)heap.push({read(runs_[c.run]),c.run});
    }
  }
  void flush() {
    if (buffer_.empty()) return;
    if (fd_<0) {
      fd_=temporary_file();
    }
    std::sort(buffer_.begin(),buffer_.end(),less_);
    runs_.push_back({end_,buffer_.size()});
    for(const auto& r:buffer_) {
      write_row(r,fd_,end_);
    }
    buffer_.clear(); bytes_=0;
  }
 public:
  ResultSorter(std::string dir, Less less):less_(std::move(less)),dir_(std::move(dir)) {}
  ~ResultSorter(){if(fd_>=0)::close(fd_);}
  ResultSorter(const ResultSorter&)=delete;
  void add(SortRow row) {
    widest_=std::max<uint64_t>(widest_,sizeof(row)+row.text.size()+row.payload.size());
    bytes_+=sizeof(row)+row.text.size()+row.payload.size();
    buffer_.push_back(std::move(row));
    if(bytes_>=budget)flush();
  }
  void emit(size_t limit,const std::function<void(const std::string&)>& out) {
    if(runs_.empty()) {
      std::sort(buffer_.begin(),buffer_.end(),less_);
      for(size_t i=0;i<buffer_.size() && i<limit;++i)out(buffer_[i].payload);
      return;
    }
    flush();
    // Restrict merge fan-in too, not only initial runs: wide rows must not
    // cause all runs' heads to fill memory. Alternate files to bound disk use.
    const size_t fan=std::max<size_t>(2,std::min<size_t>(16,budget/widest_));
    while(runs_.size()>fan) {
      const int next=temporary_file();uint64_t offset=0;
      std::vector<Run> next_runs;
      try {
        for(size_t i=0;i<runs_.size();i+=fan) {
          const size_t last=std::min(i+fan,runs_.size());uint64_t count=0;
          for(size_t j=i;j<last;++j)count+=runs_[j].count;
          next_runs.push_back({offset,count});
          merge(i,last,static_cast<size_t>(count),[&](const SortRow& r){write_row(r,next,offset);});
        }
      } catch(...) {::close(next);throw;}
      ::close(fd_);fd_=next;end_=offset;runs_=std::move(next_runs);
    }
    merge(0,runs_.size(),limit,[&](const SortRow& r){out(r.payload);});
  }
};
}

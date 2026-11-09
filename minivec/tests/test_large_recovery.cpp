// Standalone stress test. Compile with automatic checkpoints disabled so the
// first recovery phase really exercises a WAL larger than the address limit:
// g++ -O2 -std=c++17 -Isrc -DMVDB_AUTO_CHECKPOINT_BYTES=1099511627776ULL \
//   tests/test_large_recovery.cpp src/db.cpp src/value.cpp src/flat_index.cpp -o test_large_recovery
// Default payload: 256 MiB; WAL construction/recovery address limit: 160 MiB.
// The snapshot phase uses a larger address limit because mmap address space is
// not physical memory. Its RSS is reported, not misrepresented as cgroup usage.
#include "db.h"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace mvdb;
static size_t nrows=16384, dimension=4096, limit_mib=160;
static std::string directory;
static bool checkpoint_failure=false;
static void require(bool result, const std::string& why) {
  if (!result) { std::cerr << "FAIL: " << why << '\n'; std::exit(1); }
}
static void limit_address(size_t mib) {
  const rlim_t bytes=static_cast<rlim_t>(mib)*1024*1024;
  struct rlimit limit{bytes,bytes};
  require(::setrlimit(RLIMIT_AS,&limit)==0,"setrlimit(RLIMIT_AS)");
}
static void report(const char* phase) {
  struct rusage usage{}; require(::getrusage(RUSAGE_SELF,&usage)==0,"getrusage");
  std::cout << phase << " max_rss_kib=" << usage.ru_maxrss
            << " payload_bytes=" << nrows*dimension*sizeof(float) << '\n';
}
static void verify(Database& db) {
  size_t count=0;
  db.scan("big",{},[&](const Row& row) {
    require(row.vals.size()==2,"row width");
    const int64_t id=row.vals[0].i;
    require(id>=0 && static_cast<size_t>(id)<nrows,"primary key range");
    const auto& vec=row.vals[1].v;
    require(vec.size()==dimension,"vector dimension");
    const float base=static_cast<float>(id%997);
    require(vec.front()==base && vec.back()==base+0.5f,"vector contents");
    ++count; return true;
  });
  require(count==nrows,"complete row count");
  require(db.table("big")->pk_pos.size()==nrows,"distinct primary keys");
}
static void build() {
  limit_address(limit_mib);
  Database db; std::string err; int code=0;
  require(db.open(directory,&err),"build open: "+err);
  Column id; id.name="id"; id.type=Type::BIGINT; id.is_primary=true;
  Column vec; vec.name="v"; vec.type=Type::VECTOR; vec.len=static_cast<int32_t>(dimension);
  require(db.create_table("big",{id,vec},&err,&code),"create big: "+err);
  if (checkpoint_failure) require(db.checkpoint(&err),"initial empty snapshot");
  for (size_t base=0;base<nrows;base+=128) {
    require(db.begin(&err,&code),"begin batch");
    for (size_t i=base;i<nrows && i<base+128;++i) {
      std::vector<float> v(dimension,static_cast<float>(i%997));
      v.back()+=0.5f;
      require(db.insert("big",{Value::ofInt(static_cast<int64_t>(i)),Value::ofVec(std::move(v))},&err,&code),"insert: "+err);
    }
    require(db.commit(&err,&code),"commit batch: "+err);
  }
  verify(db);
  struct stat st{};
  const int snapshot_status=::stat((directory+"/snapshot.bin").c_str(),&st);
  require(checkpoint_failure ? (snapshot_status==0 && st.st_size<65536) :
          (snapshot_status!=0 && errno==ENOENT),
          "compile this test with MVDB_AUTO_CHECKPOINT_BYTES=1099511627776ULL");
  report("build_under_address_limit");
}
static void recover_wal_and_checkpoint() {
  limit_address(limit_mib);
  Database db; std::string err;
  require(db.open(directory,&err),"WAL recovery: "+err);
  verify(db); report("wal_recovery_under_address_limit");
  require(db.checkpoint(&err),"large checkpoint: "+err);
  report("checkpoint_under_address_limit");
}
static void recover_snapshot() {
  // Permit enough virtual address space to map the snapshot. This is explicitly
  // a reporting phase; a cgroup is needed for a physical-memory cap.
  const size_t payload_mib=(nrows*dimension*sizeof(float)+(1u<<20)-1)/(1u<<20);
  limit_address(limit_mib+payload_mib+256);
  Database db; std::string err;
  require(db.open(directory,&err),"snapshot recovery: "+err);
  verify(db); report("snapshot_recovery_rss_only");
}
static uint64_t file_hash(const std::string& path) {
  std::ifstream f(path,std::ios::binary);
  require(f.is_open(),"hash open: "+path);
  uint64_t hash=14695981039346656037ull;
  char chunk[65536];
  while (f.read(chunk,sizeof(chunk)) || f.gcount()) {
    for (std::streamsize i=0;i<f.gcount();++i) {
      hash^=static_cast<unsigned char>(chunk[i]); hash*=1099511628211ull;
    }
  }
  require(f.eof(),"hash read: "+path);
  return hash;
}
static void fail_checkpoint_under_file_limit() {
  limit_address(limit_mib);
  Database db; std::string err;
  require(db.open(directory,&err),"failure-test recovery: "+err);
  verify(db);
  const auto snapshot_hash=file_hash(directory+"/snapshot.bin");
  const auto wal_hash=file_hash(directory+"/data.log");
  struct rlimit original{};
  require(::getrlimit(RLIMIT_FSIZE,&original)==0,"get file limit");
  struct sigaction ignored{},previous{};
  ignored.sa_handler=SIG_IGN; ::sigemptyset(&ignored.sa_mask);
  require(::sigaction(SIGXFSZ,&ignored,&previous)==0,"ignore SIGXFSZ");
  struct rlimit restricted=original; restricted.rlim_cur=65536;
  require(::setrlimit(RLIMIT_FSIZE,&restricted)==0,"restrict snapshot file size");
  require(!db.checkpoint(&err),"snapshot write must fail");
  require(::setrlimit(RLIMIT_FSIZE,&original)==0,"restore file limit");
  require(::sigaction(SIGXFSZ,&previous,nullptr)==0,"restore SIGXFSZ");
  require(!err.empty(),"snapshot failure has diagnostic");
  struct stat st{};
  require(::stat((directory+"/snapshot.bin.tmp").c_str(),&st)!=0 && errno==ENOENT,
          "partial snapshot removed");
  require(file_hash(directory+"/snapshot.bin")==snapshot_hash,"old snapshot unchanged");
  require(file_hash(directory+"/data.log")==wal_hash,"committed WAL unchanged");
  verify(db); db.close();
  require(db.open(directory,&err),"reopen after failed snapshot: "+err);
  verify(db);
  require(db.checkpoint(&err),"retry snapshot after restoring limit: "+err);
  report("checkpoint_write_failure_under_address_limit");
}
static void child_phase(void (*phase)()) {
  const pid_t pid=::fork(); require(pid>=0,"fork");
  if (pid==0) {
    try { phase(); ::_exit(0); }
    catch (const std::exception& e) { std::cerr << "child exception: " << e.what() << '\n'; ::_exit(2); }
  }
  int status=0; require(::waitpid(pid,&status,0)==pid,"waitpid");
  require(WIFEXITED(status) && WEXITSTATUS(status)==0,"child phase completed");
}
int main(int argc,char** argv) {
  std::cout.setf(std::ios::unitbuf); std::cerr.setf(std::ios::unitbuf);
  for (int i=1;i<argc;++i) {
    const std::string a=argv[i];
    if (a=="--rows" && i+1<argc) nrows=std::stoull(argv[++i]);
    else if (a=="--dim" && i+1<argc) dimension=std::stoull(argv[++i]);
    else if (a=="--limit-mib" && i+1<argc) limit_mib=std::stoull(argv[++i]);
    else if (a=="--checkpoint-write-failure") checkpoint_failure=true;
    else { std::cerr << "usage: test_large_recovery [--rows N] [--dim D] [--limit-mib M] [--checkpoint-write-failure]\n"; return 2; }
  }
  require(nrows>0 && dimension>1 && limit_mib>=64,"positive test parameters");
  char name[]="/tmp/mvdb-large-recovery-XXXXXX";
  const char* dir=::mkdtemp(name); require(dir!=nullptr,"mkdtemp"); directory=dir;
  std::cout << "data_directory=" << directory << '\n';
  child_phase(build);
  if (checkpoint_failure) {
    require(nrows*dimension*sizeof(float)>65536,"payload exceeds injected file limit");
    child_phase(fail_checkpoint_under_file_limit);
    child_phase(recover_snapshot);
    std::cout << "PASS failed snapshot cleanup, durable files unchanged, recovery and retry\n";
    return 0;
  }
  // Torn tail plus interruption during recovery: every retry must continue to
  // recover exactly the committed state, including the final successful retry.
  { std::ofstream tail(directory+"/data.log",std::ios::binary|std::ios::app); tail.write("bad",3); }
  int interrupted=0;
  for (unsigned delay : {50u,200u,1000u,5000u,10000u,20000u}) {
    int pipefd[2]; require(::pipe(pipefd)==0,"pipe");
    const pid_t pid=::fork(); require(pid>=0,"fork recovery");
    if (pid==0) {
      ::close(pipefd[0]); limit_address(limit_mib);
      const char ready='R'; if (::write(pipefd[1],&ready,1)!=1) ::_exit(3);
      Database db; std::string err;
      try { if (!db.open(directory,&err)) ::_exit(4); verify(db); ::_exit(0); }
      catch (...) { ::_exit(5); }
    }
    ::close(pipefd[1]); char ready=0;
    require(::read(pipefd[0],&ready,1)==1,"recovery-start handshake"); ::close(pipefd[0]);
    ::usleep(delay); ::kill(pid,SIGKILL);
    int status=0; require(::waitpid(pid,&status,0)==pid,"wait interrupted recovery");
    if (WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL) ++interrupted;
    else require(WIFEXITED(status) && WEXITSTATUS(status)==0,"recovery child result");
  }
  std::cout << "recovery_children_sigkilled=" << interrupted << '\n';
  require(interrupted>0,"at least one recovery interrupted");
  child_phase(recover_wal_and_checkpoint);
  child_phase(recover_snapshot);
  std::cout << "PASS large payload, WAL restart, repeated recovery SIGKILL, snapshot integrity\n";
  // Keep files for inspection; no broad or computed recursive deletion here.
}

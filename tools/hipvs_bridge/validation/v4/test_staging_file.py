"""Compile the actual staging-file class; verify memfd and denied-memfd fallback."""
from pathlib import Path
import subprocess,json
w=Path(__file__).resolve().parent
text=(w/'compact_adaptor.cpp').read_text()
body=text.split('struct ObCuvsSnapshotFile {',1)[1].split('struct ObCuvsSnapshotJob {',1)[0]
body='struct ObCuvsSnapshotFile {'+body
prefix='''#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <dirent.h>
#include <cerrno>
#ifdef DENY_MEMFD
static long denied_syscall(long, ...) { errno = ENOSYS; return -1; }
#define syscall denied_syscall
#endif
'''
main='''
static int descriptors() {
  DIR *d = opendir("/proc/self/fd"); assert(d); int n=0;
  while(readdir(d)) ++n;
  closedir(d); return n;
}
int main() {
 const int before = descriptors();
 for(int i=0;i<100;i++) {
   ObCuvsSnapshotFile f; assert(f.file_);
   char target[256]={}; assert(readlink(f.path_, target, sizeof(target)-1)>0);
#ifndef DENY_MEMFD
   assert(strstr(target,"memfd:seekdb-cuvs"));
#else
   assert(!strstr(target,"memfd:"));
#endif
   FILE *second=fopen(f.path_,"wb"); assert(second);
   assert(fwrite("exact bytes",1,11,second)==11); assert(fclose(second)==0);
   assert(fseeko(f.file_,0,SEEK_END)==0 && ftello(f.file_)==11);
   rewind(f.file_); char content[12]={}; assert(fread(content,1,11,f.file_)==11);
   assert(strcmp(content,"exact bytes")==0);
 }
 assert(descriptors()==before);
 puts("STAGING FILE PASS");
}
'''
(w/'test_staging_file.cpp').write_text(prefix+body+main)
for mode in ('memfd','fallback'):
    cmd=['g++','-std=c++17','-fsanitize=address,undefined','-g',str(w/'test_staging_file.cpp'),'-o',str(w/f'test_staging_{mode}')]
    if mode=='fallback':cmd.insert(1,'-DDENY_MEMFD')
    subprocess.run(cmd,check=True)
    subprocess.run([str(w/f'test_staging_{mode}')],check=True)
(w/'test_staging_file.json').write_text(json.dumps({'status':'PASS','modes':['memfd','denied-memfd fallback'],'cycles_per_mode':100,'checks':['shared file content','size','fd lifetime','ASan/UBSan']},indent=2))

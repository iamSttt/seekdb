// Diagnostic launcher: move only the database into a pre-created private cgroup.
// The Python workload driver stays outside it. Compile with cc -O2 -static.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
int main(int argc,char** argv) {
  const char prefix[]="/sys/fs/cgroup/minivec-report-";
  if(argc<3 || strncmp(argv[1],prefix,sizeof(prefix)-1) || strstr(argv[1],".."))return 64;
  char path[4096],pid[32];
  if(snprintf(path,sizeof(path),"%s/cgroup.procs",argv[1])>=(int)sizeof(path))return 65;
  int fd=open(path,O_WRONLY|O_CLOEXEC);if(fd<0){perror("cgroup.procs");return 66;}
  int length=snprintf(pid,sizeof(pid),"%ld",(long)getpid());
  if(write(fd,pid,(size_t)length)!=length){perror("move to cgroup");close(fd);return 67;}
  close(fd);execv(argv[2],argv+2);perror("exec database");return 68;
}

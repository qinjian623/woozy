#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
static void write_all(int fd,const void *memory,size_t size) {
  auto *p=static_cast<const char*>(memory);
  while(size) {auto n=::write(fd,p,size);if(n<0&&errno==EINTR)continue;if(n<=0)_exit(90);p+=n;size-=n;}
}
int main(int argc,char **argv) {
  if(argc!=2)return 91;
  if(!std::strcmp(argv[1],"ok")) {
    write_all(1,"out\n",4);write_all(2,"err\n",4);
    const unsigned char bytes[]={0,255,192};write_all(1,bytes,sizeof bytes);return 0;
  }
  if(!std::strcmp(argv[1],"fail")) {write_all(2,"failed\n",7);return 23;}
  if(!std::strcmp(argv[1],"signal")) {::kill(::getpid(),SIGTERM);return 92;}
  if(!std::strcmp(argv[1],"large")) {
    char a[256],b[256];std::memset(a,'A',sizeof a);std::memset(b,'B',sizeof b);
    for(int i=0;i<4096;++i){write_all(1,a,sizeof a);write_all(2,b,sizeof b);}return 0;
  }
  if(!std::strcmp(argv[1],"fds")) {
    int count=0;for(int fd=3;fd<1024;++fd)if(::fcntl(fd,F_GETFD)!=-1)++count;
    char line[40];auto n=std::snprintf(line,sizeof line,"fds=%d\n",count);write_all(1,line,n);return 0;
  }
  return 93;
}

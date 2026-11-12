// Permanent F1 probe: graph-stable fork no-park vs genuine graph-critical.
// Real hosted libc pthread_atfork prepare callbacks and lang_process_fork_v1.
// This is a C++ private-runtime test, not a .wz Source/CG test.
#include "gc_program.hpp"
#include "gc_world.hpp"
#include "process.hpp"
#include "os.hpp"
#include <cassert>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
static int sink=-1;
static void callback() {
 LangGcWorldCallbackToken token{};
 lang_gc_world_callback_enter_v1(&token);
 lang_gc_world_callback_leave_v1(&token);
}
static void safe_call(){assert(lang_os_write_v1(sink,"x",1)==1);}
int main(int argc,char**argv){
 assert(argc==2);sink=open("/dev/null",O_WRONLY);assert(sink>=0);assert(lang_gc_program_init_v1()==0);const char*mode=argv[1];
 if(!std::strcmp(mode,"control-callback")){callback();}
 else if(!std::strcmp(mode,"control-graph-critical")){lang_gc_world_enter_critical_v1();callback();lang_gc_world_leave_critical_v1();}
 else if(!std::strcmp(mode,"fork-token-callback")){LangGcProgramForkToken f{};lang_gc_program_fork_enter_v1(&f);callback();lang_gc_program_fork_leave_v1(&f);}
 else if(!std::strcmp(mode,"fork-token-safe")){LangGcProgramForkToken f{};lang_gc_program_fork_enter_v1(&f);safe_call();lang_gc_program_fork_leave_v1(&f);}
 else {
  const bool call_safe=!std::strcmp(mode,"language-prepare-safe");
  assert(pthread_atfork(call_safe?safe_call:callback,nullptr,nullptr)==0);
  const auto pid=!std::strcmp(mode,"control-libc-prepare-callback")?static_cast<int64_t>(fork()):lang_process_fork_v1();
  assert(pid>=0);if(!pid)_exit(0);int st=0;assert(waitpid(static_cast<pid_t>(pid),&st,0)==pid);assert(WIFEXITED(st)&&WEXITSTATUS(st)==0);
 }
 assert(lang_gc_program_finish_v1()==0);close(sink);write(1,"CALLBACK_FORK_PROBE_OK\n",23);
}

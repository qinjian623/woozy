#include "../../runtime/native/process.hpp"
#include "../../runtime/native/thread.hpp"
#include "../../runtime/native/alloc.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

namespace {
void need(bool b,const char *m){if(!b)throw std::runtime_error(m);}
LangText text(const char *s){return {reinterpret_cast<const uint8_t *>(s),std::strlen(s),std::strlen(s),nullptr};}
LangVec *bytes(const char *s,size_t n){auto *v=lang_vec_new_v1(n,1,nullptr,nullptr);for(size_t i=0;i<n;++i)lang_vec_init_v1(v,s+i);return v;}
LangVec *texts(std::initializer_list<const char *> strings){auto *v=lang_vec_new_v1(0,sizeof(LangText),nullptr,nullptr);for(auto s:strings){auto t=text(s);lang_vec_push_v1(v,&t);}return v;}
LangVec *raws(std::initializer_list<const char *> strings){auto *v=lang_vec_new_v1(0,sizeof(LangVec *),nullptr,nullptr);for(auto s:strings){auto *b=bytes(s,std::strlen(s));lang_vec_push_v1(v,&b);}return v;}
void drop_raws(LangVec *v){for(uint64_t i=0;i<v->length;++i)lang_vec_release_v1(static_cast<LangVec **>(v->data)[i]);lang_vec_release_v1(v);}
void drop_texts(LangVec *v){lang_vec_release_v1(v);}

void wait_ok(int64_t pid,int expected=0){int32_t status=-1;need(pid>0,"fork/spawn failed");need(lang_process_waitpid_v1(pid,0,&status)==pid,"waitpid failed");need(WIFEXITED(status)&&WEXITSTATUS(status)==expected,"child status differs");}
void snapshots(int argc,char **argv,char **env){
  lang_process_init_v1(argc,argv,env);
  LangVec *v;need(lang_process_args_v1(&v)==0&&v->length==static_cast<uint64_t>(argc),"args failed");drop_texts(v);
  const char *bad[]={"ok","\xff",nullptr};lang_process_init_v1(2,bad,env);
  need(lang_process_args_v1(&v)==-84,"invalid UTF8 accepted");
  need(lang_process_args_bytes_v1(&v)==0&&v->length==2,"raw args failed");lang_vec_release_v1(v);
  lang_process_init_v1(argc,argv,env);
  auto name=text("WOOZY_PROCESS_TEST"),value=text("emoji:\xf0\x9f\x98\x80");
  need(lang_process_setenv_v1(&name,&value,1)==0,"setenv failed");LangText got;
  need(lang_process_getenv_v1(&got,&name)==1&&got.bytes==value.bytes&&!std::memcmp(got.data,value.data,value.bytes),"getenv differs");lang_handle_release_v1(got.owner);
  auto other=text("keep");need(lang_process_setenv_v1(&name,&other,0)==0,"overwrite0 failed");
  auto *key=bytes("WOOZY_PROCESS_TEST",18),*invalid=bytes("\xff",1);
  need(lang_process_setenv_bytes_v1(key,invalid,1)==0,"raw setenv failed");
  need(lang_process_getenv_v1(&got,&name)==-84&&!got.owner,"invalid env UTF8 accepted");
  need(lang_process_environment_v1(&v)==-84,"invalid environment accepted");
  need(lang_process_getenv_bytes_v1(&v,key)==1&&v->length==1&&*static_cast<uint8_t *>(v->data)==255,"raw getenv failed");lang_vec_release_v1(v);
  need(lang_process_unsetenv_v1(&name)==0,"unsetenv failed");need(lang_process_getenv_v1(&got,&name)==0&&!got.owner,"missing getenv differs");
  lang_vec_release_v1(key);lang_vec_release_v1(invalid);
  need(lang_process_environment_bytes_v1(&v)==0,"env snapshot failed");
  if(v->length){auto *row=static_cast<LangVec **>(v->data)[0];if(row->length)*static_cast<uint8_t *>(row->data)=0;}
  lang_vec_release_v1(v);need(lang_process_environment_v1(&v)==0,"snapshot leaked mutation");drop_texts(v);
  auto badkey=text("a=b");need(lang_process_setenv_v1(&badkey,&value,1)==-22,"invalid env key accepted");
}
void exec_spawn(){
  auto path=text("/bin/sh"),missing=text("/definitely/missing/woozy");
  auto *args=texts({"sh","-c","printf '%s:%s' \"$1\" \"$WOOZY\"; printf err >&2; exit 7","sh","hello"});
  auto *env=texts({"WOOZY=world"});int output[2],error[2];need(pipe(output)==0&&pipe(error)==0,"pipe failed");
  auto pid=lang_process_spawn_v1(&path,args,env,-1,output[1],error[1]);close(output[1]);close(error[1]);
  char data[64]{};auto n=read(output[0],data,sizeof(data));need(n==11&&std::string(data,n)=="hello:world","redirect/env/argv differs");close(output[0]);
  n=read(error[0],data,sizeof(data));need(n==3&&std::string(data,n)=="err","stderr redirect differs");close(error[0]);wait_ok(pid,7);
  need(lang_process_spawn_v1(&missing,args,env,-1,-1,-1)==-2,"spawn lost exec errno");
  int32_t status;need(lang_process_waitpid_v1(-1,1,&status)==-10,"failed spawn left zombie");
  need(lang_process_spawn_v1(&path,args,env,-1,99999,-1)==-9,"bad redirect accepted");
  need(lang_process_execve_v1(&missing,args,env)==-2,"exec lost errno");
  auto *rawpath=bytes("/bin/sh",7),*rawargs=raws({"sh","-c","exit 3"}),*rawenv=raws({});
  pid=lang_process_spawn_bytes_v1(rawpath,rawargs,rawenv,-1,-1,-1);wait_ok(pid,3);
  lang_vec_release_v1(rawpath);drop_raws(rawargs);drop_raws(rawenv);
  auto *shortargs=texts({"sh","-c","exit 9"});pid=lang_process_fork_v1();
  if(!pid){auto r=lang_process_execve_v1(&path,shortargs,env);_exit(r<0?77:78);}wait_ok(pid,9);
  lang_vec_release_v1(shortargs);lang_vec_release_v1(args);lang_vec_release_v1(env);
}
void simultaneous_launches(){
  // Every runtime fork shares the launch lock with spawn, including the
  // lifetime of CLOEXEC error-pipe ends prepared in its parent.
  std::atomic<unsigned> errors=0;
  auto path=text("/bin/sh");auto *args=texts({"sh","-c","exit 0"}),*env=texts({});
  std::thread a([&]{for(unsigned i=0;i<30;++i){auto pid=lang_process_spawn_v1(&path,args,env,-1,-1,-1);int32_t status=-1;if(pid<0||lang_process_waitpid_v1(pid,0,&status)!=pid||status)++errors;}});
  std::thread b([&]{for(unsigned i=0;i<30;++i){auto pid=lang_process_fork_v1();if(!pid){usleep(1000);_exit(0);}int32_t status=-1;if(pid<0||lang_process_waitpid_v1(pid,0,&status)!=pid||status)++errors;}});
  a.join();b.join();need(!errors,"simultaneous spawn/fork failed");lang_vec_release_v1(args);lang_vec_release_v1(env);
}

void signals(){
  uint64_t old=0,pending=0;const uint64_t bit=uint64_t{1}<<(SIGUSR1-1);
  need(lang_signal_mask_v1(&old,0,bit)==0,"sigprocmask failed");need(kill(getpid(),SIGUSR1)==0,"kill failed");
  need(lang_signal_pending_v1(&pending)==0&&(pending&bit),"pending lost signal");LangSignalInfo info;
  need(lang_signal_wait_v1(&info,bit,1000000000)==0&&info.number==SIGUSR1&&info.pid==getpid(),"sigwait info differs");
  need(lang_signal_wait_v1(&info,bit,0)==-11,"signal timeout differs");
  uint64_t ignored;need(lang_signal_mask_v1(&ignored,2,old)==0,"restore mask failed");
  need(lang_signal_disposition_v1(SIGUSR2,1)==0,"ignore failed");kill(getpid(),SIGUSR2);
  need(lang_signal_disposition_v1(SIGUSR2,0)==0,"default failed");
  need(lang_signal_disposition_v1(SIGKILL,1)==-22,"SIGKILL changed");
}
int64_t gated(void *p) noexcept {auto *gate=*static_cast<uint32_t **>(p);while(!__atomic_load_n(gate,__ATOMIC_ACQUIRE))__asm__ volatile("pause");return 42;}
struct ForkInput{LangHandle *other;LangHandle **self;LangHandle *completed;};
int64_t runtime_fork(void *p) noexcept {
  const auto input=*static_cast<ForkInput *>(p);
  auto *other=input.other;LangHandle *self;
  while(!(self=__atomic_load_n(input.self,__ATOMIC_ACQUIRE)))__asm__ volatile("pause");
  auto pid=lang_process_fork_v1();
  if(!pid) {
    const auto r=lang_thread_wait_v1(other);
    const auto current=lang_thread_wait_v1(self);
    const bool cached=lang_thread_wait_v1(input.completed)==0&&lang_thread_result_v1(input.completed)==42;
    auto *v=lang_vec_new_v1(0,1,nullptr,nullptr);uint8_t b=1;lang_vec_push_v1(v,&b);lang_vec_release_v1(v);
    __asm__ volatile("syscall"::"a"(60L),"D"(r==-3&&current==-35&&cached?0L:81L):"rcx","r11","memory");__builtin_trap();
  }
  int32_t status=0;auto result=lang_process_waitpid_v1(pid,0,&status);
  return result==pid&&status==0?1:0;
}
void concurrent_fork(){
  std::atomic<bool> stop=false;
  std::thread allocator([&]{while(!stop.load()){void *p=lang_alloc_v1(32,8);lang_free_v1(p);}});
  uint32_t gate=0;auto *gatep=&gate;auto *other=lang_thread_spawn_v1(gated,&gatep,sizeof(gatep),nullptr,nullptr);
  need(lang_thread_error_v1(other)==0,"runtime thread failed");
  for(unsigned i=0;i<40;++i){auto pid=lang_process_fork_v1();if(!pid){for(int n=0;n<100;++n){auto *p=lang_alloc_v1(32,8);lang_free_v1(p);} _exit(lang_thread_wait_v1(other)==-3?0:80);}wait_ok(pid);}
  uint32_t ready=1;auto *readyp=&ready;
  auto *completed=lang_thread_spawn_v1(gated,&readyp,sizeof(readyp),nullptr,nullptr);
  need(lang_thread_wait_v1(completed)==0,"completed thread failed");
  LangHandle *self=nullptr;ForkInput input{other,&self,completed};
  auto *created=lang_thread_spawn_v1(runtime_fork,&input,sizeof(input),nullptr,nullptr);
  __atomic_store_n(&self,created,__ATOMIC_RELEASE);
  need(lang_thread_wait_v1(created)==0&&lang_thread_result_v1(created)==1,"fork inside runtime thread failed");lang_handle_release_v1(created);
  __atomic_store_n(&gate,1u,__ATOMIC_RELEASE);need(lang_thread_wait_v1(other)==0,"parent thread damaged");lang_handle_release_v1(other);
  lang_handle_release_v1(completed);
  stop=true;allocator.join();
}
}
int main(int argc,char **argv,char **env){try{snapshots(argc,argv,env);exec_spawn();signals();concurrent_fork();simultaneous_launches();std::cout<<"process runtime: ok\n";}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}

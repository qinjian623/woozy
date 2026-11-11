#include "../../runtime/native/thread.hpp"
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" [[noreturn]] void lang_thread_unmap_exit_v1(void *, uint64_t) noexcept;
namespace {
void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
void spin() noexcept { __asm__ volatile("pause" ::: "memory"); }
uint64_t load(const uint64_t *value) noexcept { return __atomic_load_n(value,__ATOMIC_ACQUIRE); }
void until(const auto &predicate, const std::string &message) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!predicate()) {
    need(std::chrono::steady_clock::now()<end,message);
    sched_yield();
  }
}
struct Counts { uint64_t copies=0,drops=0; };
struct Observation { uint64_t tcb=0,stack=0,canary=0; };
struct Input {
  int64_t value;
  uint32_t *gate;
  Counts *counts;
  Observation *observation;
};
void copy_input(void *pointer) {
  auto &input=*static_cast<Input *>(pointer);
  __atomic_fetch_add(&input.counts->copies,1,__ATOMIC_RELAXED);
}
void drop_input(void *pointer) {
  auto &input=*static_cast<Input *>(pointer);
  __atomic_fetch_add(&input.counts->drops,1,__ATOMIC_RELEASE);
}
__attribute__((noinline)) int64_t entry(void *pointer) noexcept {
  const auto input=*static_cast<Input *>(pointer);
  volatile char guarded[128];
  guarded[0]=3;guarded[127]=5;
  if(input.observation) {
    uint64_t tcb,canary;
    __asm__ volatile("movq %%fs:0,%0; movq %%fs:0x28,%1":"=r"(tcb),"=r"(canary));
    input.observation->stack=reinterpret_cast<uint64_t>(&guarded[0]);
    input.observation->canary=canary;
    __atomic_store_n(&input.observation->tcb,tcb,__ATOMIC_RELEASE);
  }
  if(input.gate) while(!__atomic_load_n(input.gate,__ATOMIC_ACQUIRE)) spin();
  return input.value+guarded[0]+guarded[127];
}
LangHandle *start(Input &input) {
  auto *thread=lang_thread_spawn_v1(entry,&input,sizeof(input),copy_input,drop_input);
  need(lang_thread_error_v1(thread)==0,"thread creation failed");
  return thread;
}
bool unmapped(uint64_t address) {
  unsigned char residency=0;
  errno=0;
  const int result=mincore(reinterpret_cast<void *>(address&~uint64_t{4095}),4096,&residency);
  return result==-1&&errno==ENOMEM;
}
std::string permissions(uint64_t address) {
  std::ifstream maps("/proc/self/maps");
  for(std::string line;std::getline(maps,line);) {
    uint64_t first=0,last=0;char permission[5]{};
    if(sscanf(line.c_str(),"%lx-%lx %4s",&first,&last,permission)==3&&address>=first&&address<last)
      return permission;
  }
  return {};
}
void panic(const std::string &name, const auto &operation) {
  int pipe_fds[2];need(pipe(pipe_fds)==0,"pipe failed");
  const pid_t child=fork();need(child>=0,"fork failed");
  if(!child) {
    close(pipe_fds[0]);dup2(pipe_fds[1],2);close(pipe_fds[1]);
    operation();_exit(77);
  }
  close(pipe_fds[1]);
  std::string error;char bytes[256];
  for(ssize_t size;(size=read(pipe_fds[0],bytes,sizeof(bytes)))>0;) error.append(bytes,size);
  close(pipe_fds[0]);int status=0;need(waitpid(child,&status,0)==child,"waitpid failed");
  need(WIFEXITED(status)&&WEXITSTATUS(status)==101&&error.find("LANG_NATIVE_PANIC:v1:runtime-handle-misuse:")!=std::string::npos,
       name+" did not produce the expected Panic record: "+error);
}
bool deny_syscall(unsigned number) {
  sock_filter filter[]={BPF_STMT(BPF_LD|BPF_W|BPF_ABS,0),BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,number,0,1),
                        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM),BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)};
  sock_fprog program{4,filter};
  return prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0&&prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program)==0;
}
struct JoinInput { LangHandle *target;int64_t extra; };
void copy_join(void *pointer) {lang_handle_retain_v1(static_cast<JoinInput *>(pointer)->target);}
void drop_join(void *pointer) {lang_handle_release_v1(static_cast<JoinInput *>(pointer)->target);}
int64_t join_entry(void *pointer) noexcept {
  auto &input=*static_cast<JoinInput *>(pointer);
  const auto result=lang_thread_wait_v1(input.target);
  return result?result:lang_thread_result_v1(input.target)+input.extra;
}
void no_destroy(LangHandle *) noexcept {}
uint64_t destroyed=0;
void count_destroy(LangHandle *) noexcept {__atomic_fetch_add(&destroyed,1,__ATOMIC_RELEASE);}
int64_t retain_entry(void *pointer) noexcept {
  auto *handle=static_cast<JoinInput *>(pointer)->target;
  for(size_t i=0;i<20000;++i) {lang_handle_retain_v1(handle);lang_handle_release_v1(handle);}
  return 1;
}
int64_t self_entry(void *pointer) noexcept {
  auto **slot=*static_cast<LangHandle ***>(pointer);
  LangHandle *thread;
  while(!(thread=__atomic_load_n(slot,__ATOMIC_ACQUIRE))) spin();
  return lang_thread_wait_v1(thread);
}
void suite() {
  uint64_t parent_tcb,parent_canary;
  __asm__ volatile("movq %%fs:0,%0;movq %%fs:0x28,%1":"=r"(parent_tcb),"=r"(parent_canary));
  Counts counts;uint32_t gate=0;Observation observations[4];LangHandle *threads[4];
  for(size_t i=0;i<4;++i) {
    Input input{int64_t(i),&gate,&counts,&observations[i]};threads[i]=start(input);
  }
  for(auto &observation:observations) until([&]{return load(&observation.tcb)!=0;},"entry did not start");
  for(size_t i=0;i<4;++i) {
    need(observations[i].tcb!=parent_tcb&&observations[i].canary==parent_canary&&parent_canary,"thread TLS/stack canary invalid");
    for(size_t j=0;j<i;++j) need(observations[i].tcb!=observations[j].tcb,"threads share a TCB");
    need(permissions(observations[i].tcb-LANG_THREAD_STACK_BYTES-4096).starts_with("---"),"stack guard page missing");
  }
  __atomic_store_n(&gate,1,__ATOMIC_RELEASE);
  for(size_t i=0;i<4;++i) {
    need(lang_thread_wait_v1(threads[i])==0&&lang_thread_result_v1(threads[i])==int64_t(i)+8,"thread result invalid");
    need(lang_thread_wait_v1(threads[i])==0,"repeat join failed");lang_handle_release_v1(threads[i]);
  }
  need(load(&counts.copies)==4&&load(&counts.drops)==4,"context copy/drop count invalid");

  Counts shared_counts;uint32_t shared_gate=0;Input shared{34,&shared_gate,&shared_counts,nullptr};
  auto *target=start(shared);LangHandle *joiners[8];
  for(size_t i=0;i<8;++i) {
    JoinInput input{target,int64_t(i)};
    joiners[i]=lang_thread_spawn_v1(join_entry,&input,sizeof(input),copy_join,drop_join);
    need(lang_thread_error_v1(joiners[i])==0,"joiner creation failed");
  }
  lang_handle_release_v1(target);__atomic_store_n(&shared_gate,1,__ATOMIC_RELEASE);
  for(size_t i=0;i<8;++i) {
    need(lang_thread_wait_v1(joiners[i])==0&&lang_thread_result_v1(joiners[i])==42+int64_t(i),"concurrent alias join failed");
    lang_handle_release_v1(joiners[i]);
  }
  need(load(&shared_counts.drops)==1,"join returned before context cleanup");

  LangHandle sample{1,LangHandleKind::atomic_i64,count_destroy};LangHandle *retainers[8];
  for(auto &retainer:retainers) {
    JoinInput input{&sample,0};retainer=lang_thread_spawn_v1(retain_entry,&input,sizeof(input),copy_join,drop_join);
    need(lang_thread_error_v1(retainer)==0,"retainer creation failed");
  }
  for(auto *retainer:retainers) {need(lang_thread_wait_v1(retainer)==0,"retainer wait failed");lang_handle_release_v1(retainer);}
  need(__atomic_load_n(&sample.refs,__ATOMIC_ACQUIRE)==1,"handle ARC count lost updates");
  lang_handle_release_v1(&sample);need(load(&destroyed)==1,"handle destroyed more than once");

  LangHandle *self=nullptr;auto **self_slot=&self;
  auto *self_thread=lang_thread_spawn_v1(self_entry,&self_slot,sizeof(self_slot),nullptr,nullptr);
  need(lang_thread_error_v1(self_thread)==0,"self-join creation failed");
  __atomic_store_n(&self,self_thread,__ATOMIC_RELEASE);
  need(lang_thread_wait_v1(self_thread)==0&&lang_thread_result_v1(self_thread)==-35,"self join did not return EDEADLK");
  lang_handle_release_v1(self_thread);

  Counts background_counts;uint32_t background_gate=0;Observation background;
  Input input{7,&background_gate,&background_counts,&background};auto *detached=start(input);
  until([&]{return load(&background.tcb)!=0;},"background thread did not start");
  const auto state_address=reinterpret_cast<uint64_t>(detached);
  lang_handle_release_v1(detached);__atomic_store_n(&background_gate,1,__ATOMIC_RELEASE);
  until([&]{return load(&background_counts.drops)==1&&unmapped(state_address)&&unmapped(background.tcb)&&unmapped(background.stack);},
        "last-handle drop did not release state, stack and TCB");

  panic("wrong kind",[]{LangHandle handle{1,LangHandleKind::mutex,no_destroy};lang_thread_wait_v1(&handle);});
  panic("zero reference retain",[]{LangHandle handle{0,LangHandleKind::atomic_i64,no_destroy};lang_handle_retain_v1(&handle);});
  panic("munmap epilogue failure",[]{lang_thread_unmap_exit_v1(nullptr,0);});
  panic("early result",[]{Counts counts;uint32_t gate=0;Input input{0,&gate,&counts,nullptr};auto *thread=start(input);lang_thread_result_v1(thread);});
  panic("signal blocking failure",[]{
    if(!deny_syscall(14)) _exit(78);
    Counts counts;Input input{0,nullptr,&counts,nullptr};auto *thread=start(input);lang_thread_wait_v1(thread);_exit(79);
  });
  {
    const auto child=fork();need(child>=0,"wait error fork failed");
    if(!child) {
      if(!deny_syscall(186)) _exit(82);
      Counts counts;uint32_t gate=0;Input input{0,&gate,&counts,nullptr};auto *thread=start(input);
      const auto error=lang_thread_wait_v1(thread);lang_handle_release_v1(thread);
      __atomic_store_n(&gate,1,__ATOMIC_RELEASE);_exit(error==-EPERM?0:83);
    }
    int status=0;need(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"gettid failure was not returned by wait");
  }
  if(getuid()!=0) {
    const auto child=fork();need(child>=0,"clone failure fork failed");
    if(!child) {
      rlimit limit{0,0};if(setrlimit(RLIMIT_NPROC,&limit)) _exit(80);
      Counts counts;Input input{0,nullptr,&counts,nullptr};
      auto *thread=lang_thread_spawn_v1(entry,&input,sizeof(input),copy_input,drop_input);
      const auto error=lang_thread_error_v1(thread);
      const bool valid=error<0&&lang_thread_wait_v1(thread)==error&&load(&counts.copies)==1&&load(&counts.drops)==1;
      lang_handle_release_v1(thread);_exit(valid?0:81);
    }
    int status=0;need(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"clone failure did not preserve error/drop");
  }
  std::cout<<"passed: independent TLS/canaries/guard pages, joins, ARC, self-join, detached mmap cleanup, clone errors and 5 Panic paths\n";
}
} // namespace
int main() {try{suite();}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}

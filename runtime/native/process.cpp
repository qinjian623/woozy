#include "process.hpp"
#include "alloc.hpp"
#include "gc_program.hpp"
#include "thread.hpp"
#include "weak.hpp"
#include "linux.hpp"
#include <cstddef>
#include <initializer_list>
#if defined(LANG_RUNTIME_HOSTED)
#include "../hosted/process.hpp"
#endif

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t,uint64_t,uint64_t,uint64_t) noexcept;
// Optional private async fork hooks. They are weak so a program that links only
// process does not pull runtime/native/async.cpp into the image. When async is
// absent there is no executor, no async epoll and no fork gate to restore.
extern "C" {
int64_t lang_async_fork_reserve_v1(int32_t) noexcept __attribute__((weak));
void lang_async_fork_release_v1() noexcept __attribute__((weak));
void lang_async_fork_prepare_v1(int32_t) noexcept __attribute__((weak));
void lang_async_fork_parent_v1(int32_t) noexcept __attribute__((weak));
void lang_async_fork_child_v1(int32_t) noexcept __attribute__((weak));
[[noreturn]] void lang_async_fork_refuse_v1() noexcept __attribute__((weak));
}
#include "process_values.inc"

namespace {
namespace linux_abi = lang::native::linux_x86_64;
int64_t sys(long n,long a=0,long b=0,long c=0,long d=0,long e=0,long f=0) noexcept {
  return linux_abi::syscall6(n,a,b,c,d,e,f);
}
long addr(const void *p) noexcept {return reinterpret_cast<long>(p);}

int64_t async_fork_reserve(bool exec_only) noexcept {
  return lang_async_fork_reserve_v1
             ? lang_async_fork_reserve_v1(exec_only ? 1 : 0)
             : 0;
}
void async_fork_release() noexcept {
  if (lang_async_fork_release_v1) lang_async_fork_release_v1();
}
void async_fork_prepare(bool exec_only) noexcept {
  if (lang_async_fork_prepare_v1) lang_async_fork_prepare_v1(exec_only ? 1 : 0);
}
void async_fork_parent(bool exec_only) noexcept {
  if (lang_async_fork_parent_v1) lang_async_fork_parent_v1(exec_only ? 1 : 0);
}
void async_fork_child(bool exec_only) noexcept {
  if (lang_async_fork_child_v1) lang_async_fork_child_v1(exec_only ? 1 : 0);
}
#if defined(LANG_RUNTIME_HOSTED)
[[noreturn]] void async_fork_refuse_foreign_executor() noexcept {
  // The hosted atfork prepare is void, so direct libc fork cannot return a
  // Source os.Error. Record the exact restriction and terminate instead of
  // pretending the call threw or copying an executor owned by a vanished
  // thread.
  if (lang_async_fork_refuse_v1) lang_async_fork_refuse_v1();
  lang_native_panic_v1(10, 0, 0, 0);
}
#endif

uint32_t environment_lock;
uint32_t launch_lock;
#if defined(LANG_RUNTIME_HOSTED)
// Only libc-managed threads use TLS. The freestanding branch must not access
// C++ TLS through its private FS/TCB.
thread_local bool hosted_launch_owned;
thread_local bool hosted_fork_owns_launch;
thread_local bool hosted_fork_locks_held;
// The program fork scope may be entered by the language spawn/fork wrapper
// before launch, or by our atfork prepare for a direct libc fork. The token is
// thread-scoped because an atfork handler has no parameter list.
thread_local LangGcProgramForkToken hosted_fork_token;
thread_local bool hosted_fork_scope_owned;
thread_local bool hosted_fork_atfork_scope;
thread_local bool hosted_gc_fork_hooks;
// Per-thread fork state for the optional async hooks. `reserved_outer` is set
// by a Source language fork before its LaunchGuard; `gate_held` records that
// this atfork prepare acquired the reservation itself (direct libc fork);
// `async_held` records that the executor ticket lock was taken and must be
// released by the parent/child hook.
thread_local bool hosted_fork_exec_only;
thread_local bool hosted_fork_reserved_outer;
// Set by a Source lang_process_fork_v1 after its outer reservation succeeds.
// The runtime parent hook releases this reservation before user parent
// handlers; the wrapper only releases it if the hook could not run.
thread_local bool hosted_fork_outer_owned;
thread_local bool hosted_fork_gate_held;
thread_local bool hosted_fork_async_held;
#endif
void launch_acquire() noexcept {
  while(__atomic_exchange_n(&launch_lock,1u,__ATOMIC_ACQUIRE))
    while(__atomic_load_n(&launch_lock,__ATOMIC_RELAXED))__asm__ volatile("pause");
#if defined(LANG_RUNTIME_HOSTED)
  hosted_launch_owned=true;
#endif
}
void launch_release() noexcept {
#if defined(LANG_RUNTIME_HOSTED)
  hosted_launch_owned=false;
#endif
  __atomic_store_n(&launch_lock,0u,__ATOMIC_RELEASE);
}
struct LaunchGuard {
  LaunchGuard() noexcept {launch_acquire();}
  ~LaunchGuard(){
#if defined(LANG_RUNTIME_HOSTED)
    // The child handler clears this thread's ownership. Do not unlock a lock
    // subsequently acquired by another thread in the child.
    if(!hosted_launch_owned)return;
#endif
    launch_release();
  }
};
void lock() noexcept {
  while(__atomic_exchange_n(&environment_lock,1u,__ATOMIC_ACQUIRE))
    while(__atomic_load_n(&environment_lock,__ATOMIC_RELAXED))__asm__ volatile("pause");
}
void unlock() noexcept {__atomic_store_n(&environment_lock,0u,__ATOMIC_RELEASE);}
#if defined(LANG_RUNTIME_HOSTED)
// Per-thread state makes a failed-fork fallback idempotent even when another
// thread has already entered its own atfork prepare handler.
void hosted_fork_parent_release() noexcept {
  if (hosted_fork_async_held) {
    hosted_fork_async_held = false;
    async_fork_parent(false);
  }
  if (hosted_fork_locks_held) {
    hosted_fork_locks_held=false;
    // Reverse the prepare order: runtime -> world -> gc -> Weak -> allocator.
    if (hosted_gc_fork_hooks) {
      auto runtime_parent = lang::native::gc_program::hooks_v1.runtime_fork_parent;
      auto world_parent = lang::native::gc_program::hooks_v1.world_fork_parent;
      auto gc_parent = lang::native::gc_program::hooks_v1.gc_fork_parent;
      if (runtime_parent) runtime_parent();
      if (world_parent) world_parent();
      if (gc_parent) gc_parent();
      hosted_gc_fork_hooks = false;
    }
    lang_weak_fork_parent_v1();
    lang_alloc_fork_parent_v1();
    unlock();
  }
  if(hosted_fork_owns_launch) {
    hosted_fork_owns_launch=false;
    launch_release();
  }
  // The reservation is outer to every internal lock. An atfork-acquired
  // direct-fork reservation is released here; a Source fork's outer reservation
  // is also released here when the runtime parent hook runs, with the wrapper
  // keeping an idempotent fallback for paths where it did not.
  if (hosted_fork_gate_held) {
    hosted_fork_gate_held = false;
    async_fork_release();
  }
  // A Source wrapper's reservation only protects the executor-TID decision and
  // the actual fork. Release it here, before user parent handlers run, so a
  // parent callback cannot be blocked by a binding reservation that no longer
  // protects anything. The wrapper's post-fork fallback is idempotent.
  if (hosted_fork_outer_owned) {
    hosted_fork_outer_owned = false;
    hosted_fork_reserved_outer = false;
    async_fork_release();
  }
}

void hosted_enter_fork_scope() noexcept {
  if (hosted_fork_scope_owned) return;
  auto enter = lang::native::gc_program::hooks_v1.fork_enter;
  if (!enter) return;
  enter(&hosted_fork_token);
  hosted_fork_scope_owned = true;
  hosted_fork_atfork_scope = true;
}

void hosted_leave_fork_scope() noexcept {
  if (!hosted_fork_atfork_scope) return;
  auto leave = lang::native::gc_program::hooks_v1.fork_leave;
  if (leave) leave(&hosted_fork_token);
  hosted_fork_scope_owned = false;
  hosted_fork_atfork_scope = false;
}
#endif

// One stack-owned scope for a language spawn/fork. In hosted builds an atfork
// handler may already have entered a scope; it must not duplicate the no-park
// interval or the SAFE suspension.
struct ForkScope {
  LangGcProgramForkToken token{};
#if defined(LANG_RUNTIME_HOSTED)
  bool owns = false;
#endif
  ForkScope() noexcept {
    auto enter = lang::native::gc_program::hooks_v1.fork_enter;
    if (!enter) return;
#if defined(LANG_RUNTIME_HOSTED)
    if (hosted_fork_scope_owned) return;
    enter(&token);
    hosted_fork_scope_owned = true;
    owns = true;
#else
    enter(&token);
#endif
  }
  ~ForkScope() noexcept {
    auto leave = lang::native::gc_program::hooks_v1.fork_leave;
    if (!leave) return;
#if defined(LANG_RUNTIME_HOSTED)
    if (!owns) return;
    leave(&token);
    hosted_fork_scope_owned = false;
#else
    leave(&token);
#endif
  }
  ForkScope(const ForkScope &) = delete;
  ForkScope &operator=(const ForkScope &) = delete;
};
bool gc_fork_ready() noexcept {
  auto ready = lang::native::gc_program::hooks_v1.fork_ready;
  return ready && ready() != 0;
}
// `exec_only` marks the internal spawn child: its executor is never continued
// or cleaned and a foreign executor is allowed. `reserved` says the caller
// already holds the async fork reservation (the Source language fork takes it
// before LaunchGuard so a direct fork's reservation cannot deadlock with
// launch).
int64_t fork_process(bool exec_only,bool reserved) noexcept {
#if defined(LANG_RUNTIME_HOSTED)
  // libc fork runs glibc's internal handlers and the atfork hooks registered
  // by runtime/hosted/process.cpp.  The hooks acquire/release our environment
  // and allocator locks; launch is held by the caller (spawn or
  // lang_process_fork_v1).  Only the calling thread survives in the child.
  // The atfork prepare sees the exec-only flag and takes the async reservation
  // there before launch for a direct libc fork. A Source fork already reserved
  // before its LaunchGuard.
  hosted_fork_exec_only = exec_only;
  hosted_fork_reserved_outer = reserved;
  auto pid=lang_hosted_fork_v1();
  hosted_fork_exec_only = false;
  hosted_fork_reserved_outer = false;
  if(pid<0) {
    hosted_fork_parent_release();
    return pid;
  }
  if(!pid) {
    // The child handler already reset the locks, before user child handlers.
    // Resetting again here could release a lock acquired after that reset.
    lang_thread_fork_child_v1();
  }
  return pid;
#else
  bool owns_reservation = false;
  if (!exec_only && !reserved) {
    const auto status = async_fork_reserve(false);
    if (status < 0) return status;
    owns_reservation = true;
  }
  lock();lang_alloc_fork_prepare_v1();lang_weak_fork_prepare_v1();
  const bool gc_fork_hooks = gc_fork_ready();
  if (gc_fork_hooks) {
    auto gc_prepare = lang::native::gc_program::hooks_v1.gc_fork_prepare;
    auto world_prepare = lang::native::gc_program::hooks_v1.world_fork_prepare;
    auto runtime_prepare = lang::native::gc_program::hooks_v1.runtime_fork_prepare;
    if (gc_prepare) gc_prepare();
    if (world_prepare) world_prepare();
    if (runtime_prepare) runtime_prepare();
  }
  // Async is the last prepare lock. For ordinary fork it acquires the executor
  // ticket lock; for exec-only it only records the parent tid. NoGraph programs
  // still take this path because scalar/Unit Tasks also bind the executor and
  // own the async epoll.
  async_fork_prepare(exec_only);
  auto pid=sys(57);
  if(!pid) {
    lang_alloc_fork_child_v1();
    lang_weak_fork_child_v1();
    if (gc_fork_hooks) {
      auto gc_child = lang::native::gc_program::hooks_v1.gc_fork_child;
      auto world_child = lang::native::gc_program::hooks_v1.world_fork_child;
      auto runtime_child = lang::native::gc_program::hooks_v1.runtime_fork_child;
      if (gc_child) gc_child();
      if (world_child) world_child();
      if (runtime_child) runtime_child();
    }
    // Only now reset async ticket/TID/epoll state; the inherited allocator and
    // GC locks are already consistent with the single surviving thread.
    async_fork_child(exec_only);
    __atomic_store_n(&launch_lock,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&environment_lock,0u,__ATOMIC_RELAXED);
    lang_thread_fork_child_v1();
  } else {
    // Reverse order: async -> runtime -> world -> gc -> Weak -> allocator ->
    // environment.
    async_fork_parent(exec_only);
    if (gc_fork_hooks) {
      auto runtime_parent = lang::native::gc_program::hooks_v1.runtime_fork_parent;
      auto world_parent = lang::native::gc_program::hooks_v1.world_fork_parent;
      auto gc_parent = lang::native::gc_program::hooks_v1.gc_fork_parent;
      if (runtime_parent) runtime_parent();
      if (world_parent) world_parent();
      if (gc_parent) gc_parent();
    }
    lang_weak_fork_parent_v1();lang_alloc_fork_parent_v1();unlock();
  }
  if (owns_reservation) async_fork_release();
  return pid;
#endif
}
[[noreturn]] void child_exit(int status) noexcept {sys(60,status);__builtin_trap();}
void reap(int32_t pid) noexcept {while(sys(61,pid,0,0)==-4){}}
int64_t execute(Bytes path,const LangVec *args,const LangVec *env,bool raw) noexcept {
  Exec e;auto status=e.prepare(path,args,env,raw);if(status<0)return status;
  return sys(59,addr(e.path),addr(e.arguments),addr(e.environment));
}
int64_t spawn(Bytes path,const LangVec *args,const LangVec *env,bool raw,int32_t in,int32_t out,int32_t err) noexcept {
  // Enter the graph-stable fork scope before taking launch, so a pending stop
  // parks us without any runtime lock. It remains held through the descriptor
  // handshake because the parent still holds launch while reading the exec
  // error pipe; this is deliberately not a SAFE region.
  ForkScope fork_scope;
  // Serialize every runtime fork until temporary descriptors are closed;
  // another child must not retain our exec-error pipe writer.
  LaunchGuard launch;
  Exec e;auto status=e.prepare(path,args,env,raw);if(status<0)return status;
  const int32_t sources[3]={in,out,err};int32_t copies[3]={-1,-1,-1};int32_t pipe[2]={-1,-1};
  // Validate before allocating any descriptor, including when a closed source
  // could otherwise acquire the same number as a newly-created pipe end.
  for(auto fd:sources)if(fd< -1)return -9;else if(fd>=0) {auto r=sys(72,fd,1);if(r<0)return r;}
  for(unsigned i=0;i<3;++i)if(sources[i]>=0) {
    auto r=sys(72,sources[i],1030,3);
    if(r<0) {status=r;goto cleanup;}
    copies[i]=static_cast<int32_t>(r);
  }
  status=sys(293,addr(pipe),0x80000);if(status<0)goto cleanup;
  for(unsigned i=0;i<2;++i)if(pipe[i]<3) {
    auto r=sys(72,pipe[i],1030,3);if(r<0) {status=r;goto cleanup;}
    sys(3,pipe[i]);pipe[i]=static_cast<int32_t>(r);
  }
  {
    const auto pid=fork_process(true,false);
    if(pid<0) {status=pid;goto cleanup;}
    if(!pid) {
      sys(3,pipe[0]);
      int32_t failure=0;
      for(unsigned i=0;i<3;++i)if(copies[i]>=0) {
        auto r=sys(33,copies[i],i);if(r<0) {failure=static_cast<int32_t>(-r);break;}
      }
      for(auto fd:copies)if(fd>=0)sys(3,fd);
      if(!failure)failure=static_cast<int32_t>(-sys(59,addr(e.path),addr(e.arguments),addr(e.environment)));
      const auto *p=reinterpret_cast<const uint8_t *>(&failure);uint64_t written=0;
      while(written<sizeof(failure)) {
        auto r=sys(1,pipe[1],addr(p+written),sizeof(failure)-written);
        if(r==-4)continue;
        if(r<=0)break;
        written+=static_cast<uint64_t>(r);
      }
      child_exit(127);
    }
    sys(3,pipe[1]);pipe[1]=-1;
    int32_t failure=0;uint64_t used=0;
    while(used<sizeof(failure)) {
      auto r=sys(0,pipe[0],addr(reinterpret_cast<uint8_t *>(&failure)+used),sizeof(failure)-used);
      if(r==-4)continue;
      if(r<0) {sys(62,pid,9);reap(static_cast<int32_t>(pid));status=r;goto cleanup;}
      if(!r)break;
      used+=static_cast<uint64_t>(r);
    }
    if(used) {reap(static_cast<int32_t>(pid));status=used==sizeof(failure)?-failure:-5;}
    else status=pid;
  }
cleanup:
  for(auto fd:copies)if(fd>=0)sys(3,fd);
  for(auto fd:pipe)if(fd>=0)sys(3,fd);
  return status;
}
} // namespace
#if defined(LANG_RUNTIME_HOSTED)
extern "C" void lang_hosted_process_atfork_prepare_v1() noexcept {
  // Direct libc fork enters the graph-stable scope before acquiring launch.
  // The language spawn/fork wrapper already entered it before its LaunchGuard.
  hosted_enter_fork_scope();
  const bool exec_only = hosted_fork_exec_only;
  // A Source fork took the reservation before its LaunchGuard. A direct libc
  // fork must take it here, still before launch, so it cannot deadlock with a
  // Source fork that already holds launch while waiting for the reservation.
  // The void atfork prepare cannot return -EBUSY, so a foreign executor is a
  // recorded terminal Panic.
  if (!exec_only && !hosted_fork_reserved_outer) {
    const auto reserved = async_fork_reserve(false);
    if (reserved < 0) async_fork_refuse_foreign_executor();
    hosted_fork_gate_held = true;
  }
  // Language fork/spawn already holds launch through pipe cleanup. Direct
  // libc fork must also wait until those temporary descriptors are closed.
  if(!hosted_launch_owned) {
    launch_acquire();
    hosted_fork_owns_launch=true;
  }
  lock();
  lang_alloc_fork_prepare_v1();
  lang_weak_fork_prepare_v1();
  hosted_gc_fork_hooks = gc_fork_ready();
  if (hosted_gc_fork_hooks) {
    auto gc_prepare = lang::native::gc_program::hooks_v1.gc_fork_prepare;
    auto world_prepare = lang::native::gc_program::hooks_v1.world_fork_prepare;
    auto runtime_prepare = lang::native::gc_program::hooks_v1.runtime_fork_prepare;
    if (gc_prepare) gc_prepare();
    if (world_prepare) world_prepare();
    if (runtime_prepare) runtime_prepare();
  }
  // Async is the last prepare lock (ordinary fork) or just records the parent
  // tid (exec-only spawn). NoGraph programs still pass through here.
  async_fork_prepare(exec_only);
  if (!exec_only) hosted_fork_async_held = true;
  hosted_fork_locks_held=true;
}
extern "C" void lang_hosted_process_atfork_parent_v1() noexcept {
  hosted_fork_parent_release();
  hosted_leave_fork_scope();
}
extern "C" void lang_hosted_process_atfork_child_v1() noexcept {
  hosted_fork_locks_held=false;
  hosted_fork_owns_launch=false;
  hosted_launch_owned=false;
  lang_alloc_fork_child_v1();
  lang_weak_fork_child_v1();
  if (hosted_gc_fork_hooks) {
    auto gc_child = lang::native::gc_program::hooks_v1.gc_fork_child;
    auto world_child = lang::native::gc_program::hooks_v1.world_fork_child;
    auto runtime_child = lang::native::gc_program::hooks_v1.runtime_fork_child;
    if (gc_child) gc_child();
    if (world_child) world_child();
    if (runtime_child) runtime_child();
    hosted_gc_fork_hooks = false;
  }
  // Async only after allocator/Weak/GC/world/runtime, and before the process
  // launch/environment locks are reset. The child hook resets both ticket words,
  // clears the gate and closes only its inherited epoll descriptor.
  async_fork_child(hosted_fork_exec_only);
  hosted_fork_async_held = false;
  hosted_fork_gate_held = false;
  hosted_fork_outer_owned = false;
  // The child no longer owns the parent's outer reservation (async_child reset
  // the gate). Clear the thread-local pairing before user child handlers run,
  // otherwise a nested direct libc fork from such a handler would skip its own
  // reservation and then fail the prepare assertion.
  hosted_fork_reserved_outer = false;
  __atomic_store_n(&environment_lock,0u,__ATOMIC_RELAXED);
  __atomic_store_n(&launch_lock,0u,__ATOMIC_RELAXED);
  // hosted/thread.cpp's own atfork child hook updates the pthread state.
  hosted_leave_fork_scope();
}
#endif
extern "C" int64_t lang_process_fork_v1() noexcept {
  ForkScope fork_scope;
  // Reserve before LaunchGuard. The reservation check and the fork itself must
  // not interleave with a concurrent first executor bind; taking it here also
  // prevents a direct hosted fork (which reserves before launch inside its
  // atfork prepare) from waiting on launch while a Source fork waits on that
  // reservation.
  const auto reserved = async_fork_reserve(false);
  if (reserved < 0) return reserved;
#if defined(LANG_RUNTIME_HOSTED)
  // The hosted runtime parent hook releases this reservation before user
  // parent handlers. Keep the wrapper fallback for a fork path whose parent
  // hook did not run.
  hosted_fork_outer_owned = true;
#endif
  int64_t result;
  {
    LaunchGuard launch;
    result = fork_process(false, true);
  }
  // The child hook already reset the gate; releasing here is a no-op there.
#if defined(LANG_RUNTIME_HOSTED)
  if (hosted_fork_outer_owned) {
    hosted_fork_outer_owned = false;
    async_fork_release();
  }
#else
  async_fork_release();
#endif
  return result;
}
extern "C" int64_t lang_process_waitpid_v1(int32_t pid,int32_t options,int32_t *status) noexcept {
  if(!status)return -14;
  int32_t result=0;int64_t r;
  {
    lang::native::gc_program::SafeGuard safe;
    r=sys(61,pid,addr(&result),options);
  }
  if(r>0)*status=result;else if(!r)*status=0;
  return r;
}
extern "C" int64_t lang_process_execve_v1(const LangText *path,const LangVec *args,const LangVec *env) noexcept {
  Bytes p;auto status=text_view(path,p);return status<0?status:execute(p,args,env,false);
}
extern "C" int64_t lang_process_execve_bytes_v1(const LangVec *path,const LangVec *args,const LangVec *env) noexcept {
  Bytes p;auto status=byte_view(path,p);return status<0?status:execute(p,args,env,true);
}
extern "C" int64_t lang_process_spawn_v1(const LangText *path,const LangVec *args,const LangVec *env,int32_t in,int32_t out,int32_t err) noexcept {
  Bytes p;auto status=text_view(path,p);return status<0?status:spawn(p,args,env,false,in,out,err);
}
extern "C" int64_t lang_process_spawn_bytes_v1(const LangVec *path,const LangVec *args,const LangVec *env,int32_t in,int32_t out,int32_t err) noexcept {
  Bytes p;auto status=byte_view(path,p);return status<0?status:spawn(p,args,env,true,in,out,err);
}
extern "C" int64_t lang_signal_mask_v1(uint64_t *old,int32_t how,uint64_t mask) noexcept {
  if(!old)return -14;
  return sys(14,how,addr(&mask),addr(old),8);
}
extern "C" int64_t lang_signal_pending_v1(uint64_t *out) noexcept {
  return out?sys(127,addr(out),8):-14;
}
extern "C" int64_t lang_signal_wait_v1(LangSignalInfo *out,uint64_t mask,int64_t timeout) noexcept {
  if(!out)return -14;
  if(timeout< -1)return -22;
  alignas(8) uint8_t info[128]{};
  const int64_t time[2]={timeout/1000000000,timeout%1000000000};
  int64_t result;
  {
    lang::native::gc_program::SafeGuard safe;
    result=sys(128,addr(&mask),addr(info),timeout<0?0:addr(time),8);
  }
  if(result<0)return result;
  auto load32=[&](unsigned off) {int32_t n;copy(&n,info+off,4);return n;};
  // SIGCHLD shares the pid/uid prefix, followed by status. Queued signals
  // carry their sigval in that same union slot at byte 24.
  int64_t value=0;copy(&value,info+24,8);
  *out={load32(0),load32(8),load32(16),static_cast<uint32_t>(load32(20)),load32(24),0,value};
  return 0;
}
extern "C" int64_t lang_signal_disposition_v1(int32_t number,int32_t action) noexcept {
  if(number<1 || number>64 || number==9 || number==19 || (action!=0 && action!=1))return -22;
  struct Action {uint64_t handler,flags,restorer,mask;} value{static_cast<uint64_t>(action),0,0,0};
  return sys(13,number,addr(&value),0,8);
}

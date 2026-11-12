#include "process.hpp"
#include "alloc.hpp"
#include "thread.hpp"
#include "weak.hpp"
#include "linux.hpp"
#include <cstddef>
#include <initializer_list>
#if defined(LANG_RUNTIME_HOSTED)
#include "../hosted/process.hpp"
#endif

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t,uint64_t,uint64_t,uint64_t) noexcept;
namespace {
namespace linux = lang::native::linux_x86_64;
int64_t sys(long n,long a=0,long b=0,long c=0,long d=0,long e=0,long f=0) noexcept {
  return linux::syscall6(n,a,b,c,d,e,f);
}
long addr(const void *p) noexcept {return reinterpret_cast<long>(p);}
uint64_t startup_argc;
const char *const *startup_argv;
const char *const *startup_environment;
uint32_t environment_lock;
uint32_t launch_lock;
#if defined(LANG_RUNTIME_HOSTED)
// Only libc-managed threads use TLS. The freestanding branch must not access
// C++ TLS through its private FS/TCB.
thread_local bool hosted_launch_owned;
thread_local bool hosted_fork_owns_launch;
thread_local bool hosted_fork_locks_held;
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
LangVec *environment;
void lock() noexcept {
  while(__atomic_exchange_n(&environment_lock,1u,__ATOMIC_ACQUIRE))
    while(__atomic_load_n(&environment_lock,__ATOMIC_RELAXED))__asm__ volatile("pause");
}
void unlock() noexcept {__atomic_store_n(&environment_lock,0u,__ATOMIC_RELEASE);}
struct Guard {Guard() noexcept {lock();} ~Guard(){unlock();}};
#if defined(LANG_RUNTIME_HOSTED)
// Per-thread state makes a failed-fork fallback idempotent even when another
// thread has already entered its own atfork prepare handler.
void hosted_fork_parent_release() noexcept {
  if (hosted_fork_locks_held) {
    hosted_fork_locks_held=false;
    // Reverse the prepare order: allocator was acquired before Weak.
    lang_weak_fork_parent_v1();
    lang_alloc_fork_parent_v1();
    unlock();
  }
  if(hosted_fork_owns_launch) {
    hosted_fork_owns_launch=false;
    launch_release();
  }
}
#endif
uint64_t size(const char *s) noexcept {uint64_t n=0;while(s[n])++n;return n;}
void copy(void *out,const void *in,uint64_t n) noexcept {
  auto *a=static_cast<volatile uint8_t *>(out);
  auto *b=static_cast<const volatile uint8_t *>(in);
  for(uint64_t i=0;i<n;++i)a[i]=b[i];
}
uint64_t add(uint64_t a,uint64_t b) noexcept {
  if(b>~uint64_t{0}-a)lang_native_panic_v1(6,0,0,0);
  return a+b;
}
void *allocate(uint64_t n,uint64_t alignment=8) noexcept {
  auto *p=lang_alloc_v1(n,alignment);
  if(!p)lang_native_panic_v1(5,0,0,0);
  return p;
}
void retain_vec(void *p) {lang_vec_retain_v1(*static_cast<LangVec **>(p));}
void drop_vec(void *p) {lang_vec_release_v1(*static_cast<LangVec **>(p));}
void retain_text(void *p) {auto *s=static_cast<LangText *>(p);if(s->owner)lang_handle_retain_v1(s->owner);}
void drop_text(void *p) {auto *s=static_cast<LangText *>(p);if(s->owner)lang_handle_release_v1(s->owner);}
struct Bytes {const uint8_t *data;uint64_t length;};
int64_t byte_view(const LangVec *v,Bytes &out) noexcept {
  if(!v || v->stride!=1 || v->initialized!=v->length || v->copy || v->drop ||
     (v->length && !v->data))return -22;
  out={static_cast<const uint8_t *>(v->data),v->length};return 0;
}
int64_t text_view(const LangText *v,Bytes &out) noexcept {
  if(!v || (v->bytes && !v->data))return -22;
  out={v->data,v->bytes};return 0;
}
bool nul(Bytes b) noexcept {for(uint64_t i=0;i<b.length;++i)if(!b.data[i])return true;return false;}
int64_t key(Bytes b) noexcept {
  if(!b.length || nul(b))return -22;
  for(uint64_t i=0;i<b.length;++i)if(b.data[i]=='=')return -22;
  return 0;
}
LangVec *bytes(Bytes b) noexcept {
  auto *v=lang_vec_new_v1(b.length,1,nullptr,nullptr);
  copy(v->data,b.data,b.length);v->initialized=b.length;return v;
}
LangVec *nested() noexcept {return lang_vec_new_v1(0,sizeof(LangVec *),retain_vec,drop_vec);}
void append(LangVec *v,Bytes b) noexcept {auto *row=bytes(b);lang_vec_push_v1(v,&row);}
void load_environment() noexcept {
  if(environment)return;
  environment=nested();
  if(startup_environment)for(auto *p=startup_environment;*p;++p)
    append(environment,{reinterpret_cast<const uint8_t *>(*p),size(*p)});
}
LangVec *clone_bytes(const LangVec *v) noexcept {
  auto *result=nested();
  auto **rows=static_cast<LangVec **>(v->data);
  for(uint64_t i=0;i<v->length;++i)append(result,{static_cast<const uint8_t *>(rows[i]->data),rows[i]->length});
  return result;
}
int64_t to_texts(LangVec **out,const LangVec *input) noexcept {
  auto *result=lang_vec_new_v1(0,sizeof(LangText),retain_text,drop_text);
  auto **rows=static_cast<LangVec **>(input->data);
  for(uint64_t i=0;i<input->length;++i) {
    LangText s;
    if(lang_text_from_bytes_v1(&s,rows[i])<0) {lang_vec_release_v1(result);return -84;}
    lang_vec_push_v1(result,&s);
  }
  *out=result;return 0;
}
bool matches(const LangVec *row,Bytes name) noexcept {
  if(row->length<=name.length)return false;
  auto *p=static_cast<const uint8_t *>(row->data);
  for(uint64_t i=0;i<name.length;++i)if(p[i]!=name.data[i])return false;
  return p[name.length]=='=';
}
int64_t getenv_bytes(LangVec **out,Bytes name) noexcept {
  if(!out)return -14;
  *out=nullptr;
  auto status=key(name);if(status<0)return status;
  Guard guard;load_environment();
  auto **rows=static_cast<LangVec **>(environment->data);
  for(uint64_t i=0;i<environment->length;++i)if(matches(rows[i],name)) {
    *out=bytes({static_cast<const uint8_t *>(rows[i]->data)+name.length+1,rows[i]->length-name.length-1});
    return 1;
  }
  return 0;
}
int64_t setenv(Bytes name,Bytes value,int32_t overwrite,bool remove) noexcept {
  auto status=key(name);if(status<0)return status;
  if(!remove && nul(value))return -22;
  Guard guard;load_environment();
  auto **rows=static_cast<LangVec **>(environment->data);
  bool found=false;
  for(uint64_t i=0;i<environment->length;++i)if(matches(rows[i],name))found=true;
  if(found && !overwrite && !remove)return 0;
  // A new snapshot removes every duplicate key, including inherited duplicates.
  auto *next=nested();
  for(uint64_t i=0;i<environment->length;++i)if(!matches(rows[i],name))
    append(next,{static_cast<const uint8_t *>(rows[i]->data),rows[i]->length});
  if(!remove) {
    const auto n=add(add(name.length,1),value.length);
    auto *row=lang_vec_new_v1(n,1,nullptr,nullptr);
    auto *p=static_cast<uint8_t *>(row->data);
    copy(p,name.data,name.length);p[name.length]='=';copy(p+name.length+1,value.data,value.length);
    row->initialized=n;lang_vec_push_v1(next,&row);
  }
  lang_vec_release_v1(environment);environment=next;return 0;
}
int64_t fork_process() noexcept {
#if defined(LANG_RUNTIME_HOSTED)
  // libc fork runs glibc's internal handlers and the atfork hooks registered
  // by runtime/hosted/process.cpp.  The hooks acquire/release our environment
  // and allocator locks; launch is held by the caller (spawn or
  // lang_process_fork_v1).  Only the calling thread survives in the child.
  auto pid=lang_hosted_fork_v1();
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
  lock();lang_alloc_fork_prepare_v1();lang_weak_fork_prepare_v1();
  auto pid=sys(57);
  if(!pid) {
    lang_alloc_fork_child_v1();
    lang_weak_fork_child_v1();
    __atomic_store_n(&launch_lock,0u,__ATOMIC_RELAXED);
    __atomic_store_n(&environment_lock,0u,__ATOMIC_RELAXED);
    lang_thread_fork_child_v1();
  } else {lang_weak_fork_parent_v1();lang_alloc_fork_parent_v1();unlock();}
  return pid;
#endif
}
int64_t row(const LangVec *v,uint64_t i,bool raw,Bytes &out) noexcept {
  const uint64_t stride=raw?sizeof(LangVec *):sizeof(LangText);
  if(!v || v->stride!=stride || v->initialized!=v->length ||
     (v->length && !v->data) || i>=v->length)return -22;
  auto *p=static_cast<const uint8_t *>(v->data)+i*stride;
  return raw?byte_view(*reinterpret_cast<LangVec *const *>(p),out):text_view(reinterpret_cast<const LangText *>(p),out);
}
struct Exec {
  char *storage=nullptr,*path=nullptr;
  char **arguments=nullptr,**environment=nullptr;
  ~Exec(){lang_free_v1(storage);}
  int64_t prepare(Bytes file,const LangVec *args,const LangVec *env,bool raw) noexcept {
    if(!file.length || nul(file))return -22;
    uint64_t n=add(file.length,1);
    const uint64_t stride=raw?sizeof(LangVec *):sizeof(LangText);
    for(auto *v:{args,env}) {
      if(!v || v->stride!=stride || v->initialized!=v->length || (v->length&&!v->data))return -22;
      if(v->length>(~uint64_t{0}/sizeof(char *))-1)lang_native_panic_v1(6,0,0,0);
      n=add(n,(v->length+1)*sizeof(char *));
      for(uint64_t i=0;i<v->length;++i) {
        Bytes b;auto status=row(v,i,raw,b);if(status<0)return status;
        if(nul(b))return -22;
        n=add(n,add(b.length,1));
      }
    }
    storage=static_cast<char *>(allocate(n));
    arguments=reinterpret_cast<char **>(storage);
    environment=arguments+args->length+1;
    char *p=reinterpret_cast<char *>(environment+env->length+1);
    path=p;copy(p,file.data,file.length);p+=file.length;*p++=0;
    unsigned index=0;
    for(auto *v:{args,env}) {
      auto **pointers=index++?environment:arguments;
      for(uint64_t i=0;i<v->length;++i) {
        Bytes b;row(v,i,raw,b);pointers[i]=p;copy(p,b.data,b.length);p+=b.length;*p++=0;
      }
      pointers[v->length]=nullptr;
    }
    return 0;
  }
};
[[noreturn]] void child_exit(int status) noexcept {sys(60,status);__builtin_trap();}
void reap(int32_t pid) noexcept {while(sys(61,pid,0,0)==-4){}}
int64_t execute(Bytes path,const LangVec *args,const LangVec *env,bool raw) noexcept {
  Exec e;auto status=e.prepare(path,args,env,raw);if(status<0)return status;
  return sys(59,addr(e.path),addr(e.arguments),addr(e.environment));
}
int64_t spawn(Bytes path,const LangVec *args,const LangVec *env,bool raw,int32_t in,int32_t out,int32_t err) noexcept {
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
    const auto pid=fork_process();
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
  // Language fork/spawn already holds launch through pipe cleanup. Direct
  // libc fork must also wait until those temporary descriptors are closed.
  if(!hosted_launch_owned) {
    launch_acquire();
    hosted_fork_owns_launch=true;
  }
  lock();
  lang_alloc_fork_prepare_v1();
  lang_weak_fork_prepare_v1();
  hosted_fork_locks_held=true;
}
extern "C" void lang_hosted_process_atfork_parent_v1() noexcept {
  hosted_fork_parent_release();
}
extern "C" void lang_hosted_process_atfork_child_v1() noexcept {
  hosted_fork_locks_held=false;
  hosted_fork_owns_launch=false;
  hosted_launch_owned=false;
  lang_alloc_fork_child_v1();
  lang_weak_fork_child_v1();
  __atomic_store_n(&environment_lock,0u,__ATOMIC_RELAXED);
  __atomic_store_n(&launch_lock,0u,__ATOMIC_RELAXED);
}
#endif
extern "C" void lang_process_init_v1(uint64_t argc,const char *const *argv,const char *const *env) noexcept {
  startup_argc=argc;startup_argv=argv;startup_environment=env;
}
extern "C" int64_t lang_process_args_bytes_v1(LangVec **out) noexcept {
  if(!out)return -14;
  auto *result=nested();
  for(uint64_t i=0;i<startup_argc;++i)append(result,{reinterpret_cast<const uint8_t *>(startup_argv[i]),size(startup_argv[i])});
  *out=result;return 0;
}
extern "C" int64_t lang_process_args_v1(LangVec **out) noexcept {
  if(!out)return -14;
  LangVec *raw;lang_process_args_bytes_v1(&raw);auto status=to_texts(out,raw);lang_vec_release_v1(raw);return status;
}
extern "C" int64_t lang_process_environment_bytes_v1(LangVec **out) noexcept {
  if(!out)return -14;
  Guard guard;load_environment();*out=clone_bytes(environment);return 0;
}
extern "C" int64_t lang_process_environment_v1(LangVec **out) noexcept {
  if(!out)return -14;
  Guard guard;load_environment();return to_texts(out,environment);
}
extern "C" int64_t lang_process_getenv_bytes_v1(LangVec **out,const LangVec *name) noexcept {
  if(!out)return -14;
  *out=nullptr;
  Bytes n;auto status=byte_view(name,n);return status<0?status:getenv_bytes(out,n);
}
extern "C" int64_t lang_process_getenv_v1(LangText *out,const LangText *name) noexcept {
  if(!out)return -14;
  *out={};Bytes n;auto status=text_view(name,n);if(status<0)return status;
  LangVec *v;status=getenv_bytes(&v,n);
  if(status==1) {auto valid=lang_text_from_bytes_v1(out,v);lang_vec_release_v1(v);if(valid<0)return -84;}
  return status;
}
extern "C" int64_t lang_process_setenv_v1(const LangText *name,const LangText *value,int32_t overwrite) noexcept {
  Bytes n,v;auto status=text_view(name,n);if(!status)status=text_view(value,v);return status<0?status:setenv(n,v,overwrite,false);
}
extern "C" int64_t lang_process_setenv_bytes_v1(const LangVec *name,const LangVec *value,int32_t overwrite) noexcept {
  Bytes n,v;auto status=byte_view(name,n);if(!status)status=byte_view(value,v);return status<0?status:setenv(n,v,overwrite,false);
}
extern "C" int64_t lang_process_unsetenv_v1(const LangText *name) noexcept {
  Bytes n;auto status=text_view(name,n);return status<0?status:setenv(n,{},1,true);
}
extern "C" int64_t lang_process_unsetenv_bytes_v1(const LangVec *name) noexcept {
  Bytes n;auto status=byte_view(name,n);return status<0?status:setenv(n,{},1,true);
}
extern "C" int64_t lang_process_fork_v1() noexcept {LaunchGuard launch;return fork_process();}
extern "C" int64_t lang_process_waitpid_v1(int32_t pid,int32_t options,int32_t *status) noexcept {
  if(!status)return -14;
  int32_t result=0;auto r=sys(61,pid,addr(&result),options);
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
  auto result=sys(128,addr(&mask),addr(info),timeout<0?0:addr(time),8);
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

#include "ipc.hpp"
#include "gc_program.hpp"
#if defined(__APPLE__) && defined(__aarch64__)
#include "darwin.hpp"
#include "../darwin/posix.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#include "linux.hpp"
#endif
#include <cstddef>

extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t, uint64_t, uint64_t, uint64_t) noexcept;
namespace {
#if defined(__APPLE__)
namespace darwin = lang::native::darwin;
#else
namespace linux_abi = lang::native::linux_x86_64;
int64_t sys(long n, long a=0, long b=0, long c=0, long d=0, long e=0, long f=0) noexcept {
  return linux_abi::syscall6(n,a,b,c,d,e,f);
}
long address(const void *p) noexcept { return reinterpret_cast<long>(p); }
#endif
struct Mapping {
  LangHandle handle;
  uint8_t *data;
  uint64_t length;
  int32_t protection;
#if defined(__APPLE__)
  uint64_t page_prefix;
#endif
};
void destroy(LangHandle *handle) noexcept {
  auto *m = reinterpret_cast<Mapping *>(handle);
#if defined(__APPLE__)
  if (munmap(m->data - m->page_prefix, m->length + m->page_prefix) < 0) lang_handle_panic_v1();
#else
  if (sys(11,address(m->data),m->length)<0) lang_handle_panic_v1();
#endif
  lang_free_v1(m);
}
Mapping *mapping(LangHandle *h) noexcept {
  if (!h || h->kind!=LangHandleKind::ipc || h->destroy!=destroy) lang_handle_panic_v1();
  return reinterpret_cast<Mapping *>(h);
}
int64_t range(LangHandle *h, uint64_t offset, uint64_t bytes, uint64_t align,
              int32_t protection, uint8_t *&out) noexcept {
  auto *m=mapping(h);
  if (offset>m->length || bytes>m->length-offset || offset%align) return -22;
  if (bytes && (m->protection&protection)!=protection) return -13;
  out=m->data+offset;
  return 0;
}
void copy(uint8_t *to, const uint8_t *from, uint64_t size) noexcept {
  volatile uint8_t *a=to;
  const volatile uint8_t *b=from;
  if (reinterpret_cast<uintptr_t>(to)>reinterpret_cast<uintptr_t>(from) &&
      reinterpret_cast<uintptr_t>(to)-reinterpret_cast<uintptr_t>(from)<size) {
    while(size) {--size;a[size]=b[size];}
  } else for(uint64_t i=0;i<size;++i) a[i]=b[i];
}
int64_t create(LangHandle **out, int32_t fd, uint64_t bytes, uint64_t offset,
               int32_t protection, bool anonymous) noexcept {
  if (!out) return -14;
  if (!bytes || offset%4096 || offset>0x7fffffffffffffffULL || (protection&~3)) return -22;
#if defined(__APPLE__)
  const uint64_t page_prefix = offset % static_cast<uint64_t>(getpagesize());
  if (bytes > SIZE_MAX - page_prefix) return -22;
  const int prot = ((protection & 1) ? PROT_READ : 0) | ((protection & 2) ? PROT_WRITE : 0);
  void *base = mmap(nullptr, bytes + page_prefix, prot, MAP_SHARED | (anonymous ? MAP_ANON : 0),
                    anonymous ? -1 : fd, static_cast<off_t>(offset - page_prefix));
  if (base == MAP_FAILED) return darwin::error(errno);
  auto *m = static_cast<Mapping *>(lang_alloc_v1(sizeof(Mapping), alignof(Mapping)));
  if (!m) {
    munmap(base, bytes + page_prefix);
    lang_native_panic_v1(5, 0, 0, 0);
  }
  *m = {{1, LangHandleKind::ipc, destroy, nullptr}, static_cast<uint8_t *>(base) + page_prefix,
        bytes, protection, page_prefix};
#else
  auto result=sys(9,0,bytes,protection,anonymous?0x21:1,anonymous?-1:fd,offset);
  if (result<0 && result>=-4095) return result;
  auto *m=static_cast<Mapping *>(lang_alloc_v1(sizeof(Mapping),alignof(Mapping)));
  if (!m) {sys(11,result,bytes);lang_native_panic_v1(5,0,0,0);}
  *m={{1,LangHandleKind::ipc,destroy,nullptr},reinterpret_cast<uint8_t *>(result),bytes,protection};
#endif
  *out=&m->handle;
  return 0;
}
int64_t shm_name(const LangText *name, char *out) noexcept {
  if (!name || !name->data || name->bytes<2 || name->data[0]!='/') return -22;
  if (name->bytes>256) return -36;
  const uint64_t size=name->bytes-1;
  for(uint64_t i=0;i<size;++i) {
    auto byte=name->data[i+1];
    if (!byte || byte=='/') return -22;
    out[i]=static_cast<char>(byte);
  }
  if ((size==1 && out[0]=='.') || (size==2 && out[0]=='.' && out[1]=='.')) return -22;
  out[size]=0;
  return 0;
}
template<class T> int64_t read(T *out, LangHandle *h, uint64_t offset) noexcept {
  if (!out) return -14;
  uint8_t *p;auto status=range(h,offset,sizeof(T),alignof(T),1,p);
  if (!status) *out=*reinterpret_cast<const volatile T *>(p);
  return status;
}
template<class T> int64_t write(LangHandle *h, uint64_t offset, T value) noexcept {
  uint8_t *p;auto status=range(h,offset,sizeof(T),alignof(T),2,p);
  if (!status) *reinterpret_cast<volatile T *>(p)=value;
  return status;
}
struct Time {int64_t seconds,nanoseconds;};
#if defined(__APPLE__)
struct Deadline {
  bool finite;
  uint64_t end;
  int64_t init(int64_t ns) noexcept {
    if (ns < -1) return -22;
    finite = ns >= 0;
    if (!finite) return 0;
    const auto status = darwin::monotonic_deadline(static_cast<uint64_t>(ns), end);
    return status < 0 ? darwin::error(-status) : 0;
  }
};
int64_t wait(uint32_t *word, uint32_t value, Deadline &deadline) noexcept {
  if (deadline.finite && mach_absolute_time() >= deadline.end) return -110;
  lang::native::gc_program::SafeGuard safe;
  const auto status = deadline.finite
      ? os_sync_wait_on_address_with_deadline(word, value, sizeof(*word), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                              OS_CLOCK_MACH_ABSOLUTE_TIME, deadline.end)
      : os_sync_wait_on_address(word, value, sizeof(*word), OS_SYNC_WAIT_ON_ADDRESS_SHARED);
  return darwin::result(status);
}
int64_t wake(uint32_t *word, uint32_t count) noexcept {
  const auto status = count == 1
      ? os_sync_wake_by_address_any(word, sizeof(*word), OS_SYNC_WAKE_BY_ADDRESS_SHARED)
      : os_sync_wake_by_address_all(word, sizeof(*word), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
  return status == 0 || errno == ENOENT ? 0 : darwin::error(errno);
}
#else
struct Deadline {
  bool finite;
  Time end;
  int64_t init(int64_t ns) noexcept {
    if (ns< -1) return -22;
    finite=ns>=0;
    if (!finite) return 0;
    auto status=sys(228,1,address(&end));
    if (status<0) return status;
    if (end.seconds>0x7fffffffffffffffLL-ns/1000000000-1) return -22;
    end.seconds+=ns/1000000000;
    end.nanoseconds+=ns%1000000000;
    if (end.nanoseconds>=1000000000) {++end.seconds;end.nanoseconds-=1000000000;}
    return 0;
  }
  int64_t remaining(Time &out) noexcept {
    auto status=sys(228,1,address(&out));
    if (status<0) return status;
    out={end.seconds-out.seconds,end.nanoseconds-out.nanoseconds};
    if (out.nanoseconds<0) {--out.seconds;out.nanoseconds+=1000000000;}
    return out.seconds<0 || (!out.seconds && !out.nanoseconds) ? -110 : 0;
  }
};
int64_t wait(uint32_t *word, uint32_t value, Deadline &deadline) noexcept {
  Time remaining;
  if (deadline.finite) {
    auto status=deadline.remaining(remaining);
    if (status<0) return status;
  }
  // All mutex/condition/semaphore waits funnel through here after releasing
  // their internal locks. The graph is quiescent while this futex sleeps.
  lang::native::gc_program::SafeGuard safe;
  return sys(202,address(word),0,value,deadline.finite?address(&remaining):0);
}
int64_t wake(uint32_t *word, uint32_t count) noexcept {
  auto result=sys(202,address(word),1,count);
  return result<0?result:0;
}
#endif
void cpu_pause() noexcept {
#if defined(__APPLE__)
  darwin::cpu_pause();
#else
  __asm__ volatile("pause");
#endif
}
void spin_lock(uint32_t &lock) noexcept {
  while(__atomic_exchange_n(&lock,1u,__ATOMIC_ACQUIRE))
    while(__atomic_load_n(&lock,__ATOMIC_RELAXED)) cpu_pause();
}
void spin_unlock(uint32_t &lock) noexcept {__atomic_store_n(&lock,0u,__ATOMIC_RELEASE);}
constexpr uint32_t kMutex=0x575a4d31, kCondition=0x575a4331, kSemaphore=0x575a5331;
#if defined(__APPLE__)
constexpr uint32_t kOwnerGuard = 1u << 31;
#endif
struct Mutex {uint32_t magic,state,owner,reserved;};
struct Condition {uint32_t magic,lock,sequence,waiters;};
struct Semaphore {uint32_t magic,value,waiters,reserved;};
static_assert(sizeof(Mutex)==16 && sizeof(Condition)==16 && sizeof(Semaphore)==16);
template<class T> int64_t object(LangHandle *h,uint64_t offset,uint32_t magic,T *&out) noexcept {
  uint8_t *p;auto result=range(h,offset,sizeof(T),4,3,p);
  if(result<0) return result;
  out=reinterpret_cast<T *>(p);
  auto found = __atomic_load_n(&out->magic, __ATOMIC_ACQUIRE);
#if defined(__APPLE__)
  if (magic == kMutex) found &= ~kOwnerGuard;
#endif
  if (found != magic) lang_handle_panic_v1();
  return 0;
}
template<class T> int64_t init(LangHandle *h,uint64_t offset,uint32_t magic,uint32_t value=0) noexcept {
  uint8_t *p;auto result=range(h,offset,sizeof(T),4,3,p);
  if(result<0) return result;
  auto *out=reinterpret_cast<T *>(p);uint32_t expected=0;
  if(!__atomic_compare_exchange_n(&out->magic,&expected,~uint32_t{0},false,
                                  __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return -17;
  auto *words=reinterpret_cast<uint32_t *>(out);
  words[1]=value;words[2]=0;words[3]=0;
  __atomic_store_n(&out->magic,magic,__ATOMIC_RELEASE);
  return 0;
}
#if defined(__APPLE__)
using ThreadId = uint64_t;
ThreadId tid() noexcept {
  const auto id = darwin::thread_id();
  if (id <= 0) lang_handle_panic_v1();
  return static_cast<ThreadId>(id);
}

class OwnerGuard {
 public:
  explicit OwnerGuard(Mutex &mutex) noexcept : mutex_(mutex) {
    while (__atomic_fetch_or(&mutex_.magic, kOwnerGuard, __ATOMIC_ACQUIRE) & kOwnerGuard) cpu_pause();
  }
  ~OwnerGuard() noexcept { __atomic_fetch_and(&mutex_.magic, ~kOwnerGuard, __ATOMIC_RELEASE); }

 private:
  Mutex &mutex_;
};

// The shared ABI guarantees only 4-byte alignment, while Darwin thread ids
// occupy 64 bits. Guard the two owner words with the high bit of the magic.
bool owned_by(Mutex &m, ThreadId id) noexcept {
  OwnerGuard guard(m);
  return m.owner == static_cast<uint32_t>(id) && m.reserved == static_cast<uint32_t>(id >> 32);
}
void set_owner(Mutex &m, ThreadId id) noexcept {
  OwnerGuard guard(m);
  m.owner = static_cast<uint32_t>(id);
  m.reserved = static_cast<uint32_t>(id >> 32);
}
#else
using ThreadId = uint32_t;
ThreadId tid() noexcept {
  auto id=sys(186);if(id<=0) lang_handle_panic_v1();return static_cast<uint32_t>(id);
}
bool owned_by(Mutex &m, ThreadId id) noexcept {
  return __atomic_load_n(&m.owner, __ATOMIC_RELAXED) == id;
}
void set_owner(Mutex &m, ThreadId id) noexcept {
  __atomic_store_n(&m.owner, id, __ATOMIC_RELAXED);
}
#endif
int64_t mutex_lock(Mutex &m) noexcept {
  auto self=tid();
  if(owned_by(m,self)) lang_handle_panic_v1();
  uint32_t expected=0;
  if(!__atomic_compare_exchange_n(&m.state,&expected,1u,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
    Deadline infinite{false,{}};
    while(__atomic_exchange_n(&m.state,2u,__ATOMIC_ACQUIRE)) {
      auto result=wait(&m.state,2,infinite);
      if(result<0 && result!= -4 && result!= -11) return result;
    }
  }
  set_owner(m,self);
  return 0;
}
int64_t mutex_unlock(Mutex &m) noexcept {
  if(!owned_by(m,tid())) lang_handle_panic_v1();
  set_owner(m,0);
  auto previous=__atomic_exchange_n(&m.state,0u,__ATOMIC_RELEASE);
  if(!previous) lang_handle_panic_v1();
  return previous==2?wake(&m.state,1):0;
}
int64_t notify(LangHandle *h,uint64_t offset,bool all) noexcept {
  Condition *c;auto status=object(h,offset,kCondition,c);if(status<0)return status;
  spin_lock(c->lock);
  if(c->waiters) {
    if(c->sequence!=~uint32_t{0}) __atomic_add_fetch(&c->sequence,1u,__ATOMIC_RELEASE);
    // Before wrapping, wake every registered waiter and drain the old epoch.
    // New waiters cannot register until that epoch is empty.
    status=wake(&c->sequence,all || c->sequence==~uint32_t{0}?0x7fffffff:1);
  }
  spin_unlock(c->lock);
  return status;
}
int64_t semaphore_wait(Semaphore &s,Deadline &deadline,bool attempt) noexcept {
  auto value=__atomic_load_n(&s.value,__ATOMIC_RELAXED);
  for(;;) {
    if(value) {
      if(__atomic_compare_exchange_n(&s.value,&value,value-1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
    } else break;
  }
  if(attempt)return -11;
  if(__atomic_fetch_add(&s.waiters,1u,__ATOMIC_RELAXED)==~uint32_t{0}) lang_handle_panic_v1();
  int64_t result=0;
  for(;;) {
    value=__atomic_load_n(&s.value,__ATOMIC_RELAXED);
    if(value) {
      if(__atomic_compare_exchange_n(&s.value,&value,value-1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {result=0;break;}
      continue;
    }
    result=wait(&s.value,0,deadline);
    if(result<0 && result!= -11) break;
  }
  __atomic_fetch_sub(&s.waiters,1u,__ATOMIC_RELAXED);
  return result<0?result:0;
}
} // namespace
extern "C" int64_t lang_ipc_map_anon_v1(LangHandle **out,uint64_t n,int32_t p) noexcept {return create(out,-1,n,0,p,true);}
extern "C" int64_t lang_ipc_map_file_v1(LangHandle **out,int32_t fd,uint64_t n,uint64_t off,int32_t p) noexcept {return create(out,fd,n,off,p,false);}
extern "C" uint64_t lang_ipc_length_v1(LangHandle *h) noexcept {return mapping(h)->length;}
extern "C" int64_t lang_ipc_shm_open_v1(const LangText *name,int32_t flags,uint32_t mode) noexcept {
#if defined(__APPLE__)
  char path[257] = {'/'};
  auto status = shm_name(name, path + 1);
  if (status < 0) return status;
  if ((flags & ~(2 | 64 | 128 | 512 | 524288)) || (flags & 3) == 1 || ((flags & 512) && !(flags & 2)))
    return -22;
  const int open_flags = ((flags & 2) ? O_RDWR : O_RDONLY) | ((flags & 64) ? O_CREAT : 0) |
      ((flags & 128) ? O_EXCL : 0) | ((flags & 512) ? O_TRUNC : 0);
  // POSIX shm_open marks the descriptor close-on-exec.
  return darwin::result(shm_open(path, open_flags, static_cast<mode_t>(mode)));
#else
  char path[256];auto status=shm_name(name,path);if(status<0)return status;
  if((flags&~(2|64|128|512|524288)) || (flags&3)==1 || ((flags&512) && !(flags&2)))return -22;
  auto directory=sys(257,-100,address("/dev/shm"),0x90000,0);
  if(directory<0)return directory;
  auto result=sys(257,directory,address(path),flags|0xa0000,mode);
  sys(3,directory);return result;
#endif
}
extern "C" int64_t lang_ipc_shm_unlink_v1(const LangText *name) noexcept {
#if defined(__APPLE__)
  char path[257] = {'/'};
  const auto status = shm_name(name, path + 1);
  return status < 0 ? status : darwin::result(shm_unlink(path));
#else
  char path[256];auto status=shm_name(name,path);if(status<0)return status;
  auto directory=sys(257,-100,address("/dev/shm"),0x90000,0);if(directory<0)return directory;
  auto result=sys(263,directory,address(path),0);sys(3,directory);return result;
#endif
}
extern "C" int64_t lang_ipc_read_bytes_v1(LangVec **out,LangHandle *h,uint64_t off,uint64_t count) noexcept {
  if(!out)return -14;
  uint8_t *p;auto status=range(h,off,count,1,1,p);if(status<0)return status;
  auto *v=lang_vec_new_v1(count,1,nullptr,nullptr);copy(static_cast<uint8_t *>(v->data),p,count);
  v->initialized=count;*out=v;return 0;
}
extern "C" int64_t lang_ipc_write_bytes_v1(LangHandle *h,uint64_t off,const LangVec *v,uint64_t start,uint64_t count) noexcept {
  if(!v || v->stride!=1 || v->copy || v->drop || v->initialized!=v->length ||
     start>v->length || count>v->length-start || (v->length && !v->data))return -22;
  uint8_t *p;auto status=range(h,off,count,1,2,p);if(status<0)return status;
  if(count)copy(p,static_cast<const uint8_t *>(v->data)+start,count);
  return 0;
}
extern "C" int64_t lang_ipc_read_i32_v1(int32_t *v,LangHandle *h,uint64_t off) noexcept {return read(v,h,off);}
extern "C" int64_t lang_ipc_write_i32_v1(LangHandle *h,uint64_t off,int32_t v) noexcept {return write(h,off,v);}
extern "C" int64_t lang_ipc_read_i64_v1(int64_t *v,LangHandle *h,uint64_t off) noexcept {return read(v,h,off);}
extern "C" int64_t lang_ipc_write_i64_v1(LangHandle *h,uint64_t off,int64_t v) noexcept {return write(h,off,v);}
extern "C" int64_t lang_ipc_atomic_load_v1(int64_t *out,LangHandle *h,uint64_t off) noexcept {
  if(!out)return -14;
  uint8_t *p;auto status=range(h,off,8,8,3,p);
  if(!status)*out=__atomic_load_n(reinterpret_cast<int64_t *>(p),__ATOMIC_SEQ_CST);
  return status;
}
extern "C" int64_t lang_ipc_atomic_store_v1(LangHandle *h,uint64_t off,int64_t value) noexcept {
  uint8_t *p;auto status=range(h,off,8,8,3,p);
  if(!status)__atomic_store_n(reinterpret_cast<int64_t *>(p),value,__ATOMIC_SEQ_CST);
  return status;
}
extern "C" int64_t lang_ipc_atomic_add_v1(int64_t *out,LangHandle *h,uint64_t off,int64_t value) noexcept {
  if(!out)return -14;
  uint8_t *p;auto status=range(h,off,8,8,3,p);
  if(!status)*out=__atomic_fetch_add(reinterpret_cast<int64_t *>(p),value,__ATOMIC_SEQ_CST);
  return status;
}
extern "C" int64_t lang_ipc_atomic_compare_v1(bool *out,LangHandle *h,uint64_t off,int64_t expected,int64_t desired) noexcept {
  if(!out)return -14;
  uint8_t *p;auto status=range(h,off,8,8,3,p);
  if(!status)*out=__atomic_compare_exchange_n(reinterpret_cast<int64_t *>(p),&expected,desired,false,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
  return status;
}
extern "C" int64_t lang_ipc_mutex_init_v1(LangHandle *h,uint64_t off) noexcept {return init<Mutex>(h,off,kMutex);}
extern "C" int64_t lang_ipc_mutex_lock_v1(LangHandle *h,uint64_t off) noexcept {
  Mutex *m;auto status=object(h,off,kMutex,m);return status<0?status:mutex_lock(*m);
}
extern "C" int64_t lang_ipc_mutex_unlock_v1(LangHandle *h,uint64_t off) noexcept {
  Mutex *m;auto status=object(h,off,kMutex,m);return status<0?status:mutex_unlock(*m);
}
extern "C" int64_t lang_ipc_mutex_destroy_v1(LangHandle *h,uint64_t off) noexcept {
  Mutex *m;auto status=object(h,off,kMutex,m);if(status<0)return status;
  if(__atomic_load_n(&m->state,__ATOMIC_ACQUIRE))return -16;
  __atomic_store_n(&m->magic,0u,__ATOMIC_RELEASE);return 0;
}
extern "C" int64_t lang_ipc_cond_init_v1(LangHandle *h,uint64_t off) noexcept {return init<Condition>(h,off,kCondition);}
extern "C" int64_t lang_ipc_cond_signal_v1(LangHandle *h,uint64_t off) noexcept {return notify(h,off,false);}
extern "C" int64_t lang_ipc_cond_broadcast_v1(LangHandle *h,uint64_t off) noexcept {return notify(h,off,true);}
extern "C" int64_t lang_ipc_cond_wait_v1(LangHandle *h,uint64_t off,LangHandle *mh,uint64_t moff,int64_t timeout) noexcept {
  Condition *c;Mutex *m;
  auto status=object(h,off,kCondition,c);if(status<0)return status;
  status=object(mh,moff,kMutex,m);if(status<0)return status;
  if(!owned_by(*m,tid()))lang_handle_panic_v1();
  Deadline deadline;status=deadline.init(timeout);if(status<0)return status;
  uint32_t sequence;
  for(;;) {
    spin_lock(c->lock);
    if(c->sequence!=~uint32_t{0} || !c->waiters) {
      if(!c->waiters && c->sequence==~uint32_t{0})__atomic_store_n(&c->sequence,0u,__ATOMIC_RELEASE);
      sequence=c->sequence;
      if(c->waiters==~uint32_t{0}) {spin_unlock(c->lock);return -11;}
      __atomic_add_fetch(&c->waiters,1u,__ATOMIC_RELAXED);spin_unlock(c->lock);break;
    }
    auto count=c->waiters;spin_unlock(c->lock);
    status=wait(&c->waiters,count,deadline);
    if(status<0 && status!= -4 && status!= -11)return status;
  }
  status=mutex_unlock(*m);
  if(!status) {
    while(__atomic_load_n(&c->sequence,__ATOMIC_ACQUIRE)==sequence) {
      status=wait(&c->sequence,sequence,deadline);
      if(status<0 && status!= -4 && status!= -11)break;
      status=0;
    }
  }
  spin_lock(c->lock);
  __atomic_sub_fetch(&c->waiters,1u,__ATOMIC_RELAXED);
  if(c->sequence==~uint32_t{0}) {
    auto woke=wake(&c->waiters,0x7fffffff);if(!status)status=woke;
  }
  spin_unlock(c->lock);
  auto locked=mutex_lock(*m);
  return locked<0?locked:status;
}
extern "C" int64_t lang_ipc_cond_destroy_v1(LangHandle *h,uint64_t off) noexcept {
  Condition *c;auto status=object(h,off,kCondition,c);if(status<0)return status;
  spin_lock(c->lock);if(c->waiters)status=-16;else __atomic_store_n(&c->magic,0u,__ATOMIC_RELEASE);
  spin_unlock(c->lock);return status;
}
extern "C" int64_t lang_ipc_sem_init_v1(LangHandle *h,uint64_t off,uint32_t value) noexcept {
  return value>0x7fffffff?-22:init<Semaphore>(h,off,kSemaphore,value);
}
extern "C" int64_t lang_ipc_sem_wait_v1(LangHandle *h,uint64_t off,int64_t timeout) noexcept {
  Semaphore *s;auto status=object(h,off,kSemaphore,s);if(status<0)return status;
  Deadline deadline;status=deadline.init(timeout);return status<0?status:semaphore_wait(*s,deadline,false);
}
extern "C" int64_t lang_ipc_sem_trywait_v1(LangHandle *h,uint64_t off) noexcept {
  Semaphore *s;auto status=object(h,off,kSemaphore,s);if(status<0)return status;
  Deadline unused{false,{}};return semaphore_wait(*s,unused,true);
}
extern "C" int64_t lang_ipc_sem_post_v1(LangHandle *h,uint64_t off) noexcept {
  Semaphore *s;auto status=object(h,off,kSemaphore,s);if(status<0)return status;
  auto value=__atomic_load_n(&s->value,__ATOMIC_RELAXED);
  for(;;) {
    if(value==0x7fffffff)return -75;
    if(__atomic_compare_exchange_n(&s->value,&value,value+1,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED))break;
  }
  return __atomic_load_n(&s->waiters,__ATOMIC_RELAXED)?wake(&s->value,1):0;
}
extern "C" int64_t lang_ipc_sem_value_v1(uint32_t *out,LangHandle *h,uint64_t off) noexcept {
  if(!out)return -14;
  Semaphore *s;auto status=object(h,off,kSemaphore,s);
  if(!status)*out=__atomic_load_n(&s->value,__ATOMIC_RELAXED);
  return status;
}
extern "C" int64_t lang_ipc_sem_destroy_v1(LangHandle *h,uint64_t off) noexcept {
  Semaphore *s;auto status=object(h,off,kSemaphore,s);if(status<0)return status;
  if(__atomic_load_n(&s->waiters,__ATOMIC_RELAXED))return -16;
  __atomic_store_n(&s->magic,0u,__ATOMIC_RELEASE);return 0;
}

// No-libc static core ELF check for the private per-thread tid cache.
//
// The test runs under runtime/native/start.cpp, so it exercises the real
// initial TCB and the runtime-owned Native thread TCB. It verifies that only
// runtime-owned FS slots are cached, that a new Native thread starts with an
// empty cache, and that a fork child clears the inherited tid before gettid.
// It uses only raw syscalls and the freestanding runtime.

#include "alloc.hpp"
#include "gc_world.hpp"
#include "linux.hpp"
#include "start.hpp"
#include "thread.hpp"

using lang::native::linux_x86_64::syscall6;

namespace {

constexpr long kSysGettid = 186;
constexpr long kSysFork = 57;
constexpr long kSysWait4 = 61;
constexpr long kSysWrite = 1;
constexpr long kSysExitGroup = 231;
constexpr uintptr_t kNativeTcbMagic = 0x575a544342763031ULL;

uintptr_t fs_cached_tid() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x8, %0" : "=r"(value));
  return value;
}

uintptr_t fs_cookie() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x10, %0" : "=r"(value));
  return value;
}

uintptr_t fs_thread_state() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x30, %0" : "=r"(value));
  return value;
}

uintptr_t fs_native_marker() noexcept {
  uintptr_t value;
  __asm__ volatile("movq %%fs:0x38, %0" : "=r"(value));
  return value;
}

int64_t raw_tid() noexcept {
  return syscall6(kSysGettid, 0, 0, 0, 0, 0, 0);
}

void write_all(const char *bytes, uintptr_t size) noexcept {
  while (size) {
    const int64_t result =
        syscall6(kSysWrite, 2, reinterpret_cast<long>(bytes),
                 static_cast<long>(size), 0, 0, 0);
    if (result == -4)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uintptr_t>(result);
  }
}

[[noreturn]] void fail(const char *reason) noexcept {
  write_all("GC_TID_CACHE_STATIC_FAIL:", sizeof("GC_TID_CACHE_STATIC_FAIL:") - 1);
  write_all(reason, [](const char *text) {
    uintptr_t length = 0;
    while (text[length])
      ++length;
    return length;
  }(reason));
  write_all("\n", 1);
  syscall6(kSysExitGroup, 1, 0, 0, 0, 0, 0);
  __builtin_unreachable();
}

extern "C" int64_t native_entry(void *) noexcept {
  if (fs_cookie() != lang_runtime::kTcbTidCacheCookie)
    return 101;
  if (fs_native_marker() != kNativeTcbMagic)
    return 102;
  if (fs_thread_state() == 0)
    return 103;
  // The newly mapped TCB is explicitly cleared before the thread runs.
  if (fs_cached_tid() != 0)
    return 104;
  const int64_t tid = raw_tid();
  if (tid <= 0)
    return 105;
  if (!lang_gc_world_register_v1())
    return 106;
  if (fs_cached_tid() != static_cast<uintptr_t>(tid))
    return 107;
  if (!lang_gc_world_unregister_v1())
    return 108;
  // Unregistration does not invalidate the thread's own kernel tid cache.
  if (fs_cached_tid() != static_cast<uintptr_t>(tid))
    return 109;
  return 0;
}

} // namespace

extern "C" int main() {
  const int64_t parent_tid = raw_tid();
  if (parent_tid <= 0)
    fail("main-gettid");
  if (fs_cookie() != lang_runtime::kTcbTidCacheCookie)
    fail("main-cookie");
  if (fs_cached_tid() != 0)
    fail("main-cache-not-clear");
  // The main startup TCB must not masquerade as a Native language thread.
  if (fs_thread_state() != 0 || fs_native_marker() != 0)
    fail("main-fake-native-thread");

  if (!lang_gc_world_register_v1())
    fail("main-register");
  if (fs_cached_tid() != static_cast<uintptr_t>(parent_tid))
    fail("main-cache-warm");

  // Fork while registered. The child must replace the inherited parent tid
  // with its own tid after clearing the owned cache.
  lang_alloc_fork_prepare_v1();
  lang_gc_world_fork_prepare_v1();
  const int64_t pid = syscall6(kSysFork, 0, 0, 0, 0, 0, 0);
  if (pid < 0) {
    lang_gc_world_fork_parent_v1();
    lang_alloc_fork_parent_v1();
    fail("fork");
  }
  if (pid == 0) {
    lang_alloc_fork_child_v1();
    lang_gc_world_fork_child_v1();
    const int64_t child_tid = raw_tid();
    if (child_tid <= 0)
      fail("child-gettid");
    if (child_tid == parent_tid)
      fail("child-tid-not-new");
    if (fs_cached_tid() != static_cast<uintptr_t>(child_tid))
      fail("child-cache-not-refreshed");
    // The preserved Native-thread marker logic must return for main's TCB and
    // must not fabricate a ThreadState.
    lang_thread_fork_child_v1();
    if (fs_thread_state() != 0 || fs_native_marker() != 0)
      fail("child-main-state-changed");
    if (!lang_gc_world_unregister_v1())
      fail("child-unregister");
    if (fs_cached_tid() != static_cast<uintptr_t>(child_tid))
      fail("child-cache-after-unregister");
    syscall6(kSysExitGroup, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
  }

  lang_gc_world_fork_parent_v1();
  lang_alloc_fork_parent_v1();
  if (fs_cached_tid() != static_cast<uintptr_t>(parent_tid))
    fail("parent-cache-changed");

  int status = 0;
  int64_t waited;
  do {
    waited = syscall6(kSysWait4, pid, reinterpret_cast<long>(&status), 0, 0, 0,
                      0);
  } while (waited == -4);
  if (waited != pid)
    fail("wait4");
  if ((status & 0x7f) != 0 || ((status >> 8) & 0xff) != 0)
    fail("fork-child-checks");

  LangHandle *thread =
      lang_thread_spawn_v1(&native_entry, nullptr, 0, nullptr, nullptr);
  if (!thread)
    fail("thread-spawn");
  if (lang_thread_wait_v1(thread) != 0)
    fail("thread-wait");
  const int64_t result = lang_thread_result_v1(thread);
  lang_handle_release_v1(thread);
  if (result != 0)
    fail("native-thread-check");

  if (!lang_gc_world_unregister_v1())
    fail("main-unregister");
  if (fs_cached_tid() != static_cast<uintptr_t>(parent_tid))
    fail("main-cache-after-unregister");

  write_all("GC_TID_CACHE_STATIC_OK\n", sizeof("GC_TID_CACHE_STATIC_OK\n") - 1);
  return 0;
}

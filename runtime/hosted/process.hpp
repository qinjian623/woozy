#pragma once
#include <cstdint>

// Hosted process/fork support.  The runtime must use libc fork(3), not the
// native raw clone/fork syscall, so glibc can run its pthread_atfork handlers
// and prepare libc malloc/TLS/clock state for the single surviving thread.
extern "C" {
int64_t lang_hosted_fork_v1() noexcept;

// Defined by runtime/native/process.cpp when compiled with
// LANG_RUNTIME_HOSTED.  They acquire/release that translation unit's
// launch -> environment -> allocator locks around libc fork. An existing
// language LaunchGuard keeps launch through spawn's complete pipe handshake;
// direct libc fork takes and releases launch through these hooks instead.
//
// The same hooks also carry the optional async fork steps. A Source
// lang_process_fork_v1 reserves the async gate before its LaunchGuard and
// returns -EBUSY if another thread owns the executor. A direct libc fork has
// no return channel from this void prepare: if the executor belongs to another
// thread it records LANG_ASYNC_FORK_REFUSED and Panics instead of creating a
// child. Internal spawn marks its fork exec-only, keeps a foreign executor
// allowed, and skips task continuation.
void lang_hosted_process_atfork_prepare_v1() noexcept;
void lang_hosted_process_atfork_parent_v1() noexcept;
void lang_hosted_process_atfork_child_v1() noexcept;
}

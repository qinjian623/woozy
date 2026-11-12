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
void lang_hosted_process_atfork_prepare_v1() noexcept;
void lang_hosted_process_atfork_parent_v1() noexcept;
void lang_hosted_process_atfork_child_v1() noexcept;
}

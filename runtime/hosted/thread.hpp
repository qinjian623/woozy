#pragma once
#include "../native/thread.hpp"

// Hosted implementation of the existing lang_thread_* ABI.  It keeps the
// language-visible entry/copy/drop contract but runs each Woozy runtime thread
// on a pthread with libc-managed TLS.  It does not clone raw threads, install
// FS, or synthesize a glibc TCB.
// lang_thread_wait_for_v1 uses CLOCK_MONOTONIC and one saturated absolute
// deadline. -1 waits indefinitely, 0 polls, and values below -1 return -22.
extern "C" [[noreturn]] void lang_thread_unmap_exit_v1(void *, uint64_t) noexcept;

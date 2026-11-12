#pragma once
#include "handle.hpp"
#include "vec.hpp"
#include <cstdint>

// Matches the usual Linux thread stack reserve; pages are committed on use.
inline constexpr uint64_t LANG_THREAD_STACK_BYTES = 8 * 1024 * 1024;

using LangThreadEntry = int64_t (*)(void *) noexcept;
// The byte copy belongs to the new thread after context_copy returns. Its
// context_drop runs before completion is published, including clone failure.
extern "C" LangHandle *lang_thread_spawn_v1(LangThreadEntry, const void *,
                                            uint64_t, LangValueOp context_copy,
                                            LangValueOp context_drop) noexcept;
extern "C" int64_t lang_thread_error_v1(LangHandle *) noexcept;
extern "C" int64_t lang_thread_wait_v1(LangHandle *) noexcept;
// -1 waits forever, 0 polls once, and values below -1 are invalid.
// Returns 0 after completion, -110 at the monotonic deadline, or the existing
// creation/process/self-join error. -1 keeps lang_thread_wait_v1 semantics.
extern "C" int64_t lang_thread_wait_for_v1(LangHandle *,
                                           int64_t timeout_ns) noexcept;
extern "C" int64_t lang_thread_result_v1(LangHandle *) noexcept;

// Called only in the fork child, before returning to language code.
extern "C" void lang_thread_fork_child_v1() noexcept;

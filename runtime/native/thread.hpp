#pragma once
#include "handle.hpp"
#include "vec.hpp"
#include <cstdint>

using LangThreadEntry = int64_t (*)(void *) noexcept;
// The byte copy belongs to the new thread after context_copy returns. Its
// context_drop runs before completion is published, including clone failure.
extern "C" LangHandle *lang_thread_spawn_v1(LangThreadEntry, const void *,
                                            uint64_t, LangValueOp context_copy,
                                            LangValueOp context_drop) noexcept;
extern "C" int64_t lang_thread_error_v1(LangHandle *) noexcept;
extern "C" int64_t lang_thread_wait_v1(LangHandle *) noexcept;
extern "C" int64_t lang_thread_result_v1(LangHandle *) noexcept;

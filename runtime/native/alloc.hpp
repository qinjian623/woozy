#ifndef LANG_RUNTIME_NATIVE_ALLOC_HPP
#define LANG_RUNTIME_NATIVE_ALLOC_HPP

#include <cstddef>
#include <cstdint>

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept;
extern "C" void lang_free_v1(void *ptr) noexcept;

// Acquire before fork; parent releases, child resets inherited ticket state.
extern "C" void lang_alloc_fork_prepare_v1() noexcept;
extern "C" void lang_alloc_fork_parent_v1() noexcept;
extern "C" void lang_alloc_fork_child_v1() noexcept;

#endif

#ifndef LANG_RUNTIME_NATIVE_ALLOC_HPP
#define LANG_RUNTIME_NATIVE_ALLOC_HPP

#include <cstddef>
#include <cstdint>

extern "C" void *lang_alloc_v1(uint64_t size, uint64_t align) noexcept;
extern "C" void lang_free_v1(void *ptr) noexcept;

#endif

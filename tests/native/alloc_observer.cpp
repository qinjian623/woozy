#include <cstdint>
#include <cstdio>

// Test-only link-time wrappers. They keep the real allocator and record the
// exact user pointer passed to each successful allocation/free. The runner,
// not this wrapper, decides whether a sequence is balanced.
extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

extern "C" void *__wrap_lang_alloc_v1(uint64_t size,
                                      uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (pointer)
    std::fprintf(stderr, "ALLOC:%p\n", pointer);
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  std::fprintf(stderr, "FREE:%p\n", pointer);
  __real_lang_free_v1(pointer);
}

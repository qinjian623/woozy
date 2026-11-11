#include <cstdint>
extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;
extern "C" int __real_main();
namespace { uint64_t allocations, frees; }
extern "C" void *__wrap_lang_alloc_v1(uint64_t bytes, uint64_t align) noexcept {
  auto *value = __real_lang_alloc_v1(bytes, align);
  if (value) ++allocations;
  return value;
}
extern "C" void __wrap_lang_free_v1(void *value) noexcept {
  if (value) ++frees;
  __real_lang_free_v1(value);
}
extern "C" int __wrap_main() {
  const auto status = __real_main();
  char record[96]; unsigned size = 0;
  auto append = [&](uint64_t value) {
    char digits[24]; unsigned count = 0;
    do { digits[count++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (count) record[size++] = digits[--count];
  };
  append(allocations); record[size++] = ':'; append(frees); record[size++] = '\n';
  long written;
  __asm__ volatile("syscall" : "=a"(written)
      : "a"(1L), "D"(2L), "S"(record), "d"(uint64_t(size))
      : "rcx", "r11", "memory");
  return status ? status : allocations && allocations == frees ? 0 : 77;
}

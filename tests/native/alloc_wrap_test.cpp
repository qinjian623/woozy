#include <cstdint>

extern "C" void *lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void lang_free_v1(void *) noexcept;

// The CMake target links this file with alloc_observer.cpp and applies
// --wrap=lang_alloc_v1/--wrap=lang_free_v1. The runner parses the resulting
// ALLOC/FREE lines, so a successful exit alone is not used as evidence.
int main() {
  void *first = lang_alloc_v1(8, 8);
  void *second = lang_alloc_v1(16, 16);
  if (!first || !second || first == second)
    return 1;
  lang_free_v1(first);
  lang_free_v1(second);
  return 0;
}

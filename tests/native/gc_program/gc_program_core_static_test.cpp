// Default-core static ELF check: a program that links the allocator/panics/Vec
// and all GC layers but never calls lang_gc_program_init_v1. It must remain a
// no-libc/no-libstdc++ ELF and the Vec final release must take the disabled
// hook path without gettid or a registry lookup.

#include "vec.hpp"

extern "C" int main() {
  LangVec *value = lang_vec_new_v1(8, sizeof(uint64_t), nullptr, nullptr);
  for (uint64_t index = 0; index < 8; ++index) {
    uint64_t element = index * 3;
    lang_vec_init_v1(value, &element);
  }
  if (value->initialized != 8)
    return 1;
  lang_vec_release_v1(value);
  return 0;
}

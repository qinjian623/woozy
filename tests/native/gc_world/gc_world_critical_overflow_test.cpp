// Critical-depth overflow check. This binary is built with
// -DLANG_GC_WORLD_TEST_CRITICAL_MAX=2 so the checked overflow branch can be
// reached without four billion nested calls. Production keeps the full
// uint32_t limit; only this separate test translation unit lowers it.

#include "gc_world.hpp"

int main() {
  if (!lang_gc_world_register_v1())
    return 2;
  lang_gc_world_enter_critical_v1(); // depth 1
  lang_gc_world_enter_critical_v1(); // depth 2 == lowered maximum
  // This must Panic with gc-world:critical-depth-overflow.
  lang_gc_world_enter_critical_v1();
  return 3;
}

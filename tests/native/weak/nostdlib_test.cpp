// Minimal freestanding ELF smoke test for the Weak runtime.
//
// This translation unit has no main(), no libc and no C++ runtime. _start calls
// the test body directly, then uses the Linux exit syscall. It is linked with
// -nostdlib -static together with the production weak.cpp, alloc.cpp and
// panic.cpp objects. The accompanying runner also checks that the final ELF has
// no undefined symbols or dynamic dependencies, so host libc cannot hide a
// dependency accidentally introduced by the Weak implementation.

#include "weak.hpp"

#include <cstdint>

namespace {

constexpr uint64_t kMagic = 0x4E4F5354444C4942ULL; // "NOSTDLIB"
constexpr uint64_t kDeadMagic = 0x444541444C4942ULL;

struct Object {
  // Weak upgrade assumes the target's first field is the atomic ARC count.
  uint64_t strong_refs;
  LangWeakControl *weak_slot;
  uint64_t magic;
};

Object g_object;

// Returns true on the unique 1 -> 0 transition.
bool release_strong(Object *object) noexcept {
  uint64_t refs =
      __atomic_load_n(&object->strong_refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) return false;
    const uint64_t next = refs - 1;
    if (__atomic_compare_exchange_n(&object->strong_refs, &refs, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED))
      return next == 0;
  }
}

} // namespace

// panic.cpp exports this optional Vec observer as a weak reference. Resolve it
// locally so the freestanding ELF has no undefined symbol records at all; the
// Weak smoke test never triggers a Panic and never calls this observer.
extern "C" void lang_vec_observe_v1(uint32_t, const void *, uint64_t) noexcept {
}

extern "C" int lang_weak_nostdlib_main() noexcept {
  g_object.strong_refs = 1;
  g_object.weak_slot = nullptr;
  g_object.magic = kMagic;

  // Empty Weak operations must not require any runtime setup.
  lang_weak_retain_v1(nullptr);
  lang_weak_release_v1(nullptr);
  lang_weak_detach_v1(nullptr);
  if (lang_weak_alive_v1(nullptr) || lang_weak_upgrade_v1(nullptr))
    return 1;

  LangWeakControl *control =
      lang_weak_downgrade_v1(&g_object, &g_object.weak_slot);
  if (!control) return 2;
  if (!lang_weak_alive_v1(control)) return 3;

  // Upgrade installs one real strong reference.
  void *upgraded = lang_weak_upgrade_v1(control);
  if (upgraded != &g_object) return 4;
  if (g_object.magic != kMagic) return 5;
  if (release_strong(&g_object)) return 6; // only the upgraded ref went away

  // Copy/drop while the target is still alive.
  lang_weak_retain_v1(control);
  lang_weak_release_v1(control);
  if (lang_weak_upgrade_v1(control) != &g_object) return 7;
  if (release_strong(&g_object)) return 8;

  // Final strong release: detach before destroying the target storage.
  if (!release_strong(&g_object)) return 9;
  if (!g_object.weak_slot) return 10;
  lang_weak_detach_v1(g_object.weak_slot);
  g_object.magic = kDeadMagic;

  if (lang_weak_alive_v1(control)) return 11;
  if (lang_weak_upgrade_v1(control)) return 12;
  if (control->target != nullptr) return 13;

  // A dead control block is still copyable/droppable, then frees itself.
  lang_weak_retain_v1(control);
  lang_weak_release_v1(control);
  lang_weak_release_v1(control);
  return 0;
}

#if defined(__x86_64__) && defined(__linux__)
__asm__(
    ".global _start\n"
    ".type _start,@function\n"
    "_start:\n"
    "andq $-16, %rsp\n"
    "call lang_weak_nostdlib_main\n"
    "movl %eax, %edi\n"
    "movl $60, %eax\n"
    "syscall\n"
    "hlt\n"
    ".size _start, .-_start\n"
    ".section .note.GNU-stack,\"\",@progbits\n");
#else
#error "nostdlib Weak smoke test currently targets Linux x86_64"
#endif

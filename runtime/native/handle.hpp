#pragma once
#include <cstdint>

struct LangWeakControl;

enum class LangHandleKind : uint64_t { thread = 1, mutex, condvar, atomic_i64, text, ipc };
struct LangHandle {
  uint64_t refs;
  LangHandleKind kind;
  void (*destroy)(LangHandle *) noexcept;
  // Canonical Weak slot for ARC-shared handles. Constructors write null before
  // publication; the shared release path detaches it once before destroy.
  LangWeakControl *weak_control;
};
extern "C" void lang_handle_retain_v1(LangHandle *) noexcept;
extern "C" void lang_handle_release_v1(LangHandle *) noexcept;
extern "C" [[noreturn]] void lang_handle_panic_v1() noexcept;

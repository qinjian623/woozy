// Focused runtime evidence for the gc_program callback wrapper.  The Wz O0/O2
// projects exercise the generated bridge; this test checks the enabled/disabled
// wrapper, token layout, SAFE restoration, and registration ownership directly.
#include "gc_program.hpp"
#include "gc_world.hpp"

#include <pthread.h>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

struct ExternalResult {
  LangGcProgramCallbackToken token{};
  int32_t entered = -1;
  uint32_t created = 0;
  uint32_t saved_safe_depth = 0;
  uint32_t magic_after_leave = 1;
  bool can_collect_after_leave = true;
};

void *external_callback(void *opaque) {
  auto *result = static_cast<ExternalResult *>(opaque);
  result->entered =
      lang_gc_program_callback_enter_v1(&result->token);
  result->created = result->token.created_registration;
  result->saved_safe_depth = result->token.saved_safe_depth;
  lang_gc_program_callback_leave_v1(result->entered, &result->token);
  result->magic_after_leave = result->token.magic;
  // This query runs on the external thread.  The callback-created
  // registration must be gone, so the unregistered thread is not collectable.
  result->can_collect_after_leave = lang_gc_world_can_collect_v1();
  return nullptr;
}

} // namespace

int main() {
  static_assert(sizeof(LangGcProgramCallbackToken) == 32,
                "callback token must stay 32 bytes");
  static_assert(alignof(LangGcProgramCallbackToken) == 8,
                "callback token must stay 8-byte aligned");

  LangGcProgramCallbackToken token{};
  check(lang_gc_program_callback_enter_v1(&token) == 0,
        "disabled program must not enter callback scope");
  check(token.magic == 0, "disabled callback token must stay zero");

  check(lang_gc_program_init_v1() == LANG_GC_PROGRAM_OK,
        "GC program init");
  check(lang_gc_world_can_collect_v1(),
        "main thread is registered and ACTIVE after init");

  token = {};
  int32_t entered = lang_gc_program_callback_enter_v1(&token);
  check(entered == 1, "registered main callback enter");
  check(token.magic != 0, "callback enter sets token magic");
  check(token.created_registration == 0,
        "main callback must reuse its existing registration");
  check(token.saved_safe_depth == 0,
        "main callback started outside SAFE");
  lang_gc_program_callback_leave_v1(entered, &token);
  check(token.magic == 0, "callback leave clears token magic");
  check(lang_gc_world_can_collect_v1(),
        "main SAFE depth restored after callback leave");

  lang_gc_world_enter_safe_v1();
  check(!lang_gc_world_can_collect_v1(), "main is inside SAFE");
  token = {};
  entered = lang_gc_program_callback_enter_v1(&token);
  check(entered == 1, "callback enter from SAFE");
  check(token.saved_safe_depth == 1,
        "callback enter saves the complete SAFE depth");
  lang_gc_program_callback_leave_v1(entered, &token);
  check(!lang_gc_world_can_collect_v1(),
        "callback leave restores SAFE instead of ACTIVE");
  lang_gc_world_leave_safe_v1();
  check(lang_gc_world_can_collect_v1(),
        "outer SAFE leave returns to ACTIVE");

  for (int round = 0; round < 3; ++round) {
    ExternalResult external;
    pthread_t thread;
    check(pthread_create(&thread, nullptr, external_callback, &external) == 0,
          "create external pthread");
    check(pthread_join(thread, nullptr) == 0, "join external pthread");
    check(external.entered == 1, "external callback enter");
    check(external.created == 1,
          "external callback owns a temporary registration");
    check(external.saved_safe_depth == 0,
          "external callback started outside SAFE");
    check(external.magic_after_leave == 0,
          "external callback leave clears token");
    check(!external.can_collect_after_leave,
          "external registration removed after leave");
  }
  check(lang_gc_world_can_collect_v1(),
        "main remains registered after external callbacks");

  (void)lang_gc_program_finish_v1();
  token = {};
  check(lang_gc_program_callback_enter_v1(&token) == 0,
        "main-after-finish must not enter callback scope");
  check(token.magic == 0,
        "main-after-finish callback token must stay zero");

  if (failures == 0)
    std::puts("PASS gc_program callback wrapper");
  return failures == 0 ? 0 : 1;
}

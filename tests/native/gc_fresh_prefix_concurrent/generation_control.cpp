// Minimal hosted control for the F1 allocation-generation bookkeeping in
// observe_fresh_prefix.cpp.
//
// It contains the real shim translation unit and calls its real
// ``note_new``/``note_register`` decisions.  No collector is started, no
// thread is created, no Wz source is checked or built, and no runtime archive
// is rebuilt: the real hosted runtime archive linked here (the existing Root
// "hosted7f" build) only supplies the real ``lang_vec_new_v1`` /
// ``lang_vec_release_v1`` allocator so that the address reuse below is the
// allocator's own behaviour, not a Python model of it.
//
// The two properties under test:
//
//   * a later, differently shaped Vec that the allocator places at a released
//     target address retires the old generation and is not counted as a
//     duplicate registration of the old target or control;
//   * two registrations of the same object with no intervening constructor are
//     still one generation, so the repeat is counted and reported as a
//     duplicate (it must not be ignored).
//
// Build (single attempt, Root-confirmed hosted runtime archive):
//
//   clang++ -std=c++20 -O0 -ffunction-sections -fdata-sections \
//     generation_control.cpp <hosted-runtime.a> -pthread \
//     -Wl,--gc-sections -o generation_control
//
// Exit status 0 means every check held; 1 is a real failure.  The JSON line on
// stdout is the machine-readable summary; the shim's own destructor still
// prints its counts line on stderr and is not used as a pass signal.
#include "observe_fresh_prefix.cpp"

#include <cstdio>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char *name) noexcept {
  ++g_checks;
  if (!condition)
    ++g_failures;
  std::fprintf(stdout, "GENERATION_CHECK:%s:%s\n",
               condition ? "ok" : "failed", name);
}

LangVec *allocate(uint64_t length, uint64_t stride) noexcept {
  // Refs == 1 is the only external owner, so the release below is final and the
  // runtime allocator may hand the same slot to the next same-class request.
  return lang_vec_new_v1(length, stride, nullptr, nullptr);
}

void release(LangVec *object) noexcept {
  if (object)
    lang_vec_release_v1(object);
}

} // namespace

int main() {
  // ---- nonempty target: same-generation duplicate is counted --------------
  g_mode = Mode::NonEmpty;
  LangVec *target = allocate(kRingsLength, kPointerStride);
  check(target != nullptr, "target_allocated");
  check(note_new(target, kRingsLength, kPointerStride), "target_selected");
  check(g_target_new_hits == 1, "target_new_hits_is_one");
  check(note_register(target) == RegisterOutcome::TargetFirst,
        "first_registration_is_target");
  check(g_target_register_hits == 1, "target_register_hits_is_one");
  check(g_nonempty_register_hits == 1, "control_register_hits_is_one");
  check(note_register(target) == RegisterOutcome::TargetDuplicate,
        "same_generation_duplicate_detected");
  check(g_target_register_hits == 2, "duplicate_registration_counted");

  // ---- allocator reuse by a differently shaped Vec -----------------------
  const unsigned control_registers_before_reuse = g_nonempty_register_hits;
  const void *target_address = target;
  release(target);
  // length 1 stride 16 asks for the same size class as length 3 stride 8, so
  // the real allocator returns the released descriptor address.
  LangVec *reused = allocate(1, sizeof(PrefixNode));
  check(reused != nullptr, "reused_vec_allocated");
  check(static_cast<const void *>(reused) == target_address,
        "differently_shaped_reuse_of_raw_address");
  // A different shape must not be the old target, but the constructor return
  // still retires the previous generation at that address.
  check(!note_new(reused, 1, sizeof(PrefixNode)), "reused_new_is_not_target");
  check(g_target_track.generation == 0, "old_target_generation_retired");
  check(note_register(reused) == RegisterOutcome::Passthrough,
        "reused_register_is_passthrough");
  check(g_target_register_hits == 2, "reused_register_not_counted_as_target");
  check(g_nonempty_register_hits == control_registers_before_reuse,
        "reused_register_not_counted_as_control");
  release(reused);

  // ---- empty control: reuse by an empty Vec of another stride ------------
  // The real observer runs one mode per process; isolate this second case.
  g_nonempty_track = {};
  g_empty_track = {};
  g_target_track = {};
  g_allocation_serial = g_target_generation = 0;
  g_nonempty_new_hits = g_nonempty_register_hits = 0;
  g_empty_new_hits = g_empty_register_hits = 0;
  g_target_new_hits = g_target_register_hits = 0;
  g_target_length = g_target_stride = 0;
  g_mode = Mode::Empty;
  LangVec *empty_target = allocate(0, kPointerStride);
  check(empty_target != nullptr, "empty_target_allocated");
  check(note_new(empty_target, 0, kPointerStride), "empty_target_selected");
  check(note_register(empty_target) == RegisterOutcome::TargetFirst,
        "empty_first_registration_is_target");
  check(g_empty_register_hits == 1, "empty_control_register_hits_is_one");
  const unsigned empty_registers_before_reuse = g_target_register_hits;
  const void *empty_address = empty_target;
  release(empty_target);
  // An empty Vec[Node] has the same 80-byte request but a different stride.
  LangVec *empty_reused = allocate(0, sizeof(PrefixNode));
  check(empty_reused != nullptr, "empty_reused_vec_allocated");
  check(static_cast<const void *>(empty_reused) == empty_address,
        "empty_differently_shaped_reuse");
  check(!note_new(empty_reused, 0, sizeof(PrefixNode)),
        "empty_reused_new_is_not_target");
  check(note_register(empty_reused) == RegisterOutcome::Passthrough,
        "empty_reused_register_is_passthrough");
  check(g_empty_register_hits == 1, "empty_control_not_recounted");
  check(g_target_register_hits == empty_registers_before_reuse,
        "empty_reuse_does_not_touch_target");
  release(empty_reused);

  std::fprintf(stdout,
               "GENERATION_CONTROL:{\"status\":\"%s\",\"checks\":%d,"
               "\"failures\":%d}\n",
               g_failures == 0 ? "passed" : "failed", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

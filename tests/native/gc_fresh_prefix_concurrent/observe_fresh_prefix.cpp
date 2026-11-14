// Test-only hosted observer/shim for the fresh strong-edge Vec concurrency
// probe (SH15). It is linked only into the observed test binary; no production
// runtime unit, ABI or source-language surface is added.
//
// Two existing entry points are wrapped, and nothing else:
//
//   lang_vec_new_v1               select the one fresh Vec that the fixture
//                                 identifies by (length, pointer stride)
//   lang_gc_program_register_v1   bracket the generated prefix writes at both
//                                 ends, then perform one real registration
//
// While the mutator is inside __wrap_lang_vec_new_v1 a real host pthread
// registers itself with lang-gc-world/1 and calls the ordinary
// lang_gc_runtime_collect_v1(5 s). The stop request therefore becomes visible
// while the constructor result has initialized == 0 and can still be observed
// at the generated registration call, after the generated prefix writes. The
// wrapper never enters SAFE, never calls a Source blocking function, never
// calls lang_gc_collect_stopped_v1 and never polls the world before the real
// registration succeeded, so the interval under test is exactly the generated
// allocation-return -> registration interval.
//
// Only after __real_lang_gc_program_register_v1 returns true does the shim call
// lang_gc_world_poll_v1(). That single poll parks the mutator, lets the pending
// stop complete, and returns after the collector resumed the world. A shim that
// could not park (SAFE, critical, or fork-stable contamination, or an emitter
// that parked earlier and let the collector finish) is a hard failure with a
// distinct reason code, never a pass.
//
// The emitted report is a machine-readable record of what was actually
// observed. The Python runner asserts it; nothing here decides "passed".
//
// Selection is per allocation generation, never by a saved raw address alone.
// The allocator is free to hand a released descriptor address to a later Vec
// of another shape, so every constructor return retires any earlier tracked
// generation that occupied the same address.  A repeated registration with no
// intervening constructor is still the same generation: it is counted and
// rejected.  This is why a legitimately reused address is not counted as a
// duplicate registration of the old target while a missing ``new`` is.
//
// This observer is linked with the hosted runtime and libc/pthreads. It is not
// a freestanding core program: observe_gc.cpp supplies the allocation events,
// this file uses getenv/clock_gettime/pthread_create/sched_yield, and the
// runner records the ELF INTERP/NEEDED evidence for that link class.
#include "../../../runtime/native/gc_program.hpp"
#include "../../../runtime/native/gc_runtime.hpp"
#include "../../../runtime/native/gc_world.hpp"
#include "../../../runtime/native/vec.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t kPointerStride = sizeof(void *);
constexpr uint64_t kRingsLength = 3;
// Test-only mirror of the fixture's `struct Node { marker i32; children
// Vec[Node]; }`. The runner requires the observed element stride to equal
// sizeof(PrefixNode) and fails loudly instead of guessing another layout.
struct PrefixNode {
  int32_t marker;
  int32_t padding;
  void *children;
};
static_assert(sizeof(PrefixNode) == 16, "fixture Node stride changed");

constexpr int32_t kRingMarkers[3] = {11, 22, 33};
constexpr long long kCollectTimeoutNs = 5000000000LL;
constexpr double kHandshakeSeconds = 2.0;
constexpr double kCollectorJoinSeconds = 6.0;

long raw_write(int fd, const char *data, unsigned long count) noexcept {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(1L), "D"(static_cast<long>(fd)),
                     "S"(reinterpret_cast<long>(data)),
                     "d"(static_cast<long>(count))
                   : "rcx", "r11", "memory");
  return result;
}

struct Report {
  char bytes[4096];
  unsigned long size = 0;
  bool overflow = false;
  bool first = true;

  void put(char value) noexcept {
    if (size + 1 < sizeof(bytes))
      bytes[size++] = value;
    else
      overflow = true;
  }
  void text(const char *value) noexcept {
    while (*value) put(*value++);
  }
  void string(const char *value) noexcept {
    put('"');
    text(value);
    put('"');
  }
  void number(unsigned long long value) noexcept {
    char reversed[24];
    unsigned count = 0;
    do {
      reversed[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    while (count) put(reversed[--count]);
  }
  void signed_number(long long value) noexcept {
    if (value < 0) {
      put('-');
      number(static_cast<unsigned long long>(-value));
    } else {
      number(static_cast<unsigned long long>(value));
    }
  }
  void boolean(bool value) noexcept { text(value ? "true" : "false"); }
  void begin() noexcept {
    put('{');
    first = true;
  }
  void key(const char *name) noexcept {
    if (!first) put(',');
    first = false;
    string(name);
    put(':');
  }
  void end() noexcept { put('}'); }
  void flush(const char *prefix) noexcept {
    const char *cursor = prefix;
    unsigned long prefix_size = 0;
    while (*cursor++)
      ++prefix_size;
    raw_write(2, prefix, prefix_size);
    if (overflow) {
      const char fallback[] = "{\"overflow\":true}";
      raw_write(2, fallback, sizeof(fallback) - 1);
    } else {
      raw_write(2, bytes, size);
    }
    const char newline = '\n';
    raw_write(2, &newline, 1);
  }
};

enum class Mode { Unset, NonEmpty, Empty };

Mode g_mode = Mode::Unset;

// One tracked allocation generation.  ``generation`` is the allocation serial
// of the constructor return that produced this address and is 0 for a retired
// or never selected address.
struct TrackedAddress {
  const void *address = nullptr;
  uint64_t generation = 0;
};

// Selection state.  A target is only meaningful while ``g_target_track``
// still carries the generation recorded at its constructor return.
TrackedAddress g_nonempty_track{};
TrackedAddress g_empty_track{};
uint64_t g_allocation_serial = 0;
unsigned g_nonempty_new_hits = 0;
unsigned g_nonempty_register_hits = 0;
unsigned g_empty_new_hits = 0;
unsigned g_empty_register_hits = 0;
TrackedAddress g_target_track{};
uint64_t g_target_generation = 0;
unsigned g_target_new_hits = 0;
unsigned g_target_register_hits = 0;
uint64_t g_target_length = 0;
uint64_t g_target_stride = 0;

// Bracket observations.
uint64_t g_init_at_new = 0;
uint64_t g_refs_at_new = 0;
uint64_t g_init_at_register = 0;
uint64_t g_refs_at_register = 0;
int g_can_collect_at_new = 0;
int g_phase_at_new_return = -1;
int g_request_at_new_return = 0;
int g_collector_done_at_new_return = -1;
int g_phase_at_register = -1;
int g_collector_busy_at_register = -1;
int g_request_at_register = 0;
int g_collector_done_at_register = -1;
int g_markers_ok_at_register = 0;
int g_self_cycles_at_register = 0;
int g_real_register_ok = 0;
int g_request_after_poll = -1;
int g_phase_after_poll = -1;
int g_alive_after = 0;
int g_markers_after = 0;
int g_self_cycles_after = 0;
uint64_t g_refs_after = 0;
int g_collector_joined = 0;
int g_handshake_completed = 0;

// Collector thread state.
std::atomic<int> g_collector_ready{0};
std::atomic<int> g_collector_registered{0};
std::atomic<int> g_collector_done{0};
std::atomic<long long> g_collector_status{-99999};
LangGcStats g_collector_stats{};
pthread_t g_collector_thread;
bool g_collector_started = false;

const char *mode_name(Mode mode) noexcept {
  switch (mode) {
  case Mode::NonEmpty:
    return "nonempty";
  case Mode::Empty:
    return "empty";
  default:
    return "unset";
  }
}

[[noreturn]] void fail(const char *reason) noexcept {
  Report line;
  line.begin();
  line.key("schema");
  line.string("woozy/gc-fresh-prefix/failure/1");
  line.key("reason");
  line.string(reason);
  line.key("mode");
  line.string(mode_name(g_mode));
  line.key("target_new_hits");
  line.number(g_target_new_hits);
  line.key("target_register_hits");
  line.number(g_target_register_hits);
  line.key("nonempty_new_hits");
  line.number(g_nonempty_new_hits);
  line.key("nonempty_register_hits");
  line.number(g_nonempty_register_hits);
  line.key("empty_new_hits");
  line.number(g_empty_new_hits);
  line.key("empty_register_hits");
  line.number(g_empty_register_hits);
  line.end();
  line.flush("FRESH_PREFIX_FAIL:");
  _exit(90);
}

double monotonic_seconds() noexcept {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<double>(now.tv_sec) +
         static_cast<double>(now.tv_nsec) / 1e9;
}

bool wait_for_request(double seconds) noexcept {
  const double deadline = monotonic_seconds() + seconds;
  for (;;) {
    if (lang_gc_world_request_pending_v1())
      return true;
    if (g_collector_done.load(std::memory_order_acquire))
      return false;
    if (monotonic_seconds() >= deadline)
      return false;
    sched_yield();
  }
}

bool wait_for_collector(double seconds) noexcept {
  const double deadline = monotonic_seconds() + seconds;
  while (!g_collector_done.load(std::memory_order_acquire)) {
    if (monotonic_seconds() >= deadline)
      return false;
    sched_yield();
  }
  return true;
}

void *collector_main(void *) noexcept {
  if (!lang_gc_world_register_v1()) {
    g_collector_registered.store(0, std::memory_order_release);
    g_collector_status.store(-1000, std::memory_order_release);
    g_collector_done.store(1, std::memory_order_release);
    return nullptr;
  }
  g_collector_registered.store(1, std::memory_order_release);
  g_collector_ready.store(1, std::memory_order_release);
  LangGcStats stats{};
  const long long status =
      lang_gc_runtime_collect_v1(kCollectTimeoutNs, &stats);
  g_collector_stats = stats;
  g_collector_status.store(status, std::memory_order_release);
  // Remove this thread's world node before publishing completion: a dead
  // ACTIVE node left in the registry would block every later stop until its
  // timeout and would make the fixture's final pressure collection impossible.
  (void)lang_gc_world_unregister_v1();
  g_collector_done.store(1, std::memory_order_release);
  return nullptr;
}

void start_collector() noexcept {
  if (g_collector_started)
    fail("collector_already_started");
  g_collector_started = true;
  if (pthread_create(&g_collector_thread, nullptr, collector_main, nullptr) != 0)
    fail("collector_pthread_create");
  const double deadline = monotonic_seconds() + kHandshakeSeconds;
  while (!g_collector_ready.load(std::memory_order_acquire)) {
    if (g_collector_done.load(std::memory_order_acquire))
      break;
    if (monotonic_seconds() >= deadline)
      fail("collector_register_timeout");
    sched_yield();
  }
  if (g_collector_registered.load(std::memory_order_acquire) != 1)
    fail("collector_world_register_false");
}

// Reads the freshly written prefix of the selected object through the
// fixture's documented Node layout. Returns true only when every ring marker
// and every self-cycle pointer is what the source constructed.
bool prefix_payload_ok(const LangVec *object, bool *markers,
                       bool *self_cycles) noexcept {
  *markers = false;
  *self_cycles = false;
  if (object->length != kRingsLength || object->initialized != kRingsLength ||
      object->stride != kPointerStride || !object->data)
    return false;
  auto *children = static_cast<LangVec *const *>(object->data);
  bool all_markers = true;
  bool all_cycles = true;
  for (uint64_t index = 0; index < kRingsLength; ++index) {
    const LangVec *ring = children[index];
    if (!ring || ring->length != 1 || ring->initialized != 1 ||
        ring->stride != sizeof(PrefixNode) || !ring->data) {
      return false;
    }
    const auto *node = static_cast<const PrefixNode *>(ring->data);
    if (node->marker != kRingMarkers[index])
      all_markers = false;
    if (node->children != ring)
      all_cycles = false;
  }
  *markers = all_markers;
  *self_cycles = all_cycles;
  return all_markers && all_cycles;
}

bool empty_payload_ok(const LangVec *object) noexcept {
  return object->length == 0 && object->initialized == 0 &&
         object->stride == kPointerStride && object->data == nullptr;
}

// Retire an earlier tracked generation when a constructor returns the same raw
// address again, whatever the new shape is.
void retire_reused(TrackedAddress &tracked, const void *address) noexcept {
  if (tracked.generation && tracked.address == address) {
    tracked.address = nullptr;
    tracked.generation = 0;
  }
}

// Bookkeeping at one real ``lang_vec_new_v1`` return.  Returns true when this
// allocation generation is the selected probe target.  It reads only the
// freshly constructed descriptor; the wrapper owns the request/collector part.
bool note_new(LangVec *object, uint64_t length, uint64_t stride) noexcept {
  ++g_allocation_serial;
  retire_reused(g_nonempty_track, object);
  retire_reused(g_empty_track, object);
  retire_reused(g_target_track, object);
  if (!g_target_track.generation)
    g_target_generation = 0;

  const bool nonempty = length == kRingsLength && stride == kPointerStride;
  const bool empty = length == 0 && stride == kPointerStride;
  if (nonempty) {
    ++g_nonempty_new_hits;
    g_nonempty_track = {object, g_allocation_serial};
  }
  if (empty) {
    ++g_empty_new_hits;
    g_empty_track = {object, g_allocation_serial};
  }
  const bool selected = (g_mode == Mode::NonEmpty) ? nonempty : empty;
  if (!selected)
    return false;

  ++g_target_new_hits;
  g_target_track = {object, g_allocation_serial};
  g_target_generation = g_allocation_serial;
  g_target_length = length;
  g_target_stride = stride;
  g_init_at_new = object->initialized;
  g_refs_at_new = __atomic_load_n(&object->refs, __ATOMIC_RELAXED);
  return true;
}

enum class RegisterOutcome { Passthrough, TargetFirst, TargetDuplicate };

// Bookkeeping at one real ``lang_gc_program_register_v1`` call.  Only the
// generation selected by ``note_new`` can be the target; a second registration
// with no intervening constructor is still that generation and is reported as a
// duplicate instead of being silently ignored.
RegisterOutcome note_register(const void *object) noexcept {
  if (g_nonempty_track.generation && g_nonempty_track.address == object)
    ++g_nonempty_register_hits;
  if (g_empty_track.generation && g_empty_track.address == object)
    ++g_empty_register_hits;
  if (!g_target_track.generation || g_target_track.address != object ||
      g_target_generation == 0)
    return RegisterOutcome::Passthrough;
  ++g_target_register_hits;
  if (g_target_register_hits == 1)
    return RegisterOutcome::TargetFirst;
  return RegisterOutcome::TargetDuplicate;
}

} // namespace

__attribute__((constructor)) static void fresh_prefix_init_mode() noexcept {
  const char *value = std::getenv("GC_FRESH_PREFIX_MODE");
  if (value && value[0] == 'n' && value[1] == 'o' && value[2] == 'n')
    g_mode = Mode::NonEmpty;
  else if (value && value[0] == 'e' && value[1] == 'm' && value[2] == 'p')
    g_mode = Mode::Empty;
  else
    g_mode = Mode::Unset;
}

__attribute__((destructor)) static void fresh_prefix_report_counts() noexcept {
  Report line;
  line.begin();
  line.key("schema");
  line.string("woozy/gc-fresh-prefix/counts/1");
  line.key("mode");
  line.string(mode_name(g_mode));
  line.key("nonempty_new_hits");
  line.number(g_nonempty_new_hits);
  line.key("nonempty_register_hits");
  line.number(g_nonempty_register_hits);
  line.key("empty_new_hits");
  line.number(g_empty_new_hits);
  line.key("empty_register_hits");
  line.number(g_empty_register_hits);
  line.key("target_new_hits");
  line.number(g_target_new_hits);
  line.key("target_register_hits");
  line.number(g_target_register_hits);
  line.key("handshake_completed");
  line.boolean(g_handshake_completed == 1);
  line.key("collector_joined");
  line.boolean(g_collector_joined == 1);
  line.end();
  line.flush("FRESH_PREFIX_COUNTS:");
}

extern "C" LangVec *__real_lang_vec_new_v1(uint64_t, uint64_t, LangValueOp,
                                           LangValueOp) noexcept;

extern "C" LangVec *__wrap_lang_vec_new_v1(uint64_t length, uint64_t stride,
                                           LangValueOp copy,
                                           LangValueOp drop) noexcept {
  LangVec *object = __real_lang_vec_new_v1(length, stride, copy, drop);
  if (!object)
    return object;
  if (g_mode == Mode::Unset)
    fail("mode_unset");
  // A later constructor return at the same address retires the earlier
  // generation here, so an allocator reusing this address for another Vec can
  // never be mistaken for the old target or control.
  if (!note_new(object, length, stride))
    return object;
  g_can_collect_at_new = lang_gc_world_can_collect_v1() ? 1 : 0;

  // Preconditions at the real constructor return, before any request exists.
  if (g_init_at_new != 0)
    fail("initialized_at_new_not_zero");
  if (g_refs_at_new != 1)
    fail("target_refs_at_new");
  if (g_can_collect_at_new != 1)
    fail("mutator_not_active_before_request");

  start_collector();
  if (!wait_for_request(kHandshakeSeconds)) {
    if (g_collector_done.load(std::memory_order_acquire))
      fail("collector_finished_without_request");
    fail("request_not_observed_at_new_return");
  }
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  g_phase_at_new_return = static_cast<int>(stats.phase);
  g_request_at_new_return = lang_gc_world_request_pending_v1() ? 1 : 0;
  g_collector_done_at_new_return =
      g_collector_done.load(std::memory_order_acquire) ? 1 : 0;
  if (!g_request_at_new_return)
    fail("request_missing_at_new_return");
  if (g_collector_done_at_new_return)
    fail("collector_finished_before_new_return");
  if (g_phase_at_new_return != LANG_GC_RUNTIME_PHASE_STOPPING)
    fail("phase_not_stopping_at_new_return");
  return object;
}

extern "C" bool __real_lang_gc_program_register_v1(void *,
                                                   const LangGcOps *) noexcept;

extern "C" bool __wrap_lang_gc_program_register_v1(
    void *object, const LangGcOps *ops) noexcept {
  // Exactly one registration is allowed per selected allocation generation.
  // A second call with no intervening constructor is the same generation and
  // must be counted and rejected, not ignored.
  const RegisterOutcome outcome = note_register(object);
  if (outcome == RegisterOutcome::Passthrough)
    return __real_lang_gc_program_register_v1(object, ops);
  if (outcome == RegisterOutcome::TargetDuplicate)
    fail("duplicate_target_registration");

  auto *vec = static_cast<LangVec *>(object);
  g_init_at_register = vec->initialized;
  g_refs_at_register = __atomic_load_n(&vec->refs, __ATOMIC_RELAXED);
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  g_phase_at_register = static_cast<int>(stats.phase);
  g_collector_busy_at_register = static_cast<int>(stats.busy);
  g_request_at_register = lang_gc_world_request_pending_v1() ? 1 : 0;
  g_collector_done_at_register =
      g_collector_done.load(std::memory_order_acquire) ? 1 : 0;

  // The generated prefix writes are complete here; the pending request must
  // still span the whole interval.
  if (g_init_at_register != g_target_length)
    fail("initialized_prefix_incomplete");
  if (g_refs_at_register != 1)
    fail("target_extra_retain");
  if (g_collector_done_at_register)
    fail("collector_finished_before_register");
  if (!g_request_at_register)
    fail("request_missing_at_register");
  if (g_phase_at_register != LANG_GC_RUNTIME_PHASE_STOPPING)
    fail("phase_not_stopping_at_register");
  if (g_collector_busy_at_register != 1)
    fail("collector_not_busy_at_register");

  if (g_mode == Mode::NonEmpty) {
    bool markers = false;
    bool cycles = false;
    if (!prefix_payload_ok(vec, &markers, &cycles))
      fail("prefix_payload_invalid_at_register");
    g_markers_ok_at_register = markers ? 1 : 0;
    g_self_cycles_at_register = cycles ? 1 : 0;
  } else {
    if (!empty_payload_ok(vec))
      fail("empty_payload_invalid_at_register");
    g_markers_ok_at_register = 1;
    g_self_cycles_at_register = 1;
  }

  // Only a real, successful registration may be followed by the poll.
  g_real_register_ok = __real_lang_gc_program_register_v1(object, ops) ? 1 : 0;
  if (g_real_register_ok != 1)
    fail("real_register_returned_false");
  lang_gc_world_poll_v1();
  g_request_after_poll = lang_gc_world_request_pending_v1() ? 1 : 0;

  if (!wait_for_collector(kCollectorJoinSeconds))
    fail("collector_join_timeout");
  lang_gc_runtime_stats_v1(&stats);
  g_phase_after_poll = static_cast<int>(stats.phase);
  const long long status = g_collector_status.load(std::memory_order_acquire);
  if (status != 0) {
    if (status == -110)
      fail("collector_timeout");
    fail("collector_status_nonzero");
  }
  // A pending request after the poll means the mutator did not park: SAFE,
  // critical or fork-stable contamination, or an emitter that already parked.
  if (g_request_after_poll != 0)
    fail("poll_did_not_park");
  if (g_phase_after_poll != LANG_GC_RUNTIME_PHASE_IDLE)
    fail("phase_after_poll_not_idle");

  // Liveness after the collection completed, without any extra retain.
  g_refs_after = __atomic_load_n(&vec->refs, __ATOMIC_RELAXED);
  if (g_mode == Mode::NonEmpty) {
    bool markers = false;
    bool cycles = false;
    if (!prefix_payload_ok(vec, &markers, &cycles))
      fail("target_not_alive_after_collection");
    g_alive_after = 1;
    g_markers_after = markers ? 1 : 0;
    g_self_cycles_after = cycles ? 1 : 0;
  } else {
    if (!empty_payload_ok(vec))
      fail("empty_not_alive_after_collection");
    g_alive_after = 1;
    g_markers_after = 1;
    g_self_cycles_after = 1;
  }

  if (g_nonempty_track.generation && g_empty_track.generation &&
      g_nonempty_track.address == g_empty_track.address)
    fail("target_and_control_share_address");
  // The 6 s wait above bounds only the collector completion flag.  The join
  // itself has no separate deadline; it is bounded by the runner's outer
  // program budget (60 s by default), and its result is checked here.
  const int join_status = pthread_join(g_collector_thread, nullptr);
  if (join_status != 0)
    fail("collector_join_failed");
  g_collector_joined = 1;
  g_handshake_completed = 1;

  Report line;
  line.begin();
  line.key("schema");
  line.string("woozy/gc-fresh-prefix/handshake/1");
  line.key("mode");
  line.string(mode_name(g_mode));
  line.key("target_length");
  line.number(g_target_length);
  line.key("target_stride");
  line.number(g_target_stride);
  line.key("init_at_new");
  line.number(g_init_at_new);
  line.key("init_at_register");
  line.number(g_init_at_register);
  line.key("refs_at_new");
  line.number(g_refs_at_new);
  line.key("refs_at_register");
  line.number(g_refs_at_register);
  line.key("can_collect_at_new");
  line.number(static_cast<unsigned long long>(g_can_collect_at_new));
  line.key("phase_at_new_return");
  line.signed_number(g_phase_at_new_return);
  line.key("request_at_new_return");
  line.number(static_cast<unsigned long long>(g_request_at_new_return));
  line.key("collector_done_at_new_return");
  line.number(static_cast<unsigned long long>(g_collector_done_at_new_return));
  line.key("phase_at_register");
  line.signed_number(g_phase_at_register);
  line.key("collector_busy_at_register");
  line.number(static_cast<unsigned long long>(g_collector_busy_at_register));
  line.key("request_at_register");
  line.number(static_cast<unsigned long long>(g_request_at_register));
  line.key("collector_done_at_register");
  line.number(static_cast<unsigned long long>(g_collector_done_at_register));
  line.key("markers_ok_at_register");
  line.boolean(g_markers_ok_at_register == 1);
  line.key("self_cycles_at_register");
  line.boolean(g_self_cycles_at_register == 1);
  line.key("real_register_true");
  line.boolean(g_real_register_ok == 1);
  line.key("request_after_poll");
  line.signed_number(g_request_after_poll);
  line.key("phase_after_poll");
  line.signed_number(g_phase_after_poll);
  line.key("collector_status");
  line.signed_number(status);
  line.key("target_refs_after_collection");
  line.number(g_refs_after);
  line.key("alive_after_collection");
  line.boolean(g_alive_after == 1);
  line.key("markers_after_collection");
  line.boolean(g_markers_after == 1);
  line.key("self_cycles_after_collection");
  line.boolean(g_self_cycles_after == 1);
  line.key("collector_joined");
  line.boolean(g_collector_joined == 1);
  line.key("target_new_hits");
  line.number(g_target_new_hits);
  line.key("target_register_hits");
  line.number(g_target_register_hits);
  line.key("stats");
  line.begin();
  const struct {
    const char *name;
    uint64_t value;
  } fields[] = {
      {"registered", g_collector_stats.registered},
      {"traced_edges", g_collector_stats.traced_edges},
      {"reachable", g_collector_stats.reachable},
      {"candidates", g_collector_stats.candidates},
      {"pinned", g_collector_stats.pinned},
      {"cleared", g_collector_stats.cleared},
      {"collected", g_collector_stats.collected},
      {"registry_live_after", g_collector_stats.registry_live_after},
  };
  for (const auto &field : fields) {
    line.key(field.name);
    line.number(field.value);
  }
  line.end();
  line.end();
  line.flush("FRESH_PREFIX:");
  return true;
}

#include <cstddef>
#include <cstdint>
#include "alloc.hpp"
#include "gc_program.hpp"
#include "vec.hpp"
#include "weak.hpp"

#if defined(__x86_64__) && defined(__linux__)

static inline long sys_write(int fd, const void *buf, size_t count) noexcept {
  long ret;
  __asm__ volatile (
    "syscall"
    : "=a"(ret)
    : "a"(1), "D"(fd), "S"(buf), "d"(count)
    : "rcx", "r11", "memory"
  );
  return ret;
}

[[noreturn]] static inline void sys_exit(int code) noexcept {
  __asm__ volatile (
    "movl $231, %%eax\n\t"
    "syscall\n\t"
    "movl $60, %%eax\n\t"
    "syscall\n\t"
    "hlt"
    :
    : "D"(code)
    : "memory"
  );
  __builtin_unreachable();
}

#endif

// lang-native-scalars/1. This is a terminal Panic, not a typed language error.
// The record is also the runner's evidence; exit 101 alone is never sufficient.
extern "C" [[noreturn]] void lang_native_panic_v1(uint32_t cause,
                                                  uint64_t snapshot,
                                                  uint64_t begin,
                                                  uint64_t end) noexcept {
  char record[192];
  const char *name = cause == 1   ? "division-by-zero"
                     : cause == 2 ? "invalid-shift"
                     : cause == 3 ? "integer-conversion"
                     : cause == 4 ? "vec-index"
                     : cause == 5 ? "allocation-failed"
                     : cause == 6 ? "allocation-size-overflow"
                     : cause == 7 ? "reference-count-overflow"
                     : cause == 8 ? "active-iteration"
                     : cause == 9 ? "vec-slice"
                     : cause == 10 ? "runtime-handle-misuse"
                                  : "invalid-cause";

  size_t len = 0;
  auto append = [&](const char *s) {
    while (*s && len < sizeof(record) - 1)
      record[len++] = *s++;
  };
  auto append_u64 = [&](uint64_t val) {
    char temp[24];
    size_t tlen = 0;
    if (val == 0) {
      temp[tlen++] = '0';
    } else {
      while (val > 0) {
        temp[tlen++] = static_cast<char>('0' + (val % 10));
        val /= 10;
      }
    }
    while (tlen > 0 && len < sizeof(record) - 1)
      record[len++] = temp[--tlen];
  };

  append("LANG_NATIVE_PANIC:v1:");
  append(name);
  append(":");
  append_u64(snapshot);
  append(":");
  append_u64(begin);
  append(":");
  append_u64(end);
  append("\n");

  size_t offset = 0;
  while (offset < len) {
    long n = sys_write(2, record + offset, len - offset);
    if (n == -4) // raw -EINTR
      continue;
    if (n <= 0)
      break;
    offset += static_cast<size_t>(n);
  }
  sys_exit(101);
}

// Separate ordinary typed-error entry diagnostic; no panic or unwinding.
extern "C" void lang_native_typed_error_v1(const char *key,
                                           uint64_t size) noexcept {
  auto write_all = [](const char *bytes, size_t count) {
    while (count) {
      long n = sys_write(2, bytes, count);
      if (n == -4) // raw -EINTR
        continue;
      if (n <= 0)
        break;
      bytes += static_cast<size_t>(n);
      count -= static_cast<size_t>(n);
    }
  };
  constexpr char prefix[] = "LANG_TYPED_ERROR: ";
  write_all(prefix, sizeof(prefix) - 1);
  write_all(key, static_cast<size_t>(size));
  write_all("\n", 1);
}

// Tests may define this weak observer. There is no production setting or
// public language API. Events describe identity, initialized elements and free.
extern "C" void lang_vec_observe_v1(uint32_t, const void *, uint64_t)
    __attribute__((weak));
namespace {
void observe(uint32_t event, LangVec *v, uint64_t detail = 0) {
  if (lang_vec_observe_v1) lang_vec_observe_v1(event, v, detail);
}
[[noreturn]] void vec_panic(uint32_t cause) {
  lang_native_panic_v1(cause, 0, 0, 0);
}
void *element(LangVec *v, uint64_t i) {
  return static_cast<unsigned char *>(v->data) + i * v->stride;
}

// The descriptor and its payload are one allocation. The payload starts after
// the descriptor rounded up to the allocator's 16-byte alignment, so a
// non-empty payload never overlaps the header. Empty Vecs still allocate an
// independent descriptor and use a null data pointer instead of dummy bytes.
constexpr uint64_t kVecHeaderBytes =
    (static_cast<uint64_t>(sizeof(LangVec)) + 15) & ~uint64_t{15};
static_assert(sizeof(LangVec) == 80, "LangVec ABI must stay 80 bytes");
static_assert(offsetof(LangVec, weak_control) == 72,
              "LangVec Weak slot must follow capacity");
static_assert(kVecHeaderBytes >= sizeof(LangVec),
              "Vec payload must not overlap its descriptor");
static_assert(kVecHeaderBytes % 16 == 0,
              "Vec payload must remain 16-byte aligned");

constexpr uint64_t kMaxU64 = ~uint64_t{0};

// A non-empty inline payload begins immediately after the rounded descriptor.
// Any other non-null data pointer is a separately allocated buffer.
bool separate_payload(LangVec *v) noexcept {
  return v->data != nullptr &&
         v->data != reinterpret_cast<unsigned char *>(v) + kVecHeaderBytes;
}

uint64_t checked_payload_bytes(uint64_t capacity, uint64_t stride) noexcept {
  if (!stride || capacity > kMaxU64 / stride) vec_panic(6);
  return capacity * stride;
}

// Overlap-safe byte copy without libc. Structural moves still transfer element
// ownership bit-for-bit; this helper never invokes copy or drop callbacks.
void copy_bytes(void *destination, const void *source, uint64_t count) noexcept {
  if (!count) return;
  auto *dst = static_cast<unsigned char *>(destination);
  const auto *src = static_cast<const unsigned char *>(source);
  if (dst == src) return;
  if (reinterpret_cast<uintptr_t>(dst) < reinterpret_cast<uintptr_t>(src)) {
    for (uint64_t i = 0; i < count; ++i) dst[i] = src[i];
  } else {
    for (uint64_t i = count; i > 0; --i) dst[i - 1] = src[i - 1];
  }
}

bool aliases_payload(LangVec *v, const void *pointer) noexcept {
  if (!v->data || !pointer || !v->capacity || !v->stride) return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(v->data);
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  if (v->capacity > kMaxU64 / v->stride) return address >= begin;
  return address >= begin && address - begin < v->capacity * v->stride;
}

uint64_t grown_capacity(uint64_t current, uint64_t minimum) noexcept {
  uint64_t capacity = current ? current : 1;
  while (capacity < minimum) {
    if (capacity > kMaxU64 / 2) return minimum;
    capacity *= 2;
  }
  return capacity;
}

bool has_owner_ops(const LangVec *v) noexcept {
  return v->copy != nullptr || v->drop != nullptr;
}

// Small stage for one owning element or a short removed prefix. Avoiding the
// allocator for the common small case matters because existing release and
// set paths are allocation-counted by the Vec regression tests. A larger
// prefix uses one allocator block, never one allocation per element.
constexpr uint64_t kInlineStageBytes = 64;
class StageBuffer {
 public:
  StageBuffer() noexcept = default;
  StageBuffer(const StageBuffer &) = delete;
  StageBuffer &operator=(const StageBuffer &) = delete;
  ~StageBuffer() noexcept {
    if (heap_) lang_free_v1(heap_);
  }

  bool reserve(uint64_t bytes) noexcept {
    if (bytes <= kInlineStageBytes) {
      data_ = inline_bytes_;
      return true;
    }
    heap_ = lang_alloc_v1(bytes, 16);
    if (!heap_) return false;
    data_ = static_cast<unsigned char *>(heap_);
    return true;
  }

  unsigned char *data() const noexcept { return data_; }

 private:
  alignas(16) unsigned char inline_bytes_[kInlineStageBytes] = {};
  void *heap_ = nullptr;
  unsigned char *data_ = inline_bytes_;
};

// Caller has already rejected active iterations and checked for overflow.
void grow_storage(LangVec *v, uint64_t minimum) noexcept {
  const uint64_t capacity = grown_capacity(v->capacity, minimum);
  const uint64_t bytes = checked_payload_bytes(capacity, v->stride);
  void *data = lang_alloc_v1(bytes, 16);
  if (!data) vec_panic(5);
  if (v->initialized) {
    const uint64_t initialized_bytes =
        checked_payload_bytes(v->initialized, v->stride);
    copy_bytes(data, v->data, initialized_bytes);
  }
  // Publish the complete new storage before releasing the old separate buffer.
  // An owning Vec uses a short graph-critical window so a pending stop cannot
  // park between the payload swap and its publication. The inline allocation
  // belongs to the descriptor and stays until final release. Trivial numeric
  // Vecs keep the cheap path with no critical region or gettid.
  const bool old_separate = separate_payload(v);
  void *old_data = v->data;
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    v->data = data;
    v->capacity = capacity;
    if (old_separate) lang_free_v1(old_data);
  } else {
    v->data = data;
    v->capacity = capacity;
    if (old_separate) lang_free_v1(old_data);
  }
}

// Preserve an argument that points into storage which may be reallocated.
void *protect_alias(LangVec *v, const void *value) noexcept {
  if (!aliases_payload(v, value)) return nullptr;
  void *scratch = lang_alloc_v1(v->stride, 16);
  if (!scratch) vec_panic(5);
  copy_bytes(scratch, value, v->stride);
  return scratch;
}

// Stage removed owning values by moving their bytes into independent storage.
// The caller publishes the new length/initialized prefix first, then invokes
// drop callbacks outside any graph-critical region. Keeping the removed values
// in the stage means a destructor may re-enter the same Vec (including a
// structural push) without invalidating the remaining owners.
uint64_t stage_prefix(LangVec *v, uint64_t keep, StageBuffer &stage) noexcept {
  const uint64_t count = v->initialized > keep ? v->initialized - keep : 0;
  if (!count || !v->drop) return count;
  const uint64_t bytes = checked_payload_bytes(count, v->stride);
  if (!stage.reserve(bytes)) vec_panic(5);
  for (uint64_t index = 0; index < count; ++index)
    copy_bytes(stage.data() + index * v->stride,
               element(v, keep + index), v->stride);
  return count;
}

void drop_staged(LangVec *v, const StageBuffer &stage, uint64_t count) noexcept {
  if (!v->drop) return;
  for (uint64_t index = count; index > 0; --index)
    v->drop(stage.data() + (index - 1) * v->stride);
}

}

extern "C" LangVec *lang_vec_new_v1(uint64_t length, uint64_t stride,
                                    LangValueOp copy, LangValueOp drop) noexcept {
  if (!stride || length > 0xFFFFFFFFFFFFFFFFULL / stride) vec_panic(6);
  const uint64_t payload_bytes = length * stride;
  if (payload_bytes > 0xFFFFFFFFFFFFFFFFULL - kVecHeaderBytes) vec_panic(6);
  auto *v = static_cast<LangVec *>(
      lang_alloc_v1(kVecHeaderBytes + payload_bytes, 16));
  if (!v) vec_panic(5);
  auto *data = length ? reinterpret_cast<unsigned char *>(v) + kVecHeaderBytes
                      : nullptr;
  *v = {1, length, 0, stride, 0, data, copy, drop, length, nullptr};
  observe(1, v, length);
  return v;
}
extern "C" void lang_vec_retain_v1(LangVec *v) noexcept {
  // An existing live reference supplies ownership; this increment needs no acquire.
  uint64_t refs = __atomic_load_n(&v->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (refs == 0xFFFFFFFFFFFFFFFFULL) vec_panic(7);
    if (__atomic_compare_exchange_n(&v->refs, &refs, refs + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
      break;
  }
  observe(2, v, refs + 1);
}
extern "C" void lang_vec_release_v1(LangVec *v) noexcept {
  // Release each owner; acquire prior releases before the final cleanup.
  uint64_t refs = __atomic_load_n(&v->refs, __ATOMIC_RELAXED);
  if (!refs) vec_panic(7);
  observe(3, v, refs - 1);
  // Observers may retain the live object, so reload after the callback.
  refs = __atomic_load_n(&v->refs, __ATOMIC_RELAXED);
  for (;;) {
    if (!refs) vec_panic(7);
    const uint64_t next = refs - 1;
    if (__atomic_compare_exchange_n(&v->refs, &refs, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      if (next) return;
      break;
    }
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(&v->iterations, __ATOMIC_RELAXED)) vec_panic(8);
  // Ordinary final strong release: remove a registered object before any
  // payload drop can re-enter Woozy or observe a half-cleaned registration.
  // The disabled path is one atomic load; if gc_program.cpp is not linked the
  // hook pointer stays null and legacy single-unit builds are unchanged.
  if (lang::native::gc_program::enabled()) {
    auto final_release_hook =
        lang::native::gc_program::hooks_v1.vec_final_release;
    if (final_release_hook)
      final_release_hook(v);
  }
  // The strong count is already zero, so no upgrade can acquire the target.
  // Detach the canonical slot before payload cleanup and descriptor free; the
  // slot is never read again after this point.
  lang_weak_detach_v1(v->weak_control);
  // Final cleanup publishes an empty descriptor and moves the old payload out
  // before any arbitrary element drop can re-enter Woozy. The old storage is
  // kept alive until the loop finishes, so a drop callback still sees a valid
  // object header and a stable element address. Weak is already closed.
  const bool old_separate = separate_payload(v);
  unsigned char *old_data = static_cast<unsigned char *>(v->data);
  const uint64_t old_initialized = v->initialized;
  const uint64_t old_stride = v->stride;
  const LangValueOp drop = v->drop;
  v->length = 0;
  v->initialized = 0;
  v->data = nullptr;
  v->capacity = 0;
  if (drop || lang_vec_observe_v1) {
    for (uint64_t index = old_initialized; index > 0; --index) {
      const uint64_t element_index = index - 1;
      observe(4, v, element_index);
      if (drop)
        drop(old_data + element_index * old_stride);
    }
  }
  observe(5, v);
  // A separate payload, when present, is freed before the descriptor that
  // owns the inline allocation. The inline payload itself is interior storage.
  if (old_separate) lang_free_v1(old_data);
  lang_free_v1(v);
}
extern "C" void lang_vec_init_v1(LangVec *v, const void *value) noexcept {
  if (v->initialized >= v->length) vec_panic(4);
  // Copy bytes before publishing the new initialized prefix. A Vec trace must
  // never observe a slot whose element bytes are not yet complete.
  const uint64_t index = v->initialized;
  copy_bytes(element(v, index), value, v->stride);
  v->initialized = index + 1;
}
extern "C" void *lang_vec_at_v1(LangVec *v, uint64_t i) noexcept {
  if (i >= v->length || i >= v->initialized) vec_panic(4);
  return element(v, i);
}
// dyn Trait boxes are one-element Vecs whose payload pointer is the fat
// pointer's data field. These two helpers recover the descriptor without
// exposing LangVec layout to generated code. They are only valid for the
// compiler-created one-element box whose payload is inline.
static inline LangVec *trait_box_owner(void *data) noexcept {
  return reinterpret_cast<LangVec *>(static_cast<unsigned char *>(data) -
                                     kVecHeaderBytes);
}
extern "C" void lang_trait_box_release_v1(void *data) noexcept {
  lang_vec_release_v1(trait_box_owner(data));
}
extern "C" void *lang_trait_box_clone_v1(void *data) noexcept {
  lang_vec_retain_v1(trait_box_owner(data));
  return data;
}
extern "C" void *lang_trait_box_target_v1(void *data) noexcept {
  return trait_box_owner(data);
}
extern "C" void lang_vec_set_v1(LangVec *v, uint64_t i, const void *value) noexcept {
  void *target = lang_vec_at_v1(v, i);
  if (!has_owner_ops(v)) {
    // Trivial numeric elements have no callback and no owning edge.
    copy_bytes(target, value, v->stride);
    return;
  }
  // Set is a transfer of a pre-copied value, including when the caller passes
  // the destination slot itself after retaining its owner. Do not special-case
  // aliases: stage the old owner and the incoming bytes so publishing and the
  // old-owner drop stay balanced.
  StageBuffer staged_old;
  if (v->drop) {
    if (!staged_old.reserve(v->stride)) vec_panic(5);
    copy_bytes(staged_old.data(), target, v->stride);
  }
  // Keep the incoming bytes valid if they alias storage the publication can
  // overwrite. This is a byte move, not a second logical copy.
  void *alias = aliases_payload(v, value) ? protect_alias(v, value) : nullptr;
  const void *source = alias ? alias : value;
  {
    // Short graph-critical publish of the complete new element. No copy or
    // drop callback runs inside this region.
    lang::native::gc_program::CriticalGuard guard;
    copy_bytes(target, source, v->stride);
  }
  if (v->drop) v->drop(staged_old.data());
  if (alias) lang_free_v1(alias);
}
extern "C" LangVec *lang_vec_slice_v1(LangVec *v, uint64_t start,
                                      uint64_t end) noexcept {
  if (start > end || end > v->length) vec_panic(9);
  auto *result = lang_vec_new_v1(end-start, v->stride, v->copy, v->drop);
  // A copy callback may call arbitrary C retain code. Snapshot and retain each
  // source element before publishing the complete result prefix after all
  // callback work. Pin the source only when that callback exists, so a
  // structural re-entry cannot relocate the source slots underneath it.
  // Trivial numeric slices keep the cheap path with no pin and no gettid.
  bool pinned = false;
  if (v->copy) {
    (void)lang_vec_begin_v1(v);
    pinned = true;
  }
  // Snapshot one complete element before each copy callback. A callback may
  // legally set the source slot to a different ARC object; the result must own
  // exactly the retained raw snapshot, never a later re-read of the source.
  StageBuffer snapshot;
  if (result->copy && !snapshot.reserve(v->stride)) vec_panic(5);
  for (uint64_t i = start; i < end; ++i) {
    void *source = lang_vec_at_v1(v, i);
    if (result->copy) {
      copy_bytes(snapshot.data(), source, v->stride);
      result->copy(snapshot.data());
      copy_bytes(element(result, i - start), snapshot.data(), v->stride);
    } else {
      copy_bytes(element(result, i - start), source, v->stride);
    }
  }
  if (has_owner_ops(result)) {
    lang::native::gc_program::CriticalGuard guard;
    result->initialized = end - start;
  } else {
    result->initialized = end - start;
  }
  if (pinned) lang_vec_end_v1(v);
  return result;
}
extern "C" uint64_t lang_vec_begin_v1(LangVec *v) noexcept {
  // Concurrent read-only loops share this counter, not their element accesses.
  uint64_t iterations = __atomic_load_n(&v->iterations, __ATOMIC_RELAXED);
  for (;;) {
    if (iterations == 0xFFFFFFFFFFFFFFFFULL) vec_panic(8);
    if (__atomic_compare_exchange_n(&v->iterations, &iterations,
                                    iterations + 1, false, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
      break;
  }
  observe(6, v, iterations + 1);
  return v->length;
}
extern "C" void lang_vec_end_v1(LangVec *v) noexcept {
  uint64_t iterations = __atomic_load_n(&v->iterations, __ATOMIC_RELAXED);
  for (;;) {
    if (!iterations) vec_panic(8);
    const uint64_t next = iterations - 1;
    if (__atomic_compare_exchange_n(&v->iterations, &iterations, next, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      observe(7, v, next);
      return;
    }
  }
}
extern "C" uint64_t lang_vec_length_v1(LangVec *v) noexcept { return v->length; }
extern "C" uint64_t lang_vec_capacity_v1(LangVec *v) noexcept {
  return v->capacity;
}
extern "C" void lang_vec_reserve_v1(LangVec *v, uint64_t minimum) noexcept {
  // Reserving within existing capacity is not a structural change.
  if (minimum <= v->capacity) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  grow_storage(v, minimum);
}
extern "C" void lang_vec_push_v1(LangVec *v, const void *value) noexcept {
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  if (v->length == kMaxU64) vec_panic(6);
  const uint64_t length = v->length + 1;
  void *scratch = nullptr;
  if (length > v->capacity) {
    // A self-aliased value may live in a separate buffer that growth frees.
    scratch = protect_alias(v, value);
    grow_storage(v, length);
  }
  // Push is a transfer of an already pre-copied value: no copy callback.
  copy_bytes(element(v, v->length), scratch ? scratch : value, v->stride);
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    if (v->initialized < length) v->initialized = length;
    v->length = length;
  } else {
    if (v->initialized < length) v->initialized = length;
    v->length = length;
  }
  if (scratch) lang_free_v1(scratch);
}
extern "C" bool lang_vec_pop_v1(LangVec *v, void *out) noexcept {
  // An empty pop performs no structural change and leaves out untouched.
  if (!v->length) return false;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  const uint64_t last = v->length - 1;
  // Pop moves one owner out; it must not drop it. Publish the shorter prefix
  // before returning the transferred bytes to the caller.
  if (out) copy_bytes(out, element(v, last), v->stride);
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    v->length = last;
    if (v->initialized > last) v->initialized = last;
  } else {
    v->length = last;
    if (v->initialized > last) v->initialized = last;
  }
  return true;
}
extern "C" void lang_vec_resize_v1(LangVec *v, uint64_t length,
                                    const void *fill) noexcept {
  const uint64_t old_length = v->length;
  if (length == old_length) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  if (length < old_length) {
    // fill is intentionally not read while shrinking. Move every removed
    // owner aside, publish the shorter prefix, then drop from the stage so a
    // destructor can re-enter the same Vec without invalidating later owners.
    const uint64_t keep = v->initialized > length ? length : v->initialized;
    StageBuffer stage;
    const uint64_t removed = stage_prefix(v, keep, stage);
    if (has_owner_ops(v)) {
      lang::native::gc_program::CriticalGuard guard;
      v->initialized = keep;
      v->length = length;
    } else {
      v->initialized = keep;
      v->length = length;
    }
    drop_staged(v, stage, removed);
    return;
  }
  // Growing performs logical copies. Snapshot each iteration's raw fill bytes
  // before the copy callback, retain that snapshot, and publish exactly the
  // snapshot. A callback may set the source slot to another ARC object; it must
  // not change which owner this new element receives. The callback never runs
  // inside the graph-critical publication window.
  void *scratch = nullptr;
  if (length > v->capacity) {
    // fill may point into the old separate buffer; retain its bytes first so
    // growth can free the old storage safely.
    scratch = protect_alias(v, fill);
  }
  const void *source = scratch ? scratch : fill;
  bool pinned = false;
  if (v->copy) {
    (void)lang_vec_begin_v1(v);
    pinned = true;
  }
  if (length > v->capacity) grow_storage(v, length);
  StageBuffer snapshot;
  if (v->copy && !snapshot.reserve(v->stride)) vec_panic(5);
  for (uint64_t index = old_length; index < length; ++index) {
    if (v->copy) {
      // Retain the source owner from this iteration's independent snapshot
      // before its bytes become a published Vec edge.
      copy_bytes(snapshot.data(), source, v->stride);
      v->copy(snapshot.data());
      copy_bytes(element(v, index), snapshot.data(), v->stride);
    } else {
      copy_bytes(element(v, index), source, v->stride);
    }
  }
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    v->initialized = length;
    v->length = length;
  } else {
    v->initialized = length;
    v->length = length;
  }
  if (pinned) lang_vec_end_v1(v);
  if (scratch) lang_free_v1(scratch);
}
extern "C" void lang_vec_clear_v1(LangVec *v) noexcept {
  if (!v->length && !v->initialized) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  // Move the removed owning prefix aside before publishing an empty Vec. The
  // old owners stay valid while every destructor runs outside the critical
  // region; storage and capacity themselves stay published/unchanged.
  StageBuffer stage;
  const uint64_t removed = stage_prefix(v, 0, stage);
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    v->initialized = 0;
    v->length = 0;
  } else {
    v->initialized = 0;
    v->length = 0;
  }
  drop_staged(v, stage, removed);
}
extern "C" void lang_vec_check_resize_v1(LangVec *v) noexcept {
  // Structural changes still require caller synchronization against new iterations.
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
}

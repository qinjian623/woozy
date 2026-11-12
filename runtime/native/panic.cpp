#include <cstddef>
#include <cstdint>
#include "alloc.hpp"
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
  // The inline allocation belongs to the descriptor and stays until final
  // release. Only an earlier separate buffer is freed here.
  if (separate_payload(v)) lang_free_v1(v->data);
  v->data = data;
  v->capacity = capacity;
}

// Preserve an argument that points into storage which may be reallocated.
void *protect_alias(LangVec *v, const void *value) noexcept {
  if (!aliases_payload(v, value)) return nullptr;
  void *scratch = lang_alloc_v1(v->stride, 16);
  if (!scratch) vec_panic(5);
  copy_bytes(scratch, value, v->stride);
  return scratch;
}

void drop_initialized_to(LangVec *v, uint64_t length) noexcept {
  while (v->initialized > length) {
    const uint64_t index = --v->initialized;
    if (v->drop) v->drop(element(v, index));
  }
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
  // The strong count is already zero, so no upgrade can acquire the target.
  // Detach the canonical slot before payload cleanup and descriptor free; the
  // slot is never read again after this point.
  lang_weak_detach_v1(v->weak_control);
  if (!v->drop && !lang_vec_observe_v1) {
    // Trivial elements have no cleanup and no observer needs per-element
    // events.
    v->initialized = 0;
  } else {
    while (v->initialized) {
      auto i = --v->initialized;
      observe(4, v, i);
      if (v->drop) v->drop(element(v, i));
    }
  }
  observe(5, v);
  // A separate payload, when present, is freed before the descriptor that
  // owns the inline allocation. The inline payload itself is interior storage.
  if (separate_payload(v)) lang_free_v1(v->data);
  lang_free_v1(v);
}
extern "C" void lang_vec_init_v1(LangVec *v, const void *value) noexcept {
  if (v->initialized >= v->length) vec_panic(4);
  unsigned char *target = static_cast<unsigned char *>(element(v, v->initialized++));
  const unsigned char *src = static_cast<const unsigned char *>(value);
  for (uint64_t i = 0; i < v->stride; ++i)
    target[i] = src[i];
}
extern "C" void *lang_vec_at_v1(LangVec *v, uint64_t i) noexcept {
  if (i >= v->length || i >= v->initialized) vec_panic(4);
  return element(v, i);
}
extern "C" void lang_vec_set_v1(LangVec *v, uint64_t i, const void *value) noexcept {
  void *target = lang_vec_at_v1(v, i);
  if (v->drop) v->drop(target);
  unsigned char *dst = static_cast<unsigned char *>(target);
  const unsigned char *src = static_cast<const unsigned char *>(value);
  for (uint64_t k = 0; k < v->stride; ++k)
    dst[k] = src[k];
}
extern "C" LangVec *lang_vec_slice_v1(LangVec *v, uint64_t start,
                                      uint64_t end) noexcept {
  if (start > end || end > v->length) vec_panic(9);
  auto *result = lang_vec_new_v1(end-start, v->stride, v->copy, v->drop);
  for (uint64_t i = start; i < end; ++i) {
    lang_vec_init_v1(result, lang_vec_at_v1(v, i));
    if (result->copy)
      result->copy(element(result, result->initialized - 1));
  }
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
  copy_bytes(element(v, v->length), scratch ? scratch : value, v->stride);
  if (v->initialized < length) v->initialized = length;
  v->length = length;
  if (scratch) lang_free_v1(scratch);
}
extern "C" bool lang_vec_pop_v1(LangVec *v, void *out) noexcept {
  // An empty pop performs no structural change and leaves out untouched.
  if (!v->length) return false;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  const uint64_t last = v->length - 1;
  if (out) copy_bytes(out, element(v, last), v->stride);
  v->length = last;
  if (v->initialized > last) v->initialized = last;
  return true;
}
extern "C" void lang_vec_resize_v1(LangVec *v, uint64_t length,
                                    const void *fill) noexcept {
  const uint64_t old_length = v->length;
  if (length == old_length) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  if (length < old_length) {
    // fill is intentionally not read while shrinking.
    drop_initialized_to(v, length);
    v->length = length;
    return;
  }
  void *scratch = nullptr;
  if (length > v->capacity) {
    // fill may point into the old separate buffer; retain its bytes first.
    scratch = protect_alias(v, fill);
    grow_storage(v, length);
  }
  const void *source = scratch ? scratch : fill;
  for (uint64_t index = old_length; index < length; ++index) {
    void *target = element(v, index);
    copy_bytes(target, source, v->stride);
    if (v->copy) v->copy(target);
  }
  v->initialized = length;
  v->length = length;
  if (scratch) lang_free_v1(scratch);
}
extern "C" void lang_vec_clear_v1(LangVec *v) noexcept {
  if (!v->length && !v->initialized) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  drop_initialized_to(v, 0);
  v->length = 0;
}
extern "C" void lang_vec_check_resize_v1(LangVec *v) noexcept {
  // Structural changes still require caller synchronization against new iterations.
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
}

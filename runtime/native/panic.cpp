#include <cstddef>
#include <cstdint>
#include "alloc.hpp"
#include "gc_program.hpp"
#include "vec.hpp"
#include "weak.hpp"

#if defined(__APPLE__) && defined(__aarch64__)
#include <cerrno>
#include <unistd.h>

static inline long sys_write(int fd, const void *buf, size_t count) noexcept {
  const auto result = write(fd, buf, count);
  return result < 0 ? -errno : result;
}

[[noreturn]] static inline void sys_exit(int code) noexcept { _exit(code); }

#elif defined(__x86_64__) && defined(__linux__)

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
// One scalar slot covers the common element strides. Both halves of each
// memcpy name a distinct object, so the load completes before any store and a
// partial overlap between destination and source keeps memmove semantics.
template <typename Word>
void copy_word(void *destination, const void *source) noexcept {
  Word bits;
  __builtin_memcpy(&bits, source, sizeof(bits));
  __builtin_memcpy(destination, &bits, sizeof(bits));
}

template <typename Word>
void fill_words(void *destination, const void *source, uint64_t count) noexcept {
  Word bits;
  __builtin_memcpy(&bits, source, sizeof(bits));
  auto *out = static_cast<unsigned char *>(destination);
  for (uint64_t index = 0; index < count; ++index)
    __builtin_memcpy(out + index * sizeof(bits), &bits, sizeof(bits));
}

using CopyLane = uint64_t __attribute__((vector_size(16)));
constexpr uint64_t kCopyBlockBytes = 4 * sizeof(CopyLane);

void copy_block(unsigned char *dst, const unsigned char *src) noexcept {
  // Load the entire block before any store because the lanes may overlap.
  CopyLane first, second, third, fourth;
  __builtin_memcpy(&first, src, sizeof(CopyLane));
  __builtin_memcpy(&second, src + sizeof(CopyLane), sizeof(CopyLane));
  __builtin_memcpy(&third, src + 2 * sizeof(CopyLane), sizeof(CopyLane));
  __builtin_memcpy(&fourth, src + 3 * sizeof(CopyLane), sizeof(CopyLane));
  __builtin_memcpy(dst, &first, sizeof(CopyLane));
  __builtin_memcpy(dst + sizeof(CopyLane), &second, sizeof(CopyLane));
  __builtin_memcpy(dst + 2 * sizeof(CopyLane), &third, sizeof(CopyLane));
  __builtin_memcpy(dst + 3 * sizeof(CopyLane), &fourth, sizeof(CopyLane));
}

[[gnu::noinline]]
void copy_bytes_variable(void *destination, const void *source,
                         uint64_t count) noexcept {
  auto *dst = static_cast<unsigned char *>(destination);
  const auto *src = static_cast<const unsigned char *>(source);
  if (dst == src) return;
  const uintptr_t d = reinterpret_cast<uintptr_t>(dst);
  const uintptr_t s = reinterpret_cast<uintptr_t>(src);
  // Disjoint ranges can sweep forward even when the destination is higher;
  // only a higher destination closer than count needs the reverse sweep.
  if (d < s || d - s >= count) {
    uint64_t i = 0;
    for (; count - i >= kCopyBlockBytes; i += kCopyBlockBytes)
      copy_block(dst + i, src + i);
    for (; count - i >= sizeof(CopyLane); i += sizeof(CopyLane))
      copy_word<CopyLane>(dst + i, src + i);
    for (; i < count; ++i) dst[i] = src[i];
  } else {
    while (count >= kCopyBlockBytes) {
      count -= kCopyBlockBytes;
      copy_block(dst + count, src + count);
    }
    while (count >= sizeof(CopyLane)) {
      count -= sizeof(CopyLane);
      copy_word<CopyLane>(dst + count, src + count);
    }
    for (uint64_t i = count; i > 0; --i) dst[i - 1] = src[i - 1];
  }
}

void copy_bytes(void *destination, const void *source, uint64_t count) noexcept {
  switch (count) {
  case 0: return;
  case 1: return copy_word<uint8_t>(destination, source);
  case 2: return copy_word<uint16_t>(destination, source);
  case 4: return copy_word<uint32_t>(destination, source);
  case 8: return copy_word<uint64_t>(destination, source);
  default: return copy_bytes_variable(destination, source, count);
  }
}

bool aliases_payload(LangVec *v, const void *pointer) noexcept {
  if (!v->data || !pointer || !v->capacity || !v->stride) return false;
  const uintptr_t begin = reinterpret_cast<uintptr_t>(v->data);
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  if (v->capacity > kMaxU64 / v->stride) return address >= begin;
  return address >= begin && address - begin < v->capacity * v->stride;
}

// True when two byte ranges share at least one byte. Difference arithmetic
// avoids forming an out-of-range end address; both lengths are already checked.
bool byte_ranges_overlap(const void *first, uint64_t first_bytes,
                         const void *second, uint64_t second_bytes) noexcept {
  if (!first_bytes || !second_bytes) return false;
  const uintptr_t first_begin = reinterpret_cast<uintptr_t>(first);
  const uintptr_t second_begin = reinterpret_cast<uintptr_t>(second);
  return first_begin <= second_begin
             ? second_begin - first_begin < first_bytes
             : first_begin - second_begin < second_bytes;
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

// Publish storage only after allocation and copying succeed.
bool try_replace_storage(LangVec *v, uint64_t capacity,
                         uint64_t bytes) noexcept {
  void *data = lang_alloc_v1(bytes, 16);
  if (!data) return false;
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
  return true;
}

// Caller has already rejected active iterations and checked for overflow.
void grow_storage(LangVec *v, uint64_t minimum) noexcept {
  const uint64_t capacity = grown_capacity(v->capacity, minimum);
  const uint64_t bytes = checked_payload_bytes(capacity, v->stride);
  if (!try_replace_storage(v, capacity, bytes)) vec_panic(5);
}

// Preserve an argument that points into storage which may be reallocated.
void *protect_alias(LangVec *v, const void *value) noexcept {
  if (!aliases_payload(v, value)) return nullptr;
  void *scratch = lang_alloc_v1(v->stride, 16);
  if (!scratch) vec_panic(5);
  copy_bytes(scratch, value, v->stride);
  return scratch;
}

// Push transfers an already pre-copied value, so no element copy callback runs.
// The slot bytes are complete before the length publication below.
void append_value(LangVec *v, const void *value, uint64_t length) noexcept {
  copy_bytes(element(v, v->length), value, v->stride);
  if (has_owner_ops(v)) {
    lang::native::gc_program::CriticalGuard guard;
    if (v->initialized < length) v->initialized = length;
    v->length = length;
  } else {
    if (v->initialized < length) v->initialized = length;
    v->length = length;
  }
}

[[gnu::noinline]]
void grow_and_append(LangVec *v, const void *value, uint64_t length) noexcept {
  // A self-aliased value may live in the separate buffer that growth frees.
  void *scratch = protect_alias(v, value);
  grow_storage(v, length);
  append_value(v, scratch ? scratch : value, length);
  if (scratch) lang_free_v1(scratch);
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
extern "C" bool lang_vec_try_move_trivial_v1(
    LangVec *v, uint64_t destination, uint64_t source, uint64_t count) noexcept {
  const uint64_t limit = v->initialized < v->length ? v->initialized : v->length;
  if (destination > limit || source > limit ||
      count > limit - destination || count > limit - source)
    vec_panic(4);
  if (has_owner_ops(v)) return false;
  if (count && destination != source) {
    (void)checked_payload_bytes(limit, v->stride);
    copy_bytes(element(v, destination), element(v, source), count * v->stride);
  }
  return true;
}
extern "C" bool lang_vec_extend_trivial_v1(LangVec *values,
                                           LangVec *extra) noexcept {
  // Managed elements and partially initialized vectors keep the public
  // element loop so its copy callbacks, pin checks and index Panics still run.
  if (has_owner_ops(values) || has_owner_ops(extra)) return false;
  // Only an exactly fully initialized pair has the prefix this single
  // publication produces. A longer initialized prefix is an internal raw
  // state; copying it and then publishing length would shorten it, so leave
  // every such state to the public loop.
  if (values->initialized != values->length ||
      extra->initialized != extra->length)
    return false;
  // A distinct descriptor over the destination payload would be invalidated
  // by growth and could be overwritten by the copy. Treat that unknown shared
  // storage as unsupported instead of guessing an owner relationship.
  if (values != extra &&
      (aliases_payload(values, extra->data) ||
       aliases_payload(extra, values->data)))
    return false;
  if (values->stride != extra->stride) return false;
  const uint64_t count = extra->length;
  // An empty source changes nothing, so it succeeds even while pinned.
  if (!count) return true;
  if (__atomic_load_n(&values->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  // Overflow and allocation failure return false before publishing anything,
  // so the public loop below keeps the original per-element failure order.
  if (values->length > kMaxU64 - count) return false;
  const uint64_t length = values->length + count;
  if (!values->stride || length > kMaxU64 / values->stride) return false;
  const uint64_t old_length = values->length;
  if (length > values->capacity) {
    const uint64_t capacity = grown_capacity(values->capacity, length);
    if (capacity > kMaxU64 / values->stride ||
        !try_replace_storage(values, capacity, capacity * values->stride))
      return false;
  }
  // Resolve the source after growth: a self extend reads the current payload,
  // while an independent source keeps its own storage. One overlap-safe copy
  // publishes no partial element and invokes no callback.
  copy_bytes(element(values, old_length), element(extra, 0),
             count * values->stride);
  values->initialized = length;
  values->length = length;
  return true;
}
namespace {
// Each guarded chunk restores the carried element to the vacated slot, so
// the collector sees the same initialized slots and exact owner multiplicities
// at every critical exit. A failed carry allocation leaves all bytes unchanged.
bool move_range_one_bounded(LangVec *v, uint64_t first, uint64_t last,
                            bool toward_end) noexcept {
  constexpr uint64_t kRotationChunkBytes = 4096;
  uint64_t chunk = kRotationChunkBytes / v->stride;
  if (!chunk) chunk = 1;
  StageBuffer carry;
  if (!carry.reserve(v->stride)) return false;
  if (toward_end) {
    copy_bytes(carry.data(), element(v, last), v->stride);
    uint64_t hole = last;
    while (hole > first) {
      uint64_t count = hole - first;
      if (count > chunk) count = chunk;
      const uint64_t start = hole - count;
      const uint64_t bytes = checked_payload_bytes(count, v->stride);
      {
        lang::native::gc_program::CriticalGuard guard;
        copy_bytes(element(v, start + 1), element(v, start), bytes);
        copy_bytes(element(v, start), carry.data(), v->stride);
      }
      hole = start;
    }
    return true;
  }
  copy_bytes(carry.data(), element(v, first), v->stride);
  uint64_t hole = first;
  while (hole < last) {
    uint64_t count = last - hole;
    if (count > chunk) count = chunk;
    const uint64_t bytes = checked_payload_bytes(count, v->stride);
    {
      lang::native::gc_program::CriticalGuard guard;
      copy_bytes(element(v, hole), element(v, hole + 1), bytes);
      copy_bytes(element(v, hole + count), carry.data(), v->stride);
    }
    hole += count;
  }
  return true;
}
} // namespace

extern "C" bool lang_vec_rotate_right_one_v1(
    LangVec *v, uint64_t first, uint64_t last) noexcept {
  // The first argument ordering selects the direction without a new private
  // operation: first <= last rotates that range toward the end (insert), while
  // first > last rotates [last, first] toward the front (remove). The insert
  // path is unchanged. Both directions check the range and active pin before
  // moving bytes without invoking callbacks.
  if (first <= last) {
    // last is the final initialized slot in the rotated range. The caller has
    // already appended the new owning value, so every slot through last exists.
    if (last >= v->length || last >= v->initialized) vec_panic(4);
    // Trivial elements keep the existing try_move_trivial path in std/vec.
    if (!has_owner_ops(v)) return false;
    if (first == last) return true;
    // The public insert path already checked this before its push. Keep the
    // private operation safe for direct runtime callers too.
    if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
    return move_range_one_bounded(v, first, last, true);
  }
  // first > last: left rotation of the fully initialized suffix [last, first].
  if (first >= v->length || first >= v->initialized) vec_panic(4);
  if (!has_owner_ops(v)) return false;
  // remove_at uses the fully initialized suffix ending at the final slot.
  if (v->initialized != v->length || first + 1 != v->length) return false;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  return move_range_one_bounded(v, last, first, false);
}
extern "C" LangVec *lang_vec_slice_v1(LangVec *v, uint64_t start,
                                      uint64_t end) noexcept {
  if (start > end || end > v->length) vec_panic(9);
  const uint64_t count = end - start;
  auto *result = lang_vec_new_v1(count, v->stride, v->copy, v->drop);
  if (!has_owner_ops(result)) {
    if (count) {
      if (end > v->initialized) vec_panic(4);
      const uint64_t bytes = checked_payload_bytes(count, v->stride);
      copy_bytes(result->data, element(v, start), bytes);
    }
    result->initialized = count;
    return result;
  }
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
  if (length > v->capacity) return grow_and_append(v, value, length);
  append_value(v, value, length);
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
  // Fill scalar widths directly and double copied chunks for other widths.
  // Preserve sequential behavior when the fill overlaps the suffix.
  const uint64_t added = length - old_length;
  if (!has_owner_ops(v) &&
      !byte_ranges_overlap(element(v, old_length),
                           checked_payload_bytes(added, v->stride),
                           source, v->stride)) {
    void *destination = element(v, old_length);
    switch (v->stride) {
    case 1: fill_words<uint8_t>(destination, source, added); break;
    case 2: fill_words<uint16_t>(destination, source, added); break;
    case 4: fill_words<uint32_t>(destination, source, added); break;
    case 8: fill_words<uint64_t>(destination, source, added); break;
    default:
      copy_bytes(destination, source, v->stride);
      for (uint64_t done = 1; done < added;) {
        const uint64_t chunk = added - done < done ? added - done : done;
        copy_bytes(element(v, old_length + done), element(v, old_length),
                   chunk * v->stride);
        done += chunk;
      }
    }
    v->initialized = length;
    v->length = length;
    if (scratch) lang_free_v1(scratch);
    return;
  }
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
extern "C" void lang_io_vec_append_bytes_v1(LangVec *v,
                                             const LangVec *source,
                                             uint64_t offset,
                                             uint64_t count) noexcept {
  // This is a typed byte operation, not a generic element copy. Reject a
  // descriptor that could require element callbacks before touching storage.
  if (!v || !source || v->stride != 1 || source->stride != 1 ||
      v->copy || v->drop || source->copy || source->drop)
    vec_panic(4);
  if (offset > source->length || count > source->length - offset)
    vec_panic(9);
  if (count && (offset > source->initialized ||
                count > source->initialized - offset))
    vec_panic(4);
  // A zero-byte append is a no-op even while the target is pinned, matching
  // resize's no-change path.
  if (!count) return;
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
  // Appending after an uninitialized gap cannot publish a contiguous
  // initialized prefix, so reject that descriptor state before any write.
  if (v->initialized != v->length || !source->data) vec_panic(4);
  const uint64_t old_length = v->length;
  if (count > kMaxU64 - old_length) vec_panic(6);
  const uint64_t new_length = old_length + count;
  const bool distinct_source = source != v;
  if (distinct_source) {
    // Pin the source prefix while its bytes are read. A self-append skips the
    // pin because the target's active-iteration check already covers it and
    // growth must remain legal for this descriptor.
    (void)lang_vec_begin_v1(const_cast<LangVec *>(source));
  }
  const bool grow = new_length > v->capacity;
  const void *input =
      static_cast<const unsigned char *>(source->data) + offset;
  StageBuffer alias;
  if (grow && distinct_source &&
      byte_ranges_overlap(v->data, v->capacity, input, count)) {
    // Growth may release the old target payload. Snapshot the entire source
    // range first so a raw alias that starts before the target stays valid.
    if (!alias.reserve(count)) vec_panic(5);
    copy_bytes(alias.data(), input, count);
    input = alias.data();
  }
  if (grow) grow_storage(v, new_length);
  // A self-append must read the relocated prefix after growth, never the old
  // payload pointer captured above.
  if (!distinct_source) input = element(v, offset);
  copy_bytes(element(v, old_length), input, count);
  // Publish the completed byte range before exposing its length.
  v->initialized = new_length;
  v->length = new_length;
  if (distinct_source) lang_vec_end_v1(const_cast<LangVec *>(source));
}
extern "C" void lang_vec_check_resize_v1(LangVec *v) noexcept {
  // Structural changes still require caller synchronization against new iterations.
  if (__atomic_load_n(&v->iterations, __ATOMIC_ACQUIRE)) vec_panic(8);
}

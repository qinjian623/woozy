#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

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
                                  : "invalid-cause";
  int count = std::snprintf(record, sizeof(record),
                            "LANG_NATIVE_PANIC:v1:%s:%llu:%llu:%llu\n", name,
                            static_cast<unsigned long long>(snapshot),
                            static_cast<unsigned long long>(begin),
                            static_cast<unsigned long long>(end));
  if (count > 0 && static_cast<size_t>(count) < sizeof(record)) {
    size_t offset = 0;
    while (offset < static_cast<size_t>(count)) {
      auto n = ::write(STDERR_FILENO, record + offset,
                       static_cast<size_t>(count) - offset);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      offset += static_cast<size_t>(n);
    }
  }
  std::_Exit(101);
}

// Separate ordinary typed-error entry diagnostic; no panic or unwinding.
extern "C" void lang_native_typed_error_v1(const char *key,
                                           uint64_t size) noexcept {
  auto write_all = [](const char *bytes, size_t count) {
    while (count) {
      auto n = ::write(STDERR_FILENO, bytes, count);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        break;
      bytes += n;
      count -= static_cast<size_t>(n);
    }
  };
  constexpr char prefix[] = "LANG_TYPED_ERROR: ";
  write_all(prefix, sizeof(prefix) - 1);
  write_all(key, static_cast<size_t>(size));
  write_all("\n", 1);
}

#include "vec.hpp"
#include <cstring>
#include <limits>
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
}
extern "C" LangVec *lang_vec_new_v1(uint64_t length, uint64_t stride,
                                    LangValueOp copy, LangValueOp drop) noexcept {
  if (!stride || length > SIZE_MAX / stride) vec_panic(6);
  auto *v = static_cast<LangVec *>(std::malloc(sizeof(LangVec)));
  if (!v) vec_panic(5);
  void *data = std::malloc(length ? length * stride : 1);
  if (!data) { std::free(v); vec_panic(5); }
  *v = {1, length, 0, stride, 0, data, copy, drop};
  observe(1, v, length);
  return v;
}
extern "C" void lang_vec_retain_v1(LangVec *v) noexcept {
  if (v->refs == UINT64_MAX) vec_panic(7);
  ++v->refs;
  observe(2, v, v->refs);
}
extern "C" void lang_vec_release_v1(LangVec *v) noexcept {
  if (!v->refs) vec_panic(7);
  observe(3, v, v->refs - 1);
  if (--v->refs) return;
  if (v->iterations) vec_panic(8);
  while (v->initialized) {
    auto i = --v->initialized;
    observe(4, v, i);
    v->drop(element(v, i));
  }
  observe(5, v);
  std::free(v->data);
  std::free(v);
}
extern "C" void lang_vec_init_v1(LangVec *v, const void *value) noexcept {
  if (v->initialized >= v->length) vec_panic(4);
  std::memcpy(element(v, v->initialized++), value, v->stride);
}
extern "C" void *lang_vec_at_v1(LangVec *v, uint64_t i) noexcept {
  if (i >= v->length || i >= v->initialized) vec_panic(4);
  return element(v, i);
}
extern "C" void lang_vec_set_v1(LangVec *v, uint64_t i, const void *value) noexcept {
  void *target = lang_vec_at_v1(v, i);
  v->drop(target);
  std::memcpy(target, value, v->stride);
}
extern "C" LangVec *lang_vec_slice_v1(LangVec *v, uint64_t start,
                                      uint64_t end) noexcept {
  if (start > end || end > v->length) vec_panic(9);
  auto *result = lang_vec_new_v1(end-start, v->stride, v->copy, v->drop);
  for (uint64_t i = start; i < end; ++i) {
    lang_vec_init_v1(result, lang_vec_at_v1(v, i));
    result->copy(element(result, result->initialized - 1));
  }
  return result;
}
extern "C" uint64_t lang_vec_begin_v1(LangVec *v) noexcept {
  if (v->iterations == UINT64_MAX) vec_panic(8);
  ++v->iterations;
  observe(6, v, v->iterations);
  return v->length;
}
extern "C" void lang_vec_end_v1(LangVec *v) noexcept {
  if (!v->iterations) vec_panic(8);
  --v->iterations;
  observe(7, v, v->iterations);
}
extern "C" uint64_t lang_vec_length_v1(LangVec *v) noexcept { return v->length; }
extern "C" void lang_vec_check_resize_v1(LangVec *v) noexcept {
  if (v->iterations) vec_panic(8);
}

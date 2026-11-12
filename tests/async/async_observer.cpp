#include <cstddef>
#include <cstdint>

// Link-time observer for the async fixtures. The generated program is
// freestanding; this file emits a single COUNTS line with raw write(2) so the
// Python runner can check exact future/frame/Task/Vec allocation counts.
extern "C" void *__real_lang_async_task_new_v1() noexcept;
extern "C" void *__real_lang_async_sleep_v1(int64_t) noexcept;
extern "C" void *__real_lang_async_wait_fd_v1(int32_t, int32_t) noexcept;
extern "C" void *__real_lang_async_frame_alloc_v1(uint64_t) noexcept;
extern "C" void __real_lang_async_frame_free_v1(void *) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;
extern "C" void __real_lang_async_task_retain_v1(void *) noexcept;
extern "C" void __real_lang_async_task_release_v1(void *) noexcept;
extern "C" uint64_t __real_lang_gc_program_finish_v1() noexcept;

namespace {
constexpr int kSlots = 4096;
void *futures[kSlots];
void *frames[kSlots];
uint64_t future_new, future_free;
uint64_t frame_new, frame_free, bad_frame_free;
uint64_t task_retain, task_release;
uint64_t vec_new, vec_free, vec_retain, vec_release;

void track(void **slots, void *pointer) {
  for (int i = 0; i < kSlots; ++i)
    if (!slots[i]) {
      slots[i] = pointer;
      return;
    }
  __builtin_trap();
}

bool forget(void **slots, void *pointer) {
  for (int i = 0; i < kSlots; ++i)
    if (slots[i] == pointer) {
      slots[i] = nullptr;
      return true;
    }
  return false;
}

char buffer[512];
size_t position;

void text(const char *value) {
  while (*value)
    buffer[position++] = *value++;
}

void number(uint64_t value) {
  char digits[25];
  int length = 0;
  do {
    digits[length++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value);
  while (length)
    buffer[position++] = digits[--length];
}

void field(const char *name, uint64_t value) {
  text(name);
  number(value);
  text(" ");
}
} // namespace

extern "C" void lang_vec_observe_v1(uint32_t event, const void *,
                                    uint64_t) noexcept {
  if (event == 1) ++vec_new;
  if (event == 2) ++vec_retain;
  if (event == 3) ++vec_release;
  if (event == 5) ++vec_free;
}

extern "C" void *__wrap_lang_async_task_new_v1() noexcept {
  void *pointer = __real_lang_async_task_new_v1();
  track(futures, pointer);
  ++future_new;
  return pointer;
}

extern "C" void *__wrap_lang_async_sleep_v1(int64_t milliseconds) noexcept {
  void *pointer = __real_lang_async_sleep_v1(milliseconds);
  track(futures, pointer);
  ++future_new;
  return pointer;
}

extern "C" void *__wrap_lang_async_wait_fd_v1(int32_t fd,
                                               int32_t events) noexcept {
  void *pointer = __real_lang_async_wait_fd_v1(fd, events);
  track(futures, pointer);
  ++future_new;
  return pointer;
}

extern "C" void *__wrap_lang_async_frame_alloc_v1(uint64_t size) noexcept {
  void *pointer = __real_lang_async_frame_alloc_v1(size);
  track(frames, pointer);
  ++frame_new;
  return pointer;
}

extern "C" void __wrap_lang_async_frame_free_v1(void *pointer) noexcept {
  if (forget(frames, pointer))
    ++frame_free;
  else
    ++bad_frame_free;
  __real_lang_async_frame_free_v1(pointer);
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  if (pointer && forget(futures, pointer))
    ++future_free;
  __real_lang_free_v1(pointer);
}

extern "C" void __wrap_lang_async_task_retain_v1(void *pointer) noexcept {
  ++task_retain;
  __real_lang_async_task_retain_v1(pointer);
}

extern "C" void __wrap_lang_async_task_release_v1(void *pointer) noexcept {
  ++task_release;
  __real_lang_async_task_release_v1(pointer);
}

extern "C" uint64_t __wrap_lang_gc_program_finish_v1() noexcept {
  const uint64_t result = __real_lang_gc_program_finish_v1();
  position = 0;
  text("COUNTS ");
  field("future_new=", future_new);
  field("future_free=", future_free);
  field("frame_new=", frame_new);
  field("frame_free=", frame_free);
  field("task_retain=", task_retain);
  field("task_release=", task_release);
  field("bad_frame_free=", bad_frame_free);
  field("vec_new=", vec_new);
  field("vec_free=", vec_free);
  field("vec_retain=", vec_retain);
  field("vec_release=", vec_release);
  text("\n");
  long written;
  asm volatile("syscall"
               : "=a"(written)
               : "a"(1L), "D"(2L), "S"(buffer), "d"(position)
               : "rcx", "r11", "memory");
  return result;
}

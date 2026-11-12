// No-libc static ELF smoke for the borrowed-Slice ABI.
//
// Built with -nostdlib and linked with slice.cpp, panic.cpp, alloc.cpp, and the
// weak freestanding mem* helpers. _start uses Linux x86_64 syscalls directly;
// lang_alloc_v1/lang_free_v1 are wrapped so the test can assert zero live
// allocations and no unknown/double free without libc.
#include "../../../runtime/native/slice.hpp"

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t);
extern "C" void __real_lang_free_v1(void *);

namespace {

constexpr unsigned kMaxLive = 128;
void *g_live[kMaxLive];
unsigned g_live_count = 0;
bool g_bad_free = false;
bool g_table_full = false;
uint64_t g_drops = 0;

void drop_counter(void *) { ++g_drops; }

long sys_write(int fd, const void *data, unsigned long bytes) {
  long result;
  __asm__ volatile("syscall"
                   : "=a"(result)
                   : "a"(1 /* SYS_write */), "D"(fd), "S"(data), "d"(bytes)
                   : "rcx", "r11", "memory");
  return result;
}

[[noreturn]] void sys_exit(int code) {
  __asm__ volatile("syscall"
                   :
                   : "a"(60 /* SYS_exit */), "D"(code)
                   : "rcx", "r11", "memory");
  __builtin_unreachable();
}

void write_text(const char *text) {
  unsigned long size = 0;
  while (text[size]) ++size;
  unsigned long offset = 0;
  while (offset < size) {
    const long written = sys_write(2, text + offset, size - offset);
    if (written == -4) continue;
    if (written <= 0) break;
    offset += static_cast<unsigned long>(written);
  }
}

[[noreturn]] void fail(unsigned code) {
  write_text("slice-static: failed\n");
  sys_exit(static_cast<int>(code));
}

#define REQUIRE(condition, code) \
  do {                           \
    if (!(condition)) fail(code); \
  } while (0)

} // namespace

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (pointer) {
    if (g_live_count < kMaxLive)
      g_live[g_live_count++] = pointer;
    else
      g_table_full = true;
  }
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) {
  if (!pointer) return;
  bool found = false;
  for (unsigned i = 0; i < g_live_count; ++i) {
    if (g_live[i] != pointer) continue;
    g_live[i] = g_live[--g_live_count];
    found = true;
    break;
  }
  if (!found) g_bad_free = true;
  __real_lang_free_v1(pointer);
}

extern "C" [[noreturn]] __attribute__((force_align_arg_pointer)) void
_start() {
  LangVec *v = lang_vec_new_v1(4, sizeof(int), nullptr, drop_counter);
  REQUIRE(v != nullptr, 10);
  int values[4] = {10, 11, 12, 13};
  for (int value : values) lang_vec_init_v1(v, &value);

  LangSlice root{};
  lang_slice_new_v1(&root, v, 0, 4);
  REQUIRE(v->refs == 2 && v->iterations == 1, 11);
  lang_vec_release_v1(v);
  REQUIRE(v->refs == 1 && v->iterations == 1, 12);
  REQUIRE(*static_cast<int *>(lang_slice_at_v1(&root, 0)) == 10, 13);

  LangSlice sub{};
  lang_slice_sub_v1(&sub, &root, 1, 3);
  REQUIRE(sub.owner == v && sub.start == 1 && sub.length == 2, 14);
  REQUIRE(*static_cast<int *>(lang_slice_at_v1(&sub, 0)) == 11, 15);

  LangSlice terminal{};
  lang_slice_new_v1(&terminal, v, 4, 4);
  REQUIRE(terminal.owner == v && terminal.start == 4 && terminal.length == 0,
          16);
  LangSlice empty{};
  lang_slice_sub_v1(&empty, &sub, 2, 2);
  REQUIRE(empty.owner == v && empty.start == 3 && empty.length == 0, 17);
  REQUIRE(lang_slice_length_v1(&empty) == 0, 18);

  LangSlice copy = root;
  lang_slice_copy_v1(&copy);
  REQUIRE(v->refs == 5 && v->iterations == 5, 19);
  *static_cast<int *>(lang_slice_at_v1(&copy, 0)) = 77;
  REQUIRE(*static_cast<int *>(lang_slice_at_v1(&root, 0)) == 77, 20);

  lang_slice_drop_v1(&copy);
  REQUIRE(v->refs == 4 && v->iterations == 4, 21);
  lang_slice_drop_v1(&empty);
  lang_slice_drop_v1(&terminal);
  lang_slice_drop_v1(&sub);
  REQUIRE(v->refs == 1 && v->iterations == 1, 22);
  REQUIRE(g_drops == 0, 23);

  lang_slice_drop_v1(&root);
  REQUIRE(g_drops == 4, 24);
  REQUIRE(g_live_count == 0 && !g_bad_free && !g_table_full, 25);

  // Structural mutation is available again after the final Slice drop.
  LangVec *mut = lang_vec_new_v1(2, sizeof(int), nullptr, nullptr);
  REQUIRE(mut != nullptr, 26);
  int first = 1, second = 2;
  lang_vec_init_v1(mut, &first);
  lang_vec_init_v1(mut, &second);
  LangSlice transient{};
  lang_slice_new_v1(&transient, mut, 0, 2);
  lang_slice_drop_v1(&transient);
  REQUIRE(mut->refs == 1 && mut->iterations == 0, 27);
  int third = 3;
  lang_vec_push_v1(mut, &third);
  REQUIRE(mut->length == 3, 28);
  REQUIRE(*static_cast<int *>(lang_vec_at_v1(mut, 2)) == 3, 29);
  lang_vec_resize_v1(mut, 1, nullptr);
  REQUIRE(mut->length == 1, 30);
  lang_vec_clear_v1(mut);
  lang_vec_reserve_v1(mut, 8);
  REQUIRE(mut->capacity >= 8, 31);
  lang_vec_release_v1(mut);
  REQUIRE(g_live_count == 0 && !g_bad_free, 32);

  write_text("slice-static: passed\n");
  sys_exit(0);
}

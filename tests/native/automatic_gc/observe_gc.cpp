// Test-only freestanding observation shim for the MEM02 automatic-GC tests.
//
// It wraps the Native runtime's existing allocation entry points and forwards
// the runtime's existing weak Vec observer to stderr:
//
//   CONFIG:vec_header_bytes:<n>       once, before the first allocation
//   ALLOC:0x<user pointer>:<bytes>    successful lang_alloc_v1 request
//   FREE:0x<user pointer>             entry to lang_free_v1
//   VEC:<event>:0x<object>:<detail>   runtime lang_vec_observe_v1 stream
//
// Generation identity is assigned by the runner on each ALLOC: a pointer may
// be reused after FREE, so (pointer, generation) is the object identity.
// The shim deliberately allocates nothing (raw write syscall, fixed stack
// buffer), so its own tracking cannot recurse through the allocator it
// observes. It never retains, releases or writes the Vec it reports.
#include <cstdint>

#include "../../../runtime/native/vec.hpp"

extern "C" void *__real_lang_alloc_v1(uint64_t, uint64_t) noexcept;
extern "C" void __real_lang_free_v1(void *) noexcept;

namespace {
// Same alignment rule as the runtime's private Vec allocation. The runner
// checks this CONFIG value and rejects an observation ABI it cannot account
// for instead of silently treating a header change as a leak.
constexpr uint64_t kVecHeaderBytes =
    (static_cast<uint64_t>(sizeof(LangVec)) + 15) & ~uint64_t{15};

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

struct Line {
  char bytes[128];
  unsigned long size = 0;
  void text(const char *s) noexcept {
    while (*s) bytes[size++] = *s++;
  }
  void hex(uint64_t value) noexcept {
    text("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
      const unsigned digit = static_cast<unsigned>((value >> shift) & 0xF);
      bytes[size++] = static_cast<char>(digit < 10 ? '0' + digit
                                                   : 'a' + digit - 10);
    }
  }
  void decimal(uint64_t value) noexcept {
    char reversed[24];
    unsigned count = 0;
    do {
      reversed[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value);
    while (count) bytes[size++] = reversed[--count];
  }
  void flush() noexcept {
    raw_write(2, bytes, size);
    size = 0;
  }
};

Line line;
bool config_emitted = false;

void emit_config() noexcept {
  if (config_emitted) return;
  config_emitted = true;
  line.text("CONFIG:vec_header_bytes:");
  line.decimal(kVecHeaderBytes);
  line.text("\n");
  line.flush();
}
} // namespace

extern "C" void *__wrap_lang_alloc_v1(uint64_t size, uint64_t align) noexcept {
  void *pointer = __real_lang_alloc_v1(size, align);
  if (!pointer) return pointer;
  emit_config();
  line.text("ALLOC:");
  line.hex(reinterpret_cast<uint64_t>(pointer));
  line.text(":");
  line.decimal(size);
  line.text("\n");
  line.flush();
  return pointer;
}

extern "C" void __wrap_lang_free_v1(void *pointer) noexcept {
  line.text("FREE:");
  line.hex(reinterpret_cast<uint64_t>(pointer));
  line.text("\n");
  line.flush();
  __real_lang_free_v1(pointer);
}

// The runtime declares this weak; defining it must not change ownership.
extern "C" void lang_vec_observe_v1(uint32_t event, const void *object,
                                    uint64_t detail) noexcept {
  line.text("VEC:");
  line.decimal(event);
  line.text(":");
  line.hex(reinterpret_cast<uint64_t>(object));
  line.text(":");
  line.decimal(detail);
  line.text("\n");
  line.flush();
}

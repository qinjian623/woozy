#include <cstdint>
extern "C" int64_t __real_lang_os_write_v1(int32_t,const void *,uint64_t) noexcept;
namespace {
unsigned calls;
[[noreturn]] void fail() {
  __asm__ volatile("syscall" : : "a"(231), "D"(201) : "rcx","r11","memory");
  __builtin_unreachable();
}
}
extern "C" int64_t __wrap_lang_os_write_v1(int32_t fd,const void *data,uint64_t count) noexcept {
  if (fd != 1) return __real_lang_os_write_v1(fd,data,count);
  const char *expected = calls < 2 ? "ABCDE" : calls == 2 ? "CDE" : "CDEFG";
  const uint64_t length = calls == 2 ? 3 : 5;
  if (calls >= 4 || count != length) fail();
  const auto *bytes = static_cast<const char *>(data);
  for (uint64_t index=0;index<count;++index) if (bytes[index] != expected[index]) fail();
  ++calls;
  if (calls == 1) return -4;
  if (calls == 3) return -11;
  return __real_lang_os_write_v1(fd,data,calls == 2 ? 2 : count);
}

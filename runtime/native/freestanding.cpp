#include <cstddef>
#include <cstdint>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("no-tree-loop-distribute-patterns")
#endif

extern "C" {

#if defined(__x86_64__)

__attribute__((weak))
void *memcpy(void *dest, const void *src, size_t n) noexcept {
  unsigned char *d = static_cast<unsigned char *>(dest);
  const unsigned char *s = static_cast<const unsigned char *>(src);
  size_t count = n;
  __asm__ volatile (
    "cld\n\t"
    "rep movsb"
    : "+D"(d), "+S"(s), "+c"(count)
    :
    : "memory"
  );
  return dest;
}

__attribute__((weak))
void *memset(void *s, int c, size_t n) noexcept {
  unsigned char *d = static_cast<unsigned char *>(s);
  size_t count = n;
  __asm__ volatile (
    "cld\n\t"
    "rep stosb"
    : "+D"(d), "+c"(count)
    : "a"(static_cast<unsigned char>(c))
    : "memory"
  );
  return s;
}

__attribute__((weak))
void *memmove(void *dest, const void *src, size_t n) noexcept {
  if (!n || dest == src) return dest;
  unsigned char *d = static_cast<unsigned char *>(dest);
  const unsigned char *s = static_cast<const unsigned char *>(src);
  size_t count = n;
  if (d < s || d >= s + n) {
    __asm__ volatile (
      "cld\n\t"
      "rep movsb"
      : "+D"(d), "+S"(s), "+c"(count)
      :
      : "memory"
    );
  } else {
    d += n - 1;
    s += n - 1;
    __asm__ volatile (
      "std\n\t"
      "rep movsb\n\t"
      "cld"
      : "+D"(d), "+S"(s), "+c"(count)
      :
      : "memory"
    );
  }
  return dest;
}

#else

__attribute__((weak))
void *memcpy(void *dest, const void *src, size_t n) noexcept {
  auto *d = static_cast<volatile unsigned char *>(dest);
  auto *s = static_cast<const volatile unsigned char *>(src);
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
  return dest;
}

__attribute__((weak))
void *memset(void *s, int c, size_t n) noexcept {
  auto *p = static_cast<volatile unsigned char *>(s);
  for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(c);
  return s;
}

__attribute__((weak))
void *memmove(void *dest, const void *src, size_t n) noexcept {
  auto *d = static_cast<volatile unsigned char *>(dest);
  auto *s = static_cast<const volatile unsigned char *>(src);
  if (d < s) {
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
  } else if (d > s) {
    for (size_t i = n; i > 0; --i) d[i - 1] = s[i - 1];
  }
  return dest;
}

#endif

__attribute__((weak))
int memcmp(const void *s1, const void *s2, size_t n) noexcept {
  const unsigned char *p1 = static_cast<const unsigned char *>(s1);
  const unsigned char *p2 = static_cast<const unsigned char *>(s2);
  for (size_t i = 0; i < n; ++i) {
    if (p1[i] != p2[i])
      return p1[i] < p2[i] ? -1 : 1;
  }
  return 0;
}

__attribute__((weak))
uintptr_t __stack_chk_guard = 0;

__attribute__((weak, noreturn, no_stack_protector))
void __stack_chk_fail() noexcept {
  static constexpr char msg[] = "LANG_NATIVE_PANIC:v1:stack-corruption:0:0:0\n";
  size_t written = 0;
  while (written < sizeof(msg) - 1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret)
      : "a"(1 /* SYS_write */), "D"(2), "S"(msg + written), "d"(sizeof(msg) - 1 - written)
      : "rcx", "r11", "memory");
    if (ret == -4) continue;
    if (ret <= 0) break;
    written += static_cast<size_t>(ret);
  }
  __asm__ volatile (
    "movl $231, %%eax\n\t"
    "movl $101, %%edi\n\t"
    "syscall\n\t"
    "movl $60, %%eax\n\t"
    "syscall\n\t"
    "hlt\n\t"
    ::: "memory"
  );
  __builtin_unreachable();
}

}

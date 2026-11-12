#ifndef LANG_UNICODE_UTF8PROC_FREESTANDING_H
#define LANG_UNICODE_UTF8PROC_FREESTANDING_H

/*
 * Hosted-runtime adaptation for the vendored utf8proc translation unit.
 *
 * Include this file after the vendored utf8proc.h (so the declarations from
 * <stdlib.h> are not rewritten) and before the vendored utf8proc.c. The
 * function-like macros below then redirect the few hosted memory calls in
 * utf8proc.c to this runtime's allocator and byte loops. No global malloc,
 * free, realloc, strlen, memcpy, memset, or memmove symbol is introduced.
 */

#include <stddef.h>
#include <stdint.h>

extern void *lang_alloc_v1(uint64_t size, uint64_t align);
extern void lang_free_v1(void *ptr);

typedef struct LangUtf8procAllocation {
  uint64_t payload_bytes;
  uint64_t reserved;
} LangUtf8procAllocation;

static void *lang_utf8proc_malloc_hook(size_t size) {
  LangUtf8procAllocation *allocation;
  if ((uint64_t)size > UINT64_MAX - sizeof(LangUtf8procAllocation)) return 0;
  allocation = (LangUtf8procAllocation *)lang_alloc_v1(
      sizeof(LangUtf8procAllocation) + (uint64_t)size, 16);
  if (!allocation) return 0;
  allocation->payload_bytes = (uint64_t)size;
  allocation->reserved = 0;
  return (void *)(allocation + 1);
}

static void lang_utf8proc_free_hook(void *ptr) {
  if (!ptr) return;
  lang_free_v1((LangUtf8procAllocation *)ptr - 1);
}

static void *lang_utf8proc_realloc_hook(void *ptr, size_t size) {
  LangUtf8procAllocation *old_allocation;
  uint64_t old_bytes, copy_bytes;
  unsigned char *source, *destination;
  void *replacement;
  if (!ptr) return lang_utf8proc_malloc_hook(size);
  if (!size) {
    lang_utf8proc_free_hook(ptr);
    return 0;
  }
  old_allocation = (LangUtf8procAllocation *)ptr - 1;
  old_bytes = old_allocation->payload_bytes;
  replacement = lang_utf8proc_malloc_hook(size);
  if (!replacement) return 0;
  copy_bytes = old_bytes < (uint64_t)size ? old_bytes : (uint64_t)size;
  source = (unsigned char *)ptr;
  destination = (unsigned char *)replacement;
  for (uint64_t i = 0; i < copy_bytes; ++i) destination[i] = source[i];
  lang_utf8proc_free_hook(ptr);
  return replacement;
}

static size_t lang_utf8proc_strlen_hook(const char *text) {
  size_t length = 0;
  while (text[length]) ++length;
  return length;
}

static void *lang_utf8proc_memcpy_hook(void *destination, const void *source,
                                       size_t bytes) {
  unsigned char *to = (unsigned char *)destination;
  const unsigned char *from = (const unsigned char *)source;
  for (size_t i = 0; i < bytes; ++i) to[i] = from[i];
  return destination;
}

static void *lang_utf8proc_memmove_hook(void *destination, const void *source,
                                        size_t bytes) {
  unsigned char *to = (unsigned char *)destination;
  const unsigned char *from = (const unsigned char *)source;
  if (to == from || !bytes) return destination;
  if ((uintptr_t)to < (uintptr_t)from) {
    for (size_t i = 0; i < bytes; ++i) to[i] = from[i];
  } else {
    for (size_t i = bytes; i; --i) to[i - 1] = from[i - 1];
  }
  return destination;
}

static void *lang_utf8proc_memset_hook(void *destination, int value,
                                       size_t bytes) {
  unsigned char *to = (unsigned char *)destination;
  for (size_t i = 0; i < bytes; ++i) to[i] = (unsigned char)value;
  return destination;
}

#define malloc(size) lang_utf8proc_malloc_hook(size)
#define free(ptr) lang_utf8proc_free_hook(ptr)
#define realloc(ptr, size) lang_utf8proc_realloc_hook((ptr), (size))
#define strlen(text) lang_utf8proc_strlen_hook(text)
#define memcpy(destination, source, bytes) \
  lang_utf8proc_memcpy_hook((destination), (source), (bytes))
#define memmove(destination, source, bytes) \
  lang_utf8proc_memmove_hook((destination), (source), (bytes))
#define memset(destination, value, bytes) \
  lang_utf8proc_memset_hook((destination), (value), (bytes))

#endif

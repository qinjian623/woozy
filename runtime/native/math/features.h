#ifndef LANG_MATH_FEATURES_H
#define LANG_MATH_FEATURES_H

/* Minimal freestanding replacement for musl's feature/visibility header.
 * Only the attributes used by the vendored math sources are provided. */
#if defined(__GNUC__) || defined(__clang__)
#define hidden __attribute__((visibility("hidden")))
#define weak __attribute__((weak))
#define weak_alias(name, aliasname) /* aliases are not part of this ABI */
#else
#define hidden
#define weak
#define weak_alias(name, aliasname)
#endif

#endif

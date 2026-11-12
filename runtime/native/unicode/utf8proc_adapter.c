/*
 * The vendored utf8proc.c is compiled unchanged in this translation unit.
 * The adaptation header only replaces hosted memory primitives after the
 * upstream declarations have been processed.
 */
#define UTF8PROC_STATIC 1
#include "vendor/utf8proc/utf8proc.h"
#include "utf8proc_freestanding.h"
#include "vendor/utf8proc/utf8proc.c"

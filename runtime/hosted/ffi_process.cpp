// Hosted translation unit for the public process runtime. The native archive
// compiles runtime/native/process.cpp without LANG_RUNTIME_HOSTED; this wrapper
// gives the hosted archive the libc fork branch without a global source option
// leaking into the freestanding target.
#define LANG_RUNTIME_HOSTED 1
#include "../native/process.cpp"

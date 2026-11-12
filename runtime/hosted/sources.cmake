# Source/flag manifest for the explicitly selected hosted link mode.
#
# This manifest is data only; it does not modify the existing native target.
# The existing freestanding archive remains:
#   runtime/native/*.cpp + runtime/native/math/sources.cmake
# A hosted archive replaces start/thread, adds the hosted process/fork wrapper,
# compiles runtime/native/process.cpp with LANG_RUNTIME_HOSTED, and omits
# freestanding.cpp.

get_filename_component(LANG_HOSTED_LANG_ROOT
  "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(LANG_HOSTED_REPLACEMENT_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/start.cpp
  ${CMAKE_CURRENT_LIST_DIR}/thread.cpp
  ${CMAKE_CURRENT_LIST_DIR}/process.cpp)

# Compile these into the hosted archive, but not native start/thread/
# freestanding.  Keep the existing freestanding flags for these public units.
set(LANG_HOSTED_PUBLIC_SOURCES
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/alloc.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/panic.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/os.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/os_buffers.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/sync.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/text.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/system.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/system_buffers.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/process.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/ipc.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/dns.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/network_buffers.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/network.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/numbers.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/slice.cpp
  # Unicode is public runtime functionality and must be present in hosted
  # links of std/unicode. The C adapter keeps the vendored utf8proc TU local to
  # this runtime and introduces no libc memory symbols.
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/unicode/unicode.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/unicode/utf8proc_adapter.c
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/math/support.cpp)

# These are intentionally absent from the hosted archive.
set(LANG_HOSTED_EXCLUDED_NATIVE_SOURCES
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/start.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/thread.cpp
  ${LANG_HOSTED_LANG_ROOT}/runtime/native/freestanding.cpp)

# Apply these to LANG_HOSTED_PUBLIC_SOURCES (and the math C units through
# runtime/native/math/sources.cmake).
set(LANG_HOSTED_PUBLIC_COMPILE_OPTIONS
  -std=c++20
  -fPIC
  -ffreestanding
  -fno-builtin
  -fno-exceptions
  -fno-rtti
  -fno-sanitize=all
  -fno-builtin-memcpy
  -fno-builtin-memset
  -fno-builtin-memmove)

# Apply these to LANG_HOSTED_REPLACEMENT_SOURCES.  Do not inherit
# -ffreestanding here: these objects use libc/pthread declarations.  Keep
# exceptions disabled: LangThreadEntry/ValueOp are no-throws ABI boundaries.
set(LANG_HOSTED_REPLACEMENT_COMPILE_OPTIONS
  -std=c++20
  -fPIC
  -fno-exceptions
  -fno-rtti
  -pthread
  -fstack-protector-strong)

# Apply this definition only when compiling runtime/native/process.cpp for the
# hosted archive.  The native/freestanding build must keep the original raw
# fork branch.
set(LANG_HOSTED_PROCESS_COMPILE_DEFINITION -DLANG_RUNTIME_HOSTED)

# Include runtime/native/math/sources.cmake and call
#   lang_native_math_sources(<hosted-runtime-target>)
# as the native target does.

# Force the constructor member out of a static archive.  Without this (or an
# equivalent --whole-archive section) argc/argv/envp are never published.
# A hosted link must also use the normal CRT/driver rather than the native
# -nostdlib -static path.  The runtime objects themselves need no C++ exception
# runtime; a C++ harness may of course link its own standard library.
set(LANG_HOSTED_LINK_OPTIONS
  -Wl,-u,lang_hosted_startup_v1
  -pthread)

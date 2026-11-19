# May be added after the root frontend targets, or by tests/native standalone.
find_package(LLVM REQUIRED CONFIG)
find_package(json-c CONFIG REQUIRED)
if(NOT LLVM_PACKAGE_VERSION STREQUAL "22.1.8")
  message(FATAL_ERROR "LLVM 22.1.8 required; found ${LLVM_PACKAGE_VERSION}")
endif()
get_filename_component(LANG_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_library(lang_native_ir STATIC
  ${LANG_NATIVE_ROOT}/src/ir/lower.cpp ${LANG_NATIVE_ROOT}/src/ir/verify.cpp)
target_link_libraries(lang_native_ir PUBLIC lang_hir)
target_compile_features(lang_native_ir PUBLIC cxx_std_20)
add_library(lang_native STATIC emit.cpp c_ffi.cpp)
target_link_libraries(lang_native PUBLIC lang_native_ir)
target_include_directories(lang_native SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native PRIVATE -fno-rtti)
llvm_map_components_to_libnames(LANG_NATIVE_LLVM_LIBS Core Support nativecodegen Passes)
target_link_libraries(lang_native PRIVATE ${LANG_NATIVE_LLVM_LIBS})
add_library(lang_native_runtime STATIC
  ${LANG_NATIVE_ROOT}/runtime/native/alloc.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/panic.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/weak.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/gc.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/gc_world.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/gc_runtime.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/gc_program.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/start.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/freestanding.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/os.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/os_buffers.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/thread.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/async.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/sync.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/text.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/unicode/unicode.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/unicode/utf8proc_adapter.c
  ${LANG_NATIVE_ROOT}/runtime/native/system.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/system_buffers.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/process.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/ipc.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/dns.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/network_buffers.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/network.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/numbers.cpp
  ${LANG_NATIVE_ROOT}/runtime/native/slice.cpp)
enable_language(C)
include(${LANG_NATIVE_ROOT}/runtime/native/math/sources.cmake)
lang_native_math_sources(lang_native_runtime)
target_compile_features(lang_native_runtime PRIVATE cxx_std_20)
# This archive is linked into generated programs, independently of compiler instrumentation.
target_compile_options(lang_native_runtime PRIVATE -ffreestanding -fno-builtin -fno-exceptions -fno-rtti -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-memmove
  $<$<CXX_COMPILER_ID:GNU>:-fno-tree-loop-distribute-patterns>)
set_target_properties(lang_native_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Explicit hosted runtime archive. It is never used by the default freestanding
# link mode and is built from the published hosted source manifest.
include(${LANG_NATIVE_ROOT}/runtime/hosted/sources.cmake)
set(LANG_HOSTED_PUBLIC_SOURCES_NO_PROCESS ${LANG_HOSTED_PUBLIC_SOURCES})
list(REMOVE_ITEM LANG_HOSTED_PUBLIC_SOURCES_NO_PROCESS
  ${LANG_NATIVE_ROOT}/runtime/native/process.cpp)
add_library(lang_hosted_public OBJECT
  ${LANG_HOSTED_PUBLIC_SOURCES_NO_PROCESS}
  ${LANG_NATIVE_ROOT}/runtime/hosted/ffi_process.cpp
  ${LANG_MATH_C_SOURCES})
target_compile_features(lang_hosted_public PRIVATE cxx_std_20)
target_compile_options(lang_hosted_public PRIVATE
  -fPIC -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
  -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
  -fno-builtin-memmove
  $<$<CXX_COMPILER_ID:GNU>:-fno-tree-loop-distribute-patterns>)
set_source_files_properties(${LANG_MATH_C_SOURCES} PROPERTIES
  LANGUAGE C
  COMPILE_OPTIONS "${LANG_MATH_C_FLAGS}")
add_library(lang_hosted_replacement OBJECT
  ${LANG_HOSTED_REPLACEMENT_SOURCES})
target_compile_features(lang_hosted_replacement PRIVATE cxx_std_20)
target_compile_options(lang_hosted_replacement PRIVATE
  -fPIC -fno-exceptions -fno-rtti -pthread -fstack-protector-strong)
add_library(lang_hosted_runtime STATIC
  $<TARGET_OBJECTS:lang_hosted_public>
  $<TARGET_OBJECTS:lang_hosted_replacement>)
set_target_properties(lang_hosted_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_program(LANG_NATIVE_NINJA NAMES ninja REQUIRED)
add_executable(lang_native_helper helper.cpp)
target_include_directories(lang_native_helper SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(lang_native_helper PRIVATE lang_sema lang_native lang_driver_process json-c::json-c)
target_compile_definitions(lang_native_helper PRIVATE
  LANG_NATIVE_RUNTIME="$<TARGET_FILE:lang_native_runtime>"
  LANG_HOSTED_RUNTIME="$<TARGET_FILE:lang_hosted_runtime>"
  LANG_NATIVE_CLANG="${LLVM_TOOLS_BINARY_DIR}/clang"
  LANG_NATIVE_LLD="${LLVM_TOOLS_BINARY_DIR}/ld.lld"
  LANG_NATIVE_OPT="${LLVM_TOOLS_BINARY_DIR}/opt"
  LANG_NATIVE_LLC="${LLVM_TOOLS_BINARY_DIR}/llc"
  LANG_NATIVE_NINJA="${LANG_NATIVE_NINJA}")
add_dependencies(lang_native_helper lang_native_runtime lang_hosted_runtime)

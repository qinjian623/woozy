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
add_library(lang_native STATIC emit.cpp)
target_link_libraries(lang_native PUBLIC lang_native_ir)
target_include_directories(lang_native SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native PRIVATE -fno-rtti)
llvm_map_components_to_libnames(LANG_NATIVE_LLVM_LIBS Core Support nativecodegen Passes)
target_link_libraries(lang_native PRIVATE ${LANG_NATIVE_LLVM_LIBS})
add_library(lang_native_runtime STATIC ${LANG_NATIVE_ROOT}/runtime/native/panic.cpp)
target_compile_features(lang_native_runtime PRIVATE cxx_std_20)
# This archive is linked into generated programs, independently of compiler instrumentation.
target_compile_options(lang_native_runtime PRIVATE -fno-exceptions -fno-rtti -fno-sanitize=all)
set_target_properties(lang_native_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_executable(lang_native_helper helper.cpp)
target_link_libraries(lang_native_helper PRIVATE lang_sema lang_native lang_driver_process json-c::json-c)
target_compile_definitions(lang_native_helper PRIVATE
  LANG_NATIVE_RUNTIME="$<TARGET_FILE:lang_native_runtime>"
  LANG_NATIVE_CLANG="${LLVM_TOOLS_BINARY_DIR}/clang"
  LANG_NATIVE_LLD="${LLVM_TOOLS_BINARY_DIR}/ld.lld")
add_dependencies(lang_native_helper lang_native_runtime)

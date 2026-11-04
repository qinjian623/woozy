cmake_minimum_required(VERSION 3.20)
get_filename_component(LANG_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lang_native_validation LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "Build isolated native helper" FORCE)
  add_subdirectory(${LANG_SOURCE_ROOT} frontend)
  return()
endif()
add_executable(lang_native_ir_test ir_test.cpp)
target_link_libraries(lang_native_ir_test PRIVATE lang_sema lang_native)
add_test(NAME native.ir COMMAND lang_native_ir_test ${LANG_SOURCE_ROOT}/tests/fixtures/core/lang.toml)
find_package(LLVM REQUIRED CONFIG)
add_executable(lang_native_llvm_test llvm_test.cpp)
target_include_directories(lang_native_llvm_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_llvm_test PRIVATE -fno-rtti)
llvm_map_components_to_libnames(LANG_NATIVE_TEST_LLVM_LIBS AsmParser Core Support)
target_link_libraries(lang_native_llvm_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_test(NAME native.llvm COMMAND lang_native_llvm_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/float-transfer/lang.toml)
if(LANG_TEST_PYTHON)
  add_test(NAME native.cli COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_cli.py --cli $<TARGET_FILE:langc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --out ${CMAKE_CURRENT_BINARY_DIR}/cli-results)
  set_tests_properties(native.cli PROPERTIES TIMEOUT 180)
  add_test(NAME native.generics COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_generics.py --cli $<TARGET_FILE:langc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --out ${CMAKE_CURRENT_BINARY_DIR}/generic-results)
  set_tests_properties(native.generics PROPERTIES TIMEOUT 180)
else()
  message(STATUS "native.cli: not_run (test Python not found)")
endif()

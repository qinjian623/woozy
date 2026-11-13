# Root-registerable CTest fragment for tests/native/c_retained_capturing.
#
# Include this file from tests/native/CMakeLists.txt:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/c_retained_capturing/CMakeLists.txt)
#
# or add the directory inside the existing if(LANG_TEST_PYTHON) block:
#
#   add_subdirectory(c_retained_capturing)
#
# It does not modify any shared CMakeLists.txt.
get_filename_component(LANG_CAPTURING_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)

if(NOT TARGET Threads::Threads)
  find_package(Threads REQUIRED)
endif()

if(LLVM_TOOLS_BINARY_DIR)
  set(LANG_CAPTURING_CLANG "${LLVM_TOOLS_BINARY_DIR}/clang")
else()
  find_program(LANG_CAPTURING_CLANG NAMES clang)
endif()

if(LANG_CAPTURING_CLANG AND EXISTS "${LANG_CAPTURING_CLANG}")
  set(LANG_CAPTURING_FIXTURE_SO
    "${CMAKE_CURRENT_BINARY_DIR}/libc_capturing.so")
  add_custom_command(
    OUTPUT "${LANG_CAPTURING_FIXTURE_SO}"
    COMMAND "${LANG_CAPTURING_CLANG}"
      --target=x86_64-unknown-linux-gnu
      -std=c11 -O2 -fPIC -pthread -Wall -Wextra
      -shared -Wl,-soname,libc_capturing.so
      "${LANG_CAPTURING_DIR}/fixture/capturing_fixture.c"
      -o "${LANG_CAPTURING_FIXTURE_SO}"
    DEPENDS "${LANG_CAPTURING_DIR}/fixture/capturing_fixture.c"
      "${LANG_CAPTURING_DIR}/fixture/capturing_fixture.h"
    COMMENT "Building the retained capturing C fixture"
    VERBATIM)
  add_custom_target(lang_c_retained_capturing_fixture_lib
    DEPENDS "${LANG_CAPTURING_FIXTURE_SO}")

  add_executable(lang_c_retained_capturing_fixture_test
    "${LANG_CAPTURING_DIR}/fixture_lifecycle_test.cpp")
  add_dependencies(lang_c_retained_capturing_fixture_test
    lang_c_retained_capturing_fixture_lib)
  target_include_directories(lang_c_retained_capturing_fixture_test PRIVATE
    "${LANG_CAPTURING_DIR}/fixture")
  target_compile_features(lang_c_retained_capturing_fixture_test PRIVATE cxx_std_20)
  target_link_libraries(lang_c_retained_capturing_fixture_test
    PRIVATE "${LANG_CAPTURING_FIXTURE_SO}" Threads::Threads)
  set_target_properties(lang_c_retained_capturing_fixture_test PROPERTIES
    BUILD_RPATH "${CMAKE_CURRENT_BINARY_DIR}")
  add_test(NAME native.c_retained_capturing_fixture
    COMMAND lang_c_retained_capturing_fixture_test)
  set_tests_properties(native.c_retained_capturing_fixture PROPERTIES TIMEOUT 60)
endif()

if(LANG_TEST_PYTHON AND TARGET lang_hosted_runtime AND TARGET woozyc
   AND LANG_CAPTURING_CLANG AND EXISTS "${LANG_CAPTURING_CLANG}")
  set(LANG_CAPTURING_FIXTURE_ARG)
  if(TARGET lang_c_retained_capturing_fixture_test)
    list(APPEND LANG_CAPTURING_FIXTURE_ARG
      --fixture-test $<TARGET_FILE:lang_c_retained_capturing_fixture_test>)
  endif()
  add_test(NAME native.c_retained_capturing COMMAND ${LANG_TEST_PYTHON}
    "${LANG_SOURCE_ROOT}/tests/run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/c-retained-capturing-results"
    "${LANG_CAPTURING_DIR}/run_capturing.py"
    --cli $<TARGET_FILE:woozyc>
    --root "${LANG_SOURCE_ROOT}"
    --clang "${LANG_CAPTURING_CLANG}"
    --hosted-runtime $<TARGET_FILE:lang_hosted_runtime>
    --native-runtime $<TARGET_FILE:lang_native_runtime>
    --source-tests require
    ${LANG_CAPTURING_FIXTURE_ARG})
  set_tests_properties(native.c_retained_capturing PROPERTIES TIMEOUT 900)
endif()

# Root-registerable CTest fragment for tests/native/c_retained_static.
#
# Either include this file from tests/native/CMakeLists.txt:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/c_retained_static/CMakeLists.txt)
#
# or add it as a subdirectory inside the existing if(LANG_TEST_PYTHON) block:
#
#   add_subdirectory(c_retained_static)
#
# It deliberately does not modify the shared tests/native/CMakeLists.txt.
if(TARGET lang_native_runtime AND TARGET woozyc)
  if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
  endif()
  add_executable(lang_retained_callback_gc_program_test
    ${CMAKE_CURRENT_LIST_DIR}/gc_program_callback_test.cpp)
  target_compile_features(lang_retained_callback_gc_program_test PRIVATE cxx_std_20)
  target_include_directories(lang_retained_callback_gc_program_test
    PRIVATE ${LANG_SOURCE_ROOT}/runtime/native)
  target_link_libraries(lang_retained_callback_gc_program_test
    PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.c_retained_static_gc_program
    COMMAND lang_retained_callback_gc_program_test)
  set_tests_properties(native.c_retained_static_gc_program PROPERTIES TIMEOUT 60)
endif()

if(LANG_TEST_PYTHON AND TARGET lang_hosted_runtime)
  add_test(NAME native.c_retained_static COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_retained_static.py
    --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/c-retained-static-results
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --ar ${LLVM_TOOLS_BINARY_DIR}/llvm-ar
    --hosted-runtime $<TARGET_FILE:lang_hosted_runtime>
    --native-runtime $<TARGET_FILE:lang_native_runtime>
    --callback-gc require)
  set_tests_properties(native.c_retained_static PROPERTIES TIMEOUT 300)
endif()

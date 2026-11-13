# Registration fragment for tests/native/async_memory.
#
# Root can include this directory from tests/native/CMakeLists.txt with:
#   add_subdirectory(async_memory)
#
# The runner also accepts --libxml2-lib and --only. The CMake test below keeps
# the default full O0/O2 matrix and relies on the caller's LD_LIBRARY_PATH.
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND
   TARGET lang_native_helper AND TARGET lang_native_runtime)
  add_test(NAME native.async_memory COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_async_memory.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --toolchain ${LLVM_TOOLS_BINARY_DIR}
    --root "${LANG_SOURCE_ROOT}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/async-memory-results")
  set_tests_properties(native.async_memory PROPERTIES TIMEOUT 900)
else()
  message(STATUS "native.async_memory: not_run (LLVM/Native helper unavailable)")
endif()

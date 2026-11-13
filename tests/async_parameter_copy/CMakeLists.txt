# Registration fragment for tests/async_parameter_copy.
#
# Registered from the root CMakeLists.txt with tests/async.
#
# The runner also accepts --libxml2-lib and --only. This fragment keeps the
# default full O0/O2 matrix and relies on the caller's LD_LIBRARY_PATH.
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND
   TARGET lang_native_helper AND TARGET lang_native_runtime)
  add_test(NAME native.async_parameter_copy COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/../run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/async-parameter-copy-results"
    "${CMAKE_CURRENT_LIST_DIR}/run_parameter_copy.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --toolchain ${LLVM_TOOLS_BINARY_DIR}
    --root "${CMAKE_CURRENT_LIST_DIR}/../..")
  set_tests_properties(native.async_parameter_copy PROPERTIES TIMEOUT 900)
else()
  message(STATUS "native.async_parameter_copy: not_run (LLVM/Native helper unavailable)")
endif()

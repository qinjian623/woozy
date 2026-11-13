if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND TARGET lang_hosted_runtime)
  add_test(NAME native.hosted_thread_gc COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/../../run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/results"
    "${CMAKE_CURRENT_LIST_DIR}/run_thread_cleanup.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_hosted_runtime>
    --toolchain ${LLVM_TOOLS_BINARY_DIR}
    --root "${CMAKE_CURRENT_LIST_DIR}/../../..")
  set_tests_properties(native.hosted_thread_gc PROPERTIES TIMEOUT 300)
endif()

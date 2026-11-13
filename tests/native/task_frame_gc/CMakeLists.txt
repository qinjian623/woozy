if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND
   TARGET lang_native_helper AND TARGET lang_native_runtime)
  add_test(NAME native.task_frame_gc COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/../../run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/results"
    "${CMAKE_CURRENT_LIST_DIR}/run_task_frame_gc.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --toolchain ${LLVM_TOOLS_BINARY_DIR}
    --root "${CMAKE_CURRENT_LIST_DIR}/../../..")
  set_tests_properties(native.task_frame_gc PROPERTIES TIMEOUT 900)
endif()

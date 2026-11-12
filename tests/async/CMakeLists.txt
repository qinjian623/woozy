if(LANG_TEST_PYTHON)
  add_test(NAME native.async_source COMMAND "${LANG_TEST_PYTHON}"
    "${PROJECT_SOURCE_DIR}/tests/run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/async-source-results"
    "${CMAKE_CURRENT_LIST_DIR}/run_async_cases.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --clang "${LLVM_TOOLS_BINARY_DIR}/clang++"
    --ld "${LLVM_TOOLS_BINARY_DIR}/ld.lld"
    --root "${PROJECT_SOURCE_DIR}" --observe on)
  set_tests_properties(native.async_source PROPERTIES TIMEOUT 600)
endif()

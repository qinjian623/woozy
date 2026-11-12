if(LANG_TEST_PYTHON)
  if(LANG_ENABLE_LLVM)
    add_test(NAME trait_objects.cases COMMAND "${LANG_TEST_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/run_trait_cases.py"
      --cli $<TARGET_FILE:woozyc>
      --root "${CMAKE_CURRENT_SOURCE_DIR}/../.."
      --out "${CMAKE_CURRENT_BINARY_DIR}/trait-objects"
      --python "${LANG_TEST_PYTHON}"
      --native)
  else()
    add_test(NAME trait_objects.cases COMMAND "${LANG_TEST_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/run_trait_cases.py"
      --cli $<TARGET_FILE:woozyc>
      --root "${CMAKE_CURRENT_SOURCE_DIR}/../.."
      --out "${CMAKE_CURRENT_BINARY_DIR}/trait-objects"
      --python "${LANG_TEST_PYTHON}")
  endif()
  set_tests_properties(trait_objects.cases PROPERTIES TIMEOUT 600)
else()
  message(STATUS "trait_objects.cases: not_run (test Python not found)")
endif()

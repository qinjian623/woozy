# Kept local to this suite; the integrator owns the shared test inventory.
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON)
  foreach(mode IN ITEMS native-o0 native-o2)
    add_test(NAME stdlib.fs_remove.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_SOURCE_DIR}/tests/tooling/stdlib_project_test.py
      $<TARGET_FILE:woozyc> ${CMAKE_CURRENT_SOURCE_DIR}/lang.toml ${mode}
      --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_remove.${mode} PROPERTIES TIMEOUT 240)

    add_test(NAME stdlib.fs_remove.low_limit.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/run_low_limit.py
      $<TARGET_FILE:woozyc> ${CMAKE_CURRENT_SOURCE_DIR}/low_limit/lang.toml
      ${mode} --limit 64 --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_remove.low_limit.${mode} PROPERTIES TIMEOUT 240)

    add_test(NAME stdlib.fs_remove.race.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/run_race.py
      $<TARGET_FILE:woozyc> ${CMAKE_CURRENT_SOURCE_DIR}/race/lang.toml
      ${mode} --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_remove.race.${mode} PROPERTIES TIMEOUT 240)
  endforeach()
endif()

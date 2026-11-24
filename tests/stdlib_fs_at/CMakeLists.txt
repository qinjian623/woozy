add_executable(lang_stdlib_fs_at_runtime_test runtime_test.cpp)
target_compile_features(lang_stdlib_fs_at_runtime_test PRIVATE cxx_std_20)
target_link_libraries(lang_stdlib_fs_at_runtime_test PRIVATE lang_native_runtime)
add_test(NAME stdlib.fs_at.runtime COMMAND lang_stdlib_fs_at_runtime_test)
set_tests_properties(stdlib.fs_at.runtime PROPERTIES TIMEOUT 60)

if(LANG_TEST_PYTHON)
  foreach(mode IN ITEMS native-o0 native-o2)
    add_test(NAME stdlib.fs_at.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_SOURCE_DIR}/tests/tooling/stdlib_project_test.py
      $<TARGET_FILE:woozyc> ${CMAKE_CURRENT_SOURCE_DIR}/lang.toml ${mode}
      --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_at.${mode} PROPERTIES TIMEOUT 240)
  endforeach()
endif()

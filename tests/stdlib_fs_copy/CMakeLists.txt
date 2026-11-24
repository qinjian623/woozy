# Linux x86_64 only: the runtime operations under test use direct Linux
# syscalls and the Wz fixture inspects /proc/self/fd.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  return()
endif()

add_executable(lang_stdlib_fs_copy_runtime_test runtime_test.cpp)
target_compile_features(lang_stdlib_fs_copy_runtime_test PRIVATE cxx_std_20)
target_link_libraries(lang_stdlib_fs_copy_runtime_test PRIVATE lang_native_runtime)
add_test(NAME stdlib.fs_copy.runtime COMMAND lang_stdlib_fs_copy_runtime_test)
set_tests_properties(stdlib.fs_copy.runtime PROPERTIES TIMEOUT 60)

if(LANG_TEST_PYTHON)
  foreach(mode IN ITEMS native-o0 native-o2)
    add_test(NAME stdlib.fs_copy.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_SOURCE_DIR}/tests/tooling/stdlib_project_test.py
      $<TARGET_FILE:woozyc> ${CMAKE_CURRENT_SOURCE_DIR}/lang.toml ${mode}
      --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_copy.${mode} PROPERTIES TIMEOUT 240)
    add_test(NAME stdlib.fs_copy.race.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/run_race.py $<TARGET_FILE:woozyc>
      ${CMAKE_CURRENT_SOURCE_DIR}/race/lang.toml ${mode}
      --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_copy.race.${mode} PROPERTIES TIMEOUT 240)
    add_test(NAME stdlib.fs_copy.rlimit.${mode} COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/run_low_rlimit.py $<TARGET_FILE:woozyc>
      ${CMAKE_CURRENT_SOURCE_DIR}/limits/lang.toml ${mode}
      --libxml ${LANG_TEST_LIBXML_DIR})
    set_tests_properties(stdlib.fs_copy.rlimit.${mode} PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77)
  endforeach()
endif()

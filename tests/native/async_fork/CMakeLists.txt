# Async fork recovery: real Source checks plus private-runtime stress. The
# runtime test uses the accepted lang-async/1 and lang-gc-program/1 ABIs.
if(TARGET lang_native_runtime AND TARGET Threads::Threads)
  add_executable(lang_native_async_fork_runtime_test
    async_fork_runtime_test.cpp)
  target_compile_features(lang_native_async_fork_runtime_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_async_fork_runtime_test
    PRIVATE lang_native_runtime Threads::Threads)
  target_link_options(lang_native_async_fork_runtime_test PRIVATE
    -Wl,--wrap=lang_gc_program_unregister_v1)
  foreach(mode IN ITEMS same-thread failed-fork race gc)
    add_test(NAME native.async_fork_runtime_${mode}
      COMMAND lang_native_async_fork_runtime_test ${mode})
    set_tests_properties(native.async_fork_runtime_${mode}
      PROPERTIES TIMEOUT 60)
  endforeach()
endif()

if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND
   TARGET lang_native_helper AND TARGET lang_native_runtime)
  add_test(NAME native.async_fork COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/../../run_isolated.py"
    "${CMAKE_CURRENT_BINARY_DIR}/results"
    "${CMAKE_CURRENT_LIST_DIR}/run_async_fork.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --toolchain "${LLVM_TOOLS_BINARY_DIR}"
    --root "${CMAKE_CURRENT_LIST_DIR}/../../..")
  set_tests_properties(native.async_fork PROPERTIES TIMEOUT 900)
endif()

if(TARGET lang_hosted_runtime AND TARGET Threads::Threads)
  add_executable(lang_native_async_fork_hosted_test
    hosted_async_fork_test.cpp)
  target_compile_features(lang_native_async_fork_hosted_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_async_fork_hosted_test
    PRIVATE lang_hosted_runtime Threads::Threads)
  target_link_options(lang_native_async_fork_hosted_test PRIVATE
    -pthread -Wl,-u,lang_hosted_startup_v1)
  foreach(mode IN ITEMS same-direct failed-source nested-user-child
                   foreign-source foreign-spawn)
    add_test(NAME native.async_fork_hosted_${mode}
      COMMAND lang_native_async_fork_hosted_test ${mode})
    set_tests_properties(native.async_fork_hosted_${mode}
      PROPERTIES TIMEOUT 60)
  endforeach()
  if(LANG_TEST_PYTHON)
    foreach(mode IN ITEMS prepare-final-release-direct
                     prepare-final-release-source
                     prepare-final-release-parent)
      add_test(NAME native.async_fork_hosted_${mode}
        COMMAND "${LANG_TEST_PYTHON}"
        "${CMAKE_CURRENT_LIST_DIR}/run_hosted_atfork_release.py"
        $<TARGET_FILE:lang_native_async_fork_hosted_test> ${mode}
        --out "${CMAKE_CURRENT_BINARY_DIR}/atfork-release-${mode}.json")
      set_tests_properties(native.async_fork_hosted_${mode}
        PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME native.async_fork_hosted_foreign-direct
      COMMAND "${LANG_TEST_PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/run_hosted_direct_refusal.py"
      $<TARGET_FILE:lang_native_async_fork_hosted_test>)
    set_tests_properties(native.async_fork_hosted_foreign-direct
      PROPERTIES TIMEOUT 60)
  endif()
endif()

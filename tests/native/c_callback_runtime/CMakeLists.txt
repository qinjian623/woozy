# Root-registerable CTest fragment for tests/native/c_callback_runtime.
#
# Either include this file from tests/native/CMakeLists.txt:
#
#   include(${CMAKE_CURRENT_LIST_DIR}/c_callback_runtime/CMakeLists.txt)
#
# or add it as a subdirectory inside the existing native test block:
#
#   add_subdirectory(c_callback_runtime)
#
# It deliberately does not modify the shared tests/native/CMakeLists.txt. The
# fragment assumes Root wired runtime/hosted/c_callback.cpp into
# LANG_HOSTED_REPLACEMENT_SOURCES so lang_hosted_runtime exports the
# lang_c_registration_* entry points.
if(TARGET lang_hosted_runtime)
  if(NOT TARGET Threads::Threads)
    find_package(Threads REQUIRED)
  endif()

  add_executable(lang_c_callback_runtime_test
    ${CMAKE_CURRENT_LIST_DIR}/c_callback_runtime_test.cpp)
  target_compile_features(lang_c_callback_runtime_test PRIVATE cxx_std_20)
  target_include_directories(lang_c_callback_runtime_test PRIVATE
    ${LANG_SOURCE_ROOT}/runtime/hosted
    ${LANG_SOURCE_ROOT}/runtime/native)
  target_link_libraries(lang_c_callback_runtime_test
    PRIVATE lang_hosted_runtime Threads::Threads)
  # The harness defines __wrap_pthread_cond_wait to observe the follower's SAFE
  # condition wait.
  target_link_options(lang_c_callback_runtime_test
    PRIVATE -Wl,--wrap=pthread_cond_wait)
  add_test(NAME native.c_callback_runtime
    COMMAND lang_c_callback_runtime_test)
  set_tests_properties(native.c_callback_runtime PROPERTIES TIMEOUT 120)

  add_executable(lang_c_callback_process_test
    ${CMAKE_CURRENT_LIST_DIR}/c_callback_process_test.cpp)
  target_compile_features(lang_c_callback_process_test PRIVATE cxx_std_20)
  target_include_directories(lang_c_callback_process_test PRIVATE
    ${LANG_SOURCE_ROOT}/runtime/hosted
    ${LANG_SOURCE_ROOT}/runtime/native)
  target_link_libraries(lang_c_callback_process_test
    PRIVATE lang_hosted_runtime Threads::Threads)
  add_test(NAME native.c_callback_process
    COMMAND lang_c_callback_process_test)
  set_tests_properties(native.c_callback_process PROPERTIES TIMEOUT 120)
endif()

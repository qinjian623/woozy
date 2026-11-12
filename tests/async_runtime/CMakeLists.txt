# Focused runtime-lifetime tests for runtime/native/async.cpp
#
# The test translation unit compiles the real executor unit directly, so no
# runtime archive, LLVM helper or CTest fixture ordering is involved:
#
#   add_subdirectory(tests/async_runtime)   # inside BUILD_TESTING
#
# The Root integration owns adding that line to the top-level CMakeLists.txt
# (this directory is owned by the async-runtime lifetime task).

add_executable(lang_async_runtime_lifetime_test
  async_runtime_lifetime_test.cpp)
target_compile_features(lang_async_runtime_lifetime_test PRIVATE cxx_std_20)
target_compile_options(lang_async_runtime_lifetime_test PRIVATE
  -fno-exceptions -fno-rtti -fno-omit-frame-pointer
  -fsanitize=address,undefined
  -Wall -Wextra -Wno-unused-const-variable)
target_link_options(lang_async_runtime_lifetime_test PRIVATE
  -fsanitize=address,undefined)
add_test(NAME native.async_runtime_lifetime
  COMMAND lang_async_runtime_lifetime_test)
set_tests_properties(native.async_runtime_lifetime PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

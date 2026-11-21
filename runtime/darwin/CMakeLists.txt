cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Darwin runtime architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.4 CACHE STRING "Minimum macOS version")
project(woozy_darwin_core LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64" OR
   CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14.4)
  message(FATAL_ERROR "Woozy Darwin runtime requires arm64 and macOS 14.4 or later")
endif()

add_library(lang_darwin_core STATIC
  ../native/alloc.cpp ../native/panic.cpp ../native/weak.cpp
  ../native/gc.cpp ../native/gc_world.cpp ../native/gc_runtime.cpp ../native/gc_program.cpp
  os.cpp system.cpp process.cpp network.cpp signals.cpp
  ../native/os_buffers.cpp ../native/system_buffers.cpp ../native/network_buffers.cpp
  ../hosted/thread.cpp ../hosted/c_callback.cpp ../native/sync.cpp
  ../native/dns.cpp ../native/ipc.cpp ../native/async.cpp
  ../native/text.cpp ../native/numbers.cpp ../native/slice.cpp
  ../native/unicode/unicode.cpp ../native/unicode/utf8proc_adapter.c)
include(${CMAKE_CURRENT_LIST_DIR}/../native/math/sources.cmake)
lang_native_math_sources(lang_darwin_core)
target_compile_features(lang_darwin_core PRIVATE cxx_std_20)
set_target_properties(lang_darwin_core PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)
target_compile_options(lang_darwin_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
target_link_options(lang_darwin_core INTERFACE -Wl,-U,_lang_vec_observe_v1)

include(CTest)
if(BUILD_TESTING)
  foreach(suite IN ITEMS os network async ipc signal)
    add_executable(woozy_darwin_${suite}_test ../../tests/native/darwin_${suite}_runtime_test.cpp)
    target_compile_features(woozy_darwin_${suite}_test PRIVATE cxx_std_20)
    target_link_libraries(woozy_darwin_${suite}_test PRIVATE lang_darwin_core)
    target_compile_options(woozy_darwin_${suite}_test PRIVATE -UNDEBUG)
    target_link_options(woozy_darwin_${suite}_test PRIVATE -Wl,-dead_strip)
    add_test(NAME darwin.${suite} COMMAND woozy_darwin_${suite}_test)
  endforeach()
  add_executable(woozy_darwin_math_test ../../tests/native/math_runtime_test.cpp)
  target_compile_features(woozy_darwin_math_test PRIVATE cxx_std_20)
  target_link_libraries(woozy_darwin_math_test PRIVATE lang_darwin_core)
  add_test(NAME darwin.math COMMAND woozy_darwin_math_test)
  add_executable(woozy_darwin_unicode_test ../../tests/native/unicode/host_smoke_test.cpp)
  target_compile_features(woozy_darwin_unicode_test PRIVATE cxx_std_20)
  target_link_libraries(woozy_darwin_unicode_test PRIVATE lang_darwin_core)
  add_test(NAME darwin.unicode COMMAND woozy_darwin_unicode_test)
endif()

cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET woozyc)
  project(lang_gc_activation LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "GC activation checks need Native" FORCE)
  add_subdirectory(../../.. compiler)
  return()
endif()

if(LANG_ENABLE_LLVM AND BUILD_TESTING AND TARGET woozyc)
  find_program(LANG_GC_ACTIVATION_PYTHON NAMES python3.12 python3)
  if(LANG_GC_ACTIVATION_PYTHON)
    add_test(NAME native.gc_activation COMMAND "${LANG_GC_ACTIVATION_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/run_gc_activation.py"
      --cli $<TARGET_FILE:woozyc>
      --helper $<TARGET_FILE:lang_native_helper>
      --root "${CMAKE_CURRENT_LIST_DIR}/../../.."
      --out "${CMAKE_CURRENT_BINARY_DIR}/results")
    set_tests_properties(native.gc_activation PROPERTIES TIMEOUT 300)
  else()
    message(STATUS "native.gc_activation: not_run (test Python unavailable)")
  endif()
endif()

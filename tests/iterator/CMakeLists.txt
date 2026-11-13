cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET woozyc)
  project(lang_iterator LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "LANG04 user Iterator needs the native backend" FORCE)
  add_subdirectory(../.. compiler)
  return()
endif()

# Root registration snippet:
#   if(LANG_ENABLE_LLVM)
#     add_subdirectory(tests/iterator)
#   endif()
if(LANG_ENABLE_LLVM AND BUILD_TESTING)
  if(LANG_TEST_PYTHON AND LANG_PYTHON312 AND TARGET woozyc AND
     TARGET lang_python_emit AND TARGET lang_native_runtime)
    add_test(NAME iterator.runtime COMMAND "${LANG_TEST_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/../run_isolated.py"
      "${CMAKE_CURRENT_BINARY_DIR}/iterator-results"
      "${CMAKE_CURRENT_LIST_DIR}/run_iterator.py"
      --cli $<TARGET_FILE:woozyc>
      --python-emit $<TARGET_FILE:lang_python_emit>
      --root "${CMAKE_CURRENT_LIST_DIR}/../.."
      --python "${LANG_PYTHON312}"
      --clang "${LLVM_TOOLS_BINARY_DIR}/clang"
      --runtime $<TARGET_FILE:lang_native_runtime>
      --readelf "${LLVM_TOOLS_BINARY_DIR}/llvm-readelf")
    set_tests_properties(iterator.runtime PROPERTIES TIMEOUT 600)
  else()
    message(STATUS "iterator.runtime: not_run (LLVM/Python/helpers unavailable)")
  endif()
endif()

cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET woozyc)
  project(lang_owning_inout LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "LANG03 owning inout needs the native backend" FORCE)
  add_subdirectory(../.. compiler)
  return()
endif()

if(LANG_ENABLE_LLVM AND BUILD_TESTING)
  if(LANG_TEST_PYTHON AND LANG_PYTHON312 AND TARGET woozyc)
    add_test(NAME native.owning_inout COMMAND "${LANG_TEST_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/../run_isolated.py"
      "${CMAKE_CURRENT_BINARY_DIR}/owning-inout-results"
      "${CMAKE_CURRENT_LIST_DIR}/run_owning_inout.py"
      --cli $<TARGET_FILE:woozyc>
      --root "${CMAKE_CURRENT_LIST_DIR}/../.."
      --python "${LANG_PYTHON312}"
      --readelf "${LLVM_TOOLS_BINARY_DIR}/llvm-readelf")
    set_tests_properties(native.owning_inout PROPERTIES TIMEOUT 600)
  else()
    message(STATUS "native.owning_inout: not_run (Python or woozyc unavailable)")
  endif()
endif()

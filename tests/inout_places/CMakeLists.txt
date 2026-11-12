cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET woozyc)
  project(lang_inout_places LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "LANG03 inout places need the native backend" FORCE)
  add_subdirectory(../.. compiler)
  return()
endif()

if(LANG_ENABLE_LLVM AND BUILD_TESTING)
  find_program(LANG_INOUT_PYTHON NAMES python3.12 python3)
  if(LANG_INOUT_PYTHON AND TARGET woozyc)
    add_test(NAME native.inout_places COMMAND "${LANG_INOUT_PYTHON}"
      "${CMAKE_CURRENT_LIST_DIR}/run_inout_places.py"
      --cli $<TARGET_FILE:woozyc>
      --root "${CMAKE_CURRENT_LIST_DIR}/../.."
      --python "${LANG_INOUT_PYTHON}"
      --readelf "${LLVM_TOOLS_BINARY_DIR}/llvm-readelf"
      --out "${CMAKE_CURRENT_BINARY_DIR}/results")
    set_tests_properties(native.inout_places PROPERTIES TIMEOUT 600)
  else()
    message(STATUS "native.inout_places: not_run (Python or woozyc unavailable)")
  endif()
endif()

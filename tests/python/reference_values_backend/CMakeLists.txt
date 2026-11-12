cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(LANG_RV_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)

# Focused verification project for the Python reference-value backend. It can
# be configured on its own without editing the root CMakeLists.txt:
#   cmake -S tests/python/reference_values_backend -B DIR -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lang_python_reference_values LANGUAGES CXX)
  enable_testing()
  # Build the compiler libraries/CLI but not the unrelated root suites.
  set(BUILD_TESTING OFF CACHE BOOL "Build the root compiler test suites" FORCE)
  add_subdirectory("${LANG_RV_ROOT}" frontend)
endif()

if(NOT TARGET lang_backend_python)
  add_subdirectory("${LANG_RV_ROOT}/src/backend/python" backend-python)
endif()

find_package(json-c CONFIG REQUIRED)
add_executable(lang_python_reference_values_emitter_test emitter_test.cpp)
target_link_libraries(lang_python_reference_values_emitter_test
  PRIVATE lang_sema lang_backend_python)
add_test(NAME python.reference_values_emitter
  COMMAND lang_python_reference_values_emitter_test "${LANG_RV_ROOT}")

set(LANG_RV_PYTHON312 "" CACHE FILEPATH
  "Explicit Python 3.12 for the reference-value runtime probes")
if(NOT LANG_RV_PYTHON312 OR NOT EXISTS "${LANG_RV_PYTHON312}")
  find_program(LANG_RV_PYTHON312 NAMES python3.12 DOC
    "Explicit Python 3.12 runtime for generated programs")
endif()
if(LANG_RV_PYTHON312)
  add_test(NAME python.reference_values_runtime
    COMMAND "${LANG_RV_PYTHON312}" "${CMAKE_CURRENT_LIST_DIR}/runtime_reference_values_test.py")
  set_tests_properties(python.reference_values_runtime PROPERTIES TIMEOUT 60)
  if(TARGET woozyc)
    add_test(NAME python.reference_values_old_source
      COMMAND "${LANG_RV_PYTHON312}"
        "${CMAKE_CURRENT_LIST_DIR}/run_reference_values.py"
        --cli $<TARGET_FILE:woozyc> --root "${LANG_RV_ROOT}"
        --out "${CMAKE_CURRENT_BINARY_DIR}/reference-values"
        --python "${LANG_RV_PYTHON312}")
    set_tests_properties(python.reference_values_old_source PROPERTIES TIMEOUT 300)
  endif()
else()
  message(STATUS "python.reference_values_*: not_run (set LANG_RV_PYTHON312)")
endif()

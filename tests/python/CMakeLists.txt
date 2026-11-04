cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_EXTENSIONS OFF)
# Standalone verification without editing the root CMakeLists.txt. This file
# can also be added by the integrator after lang_sema/lang_backend_python.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lang_python_verification LANGUAGES CXX)
  enable_testing()
  get_filename_component(LANG_PYTHON_TEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  add_subdirectory("${LANG_PYTHON_TEST_ROOT}" frontend)
  return()
else()
  get_filename_component(LANG_PYTHON_TEST_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
if(NOT TARGET lang_backend_python)
  add_subdirectory("${LANG_PYTHON_TEST_ROOT}/src/backend/python" backend-python)
endif()
find_package(json-c CONFIG REQUIRED)
add_executable(lang_python_emit emit_project.cpp)
target_link_libraries(lang_python_emit PRIVATE lang_sema lang_backend_python json-c::json-c)
add_executable(lang_python_api_test api_test.cpp)
target_link_libraries(lang_python_api_test PRIVATE lang_sema lang_backend_python)
add_test(NAME python.api COMMAND lang_python_api_test "${LANG_PYTHON_TEST_ROOT}")
find_program(LANG_PYTHON312 NAMES python3.12 DOC "Explicit Python 3.12 runtime for generated programs")
if(NOT LANG_PYTHON312)
  message(STATUS "python.projects: not_run (set LANG_PYTHON312 to Python 3.12)")
  return()
endif()
add_test(NAME python.projects COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_cases.py"
  --helper $<TARGET_FILE:lang_python_emit> --root "${LANG_PYTHON_TEST_ROOT}"
  --out "${CMAKE_CURRENT_BINARY_DIR}/results" --python "${LANG_PYTHON312}")
set_tests_properties(python.projects PROPERTIES TIMEOUT 180)
add_test(NAME python.generics COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_generics.py"
  --langc $<TARGET_FILE:langc> --root "${LANG_PYTHON_TEST_ROOT}"
  --out "${CMAKE_CURRENT_BINARY_DIR}/generics" --python "${LANG_PYTHON312}")
set_tests_properties(python.generics PROPERTIES TIMEOUT 180)
add_test(NAME python.aggregates COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_aggregates.py"
  --langc $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/aggregates"
  --python "${LANG_PYTHON312}")
set_tests_properties(python.aggregates PROPERTIES TIMEOUT 180)
add_test(NAME python.strings COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_strings.py"
  --cli $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/strings"
  --python "${LANG_PYTHON312}")
set_tests_properties(python.strings PROPERTIES TIMEOUT 180)
add_test(NAME python.cleanup COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_cleanup.py"
  --helper $<TARGET_FILE:lang_python_emit> --root "${LANG_PYTHON_TEST_ROOT}"
  --out "${CMAKE_CURRENT_BINARY_DIR}/cleanup" --python "${LANG_PYTHON312}")
set_tests_properties(python.cleanup PROPERTIES TIMEOUT 180)
add_test(NAME python.cleanup_regressions COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_cleanup_regressions.py"
  --helper $<TARGET_FILE:lang_python_emit> --cli $<TARGET_FILE:langc>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/cleanup-regressions"
  --python "${LANG_PYTHON312}")
set_tests_properties(python.cleanup_regressions PROPERTIES TIMEOUT 180)
add_test(NAME python.cli COMMAND ${CMAKE_COMMAND}
  -DLANGC=$<TARGET_FILE:langc> -DPYTHON=${LANG_PYTHON312}
  -DROOT=${LANG_PYTHON_TEST_ROOT} -DNATIVE_AVAILABLE=${LANG_ENABLE_LLVM}
  -P ${CMAKE_CURRENT_LIST_DIR}/cli.cmake)

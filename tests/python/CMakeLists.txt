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
foreach(fixture IN ITEMS strings string-constants string-aggregates string-vec-aggregates aggregate-constants)
  add_test(NAME python.${fixture} COMMAND "${LANG_PYTHON312}"
    "${CMAKE_CURRENT_LIST_DIR}/run_strings.py"
    --cli $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/${fixture}"
    --python "${LANG_PYTHON312}" --fixture ${fixture})
  set_tests_properties(python.${fixture} PROPERTIES TIMEOUT 180)
endforeach()
add_test(NAME python.string-aggregate-binding COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_string_binding.py"
  --cli $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/string-aggregate-binding"
  --python "${LANG_PYTHON312}")
set_tests_properties(python.string-aggregate-binding PROPERTIES TIMEOUT 180)
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
if(LANG_ENABLE_LLVM)
  add_executable(lang_python_float_oracle float_oracle.cpp)
  target_include_directories(lang_python_float_oracle SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
  llvm_map_components_to_libnames(LANG_PYTHON_ORACLE_LIBS Support)
  target_link_libraries(lang_python_float_oracle PRIVATE ${LANG_PYTHON_ORACLE_LIBS})
  set(LANG_PYTHON_FLOAT_ORACLE_ARGS --oracle $<TARGET_FILE:lang_python_float_oracle>)
endif()
add_test(NAME python.floating COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_floating.py"
  --cli $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/floating"
  --python "${LANG_PYTHON312}" ${LANG_PYTHON_FLOAT_ORACLE_ARGS})
set_tests_properties(python.floating PROPERTIES TIMEOUT 180)
add_test(NAME python.cli COMMAND ${CMAKE_COMMAND}
  -DLANGC=$<TARGET_FILE:langc> -DPYTHON=${LANG_PYTHON312}
  -DROOT=${LANG_PYTHON_TEST_ROOT} -DNATIVE_AVAILABLE=${LANG_ENABLE_LLVM}
  -P ${CMAKE_CURRENT_LIST_DIR}/cli.cmake)
add_test(NAME python.ownership COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/run_ownership.py"
  --cli $<TARGET_FILE:langc> --helper $<TARGET_FILE:lang_python_emit>
  --root "${LANG_PYTHON_TEST_ROOT}" --out "${CMAKE_CURRENT_BINARY_DIR}/ownership"
  --python "${LANG_PYTHON312}")
set_tests_properties(python.ownership PROPERTIES TIMEOUT 180)
add_test(NAME python.ownership_runtime COMMAND "${LANG_PYTHON312}"
  "${CMAKE_CURRENT_LIST_DIR}/ownership_runtime_test.py")
add_executable(lang_python_ownership_ir_test ownership_ir_test.cpp)
target_link_libraries(lang_python_ownership_ir_test PRIVATE lang_sema lang_backend_python)
add_test(NAME python.ownership_ir COMMAND lang_python_ownership_ir_test)
if(TARGET langc AND TARGET lang_native_convert_probe AND TARGET lang_native_runtime
   AND TARGET lang_python_emit)
  add_test(NAME python.conversions COMMAND "${LANG_PYTHON312}"
    "${LANG_PYTHON_TEST_ROOT}/tests/numeric/run_conversions.py"
    --cli $<TARGET_FILE:langc> --probe $<TARGET_FILE:lang_native_convert_probe>
    --root "${LANG_PYTHON_TEST_ROOT}" --python "${LANG_PYTHON312}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --helper $<TARGET_FILE:lang_python_emit>
    --out "${CMAKE_CURRENT_BINARY_DIR}/conversion-results")
  set_tests_properties(python.conversions PROPERTIES TIMEOUT 900)
endif()
# T10 generated host/binding and original-04 execution.  The interpreter must
# be the dedicated CPython 3.12 environment with NumPy/PyTorch CPU installed;
# a plain python3.12 without those packages is intentionally not substituted.
set(LANG_T10_PYTHON "" CACHE FILEPATH "CPython 3.12 venv with NumPy/PyTorch CPU for T10")
if(LANG_T10_PYTHON AND EXISTS "${LANG_T10_PYTHON}")
  add_test(NAME python.t10_bindings COMMAND "${LANG_T10_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_t10_bindings.py"
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_PYTHON_TEST_ROOT}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/t10-bindings"
    --python "${LANG_T10_PYTHON}"
    --binding "${LANG_PYTHON_TEST_ROOT}/tests/python/t10_binding.json"
    --example-binding "${LANG_PYTHON_TEST_ROOT}/contracts/python_bindings.example.json"
    --reference "${LANG_PYTHON_TEST_ROOT}/baseline/samples/numpy_torch_reference.py"
    --observe "${CMAKE_CURRENT_LIST_DIR}/observe_t10.py"
    --original-observer "${CMAKE_CURRENT_LIST_DIR}/t10_original04.py"
    --reference-observer "${CMAKE_CURRENT_LIST_DIR}/t10_reference04.py")
  set_tests_properties(python.t10_bindings PROPERTIES TIMEOUT 600)
  add_test(NAME python.foreign_ownership COMMAND "${LANG_T10_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_t10_bindings.py"
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_PYTHON_TEST_ROOT}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/foreign-ownership"
    --python "${LANG_T10_PYTHON}"
    --binding "${LANG_PYTHON_TEST_ROOT}/tests/python/t10_binding.json"
    --example-binding "${LANG_PYTHON_TEST_ROOT}/contracts/python_bindings.example.json"
    --reference "${LANG_PYTHON_TEST_ROOT}/baseline/samples/numpy_torch_reference.py"
    --observe "${CMAKE_CURRENT_LIST_DIR}/observe_t10.py"
    --original-observer "${CMAKE_CURRENT_LIST_DIR}/t10_original04.py"
    --reference-observer "${CMAKE_CURRENT_LIST_DIR}/t10_reference04.py"
    --only foreign-ownership-move-aggregate
           foreign-ownership-cleanup
           foreign-ownership-rejections)
  set_tests_properties(python.foreign_ownership PROPERTIES TIMEOUT 300)
  set(LANG_T10_CLI_NATIVE_ARGS)
  if(LANG_ENABLE_LLVM AND TARGET lang_native_helper)
    list(APPEND LANG_T10_CLI_NATIVE_ARGS
      --native-helper $<TARGET_FILE:lang_native_helper>)
  endif()
  add_test(NAME python.t10_cli COMMAND "${LANG_T10_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_t10_cli.py"
    --langc $<TARGET_FILE:langc>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_PYTHON_TEST_ROOT}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/t10-cli"
    --python "${LANG_T10_PYTHON}"
    --binding "${LANG_PYTHON_TEST_ROOT}/contracts/python_bindings.example.json"
    --reference "${LANG_PYTHON_TEST_ROOT}/baseline/samples/numpy_torch_reference.py"
    --original-observer "${CMAKE_CURRENT_LIST_DIR}/t10_original04.py"
    --reference-observer "${CMAKE_CURRENT_LIST_DIR}/t10_reference04.py"
    ${LANG_T10_CLI_NATIVE_ARGS})
  set_tests_properties(python.t10_cli PROPERTIES TIMEOUT 1800)
else()
  message(STATUS "python.t10_bindings: not_run (set LANG_T10_PYTHON)")
  message(STATUS "python.t10_cli: not_run (set LANG_T10_PYTHON)")
endif()

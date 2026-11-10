cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT TARGET lang_sema)
  project(lang_t07_static LANGUAGES C CXX)
  include(CTest)
  # Root currently does not include this directory. The target guard also
  # permits the integrator to add_subdirectory(tests/generics) later.
  add_subdirectory(../.. compiler)
  # A root that includes this directory has already created the test target.
  if(TARGET lang_generics_type_test)
    return()
  endif()
endif()
find_package(json-c CONFIG REQUIRED)
if(NOT TARGET lang_generics_type_test)
  add_executable(lang_generics_type_test type_test.cpp)
  target_link_libraries(lang_generics_type_test PRIVATE lang_sema json-c::json-c)
  get_filename_component(LANG_T07_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  add_test(NAME sema.t07 COMMAND lang_generics_type_test "${LANG_T07_ROOT}")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME generics.list_inference COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_list_inference.py"
    --cli $<TARGET_FILE:woozyc>
    --root "${LANG_T07_ROOT}"
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/list-inference")
  set_tests_properties(generics.list_inference PROPERTIES TIMEOUT 120)
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.move_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_move.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/move-runtime")
  set_tests_properties(generics.move_runtime PROPERTIES TIMEOUT 420)
else()
  message(STATUS "generics.move_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.inout_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_inout.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/inout-runtime")
  set_tests_properties(generics.inout_runtime PROPERTIES TIMEOUT 240)
else()
  message(STATUS "generics.inout_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.method_move_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_method_move.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/method-move-runtime")
  set_tests_properties(generics.method_move_runtime PROPERTIES TIMEOUT 300)
else()
  message(STATUS "generics.method_move_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.method_inout_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_method_inout.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/method-inout-runtime")
  set_tests_properties(generics.method_inout_runtime PROPERTIES TIMEOUT 300)
else()
  message(STATUS "generics.method_inout_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.constrained_inout_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_constrained_inout.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/constrained-inout-runtime")
  set_tests_properties(generics.constrained_inout_runtime PROPERTIES TIMEOUT 300)
else()
  message(STATUS "generics.constrained_inout_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_python_emit)
  add_test(NAME generics.aggregate_inout_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_aggregate_inout.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/aggregate-inout-runtime")
  set_tests_properties(generics.aggregate_inout_runtime PROPERTIES TIMEOUT 420)
else()
  message(STATUS "generics.aggregate_inout_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.nested_patterns_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_nested_patterns.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/nested-patterns-runtime")
  set_tests_properties(generics.nested_patterns_runtime PROPERTIES TIMEOUT 420)
else()
  message(STATUS "generics.nested_patterns_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_python_emit)
  add_test(NAME generics.generic_trait_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_generic_traits.py"
    --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/generic-trait-runtime")
  set_tests_properties(generics.generic_trait_runtime PROPERTIES TIMEOUT 420)
else()
  message(STATUS "generics.generic_trait_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.indexed_calls_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_indexed_calls.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/indexed-calls-runtime")
  set_tests_properties(generics.indexed_calls_runtime PROPERTIES TIMEOUT 420)
else()
  message(STATUS "generics.indexed_calls_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_native_string_probe AND
   TARGET lang_python_emit)
  add_test(NAME generics.builtin_impl_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_builtin_impl.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --string-probe $<TARGET_FILE:lang_native_string_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/builtin-impl-runtime")
  set_tests_properties(generics.builtin_impl_runtime PROPERTIES TIMEOUT 600)
else()
  message(STATUS "generics.builtin_impl_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_native_string_probe AND
   TARGET lang_python_emit)
  add_test(NAME generics.self_modes_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_self_modes.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --string-probe $<TARGET_FILE:lang_native_string_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/self-modes-runtime")
  set_tests_properties(generics.self_modes_runtime PROPERTIES TIMEOUT 600)
else()
  message(STATUS "generics.self_modes_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()
if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_native_string_probe AND
   TARGET lang_python_emit)
  add_test(NAME generics.unbound_method_values_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_unbound_methods.py"
    --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_ownership_probe>
    --string-probe $<TARGET_FILE:lang_native_string_probe>
    --helper $<TARGET_FILE:lang_python_emit>
    --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/unbound-method-values-runtime")
  set_tests_properties(generics.unbound_method_values_runtime PROPERTIES TIMEOUT 600)
else()
  message(STATUS "generics.unbound_method_values_runtime: not_run (LLVM/3.12 helper unavailable)")
endif()

if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND LANG_PYTHON312 AND
   TARGET lang_native_ownership_probe AND TARGET lang_python_emit)
  add_test(NAME generics.receiver_modes_runtime COMMAND "${LANG_TEST_PYTHON}"
    "${CMAKE_CURRENT_LIST_DIR}/run_receiver_modes.py"
    --cli $<TARGET_FILE:woozyc> --probe $<TARGET_FILE:lang_native_ownership_probe>
    --helper $<TARGET_FILE:lang_python_emit> --root "${LANG_T07_ROOT}"
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --python "${LANG_PYTHON312}" --out "${CMAKE_CURRENT_BINARY_DIR}/receiver-modes-runtime")
  set_tests_properties(generics.receiver_modes_runtime PROPERTIES TIMEOUT 600)
endif()

add_executable(lang_aggregate_const_test aggregate_const_test.cpp)
target_link_libraries(lang_aggregate_const_test PRIVATE lang_sema)
if(TARGET lang_native)
  target_link_libraries(lang_aggregate_const_test PRIVATE lang_native)
  target_compile_definitions(lang_aggregate_const_test PRIVATE LANG_AGGREGATE_NATIVE=1)
endif()
add_test(NAME sema.aggregate_constants COMMAND lang_aggregate_const_test "${LANG_T07_ROOT}")
if(LANG_TEST_PYTHON)
  set(aggregate_rejection_backends python)
  if(LANG_ENABLE_LLVM)
    list(PREPEND aggregate_rejection_backends native)
  endif()
  add_test(NAME sema.aggregate_constant_rejections COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_aggregate_const_rejections.py --cli $<TARGET_FILE:woozyc>
    --out ${CMAKE_CURRENT_BINARY_DIR}/aggregate-constant-rejections
    --backends ${aggregate_rejection_backends})
endif()

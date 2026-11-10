cmake_minimum_required(VERSION 3.20)
get_filename_component(LANG_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lang_native_validation LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "Build isolated native helper" FORCE)
  add_subdirectory(${LANG_SOURCE_ROOT} frontend)
  return()
endif()
add_executable(lang_native_ir_test ir_test.cpp)
target_link_libraries(lang_native_ir_test PRIVATE lang_sema lang_native)
add_test(NAME native.ir COMMAND lang_native_ir_test ${LANG_SOURCE_ROOT}/tests/fixtures/core/lang.toml
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/literal-switch/lang.toml)
add_executable(lang_native_aggregate_test aggregate_test.cpp)
target_link_libraries(lang_native_aggregate_test PRIVATE lang_sema lang_native)
add_test(NAME native.aggregates COMMAND lang_native_aggregate_test ${LANG_SOURCE_ROOT})
add_executable(lang_native_indexed_field_probe indexed_field_probe.cpp)
target_link_libraries(lang_native_indexed_field_probe PRIVATE lang_sema lang_native)
add_test(NAME native.indexed_field_ir COMMAND lang_native_indexed_field_probe ${LANG_SOURCE_ROOT})
find_package(LLVM REQUIRED CONFIG)
add_executable(lang_native_llvm_test llvm_test.cpp)
target_include_directories(lang_native_llvm_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_llvm_test PRIVATE -fno-rtti)
llvm_map_components_to_libnames(LANG_NATIVE_TEST_LLVM_LIBS AsmParser Core Support)
target_link_libraries(lang_native_llvm_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_test(NAME native.llvm COMMAND lang_native_llvm_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/float-transfer/lang.toml
  ${LANG_SOURCE_ROOT}/tests/native/pod/empty-and-float/lang.toml)
add_executable(lang_native_emit_exception_test emit_exception_test.cpp)
target_compile_options(lang_native_emit_exception_test PRIVATE -fno-rtti)
target_link_libraries(lang_native_emit_exception_test PRIVATE lang_sema lang_native
  ${LANG_NATIVE_TEST_LLVM_LIBS})
target_link_options(lang_native_emit_exception_test PRIVATE
  -Wl,--wrap=_ZN4lang2ir4dumpB5cxx11ERKNS0_6ModuleE)
add_test(NAME native.emit_exception COMMAND lang_native_emit_exception_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/unit-main/lang.toml)
set_tests_properties(native.emit_exception PROPERTIES TIMEOUT 60)
add_executable(lang_native_string_probe string_probe.cpp)
target_include_directories(lang_native_string_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_string_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_string_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON)
  foreach(fixture IN ITEMS strings string-constants string-aggregates string-vec-aggregates aggregate-constants)
    add_test(NAME native.${fixture} COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_LIST_DIR}/run_strings.py --cli $<TARGET_FILE:langc>
      --probe $<TARGET_FILE:lang_native_string_probe> --root ${LANG_SOURCE_ROOT}
      --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
      --out ${CMAKE_CURRENT_BINARY_DIR}/${fixture}-results --fixture ${fixture})
    set_tests_properties(native.${fixture} PROPERTIES TIMEOUT 180)
  endforeach()
  add_test(NAME native.cli COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_cli.py --cli $<TARGET_FILE:langc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --out ${CMAKE_CURRENT_BINARY_DIR}/cli-results
    --helper $<TARGET_FILE:lang_native_helper>
    --invalid-json-cli $<TARGET_FILE:langc_native_invalid_json_probe>)
  set_tests_properties(native.cli PROPERTIES TIMEOUT 180)
  add_test(NAME native.generics COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_generics.py --cli $<TARGET_FILE:langc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --out ${CMAKE_CURRENT_BINARY_DIR}/generic-results)
  set_tests_properties(native.generics PROPERTIES TIMEOUT 180)
else()
  message(STATUS "native.cli: not_run (test Python not found)")
endif()
add_executable(lang_native_cleanup_probe cleanup_probe.cpp)
target_include_directories(lang_native_cleanup_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_cleanup_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_cleanup_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON)
  add_test(NAME native.cleanup COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_cleanup.py --cli $<TARGET_FILE:langc>
    --probe $<TARGET_FILE:lang_native_cleanup_probe> --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/cleanup-results)
  set_tests_properties(native.cleanup PROPERTIES TIMEOUT 240)
endif()
add_executable(lang_native_float_probe float_probe.cpp)
target_include_directories(lang_native_float_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_float_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_float_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_executable(lang_native_convert_probe convert_probe.cpp)
target_include_directories(lang_native_convert_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_convert_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_convert_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME native.conversions COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/numeric/run_conversions.py
    --cli $<TARGET_FILE:langc> --probe $<TARGET_FILE:lang_native_convert_probe>
    --root ${LANG_SOURCE_ROOT} --python ${LANG_PYTHON312}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/conversion-results)
  set_tests_properties(native.conversions PROPERTIES TIMEOUT 600)
else()
  message(STATUS "native.conversions: not_run (test Python 3.12 not configured)")
endif()
if(LANG_TEST_PYTHON)
  add_test(NAME native.floating COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_floating.py --cli $<TARGET_FILE:langc>
    --probe $<TARGET_FILE:lang_native_float_probe> --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/floating-results)
  set_tests_properties(native.floating PROPERTIES TIMEOUT 240)
endif()
add_executable(lang_native_vec_runtime_test vec_runtime_test.cpp)
target_compile_options(lang_native_vec_runtime_test PRIVATE -fno-builtin)
target_link_libraries(lang_native_vec_runtime_test PRIVATE lang_native_runtime)
target_link_options(lang_native_vec_runtime_test PRIVATE -Wl,--wrap=lang_alloc_v1)
add_test(NAME native.vec_runtime COMMAND lang_native_vec_runtime_test)
set_tests_properties(native.vec_runtime PROPERTIES TIMEOUT 30)
add_executable(lang_native_startup_test startup_test.cpp)
target_compile_features(lang_native_startup_test PRIVATE cxx_std_20)
add_test(NAME native.startup COMMAND lang_native_startup_test)
add_executable(lang_native_alloc_wrap_test alloc_wrap_test.cpp alloc_observer.cpp)
target_compile_features(lang_native_alloc_wrap_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_alloc_wrap_test PRIVATE lang_native_runtime)
target_link_options(lang_native_alloc_wrap_test PRIVATE
  -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.alloc_wrap COMMAND lang_native_alloc_wrap_test)
add_executable(lang_native_ownership_probe ownership_probe.cpp)
target_include_directories(lang_native_ownership_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_ownership_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_ownership_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON)
  add_test(NAME native.ownership COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_ownership.py --cli $<TARGET_FILE:langc>
    --probe $<TARGET_FILE:lang_native_ownership_probe> --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --runtime-test $<TARGET_FILE:lang_native_vec_runtime_test>
    --alloc-selftest $<TARGET_FILE:lang_native_alloc_wrap_test>
    --out ${CMAKE_CURRENT_BINARY_DIR}/ownership-results)
  set_tests_properties(native.ownership PROPERTIES TIMEOUT 240)
endif()
add_executable(lang_native_unreachable_try_test unreachable_try_test.cpp)
# Private test-only helper for the driver's strict invalid-JSON exit-code context.
add_executable(lang_native_exit2_helper fake_exit2_helper.cpp)
add_executable(langc_native_invalid_json_probe ${LANG_SOURCE_ROOT}/src/driver/main.cpp)
target_link_libraries(langc_native_invalid_json_probe PRIVATE
  lang_sema lang_backend_python lang_driver_process json-c::json-c)
target_compile_definitions(langc_native_invalid_json_probe PRIVATE
  LANG_NATIVE_HELPER="$<TARGET_FILE:lang_native_exit2_helper>")
add_dependencies(langc_native_invalid_json_probe lang_native_exit2_helper)
target_include_directories(lang_native_unreachable_try_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_unreachable_try_test PRIVATE -fno-rtti)
target_link_libraries(lang_native_unreachable_try_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON)
  add_test(NAME native.unreachable_try COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_unreachable_try.py --cli $<TARGET_FILE:langc>
    --probe $<TARGET_FILE:lang_native_unreachable_try_test> --root ${LANG_SOURCE_ROOT}
    --python ${LANG_TEST_PYTHON} --cxx ${LLVM_TOOLS_BINARY_DIR}/clang++
    --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/unreachable-try-results)
  set_tests_properties(native.unreachable_try PROPERTIES TIMEOUT 300)
endif()
add_executable(lang_native_vec_scratch_test vec_scratch_test.cpp)
target_include_directories(lang_native_vec_scratch_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_vec_scratch_test PRIVATE -fno-rtti)
target_link_libraries(lang_native_vec_scratch_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_test(NAME native.vec_scratch COMMAND lang_native_vec_scratch_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/vec-scratch/lang.toml)
add_executable(lang_native_vec_constant_test vec_constant_test.cpp)
target_include_directories(lang_native_vec_constant_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_vec_constant_test PRIVATE -fno-rtti)
target_link_libraries(lang_native_vec_constant_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_test(NAME native.vec_constant COMMAND lang_native_vec_constant_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/vec-constants/lang.toml)

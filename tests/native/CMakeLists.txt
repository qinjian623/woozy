cmake_minimum_required(VERSION 3.20)
get_filename_component(LANG_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(lang_native_validation LANGUAGES C CXX)
  include(CTest)
  set(LANG_ENABLE_LLVM ON CACHE BOOL "Build isolated native helper" FORCE)
  add_subdirectory(${LANG_SOURCE_ROOT} frontend)
  return()
endif()
add_executable(lang_os_runtime_test os_runtime_test.cpp
  ${LANG_SOURCE_ROOT}/runtime/native/os.cpp)
target_compile_features(lang_os_runtime_test PRIVATE cxx_std_20)
add_test(NAME native.os_runtime COMMAND lang_os_runtime_test)
set_tests_properties(native.os_runtime PROPERTIES TIMEOUT 30)
add_executable(lang_native_system_ir_test system_ir_test.cpp)
target_link_libraries(lang_native_system_ir_test PRIVATE lang_sema lang_native)
add_test(NAME native.system_ir COMMAND lang_native_system_ir_test)
add_executable(lang_c_ffi_forged_test c_ffi/forged_test.cpp)
target_link_libraries(lang_c_ffi_forged_test PRIVATE lang_sema lang_native_ir)
add_test(NAME native.c_ffi_forged COMMAND lang_c_ffi_forged_test
  ${LANG_SOURCE_ROOT}/tests/native/c_ffi/forged-project/lang.toml)
set_tests_properties(native.c_ffi_forged PROPERTIES TIMEOUT 60)

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
add_executable(lang_slice_forged_test slice_source/forged_test.cpp)
target_link_libraries(lang_slice_forged_test PRIVATE lang_sema lang_native_ir)
add_test(NAME native.slice_forged COMMAND lang_slice_forged_test
  ${LANG_SOURCE_ROOT}/tests/native/slice_source/fixtures/core/lang.toml)
set_tests_properties(native.slice_forged PROPERTIES TIMEOUT 60)
if(LANG_TEST_PYTHON)
  add_test(NAME native.tls_source COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/tls-source-results
    ${LANG_SOURCE_ROOT}/tests/native/tls_source/run_tls_source.py
    --cli $<TARGET_FILE:woozyc> --cxx ${LLVM_TOOLS_BINARY_DIR}/clang++
    --cc ${LLVM_TOOLS_BINARY_DIR}/clang --python ${LANG_TEST_PYTHON}
    --http-regression)
  set_tests_properties(native.tls_source PROPERTIES TIMEOUT 900)
  add_test(NAME native.slice_source COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/native/slice_source/run_slice_source.py
    --cli $<TARGET_FILE:woozyc> --helper $<TARGET_FILE:lang_native_helper>
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld
    --runtime $<TARGET_FILE:lang_native_runtime> --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/slice-source-results)
  set_tests_properties(native.slice_source PROPERTIES TIMEOUT 600)
endif()
if(LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME native.box_source COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/box_source/run_box_source.py
    --cli $<TARGET_FILE:woozyc> --helper $<TARGET_FILE:lang_native_helper>
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld
    --runtime $<TARGET_FILE:lang_native_runtime> --root ${LANG_SOURCE_ROOT}
    --python ${LANG_PYTHON312}
    --python-helper $<TARGET_FILE:lang_python_emit>
    --out ${CMAKE_CURRENT_BINARY_DIR}/box-source-results)
  set_tests_properties(native.box_source PROPERTIES TIMEOUT 600)
endif()
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
  add_test(NAME native.os COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_os.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/os-results)
  set_tests_properties(native.os PROPERTIES TIMEOUT 180)
  foreach(fixture IN ITEMS strings string-constants string-aggregates string-vec-aggregates aggregate-constants)
    add_test(NAME native.${fixture} COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_LIST_DIR}/run_strings.py --cli $<TARGET_FILE:woozyc>
      --probe $<TARGET_FILE:lang_native_string_probe> --root ${LANG_SOURCE_ROOT}
      --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
      --out ${CMAKE_CURRENT_BINARY_DIR}/${fixture}-results --fixture ${fixture})
    set_tests_properties(native.${fixture} PROPERTIES TIMEOUT 180)
  endforeach()
  add_test(NAME native.cli COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_cli.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --out ${CMAKE_CURRENT_BINARY_DIR}/cli-results
    --helper $<TARGET_FILE:lang_native_helper>
    --invalid-json-cli $<TARGET_FILE:langc_native_invalid_json_probe>)
  set_tests_properties(native.cli PROPERTIES TIMEOUT 180)
  add_test(NAME native.generics COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_generics.py --cli $<TARGET_FILE:woozyc>
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
    ${CMAKE_CURRENT_LIST_DIR}/run_cleanup.py --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_cleanup_probe> --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/cleanup-results)
  set_tests_properties(native.cleanup PROPERTIES TIMEOUT 240)
  add_test(NAME native.reference_values_backend COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/reference_values_backend/run_reference_values_backend.py
    --helper $<TARGET_FILE:lang_native_helper> --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/reference-values-backend-results)
  set_tests_properties(native.reference_values_backend PROPERTIES TIMEOUT 300)
  add_test(NAME native.weak_source COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/weak_source/run_weak_source.py
    --cli $<TARGET_FILE:woozyc> --helper $<TARGET_FILE:lang_native_helper>
    --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/weak-source-results)
  set_tests_properties(native.weak_source PROPERTIES TIMEOUT 900)
  add_test(NAME native.language_gc_source COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/language-gc-source-results
    ${CMAKE_CURRENT_LIST_DIR}/language_gc_source/run_language_gc_source.py
    --cli $<TARGET_FILE:woozyc> --helper $<TARGET_FILE:lang_native_helper>
    --runtime $<TARGET_FILE:lang_native_runtime>
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --readelf ${LLVM_TOOLS_BINARY_DIR}/llvm-readelf
    --root ${LANG_SOURCE_ROOT} --out @RUN_OUT@)
  set_tests_properties(native.language_gc_source PROPERTIES TIMEOUT 600)
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
    --cli $<TARGET_FILE:woozyc> --probe $<TARGET_FILE:lang_native_convert_probe>
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
    ${CMAKE_CURRENT_LIST_DIR}/run_floating.py --cli $<TARGET_FILE:woozyc>
    --probe $<TARGET_FILE:lang_native_float_probe> --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --runtime $<TARGET_FILE:lang_native_runtime>
    --out ${CMAKE_CURRENT_BINARY_DIR}/floating-results)
  set_tests_properties(native.floating PROPERTIES TIMEOUT 240)
endif()
add_executable(lang_native_vec_runtime_test vec_runtime_test.cpp)
target_compile_options(lang_native_vec_runtime_test PRIVATE -fno-builtin)
target_link_libraries(lang_native_vec_runtime_test PRIVATE lang_native_runtime)
target_link_options(lang_native_vec_runtime_test PRIVATE
  -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.vec_runtime COMMAND lang_native_vec_runtime_test)
set_tests_properties(native.vec_runtime PROPERTIES TIMEOUT 30)
add_executable(lang_native_weak_objects_test
  weak_objects/weak_objects_test.cpp weak_objects/weak_objects_alloc_observer.cpp)
target_include_directories(lang_native_weak_objects_test PRIVATE
  ${LANG_SOURCE_ROOT}/runtime/native)
target_link_libraries(lang_native_weak_objects_test PRIVATE lang_native_runtime)
target_link_options(lang_native_weak_objects_test PRIVATE
  -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.weak_objects COMMAND lang_native_weak_objects_test)
set_tests_properties(native.weak_objects PROPERTIES TIMEOUT 120)
add_executable(lang_native_weak_objects_hosted_test
  weak_objects/weak_objects_test.cpp weak_objects/weak_objects_alloc_observer.cpp)
target_include_directories(lang_native_weak_objects_hosted_test PRIVATE
  ${LANG_SOURCE_ROOT}/runtime/native)
target_link_libraries(lang_native_weak_objects_hosted_test PRIVATE lang_hosted_runtime)
target_link_options(lang_native_weak_objects_hosted_test PRIVATE
  -pthread -Wl,-u,lang_hosted_startup_v1
  -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.weak_objects_hosted COMMAND lang_native_weak_objects_hosted_test)
set_tests_properties(native.weak_objects_hosted PROPERTIES TIMEOUT 120)
add_test(NAME native.gc_program
  COMMAND ${CMAKE_COMMAND} -E env
    "CXX=${LLVM_TOOLS_BINARY_DIR}/clang++"
    "LANG_LLVM_ROOT=${LLVM_TOOLS_BINARY_DIR}/.."
    "GC_PROGRAM_BUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/gc-program-results"
    bash ${CMAKE_CURRENT_LIST_DIR}/gc_program/run_all.sh)
set_tests_properties(native.gc_program PROPERTIES TIMEOUT 600)
add_test(NAME native.gc_cycle
  COMMAND ${CMAKE_COMMAND} -E env
    "CXX=${LLVM_TOOLS_BINARY_DIR}/clang++"
    "LANG_LLVM_ROOT=${LLVM_TOOLS_BINARY_DIR}/.."
    "CYCLE_GC_OUT=${CMAKE_CURRENT_BINARY_DIR}/cycle-gc-results"
    bash ${CMAKE_CURRENT_LIST_DIR}/cycle_gc/run_tests.sh)
set_tests_properties(native.gc_cycle PROPERTIES TIMEOUT 900)
add_executable(lang_native_startup_test startup_test.cpp)
target_compile_features(lang_native_startup_test PRIVATE cxx_std_20)
add_test(NAME native.startup COMMAND lang_native_startup_test)
add_executable(lang_native_alloc_wrap_test alloc_wrap_test.cpp alloc_observer.cpp)
target_compile_features(lang_native_alloc_wrap_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_alloc_wrap_test PRIVATE lang_native_runtime)
target_link_options(lang_native_alloc_wrap_test PRIVATE
  -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.alloc_wrap COMMAND lang_native_alloc_wrap_test)
add_executable(lang_native_alloc_reuse_test alloc_reuse_test.cpp)
target_compile_features(lang_native_alloc_reuse_test PRIVATE cxx_std_20)
target_compile_options(lang_native_alloc_reuse_test PRIVATE -fno-builtin)
target_link_libraries(lang_native_alloc_reuse_test PRIVATE lang_native_runtime)
add_test(NAME native.alloc_reuse COMMAND lang_native_alloc_reuse_test)
set_tests_properties(native.alloc_reuse PROPERTIES TIMEOUT 30)
add_test(NAME native.alloc_reuse_deny_mmap COMMAND lang_native_alloc_reuse_test reuse-deny-mmap)
set_tests_properties(native.alloc_reuse_deny_mmap PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
if(LANG_TEST_PYTHON)
  add_test(NAME native.trivial_vec COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_trivial_vec.py --cli $<TARGET_FILE:woozyc>
    --helper $<TARGET_FILE:lang_native_helper>
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --runtime $<TARGET_FILE:lang_native_runtime>
    --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/trivial-vec-results)
  set_tests_properties(native.trivial_vec PROPERTIES TIMEOUT 240)
else()
  message(STATUS "native.trivial_vec: not_run (test Python not found)")
endif()
add_executable(lang_native_ownership_probe ownership_probe.cpp)
target_include_directories(lang_native_ownership_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_ownership_probe PRIVATE -fno-rtti)
target_link_libraries(lang_native_ownership_probe PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
if(LANG_TEST_PYTHON)
  add_test(NAME native.ownership COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_ownership.py --cli $<TARGET_FILE:woozyc>
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
    ${CMAKE_CURRENT_LIST_DIR}/run_unreachable_try.py --cli $<TARGET_FILE:woozyc>
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
add_executable(lang_native_vec_negative_constant_test vec_negative_constant_test.cpp)
target_include_directories(lang_native_vec_negative_constant_test SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_options(lang_native_vec_negative_constant_test PRIVATE -fno-rtti)
target_link_libraries(lang_native_vec_negative_constant_test PRIVATE lang_sema lang_native ${LANG_NATIVE_TEST_LLVM_LIBS})
add_test(NAME native.vec_negative_constant COMMAND lang_native_vec_negative_constant_test
  ${LANG_SOURCE_ROOT}/tests/native/fixtures/vec-negative-constants/lang.toml)
if(LANG_TEST_PYTHON)
  add_test(NAME native.vec_negative_runtime COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_vec_negative.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/vec-negative-results)
  set_tests_properties(native.vec_negative_runtime PROPERTIES TIMEOUT 180)
else()
  message(STATUS "native.vec_negative_runtime: not_run (test Python not found)")
endif()

find_package(Threads REQUIRED)
foreach(part IN ITEMS arc alloc)
  add_executable(lang_native_${part}_concurrency_test ${part}_concurrency_test.cpp)
  target_compile_features(lang_native_${part}_concurrency_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_${part}_concurrency_test PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.${part}_concurrency COMMAND lang_native_${part}_concurrency_test)
  set_tests_properties(native.${part}_concurrency PROPERTIES TIMEOUT 60)
endforeach()
foreach(part IN ITEMS sync thread)
  add_executable(lang_native_${part}_runtime_test ${part}_runtime_test.cpp)
  target_compile_features(lang_native_${part}_runtime_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_${part}_runtime_test PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.${part}_runtime COMMAND lang_native_${part}_runtime_test)
  set_tests_properties(native.${part}_runtime PROPERTIES TIMEOUT 60)
endforeach()

if(LANG_TEST_PYTHON)
  add_test(NAME native.c_ffi COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/c_ffi/run_c_ffi.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/c-ffi-results
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --ar ${LLVM_TOOLS_BINARY_DIR}/llvm-ar
    --hosted-runtime $<TARGET_FILE:lang_hosted_runtime>
    --native-runtime $<TARGET_FILE:lang_native_runtime>
    --forged $<TARGET_FILE:lang_c_ffi_forged_test>)
  set_tests_properties(native.c_ffi PROPERTIES TIMEOUT 600)

  add_test(NAME native.concurrency COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_concurrency.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/concurrency-results)
  set_tests_properties(native.concurrency PROPERTIES TIMEOUT 240)
endif()

target_compile_options(lang_native_thread_runtime_test PRIVATE -fstack-protector-strong)

target_link_options(lang_native_sync_runtime_test PRIVATE -Wl,--wrap=lang_free_v1)

add_executable(lang_native_vec_growth_test vec_growth_test.cpp)
target_compile_features(lang_native_vec_growth_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_vec_growth_test PRIVATE lang_native_runtime)
target_link_options(lang_native_vec_growth_test PRIVATE -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.vec_growth COMMAND lang_native_vec_growth_test)
set_tests_properties(native.vec_growth PROPERTIES TIMEOUT 60)
foreach(part IN ITEMS text system)
  add_executable(lang_native_${part}_runtime_test ${part}_runtime_test.cpp)
  target_compile_features(lang_native_${part}_runtime_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_${part}_runtime_test PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.${part}_runtime COMMAND lang_native_${part}_runtime_test)
  set_tests_properties(native.${part}_runtime PROPERTIES TIMEOUT 60)
endforeach()
add_executable(lang_native_system_buffers_test system_buffers_test.cpp)
target_compile_features(lang_native_system_buffers_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_system_buffers_test PRIVATE lang_native_runtime)
add_test(NAME native.system_buffers COMMAND lang_native_system_buffers_test)

add_executable(lang_native_value_library_test value_library_test.cpp)
target_link_libraries(lang_native_value_library_test PRIVATE lang_sema lang_native_ir)
add_test(NAME native.value_library_ir COMMAND lang_native_value_library_test ${LANG_SOURCE_ROOT})
if(LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME native.value_library COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_value_library.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --runtime $<TARGET_FILE:lang_native_runtime>
    --python ${LANG_PYTHON312} --out ${CMAKE_CURRENT_BINARY_DIR}/value-library-results)
  set_tests_properties(native.value_library PROPERTIES TIMEOUT 240)
endif()
foreach(part IN ITEMS network numbers)
  add_executable(lang_native_${part}_runtime_test ${part}_runtime_test.cpp)
  target_compile_features(lang_native_${part}_runtime_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_${part}_runtime_test PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.${part}_runtime COMMAND lang_native_${part}_runtime_test)
  set_tests_properties(native.${part}_runtime PROPERTIES TIMEOUT 60)
endforeach()
target_link_options(lang_native_network_runtime_test PRIVATE -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)

add_executable(lang_native_math_runtime_test math_runtime_test.cpp)
target_compile_features(lang_native_math_runtime_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_math_runtime_test PRIVATE lang_native_runtime m)
add_test(NAME native.math_runtime COMMAND lang_native_math_runtime_test)
set_tests_properties(native.math_runtime PROPERTIES TIMEOUT 60)

if(LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME native.stdlib COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_stdlib.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --clang ${LLVM_TOOLS_BINARY_DIR}/clang
    --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld --runtime $<TARGET_FILE:lang_native_runtime>
    --python ${LANG_PYTHON312} --role reference --helper $<TARGET_FILE:lang_native_helper>
    --out ${CMAKE_CURRENT_BINARY_DIR}/stdlib-results)
  set_tests_properties(native.stdlib PROPERTIES TIMEOUT 300)
endif()

add_executable(lang_native_dns_runtime_test dns_runtime_test.cpp)
target_compile_features(lang_native_dns_runtime_test PRIVATE cxx_std_20)
target_link_libraries(lang_native_dns_runtime_test PRIVATE lang_native_runtime)
target_link_options(lang_native_dns_runtime_test PRIVATE -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
add_test(NAME native.dns_runtime COMMAND lang_native_dns_runtime_test)
set_tests_properties(native.dns_runtime PROPERTIES TIMEOUT 60)

if(LANG_TEST_PYTHON)
  add_test(NAME native.runtime_symbols COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/check_runtime_symbols.py
    --archive $<TARGET_FILE:lang_native_runtime> --nm ${CMAKE_NM})
endif()

foreach(part IN ITEMS process ipc)
  add_executable(lang_native_${part}_runtime_test ${part}_runtime_test.cpp)
  target_compile_features(lang_native_${part}_runtime_test PRIVATE cxx_std_20)
  target_link_libraries(lang_native_${part}_runtime_test PRIVATE lang_native_runtime Threads::Threads)
  add_test(NAME native.${part}_runtime COMMAND lang_native_${part}_runtime_test)
  set_tests_properties(native.${part}_runtime PROPERTIES TIMEOUT 90)
endforeach()

if(LANG_TEST_PYTHON)
  add_test(NAME native.process COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/process/run_process.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/process-results)
  set_tests_properties(native.process PROPERTIES TIMEOUT 300)
endif()

if(LANG_TEST_PYTHON)
  add_test(NAME native.stdlib_io_errors COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/stdlib-io-errors/run_io_errors.py
    --cli $<TARGET_FILE:woozyc> --root ${LANG_SOURCE_ROOT}
    --out ${CMAKE_CURRENT_BINARY_DIR}/stdlib-io-errors-results)
  set_tests_properties(native.stdlib_io_errors PROPERTIES TIMEOUT 180)
endif()

if(LANG_TEST_PYTHON AND LANG_PYTHON312)
  add_test(NAME native.collections COMMAND ${LANG_TEST_PYTHON}
    ${CMAKE_CURRENT_LIST_DIR}/run_collections.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --python ${LANG_PYTHON312}
    --out ${CMAKE_CURRENT_BINARY_DIR}/collections-results)
  set_tests_properties(native.collections PROPERTIES TIMEOUT 600)
endif()

if(LANG_PYTHON312)
  add_test(NAME python.recursive_json COMMAND ${LANG_PYTHON312}
    ${LANG_SOURCE_ROOT}/tests/python/recursive_json_test.py --cli $<TARGET_FILE:woozyc>
    --python ${LANG_PYTHON312} --out ${CMAKE_CURRENT_BINARY_DIR}/recursive-json-results)
  set_tests_properties(python.recursive_json PROPERTIES TIMEOUT 300)
  add_test(NAME generics.map_budget COMMAND ${LANG_PYTHON312}
    ${LANG_SOURCE_ROOT}/tests/generics/map_budget/run.py --cli $<TARGET_FILE:woozyc>
    --python ${LANG_PYTHON312} --out ${CMAKE_CURRENT_BINARY_DIR}/map-budget-results --native)
  set_tests_properties(generics.map_budget PROPERTIES TIMEOUT 300)
endif()

if(LANG_TEST_PYTHON)
  add_test(NAME native.chat COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/native/run_chat.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/chat-results
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --lld ${LLVM_TOOLS_BINARY_DIR}/ld.lld
    --runtime $<TARGET_FILE:lang_native_runtime>)
  set_tests_properties(native.chat PROPERTIES TIMEOUT 300)
  add_test(NAME performance.plb_output_checks COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/performance/run_plb.py selftest)
  set_tests_properties(performance.plb_output_checks PROPERTIES TIMEOUT 30)
  add_test(NAME native.timezone COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/timezone-results
    ${LANG_SOURCE_ROOT}/tests/native/timezone/run.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT})
  set_tests_properties(native.timezone PROPERTIES TIMEOUT 600)
  add_test(NAME native.channel COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/native/channels/run_channels.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --out ${CMAKE_CURRENT_BINARY_DIR}/channel-results)
  set_tests_properties(native.channel PROPERTIES TIMEOUT 300)
  add_test(NAME native.channel_waits COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/channel-wait-results
    ${LANG_SOURCE_ROOT}/tests/native/channel-waits/run.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT})
  set_tests_properties(native.channel_waits PROPERTIES TIMEOUT 300)
  add_test(NAME native.threadpool COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/threadpool-results
    ${LANG_SOURCE_ROOT}/tests/native/threadpool/run_threadpool.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT})
  set_tests_properties(native.threadpool PROPERTIES TIMEOUT 300)
  add_test(NAME native.concurrency_timeouts COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/concurrency-timeout-results
    ${LANG_SOURCE_ROOT}/tests/native/concurrency-timeouts/run.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT})
  set_tests_properties(native.concurrency_timeouts PROPERTIES TIMEOUT 180)
  add_test(NAME native.unicode_runtime COMMAND ${LANG_TEST_PYTHON}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/unicode-runtime-results
    ${LANG_SOURCE_ROOT}/tests/native/unicode/run.py --root ${LANG_SOURCE_ROOT}
    --clang ${LLVM_TOOLS_BINARY_DIR}/clang --clangxx ${LLVM_TOOLS_BINARY_DIR}/clang++
    --ar ${LLVM_TOOLS_BINARY_DIR}/llvm-ar)
  set_tests_properties(native.unicode_runtime PROPERTIES TIMEOUT 300)
endif()

if(LANG_PYTHON312)
  add_test(NAME python.unicode18 COMMAND ${LANG_PYTHON312}
    ${LANG_SOURCE_ROOT}/tests/python/unicode18_test.py)
  set_tests_properties(python.unicode18 PROPERTIES TIMEOUT 180)
  add_test(NAME values.unicode_source COMMAND ${LANG_PYTHON312}
    ${LANG_SOURCE_ROOT}/tests/run_isolated.py ${CMAKE_CURRENT_BINARY_DIR}/unicode-source-results
    ${LANG_SOURCE_ROOT}/tests/native/unicode/run_source.py --cli $<TARGET_FILE:woozyc>
    --root ${LANG_SOURCE_ROOT} --python ${LANG_PYTHON312})
  set_tests_properties(values.unicode_source PROPERTIES TIMEOUT 300)
endif()

add_subdirectory(gc_activation)
add_subdirectory(c_retained_static)

add_subdirectory(task_frame_gc)

add_subdirectory(async_memory)

add_subdirectory(hosted_thread_gc)

add_subdirectory(async_fork)

add_subdirectory(c_callback_runtime)
add_subdirectory(c_retained_capturing)

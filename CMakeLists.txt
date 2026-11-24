cmake_minimum_required(VERSION 3.20)
project(lang VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
option(LANG_ENABLE_LLVM "Build the isolated LLVM native helper and integration probe" OFF)
add_library(lang_frontend STATIC
  src/support/digest.cpp
  src/support/file_digest.cpp
  src/support/version.cpp
  src/support/source.cpp
  src/support/diagnostic.cpp
  src/support/input.cpp
  src/syntax/lexer.cpp
  src/syntax/parser.cpp
  src/syntax/format.cpp)
target_include_directories(lang_frontend PUBLIC include)
target_compile_features(lang_frontend PUBLIC cxx_std_20)
add_library(lang_driver_process STATIC src/support/process.cpp src/support/publish.cpp
  src/support/resource.cpp)
target_include_directories(lang_driver_process PUBLIC include)
target_compile_features(lang_driver_process PUBLIC cxx_std_20)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(json-c CONFIG REQUIRED)
add_library(lang_modules STATIC src/modules/resolve.cpp src/modules/project.cpp src/modules/parse_cache.cpp)
target_link_libraries(lang_modules PUBLIC lang_frontend
  PRIVATE tomlplusplus::tomlplusplus json-c::json-c lang_driver_process)
# Retained for build-tree introspection; sources resolve resources relative to
# the executable so these paths are not embedded in compiler binaries.
target_compile_definitions(lang_modules PRIVATE
  LANG_STD_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/stdlib")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
    "${CMAKE_BINARY_DIR}/stdlib" SYMBOLIC RESULT lang_stdlib_link)
  if(lang_stdlib_link)
    message(FATAL_ERROR "cannot create build-tree stdlib resource: ${lang_stdlib_link}")
  endif()
endif()
add_library(lang_hir STATIC src/hir/hir.cpp src/hir/system_operation.cpp src/hir/ownership.cpp
  src/hir/pattern_analysis.cpp src/hir/literal_value.cpp src/hir/constant_value.cpp src/hir/identity.cpp)
target_link_libraries(lang_hir PUBLIC lang_modules)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GMPXX REQUIRED IMPORTED_TARGET gmpxx>=6.3.0)
target_link_libraries(lang_hir PUBLIC PkgConfig::GMPXX)
add_library(lang_sema STATIC src/sema/check.cpp src/sema/front_cache.cpp)
target_link_libraries(lang_sema PUBLIC lang_hir PRIVATE PkgConfig::GMPXX)
add_subdirectory(src/backend/python)
add_library(lang_editor STATIC src/driver/lsp.cpp)
target_link_libraries(lang_editor PUBLIC lang_frontend PRIVATE json-c::json-c)
add_executable(woozyc src/driver/main.cpp)
target_link_libraries(woozyc PRIVATE lang_sema lang_backend_python lang_driver_process lang_editor json-c::json-c)
if(LANG_ENABLE_LLVM)
  include(cmake/LLVM.cmake)
  add_subdirectory(src/backend/native "${CMAKE_BINARY_DIR}/native")
  target_compile_definitions(woozyc PRIVATE LANG_NATIVE_HELPER="$<TARGET_FILE:lang_native_helper>")
  add_dependencies(woozyc lang_native_helper)
endif()
include(CTest)
if(BUILD_TESTING)
  find_program(LANG_TEST_PYTHON NAMES python3.12 python3)
  if(LANG_TEST_PYTHON)
    add_test(NAME tooling.editor_cli COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/editor_cli_test.py
      --cli $<TARGET_FILE:woozyc>)
    set_tests_properties(tooling.editor_cli PROPERTIES TIMEOUT 60)
    add_test(NAME tooling.release_archive COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/release_archive_test.py)
    add_test(NAME tooling.release_installer COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/release_installer_test.py)
    add_test(NAME tooling.editor_setup COMMAND ${LANG_TEST_PYTHON} -B
      ${CMAKE_CURRENT_SOURCE_DIR}/development/editor/test_setup.py)
  endif()
  add_executable(lang_file_digest_probe tests/incremental/file_digest_probe.cpp)
  target_link_libraries(lang_file_digest_probe PRIVATE lang_frontend lang_driver_process)
  if(LANG_TEST_PYTHON)
    add_test(NAME support.file_digest COMMAND ${LANG_TEST_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py
      ${CMAKE_BINARY_DIR}/file-digest-results -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/incremental/file_digest_test.py
      --cpp $<TARGET_FILE:lang_file_digest_probe>)
  endif()
  add_executable(lang_identity_test tests/incremental/identity_test.cpp)
  target_link_libraries(lang_identity_test PRIVATE lang_hir)
  add_test(NAME modules.identity COMMAND lang_identity_test)
  add_executable(lang_declaration_key_test tests/modules/declaration_key_test.cpp)
  target_link_libraries(lang_declaration_key_test PRIVATE lang_frontend)
  target_compile_options(lang_declaration_key_test PRIVATE -UNDEBUG)
  add_test(NAME modules.declaration_key COMMAND lang_declaration_key_test)
  add_executable(lang_publish_test tests/incremental/publish_test.cpp)
  target_link_libraries(lang_publish_test PRIVATE lang_driver_process)
  add_test(NAME support.publish COMMAND lang_publish_test ${CMAKE_BINARY_DIR}/publish-test-results)
  set_tests_properties(support.publish PROPERTIES TIMEOUT 60)
  add_executable(lang_process_test tests/support/process_test.cpp)
  target_link_libraries(lang_process_test PRIVATE lang_driver_process)
  add_test(NAME support.process COMMAND lang_process_test)
  if(LANG_ENABLE_LLVM)
    if(LANG_TEST_PYTHON)
    get_filename_component(LANG_DEFAULT_XML "${LLVM_TOOLS_BINARY_DIR}/../../libxml2-legacy-2.13.9/usr/lib" ABSOLUTE)
    set(LANG_TEST_LIBXML_DIR "${LANG_DEFAULT_XML}" CACHE PATH "Legacy libxml directory for fixed LLVM tests")
    add_test(NAME native.package_incremental_scalar
      COMMAND ${LANG_TEST_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py
        ${CMAKE_BINARY_DIR}/package-incremental-results
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/incremental/package_objects_test.py
        --cli $<TARGET_FILE:woozyc>
        --libxml ${LANG_TEST_LIBXML_DIR})
    add_test(NAME native.package_incremental_fixtures
      COMMAND ${LANG_TEST_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py
        ${CMAKE_BINARY_DIR}/package-fixture-results
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/incremental/fixture_cache_test.py
        --cli $<TARGET_FILE:woozyc> --libxml ${LANG_TEST_LIBXML_DIR}
        --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/incremental/fixtures)
    endif()
    add_subdirectory(tests/native)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      add_subdirectory(tests/stdlib_fs_at)
      add_subdirectory(tests/stdlib_fs_copy)
      add_subdirectory(tests/stdlib_fs_remove)
    endif()
    if(TARGET langc_native_invalid_json_probe AND TARGET lang_native_exit2_helper)
      add_custom_target(lang_invalid_json_helper_link
        COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_FILE_DIR:langc_native_invalid_json_probe>/native"
        COMMAND ${CMAKE_COMMAND} -E create_symlink
          "$<TARGET_FILE:lang_native_exit2_helper>"
          "$<TARGET_FILE_DIR:langc_native_invalid_json_probe>/native/lang_native_helper"
        DEPENDS lang_native_exit2_helper)
      add_dependencies(langc_native_invalid_json_probe lang_invalid_json_helper_link)
    endif()
    add_subdirectory(tests/selfhost)
  endif()
  add_subdirectory(tests/python)
  function(lang_add_stdlib_fixture suite project_dir python_backend)
    set(stdlib_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/${project_dir}/lang.toml")
    set(stdlib_runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/stdlib_project_test.py")
    if(python_backend AND LANG_PYTHON312)
      add_test(NAME stdlib.${suite}.python COMMAND "${LANG_PYTHON312}" -B
        "${stdlib_runner}" $<TARGET_FILE:woozyc> "${stdlib_manifest}" python)
      set_tests_properties(stdlib.${suite}.python PROPERTIES TIMEOUT 240)
    endif()
    if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON)
      foreach(mode IN ITEMS native-o0 native-o2)
        add_test(NAME stdlib.${suite}.${mode} COMMAND "${LANG_TEST_PYTHON}" -B
          "${stdlib_runner}" $<TARGET_FILE:woozyc> "${stdlib_manifest}" "${mode}"
          --libxml "${LANG_TEST_LIBXML_DIR}")
        set_tests_properties(stdlib.${suite}.${mode} PROPERTIES TIMEOUT 240)
      endforeach()
    endif()
  endfunction()
  foreach(suite IN ITEMS algo containers csv sort text text_helpers regex regex_extended strconv unicode_case vec_sort_helpers)
    lang_add_stdlib_fixture(${suite} stdlib_${suite} TRUE)
  endforeach()
  lang_add_stdlib_fixture(csv_io stdlib_csv_io FALSE)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    lang_add_stdlib_fixture(csv_io_failure stdlib_csv_io/linux_failure FALSE)
  endif()
  lang_add_stdlib_fixture(fs_atomic stdlib_fs_atomic FALSE)
  lang_add_stdlib_fixture(http_stream_codec stdlib_http_stream_codec FALSE)
  lang_add_stdlib_fixture(json stdlib_json_helpers/pure TRUE)
  lang_add_stdlib_fixture(json_io stdlib_json_helpers/native FALSE)
  lang_add_stdlib_fixture(unicode_time_linear stdlib_unicode_time_linear FALSE)
  if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON)
    add_test(NAME stdlib.fs_helpers.native
      COMMAND "${LANG_TEST_PYTHON}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_fs_helpers/run.py"
        --compiler $<TARGET_FILE:woozyc> --libxml "${LANG_TEST_LIBXML_DIR}")
    set_tests_properties(stdlib.fs_helpers.native PROPERTIES TIMEOUT 480)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME stdlib.task_group.native
        COMMAND "${LANG_TEST_PYTHON}" -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py"
          "${CMAKE_BINARY_DIR}/task-group-results" -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_task_group/run_tests.py"
          --compiler $<TARGET_FILE:woozyc> --libxml "${LANG_TEST_LIBXML_DIR}")
      set_tests_properties(stdlib.task_group.native PROPERTIES TIMEOUT 480)
    endif()
    foreach(opt IN ITEMS 0 2)
      add_test(NAME stdlib.http_async_handler.native-o${opt}
        COMMAND "${LANG_TEST_PYTHON}" -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py"
          "${CMAKE_BINARY_DIR}/http-async-handler-o${opt}-results" -B
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_http_async_handler/run_async_handler.py"
          --cli $<TARGET_FILE:woozyc> --only-opt ${opt} --libxml "${LANG_TEST_LIBXML_DIR}")
      set_tests_properties(stdlib.http_async_handler.native-o${opt} PROPERTIES TIMEOUT 690)
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME stdlib.http_server_stream.native-o${opt}
          COMMAND "${LANG_TEST_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_isolated.py"
            "${CMAKE_BINARY_DIR}/http-server-stream-o${opt}-results" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_http_server_stream/run_stream_server.py"
            --cli $<TARGET_FILE:woozyc> --only-opt ${opt} --libxml "${LANG_TEST_LIBXML_DIR}")
        set_tests_properties(stdlib.http_server_stream.native-o${opt} PROPERTIES TIMEOUT 690)
      endif()
    endforeach()
  endif()
  function(lang_add_stdlib_panic_fixture suite project_dir reason)
    if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON)
      foreach(opt IN ITEMS 0 2)
        add_test(NAME stdlib.${suite}.native-o${opt}
          COMMAND "${LANG_TEST_PYTHON}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/stdlib_project_test.py"
            $<TARGET_FILE:woozyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/${project_dir}/lang.toml"
            native-o${opt} --libxml "${LANG_TEST_LIBXML_DIR}" --panic ${reason})
        set_tests_properties(stdlib.${suite}.native-o${opt} PROPERTIES TIMEOUT 240)
      endforeach()
    endif()
  endfunction()
  lang_add_stdlib_panic_fixture(deque_overflow stdlib_containers/oversized_reserve allocation-size-overflow)
  lang_add_stdlib_panic_fixture(map.contains_reentry stdlib_containers/panic-map-contains-reentry vec-index)
  if(LANG_PYTHON312)
    add_test(NAME stdlib.map.contains_reentry.python COMMAND "${LANG_PYTHON312}" -B
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/stdlib_project_test.py"
      $<TARGET_FILE:woozyc> "${CMAKE_CURRENT_SOURCE_DIR}/tests/stdlib_containers/panic-map-contains-reentry/lang.toml"
      python --expect-panic "LANG_PANIC: Vec index out of bounds")
    set_tests_properties(stdlib.map.contains_reentry.python PROPERTIES TIMEOUT 240)
  endif()
  lang_add_stdlib_panic_fixture(heap.compare_alias_push stdlib_containers/panic-reentrant-push active-iteration)
  lang_add_stdlib_panic_fixture(sort.compare_alias_push stdlib_sort/fixtures/panic-compare-alias-push active-iteration)
  lang_add_stdlib_panic_fixture(sort.key_alias_push stdlib_sort/fixtures/panic-key-alias-push active-iteration)
  lang_add_stdlib_panic_fixture(sort.key_compare_alias_push stdlib_sort/fixtures/panic-key-compare-alias-push active-iteration)
  lang_add_stdlib_panic_fixture(sort.late_merge stdlib_sort/fixtures/panic-late-merge active-iteration)
  foreach(fixture IN ITEMS find-index-push min-by-push max-by-push min-max-by-push filter-clear filter-reserve filter-push fold-push fold-resize map-push map-resize)
    lang_add_stdlib_panic_fixture(algo.${fixture} stdlib_algo/panic/${fixture} active-iteration)
  endforeach()
  foreach(fixture IN ITEMS bounds-insert bounds-remove-at bounds-swap-remove)
    lang_add_stdlib_panic_fixture(vec.${fixture} stdlib_vec_sort_helpers/fixtures/${fixture} vec-index)
  endforeach()
  foreach(fixture IN ITEMS pin-insert pin-remove-at pin-swap-remove pin-extend)
    lang_add_stdlib_panic_fixture(vec.${fixture} stdlib_vec_sort_helpers/fixtures/${fixture} active-iteration)
  endforeach()
  foreach(path IN ITEMS stdlib_algo/panic-pin stdlib_sort/panic-pin
      stdlib_containers/panic-reentrant-push stdlib_containers/panic-pop-pinned)
    string(REPLACE "/" "." suite "${path}")
    set(manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/${path}/lang.toml")
    set(runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/stdlib_project_test.py")
    if(LANG_PYTHON312)
      add_test(NAME ${suite}.python COMMAND "${LANG_PYTHON312}" -B
        "${runner}" $<TARGET_FILE:woozyc> "${manifest}" python
        --expect-panic "Vec length changes during iteration")
    endif()
    if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON AND NOT path STREQUAL "stdlib_containers/panic-reentrant-push")
      foreach(mode IN ITEMS native-o0 native-o2)
        add_test(NAME ${suite}.${mode} COMMAND "${LANG_TEST_PYTHON}" -B
          "${runner}" $<TARGET_FILE:woozyc> "${manifest}" "${mode}"
          --libxml "${LANG_TEST_LIBXML_DIR}" --expect-panic active-iteration)
      endforeach()
    endif()
  endforeach()
  add_subdirectory(tests/generics)
  add_subdirectory(tests/trait_objects)
  add_subdirectory(tests/async_runtime)
  if(LANG_ENABLE_LLVM)
    add_subdirectory(tests/async)
    add_subdirectory(tests/async_parameter_copy)
    add_subdirectory(tests/iterator)
  endif()
  add_subdirectory(tests/cleanup)
  add_subdirectory(tests/ownership)
  add_subdirectory(tests/bindings)
  if(LANG_ENABLE_LLVM)
    add_subdirectory(tests/inout_places)
    add_subdirectory(tests/owning_inout)
  endif()
  add_executable(lang_concurrency_frontend_test tests/sema/concurrency_test.cpp
    src/ir/lower.cpp src/ir/verify.cpp)
  target_link_libraries(lang_concurrency_frontend_test PRIVATE lang_sema)
  add_test(NAME sema.concurrency COMMAND lang_concurrency_frontend_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(lang_value_library_test tests/sema/value_library_test.cpp)
  target_link_libraries(lang_value_library_test PRIVATE lang_sema)
  add_test(NAME sema.value_library COMMAND lang_value_library_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(lang_system_frontend_test tests/sema/system_test.cpp)
  target_link_libraries(lang_system_frontend_test PRIVATE lang_sema)
  add_test(NAME sema.system COMMAND lang_system_frontend_test)
  add_executable(lang_check_test tests/sema/check_test.cpp)
  target_link_libraries(lang_check_test PRIVATE lang_sema json-c::json-c)
  add_test(NAME sema.scalar COMMAND lang_check_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME sema.cli COMMAND ${CMAKE_COMMAND}
    -DLANGC=$<TARGET_FILE:woozyc> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sema/cli.cmake)
  add_executable(lang_hir_type_test tests/hir/type_test.cpp)
  target_link_libraries(lang_hir_type_test PRIVATE lang_hir)
  add_test(NAME hir.types COMMAND lang_hir_type_test)
  add_executable(lang_hir_test tests/hir/hir_test.cpp)
  target_link_libraries(lang_hir_test PRIVATE lang_hir)
  add_test(NAME hir.scalar COMMAND lang_hir_test)
  # First source Weak frontend stage. Real .wz fixtures also lower verified HIR
  # to NativeIR to reject forged operation metadata. Runtime emission is owned
  # by the native/python backends.
  add_executable(lang_reference_frontend_test
    tests/reference_frontend/reference_frontend_test.cpp
    src/ir/lower.cpp src/ir/verify.cpp)
  target_link_libraries(lang_reference_frontend_test PRIVATE lang_sema)
  add_test(NAME reference_frontend.weak
    COMMAND lang_reference_frontend_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(lang_reference_closure_frontend_test
    tests/reference_closure_frontend/reference_closure_frontend_test.cpp
    src/ir/lower.cpp src/ir/verify.cpp)
  target_link_libraries(lang_reference_closure_frontend_test PRIVATE lang_sema)
  add_test(NAME reference_closure_frontend
    COMMAND lang_reference_closure_frontend_test ${CMAKE_CURRENT_SOURCE_DIR})
  add_executable(lang_hir_reference_values_schema_test
    tests/hir/reference_values_schema/reference_values_schema_test.cpp)
  target_link_libraries(lang_hir_reference_values_schema_test PRIVATE lang_sema)
  add_test(NAME hir.reference_values_schema COMMAND lang_hir_reference_values_schema_test)
  add_executable(lang_module_check tests/modules/module_check.cpp)
  target_link_libraries(lang_module_check PRIVATE lang_modules)
  add_executable(lang_project_test tests/modules/project_test.cpp)
  target_link_libraries(lang_project_test PRIVATE lang_modules)
  add_test(NAME modules.project COMMAND lang_project_test)
  add_executable(lang_resolve_test tests/modules/resolve_test.cpp)
  target_link_libraries(lang_resolve_test PRIVATE lang_modules)
  add_test(NAME modules.names COMMAND lang_resolve_test)
  find_program(LANG_TEST_PYTHON NAMES python3.12 python3)
  if(LANG_TEST_PYTHON)
    add_test(NAME modules.cases COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/modules/run_cases.py $<TARGET_FILE:lang_module_check>)
    add_test(NAME syntax.cases COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/syntax/run_cases.py $<TARGET_FILE:woozyc>)
    add_test(NAME syntax.fixture_guard COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/syntax/test_run_cases.py)
  else()
    message(STATUS "syntax.cases and fixture_guard: not_run (test Python not found)")
  endif()
  add_executable(lang_lexer_test tests/syntax/lexer_test.cpp)
  target_link_libraries(lang_lexer_test PRIVATE lang_frontend)
  add_test(NAME syntax.lexer COMMAND lang_lexer_test)
  add_executable(lang_parser_test tests/syntax/parser_test.cpp)
  target_link_libraries(lang_parser_test PRIVATE lang_frontend)
  add_test(NAME syntax.parser COMMAND lang_parser_test)
  find_package(Threads REQUIRED)
  add_executable(lang_phase_test tests/support/phase_test.cpp)
  target_link_libraries(lang_phase_test PRIVATE lang_frontend Threads::Threads)
  add_test(NAME support.phase COMMAND lang_phase_test)
  add_executable(lang_support_test tests/support/support_test.cpp)
  target_link_libraries(lang_support_test PRIVATE lang_frontend)
  add_test(NAME support.core COMMAND lang_support_test)
  add_test(NAME bootstrap.cli COMMAND ${CMAKE_COMMAND}
    -DLANGC=$<TARGET_FILE:woozyc> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap/cli.cmake)
endif()

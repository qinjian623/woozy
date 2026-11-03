cmake_minimum_required(VERSION 3.20)
project(lang VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
option(LANG_ENABLE_LLVM "Build LLVM integration probe (no code generation)" OFF)
add_library(lang_frontend STATIC
  src/support/version.cpp
  src/support/source.cpp
  src/support/diagnostic.cpp
  src/support/input.cpp
  src/syntax/lexer.cpp
  src/syntax/parser.cpp)
target_include_directories(lang_frontend PUBLIC include)
target_compile_features(lang_frontend PUBLIC cxx_std_20)
add_executable(langc src/driver/main.cpp)
target_link_libraries(langc PRIVATE lang_frontend)
if(LANG_ENABLE_LLVM)
  include(cmake/LLVM.cmake)
endif()
include(CTest)
if(BUILD_TESTING)
  find_program(LANG_TEST_PYTHON NAMES python3.12 python3)
  if(LANG_TEST_PYTHON)
    add_test(NAME syntax.cases COMMAND ${LANG_TEST_PYTHON}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/syntax/run_cases.py $<TARGET_FILE:langc>)
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
    -DLANGC=$<TARGET_FILE:langc> -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bootstrap/cli.cmake)
endif()

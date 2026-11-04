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

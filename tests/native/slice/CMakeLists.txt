cmake_minimum_required(VERSION 3.20)
project(lang_slice_runtime_validation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

get_filename_component(LANG_SLICE_RUNTIME_DIR
  "${CMAKE_CURRENT_LIST_DIR}/../../../runtime/native" ABSOLUTE)
set(LANG_SLICE_CORE
  "${LANG_SLICE_RUNTIME_DIR}/slice.cpp"
  "${LANG_SLICE_RUNTIME_DIR}/panic.cpp"
  "${LANG_SLICE_RUNTIME_DIR}/alloc.cpp")
set(LANG_SLICE_FREESTANDING_FLAGS
  -ffreestanding -fno-exceptions -fno-rtti -fno-builtin
  -fno-stack-protector -fno-asynchronous-unwind-tables
  -Wall -Wextra -Werror)

function(add_slice_runtime_objects name optimization)
  add_library(${name} OBJECT ${LANG_SLICE_CORE})
  target_include_directories(${name} PRIVATE "${LANG_SLICE_RUNTIME_DIR}")
  target_compile_options(${name} PRIVATE
    -O${optimization} ${LANG_SLICE_FREESTANDING_FLAGS})
endfunction()

add_slice_runtime_objects(slice_runtime_objects_o0 0)
add_slice_runtime_objects(slice_runtime_objects_o2 2)

function(add_slice_hosted_test name optimization objects)
  add_executable(${name} slice_runtime_test.cpp $<TARGET_OBJECTS:${objects}>)
  target_compile_options(${name} PRIVATE
    -O${optimization} -Wall -Wextra -Werror)
  target_link_libraries(${name} PRIVATE Threads::Threads)
  target_link_options(${name} PRIVATE
    -Wl,--wrap=lang_alloc_v1 -Wl,--wrap=lang_free_v1)
  add_test(NAME slice.runtime.O${optimization} COMMAND ${name})
  set_tests_properties(slice.runtime.O${optimization}
    PROPERTIES TIMEOUT 120)
endfunction()

add_slice_hosted_test(slice_runtime_test_o0 0 slice_runtime_objects_o0)
add_slice_hosted_test(slice_runtime_test_o2 2 slice_runtime_objects_o2)

add_executable(slice_freestanding
  slice_freestanding.cpp
  ${LANG_SLICE_CORE}
  "${LANG_SLICE_RUNTIME_DIR}/freestanding.cpp")
target_include_directories(slice_freestanding PRIVATE
  "${LANG_SLICE_RUNTIME_DIR}")
target_compile_options(slice_freestanding PRIVATE
  -O2 ${LANG_SLICE_FREESTANDING_FLAGS})
target_link_options(slice_freestanding PRIVATE
  -nostdlib -static
  -Wl,-e,_start
  -Wl,--build-id=none
  -Wl,--wrap=lang_alloc_v1
  -Wl,--wrap=lang_free_v1)
add_test(NAME slice.static.run COMMAND slice_freestanding)
add_test(NAME slice.static.elf COMMAND sh
  "${CMAKE_CURRENT_LIST_DIR}/check_static_elf.sh"
  "$<TARGET_FILE:slice_freestanding>")
set_tests_properties(slice.static.run slice.static.elf
  PROPERTIES TIMEOUT 30)

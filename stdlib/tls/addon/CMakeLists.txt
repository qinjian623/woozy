cmake_minimum_required(VERSION 3.20)
project(woozy_tls_addon LANGUAGES C CXX)

# Standalone addon library, not part of the compiler's root CMake build.
#
# The generated Woozy program links this archive explicitly when its manifest
# selects hosted runtime and declares:
#
#   [dependencies.woozy_tls]
#   kind = "native"
#   binding = "<repo>/stdlib/tls/bindings/tls.json"
#   libraries = ["ssl", "crypto", "stdc++"]
#   library_dirs = ["<this build directory>"]
#
# The library name "woozy_tls" is also declared by the binding module, so the
# project manifest only has to provide the OpenSSL/C++ runtime libraries.

set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(LANG_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)

if(APPLE)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../bindings/tls.json" binding)
  string(JSON binding SET "${binding}" target "\"arm64-apple-darwin\"")
  file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tls.json" CONTENT "${binding}\n" @ONLY)
else()
  configure_file("../bindings/tls.json" tls.json COPYONLY)
endif()

add_library(woozy_tls STATIC
  "${LANG_ROOT}/runtime/hosted/tls.cpp"
  "${LANG_ROOT}/runtime/hosted/tls_source.cpp")
target_include_directories(woozy_tls PUBLIC "${LANG_ROOT}/runtime/hosted")
target_compile_features(woozy_tls PUBLIC cxx_std_20)
target_link_libraries(woozy_tls PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(woozy_tls PRIVATE
  -O2 -Wall -Wextra -Wpedantic -Werror -fno-omit-frame-pointer)

# Optional C11 header probe; it is not linked into Woozy programs.
add_executable(woozy_tls_header_probe EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_LIST_DIR}/header_probe.c")
target_include_directories(woozy_tls_header_probe PRIVATE
  "${LANG_ROOT}/runtime/hosted")
target_link_libraries(woozy_tls_header_probe PRIVATE woozy_tls)
set_target_properties(woozy_tls_header_probe PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX)

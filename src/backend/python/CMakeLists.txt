# Integrator: add_subdirectory(src/backend/python) after defining lang_hir.
get_filename_component(LANG_PYTHON_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
file(READ "${LANG_PYTHON_ROOT}/runtime/python/langrt.py" LANG_PYTHON_RUNTIME)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${LANG_PYTHON_ROOT}/runtime/python/langrt.py")
configure_file(runtime.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/runtime.hpp" @ONLY)
add_library(lang_backend_python STATIC emitter.cpp)
target_include_directories(lang_backend_python PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(lang_backend_python PUBLIC lang_hir)
target_compile_features(lang_backend_python PUBLIC cxx_std_20)

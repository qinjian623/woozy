find_package(json-c CONFIG REQUIRED)
add_executable(lang_binding_static_test static_test.cpp)
target_link_libraries(lang_binding_static_test PRIVATE lang_sema lang_backend_python json-c::json-c)
add_test(NAME sema.t10 COMMAND lang_binding_static_test "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/cases")
if(LANG_ENABLE_LLVM)
  target_link_libraries(lang_binding_static_test PRIVATE lang_native)
  target_compile_definitions(lang_binding_static_test PRIVATE LANG_T10_NATIVE=1)
endif()

# Retained capturing C callbacks are checked through the LLVM-independent
# frontend and NativeIR verification. Runtime linking belongs to the
# backend/runtime owners.
add_executable(lang_binding_c_registration_test
  c_registration_test.cpp
  "${CMAKE_SOURCE_DIR}/src/ir/lower.cpp"
  "${CMAKE_SOURCE_DIR}/src/ir/verify.cpp")
target_link_libraries(lang_binding_c_registration_test
  PRIVATE lang_sema json-c::json-c)
target_compile_features(lang_binding_c_registration_test PRIVATE cxx_std_20)
add_test(NAME sema.c_callback_registration COMMAND lang_binding_c_registration_test
  "${CMAKE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/c_registration_cases")

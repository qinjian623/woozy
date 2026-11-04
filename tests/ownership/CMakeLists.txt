add_executable(lang_ownership_static_test static_test.cpp)
target_link_libraries(lang_ownership_static_test PRIVATE lang_sema json-c::json-c)
add_test(NAME sema.t09 COMMAND lang_ownership_static_test "${CMAKE_SOURCE_DIR}")

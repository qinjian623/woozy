if(LANG_ENABLE_LLVM AND LANG_TEST_PYTHON)
  add_test(NAME selfhost.source
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/source/run.py
      --woozyc $<TARGET_FILE:woozyc>)
  add_test(NAME selfhost.parser
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/parser/run.py
      --woozyc $<TARGET_FILE:woozyc>)
  add_test(NAME selfhost.bigint
    COMMAND ${CMAKE_COMMAND} -E env WOOZYC=$<TARGET_FILE:woozyc>
      ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/bigint/run.py --opt 0 2)
  add_test(NAME selfhost.frontend
    COMMAND ${LANG_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/front_diff.py
      --stage0 $<TARGET_FILE:woozyc> --cxx ${CMAKE_CXX_COMPILER}
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/frontend --require-wz)
  add_test(NAME selfhost.cli
    COMMAND ${LANG_TEST_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/smoke.py
      --stage0 $<TARGET_FILE:woozyc> --work-dir ${CMAKE_CURRENT_BINARY_DIR}/cli)
  add_test(NAME selfhost.toml
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/toml/run_toml_tests.py
      --cli $<TARGET_FILE:woozyc> --workspace ${CMAKE_CURRENT_BINARY_DIR}/toml)
  add_test(NAME selfhost.digest
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/digest/run.py
      --woozyc $<TARGET_FILE:woozyc>)
  set_tests_properties(selfhost.source selfhost.parser selfhost.bigint
    selfhost.frontend selfhost.cli selfhost.toml selfhost.digest PROPERTIES TIMEOUT 1200)
endif()

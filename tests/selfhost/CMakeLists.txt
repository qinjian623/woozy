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
  add_test(NAME selfhost.input
    COMMAND ${CMAKE_COMMAND} -E env WOOZYC=$<TARGET_FILE:woozyc>
      ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/input/run.py)
  add_test(NAME selfhost.paths
    COMMAND ${CMAKE_COMMAND} -E env WOOZYC=$<TARGET_FILE:woozyc>
      ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/paths/run.py)
  add_test(NAME selfhost.system
    COMMAND ${CMAKE_COMMAND} -E env WOOZYC=$<TARGET_FILE:woozyc> CXX=${CMAKE_CXX_COMPILER}
      bash ${PROJECT_SOURCE_DIR}/selfhost/tests/system/run.sh)
  add_test(NAME selfhost.number
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/number/run_number.py
      --woozyc $<TARGET_FILE:woozyc>)
  add_test(NAME selfhost.types
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/types/run.py
      --woozyc $<TARGET_FILE:woozyc>
      --oracle-build-dir $<TARGET_FILE_DIR:woozyc>
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/types)
  add_test(NAME selfhost.manifest
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/manifest/run_manifest_tests.py
      --woozyc $<TARGET_FILE:woozyc>
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/manifest)
  add_test(NAME selfhost.jsondoc
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/jsondoc/run.py
      --woozyc $<TARGET_FILE:woozyc> --clang ${CMAKE_CXX_COMPILER}
      --work-dir ${CMAKE_CURRENT_BINARY_DIR}/jsondoc)
  add_test(NAME selfhost.resolve
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/resolve/run.py
      --woozyc $<TARGET_FILE:woozyc>)
  add_test(NAME selfhost.resolve_differential
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/resolve/compare.py
      --woozyc $<TARGET_FILE:woozyc> --cxx ${CMAKE_CXX_COMPILER})
  add_test(NAME selfhost.hir_records
    COMMAND ${LANG_TEST_PYTHON} ${PROJECT_SOURCE_DIR}/selfhost/tests/hir_records/run.py
      --woozyc $<TARGET_FILE:woozyc>
      --root-include ${PROJECT_SOURCE_DIR}/include
      --types-source ${PROJECT_SOURCE_DIR}/selfhost/src/wzcc/types)
  set_tests_properties(selfhost.source selfhost.parser selfhost.bigint
    selfhost.frontend selfhost.cli selfhost.toml selfhost.digest selfhost.input
    selfhost.paths selfhost.system selfhost.number selfhost.types
    selfhost.manifest selfhost.jsondoc selfhost.resolve
    selfhost.resolve_differential selfhost.hir_records PROPERTIES TIMEOUT 1200)
endif()

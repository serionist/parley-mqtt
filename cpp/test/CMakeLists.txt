# Native test build: doctest (no-exceptions mode) driving the vector runner + unit suite.

add_executable(parley_tests
  doctest_main.cpp
  alloc_counter.cpp
  vector_runner.cpp
  conformance_tests.cpp
  json_compare.cpp
  unit_commander_tests.cpp
  unit_handler_tests.cpp
  unit_core_tests.cpp)

target_link_libraries(parley_tests PRIVATE parley_core)
target_include_directories(parley_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(parley_tests SYSTEM PRIVATE ${PARLEY_VENDOR_DIR} ${PARLEY_VENDOR_DIR}/ArduinoJson)

# doctest with -fno-exceptions (plan F6): the whole assertion machinery routed through the
# no-exceptions path; _BUT_WITH_ALL_ASSERTS keeps REQUIRE/CHECK usable (as non-throwing checks).
target_compile_definitions(parley_tests PRIVATE
  DOCTEST_CONFIG_NO_EXCEPTIONS
  DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
  PARLEY_CONFORMANCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../docs/conformance")

target_compile_options(parley_tests PRIVATE ${PARLEY_CXX_FLAGS})

add_test(NAME parley_tests COMMAND parley_tests)

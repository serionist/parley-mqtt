cmake_minimum_required(VERSION 3.16)
project(parley_testpeer CXX)

# Standalone desktop build of the Parley layer-3 interop test peer (docs/testing.md). It compiles the
# frozen, non-templated protocol core straight from ../src (no edits to the core or its CMake) and the
# generic scenario interpreter over a synchronously-pumped libmosquitto transport. Same no-exceptions /
# no-RTTI discipline and the same pinned ARDUINOJSON_USE_DOUBLE as the frozen conformance build.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(PARLEY_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Reuse the pinned single-header ArduinoJson 7.4.3 already vendored for the frozen conformance build.
set(PARLEY_VENDOR_DIR ${PARLEY_ROOT}/test/vendor)

add_compile_definitions(ARDUINOJSON_USE_DOUBLE=1)

add_executable(parley_testpeer
  main.cpp
  ${PARLEY_ROOT}/src/detail/wire.cpp
  ${PARLEY_ROOT}/src/detail/commander_core.cpp
  ${PARLEY_ROOT}/src/detail/handler_core.cpp)

target_include_directories(parley_testpeer PRIVATE ${PARLEY_ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR})
# Third-party as SYSTEM so -Werror does not fire on ArduinoJson's template code.
target_include_directories(parley_testpeer SYSTEM PRIVATE
  ${PARLEY_VENDOR_DIR} ${PARLEY_VENDOR_DIR}/ArduinoJson)

target_compile_options(parley_testpeer PRIVATE
  -Wall -Wextra -Werror -fno-exceptions -fno-rtti)

# libmosquitto (Debian: libmosquitto-dev). The peer pumps mosquitto_loop() synchronously.
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)
find_library(MOSQUITTO_LIBRARY mosquitto REQUIRED)
target_include_directories(parley_testpeer SYSTEM PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(parley_testpeer PRIVATE ${MOSQUITTO_LIBRARY})

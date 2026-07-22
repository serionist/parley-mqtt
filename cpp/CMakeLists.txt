cmake_minimum_required(VERSION 3.16)
project(parley_cpp CXX)

# Native desktop build of the Parley C++ core + tests (the conformance target; docs/testing.md layer 1&2).
# The embedded/Arduino build uses the same src/ tree through the Arduino/PlatformIO toolchain.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(PARLEY_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# The no-exceptions / no-RTTI discipline (CONVENTIONS.md C++), warnings-as-errors, and the pinned
# ArduinoJson double precision (plan F3).
set(PARLEY_CXX_FLAGS
    -Wall -Wextra -Werror
    -fno-exceptions -fno-rtti)
add_compile_definitions(ARDUINOJSON_USE_DOUBLE=1)

if(PARLEY_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# Vendored, pinned single-header dependencies (native build only; Arduino consumers supply their own
# ArduinoJson). Added as SYSTEM includes so -Werror does not fire on third-party template code.
set(PARLEY_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/test/vendor)

# --- the shared, non-templated protocol core (plan F9: compiled once) ---
add_library(parley_core STATIC
  src/detail/wire.cpp
  src/detail/commander_core.cpp
  src/detail/handler_core.cpp)
target_include_directories(parley_core PUBLIC src)
target_include_directories(parley_core SYSTEM PUBLIC ${PARLEY_VENDOR_DIR} ${PARLEY_VENDOR_DIR}/ArduinoJson)
target_compile_options(parley_core PRIVATE ${PARLEY_CXX_FLAGS})

enable_testing()
add_subdirectory(test)

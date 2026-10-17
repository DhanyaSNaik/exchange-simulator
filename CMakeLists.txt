cmake_minimum_required(VERSION 3.16)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clang-tidy

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# SANITIZE=address,undefined (etc.) builds a sanitizer-instrumented variant.
# Used by CI explicitly; not the default build type.
if(DEFINED SANITIZE)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=${SANITIZE})
endif()

add_library(lob_core
  src/order_book.cpp
  src/matching_engine.cpp
)
target_include_directories(lob_core PUBLIC include)

enable_testing()

add_executable(unit_tests
  tests/unit/main.cpp
  tests/unit/test_order_book.cpp
  tests/unit/test_matching_engine.cpp
  tests/unit/test_adversarial.cpp
  tests/unit/test_sequence_monotonicity.cpp
)
target_include_directories(unit_tests PRIVATE tests)
target_link_libraries(unit_tests PRIVATE lob_core)
add_test(NAME unit_tests COMMAND unit_tests)

add_executable(property_test tests/property/property_test.cpp)
target_link_libraries(property_test PRIVATE lob_core)
# Scale kept modest for CI runtime; see BENCHMARKS.md / Phase 2 for the
# full-scale (100k/1M/10M) event-stream numbers, which are a separate,
# non-assertion-checking benchmark binary, not this correctness test.
add_test(NAME property_test COMMAND property_test 300 1000 1234567)

add_executable(differential_test
  tests/reference/test_differential.cpp
)
target_include_directories(differential_test PRIVATE tests/reference)
target_link_libraries(differential_test PRIVATE lob_core)
add_test(NAME differential_test COMMAND differential_test 300 300 2024)

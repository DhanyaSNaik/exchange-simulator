cmake_minimum_required(VERSION 3.16)
project(exchange_simulator LANGUAGES CXX)

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
  src/csv_event_reader.cpp
  src/simulation_loop.cpp
  src/market_maker.cpp
  src/market_session.cpp
  src/analytics.cpp
)
target_include_directories(lob_core PUBLIC include)

enable_testing()

add_executable(unit_tests
  tests/unit/main.cpp
  tests/unit/test_order_book.cpp
  tests/unit/test_matching_engine.cpp
  tests/unit/test_adversarial.cpp
  tests/unit/test_sequence_monotonicity.cpp
  tests/unit/test_csv_event_reader.cpp
  tests/unit/test_simulation_loop.cpp
  tests/unit/test_market_maker.cpp
  tests/unit/test_market_maker_pnl.cpp
  tests/unit/test_market_session.cpp
  tests/unit/test_analytics.cpp
  tests/unit/test_pnl_hand_calculation.cpp
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

add_executable(replay_property_test tests/property/replay_property_test.cpp)
target_link_libraries(replay_property_test PRIVATE lob_core)
add_test(NAME replay_property_test COMMAND replay_property_test 200 300 90000)

add_executable(differential_test
  tests/reference/test_differential.cpp
)
target_include_directories(differential_test PRIVATE tests/reference)
target_link_libraries(differential_test PRIVATE lob_core)
add_test(NAME differential_test COMMAND differential_test 300 300 2024)

# --- Phase 2: benchmark-only artifacts (bench/). NOT part of the Phase 1
# library; see DESIGN_DECISIONS.md "Phase 2" entry for why the flat-vector
# alternative stays here rather than being promoted into include/lob/.
add_executable(check_flat_correctness bench/check_flat_correctness.cpp)
target_include_directories(check_flat_correctness PRIVATE bench)
target_link_libraries(check_flat_correctness PRIVATE lob_core)
add_test(NAME check_flat_correctness COMMAND check_flat_correctness)

add_executable(run_benchmark bench/run_benchmark.cpp)
target_include_directories(run_benchmark PRIVATE bench)
target_link_libraries(run_benchmark PRIVATE lob_core)
target_compile_options(run_benchmark PRIVATE -O3 -DNDEBUG)
# Not added via add_test(): this is a benchmark, not a pass/fail
# correctness check. Run manually: ./run_benchmark <n> <seed> <map|flat>,
# or bench/run_benchmarks.sh <path-to-binary> <seed> to sweep all scales.

# --- Phase 5: example/demo programs (examples/). Not tests -- these are
# the runnable, end-to-end demonstrations referenced from README.md.
add_executable(replay_demo examples/replay_demo.cpp)
target_link_libraries(replay_demo PRIVATE lob_core)

add_executable(market_maker_demo examples/market_maker_demo.cpp)
target_link_libraries(market_maker_demo PRIVATE lob_core)

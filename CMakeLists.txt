cmake_minimum_required(VERSION 3.16)
project(edge_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SANITIZE "Build with a sanitizer: address, undefined or thread" "")

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

add_library(edge_core
  src/http.cpp src/cache.cpp src/balancer.cpp src/health_checker.cpp
  src/metrics.cpp src/event_loop.cpp src/proxy.cpp)
target_include_directories(edge_core PUBLIC src)
target_link_libraries(edge_core PUBLIC Threads::Threads)

add_executable(edge-proxy src/main.cpp)
target_link_libraries(edge-proxy PRIVATE edge_core)

add_executable(fake-backend bench/fake_backend_main.cpp)
target_include_directories(fake-backend PRIVATE testutil)
target_link_libraries(fake-backend PRIVATE Threads::Threads)

option(BUILD_TESTING "Build unit and integration tests" ON)
if(BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(edge_tests
    tests/test_http.cpp tests/test_rate_limiter.cpp tests/test_cache.cpp
    tests/test_balancer.cpp tests/test_integration.cpp)
  target_include_directories(edge_tests PRIVATE testutil)
  target_link_libraries(edge_tests PRIVATE edge_core GTest::gtest GTest::gtest_main)
  add_test(NAME edge_tests COMMAND edge_tests)
endif()

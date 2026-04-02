cmake_minimum_required(VERSION 3.10)
project(MyIOMessageEngine)

# Set C++ standard to 20 for coroutines
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable release build and high-performance O3 optimization by default
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -fno-omit-frame-pointer -g -DNDEBUG")


# Include directories
include_directories(src/include)

# Find Threads and liburing
find_package(Threads REQUIRED)
find_library(LIBURING uring REQUIRED)

# --- Benchmark Support ---
# 检查系统中是否存在 google-benchmark，通常可以通过 `sudo apt-get install libbenchmark-dev` 安装
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  message(STATUS "Google Benchmark not found in system. Skipping benchmarks.")
else()
  message(STATUS "Google Benchmark found. Building benchmark targets...")
  add_executable(mpsc_bench benchmark/mpsc_bench.cc)
  target_link_libraries(mpsc_bench PRIVATE Threads::Threads benchmark::benchmark src_headers)
endif()

# 为了让编译更整洁，建议将 include_directories 抽取为 interface target
add_library(src_headers INTERFACE)
target_include_directories(src_headers INTERFACE src/include)

# Targets
add_executable(dag_test src/demo/dag_test.cc)
target_link_libraries(dag_test PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(co_scheduler_test src/demo/co_scheduler_test.cc)
target_link_libraries(co_scheduler_test PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(io_context_test src/demo/io_context_test.cc)
target_link_libraries(io_context_test PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(io_awaiter_test src/demo/io_awaiter_test.cc)
target_link_libraries(io_awaiter_test PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(io_scheduler_integrated_test src/demo/io_scheduler_integrated_test.cc)
target_link_libraries(io_scheduler_integrated_test PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(echo_server src/demo/echo_server.cc)
target_link_libraries(echo_server PRIVATE Threads::Threads ${LIBURING} src_headers)

add_executable(mpsc_io_stress_test src/demo/mpsc_io_stress_test.cc)
target_link_libraries(mpsc_io_stress_test PRIVATE Threads::Threads ${LIBURING} src_headers)

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

# Find Threads
find_package(Threads REQUIRED)

# Targets
add_executable(dag_test src/demo/dag_test.cc)
target_link_libraries(dag_test PRIVATE Threads::Threads)

add_executable(co_scheduler_test src/demo/co_scheduler_test.cc)
target_link_libraries(co_scheduler_test PRIVATE Threads::Threads)

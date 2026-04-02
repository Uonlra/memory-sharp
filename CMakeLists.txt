cmake_minimum_required(VERSION 3.16)
project(high_performance_memory_pool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(memory_pool
    src/memory_pool.cpp
)

target_include_directories(memory_pool PUBLIC include)

add_executable(memory_pool_demo
    src/main.cpp
)

target_link_libraries(memory_pool_demo PRIVATE memory_pool)

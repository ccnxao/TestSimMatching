cmake_minimum_required(VERSION 3.16)
project(sim_matcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sim_matcher
    src/sim_matcher.cpp
)

target_include_directories(sim_matcher PUBLIC include)

add_executable(sim_matcher_demo
    src/main.cpp
)

target_link_libraries(sim_matcher_demo PRIVATE sim_matcher)

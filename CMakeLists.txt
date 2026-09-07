cmake_minimum_required(VERSION 3.20)

project(ashfall LANGUAGES CXX)

option(ASHFALL_BUILD_TESTS "Build deterministic simulation tests" ON)

add_library(ashfall_sim engine/src/simulation.cpp)
target_include_directories(ashfall_sim PUBLIC engine/include)
target_compile_features(ashfall_sim PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ashfall_sim PRIVATE /W4 /permissive-)
else()
    target_compile_options(ashfall_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(ASHFALL_BUILD_TESTS)
    enable_testing()
    add_executable(ashfall_sim_tests tests/simulation_tests.cpp)
    target_link_libraries(ashfall_sim_tests PRIVATE ashfall_sim)
    add_test(NAME ashfall_sim_tests COMMAND ashfall_sim_tests)
endif()

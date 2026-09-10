cmake_minimum_required(VERSION 3.20)

project(ashfall LANGUAGES CXX)

option(ASHFALL_BUILD_TESTS "Build deterministic simulation tests" ON)

add_library(ashfall_sim engine/src/simulation.cpp engine/src/material_data.cpp engine/src/json_subset.cpp engine/src/reaction_data.cpp)
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
    add_executable(ashfall_flat_tests tests/flat_map_tests.cpp)
    target_link_libraries(ashfall_flat_tests PRIVATE ashfall_sim)
    add_test(NAME ashfall_flat_tests COMMAND ashfall_flat_tests)
    # Step benchmark harness (Group 1): manual tool, not part of ctest.
    add_executable(ashfall_bench bench/bench_step.cpp)
    target_link_libraries(ashfall_bench PRIVATE ashfall_sim)
endif()

# GDExtension bridge using godot-cpp
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/godot-cpp/CMakeLists.txt")
    set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version" FORCE)
    add_subdirectory(godot-cpp)

    add_library(ashfall_extension SHARED
        bridge/src/register_types.cpp
        bridge/src/ashfall_world.cpp
    )
    target_include_directories(ashfall_extension PRIVATE bridge/include)
    target_link_libraries(ashfall_extension PRIVATE godot::cpp ashfall_sim)
    target_compile_features(ashfall_extension PUBLIC cxx_std_20)

    set_target_properties(ashfall_extension PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME "ashfall"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    )
endif()

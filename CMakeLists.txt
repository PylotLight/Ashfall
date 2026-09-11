cmake_minimum_required(VERSION 3.20)

project(ashfall LANGUAGES CXX)

option(ASHFALL_BUILD_TESTS "Build deterministic simulation tests" ON)
option(ASHFALL_BUILD_FUZZ "libFuzzer targets (needs LLVM clang)" OFF)

add_library(ashfall_sim engine/src/simulation.cpp engine/src/material_data.cpp engine/src/json_subset.cpp engine/src/reaction_data.cpp)
target_include_directories(ashfall_sim PUBLIC engine/include)
target_compile_features(ashfall_sim PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(ashfall_sim PRIVATE /W4 /permissive-)
else()
    target_compile_options(ashfall_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# Fuzz wiring (Task F0): FUZZ=ON poisons the whole build dir with
# -fsanitize=fuzzer,address,undefined on ashfall_sim + targets. NEVER configure
# FUZZ=ON in the default build/ dir — always use a SEPARATE dir (build-fuzz).
# libFuzzer ships only with LLVM Clang (brew install llvm); Apple Clang cannot
# link -fsanitize=fuzzer and fails the probe below with a FATAL_ERROR.
if(ASHFALL_BUILD_FUZZ)
    # Compile+link probe: does this compiler provide libFuzzer runtime?
    file(WRITE "${CMAKE_BINARY_DIR}/ashfall_fuzz_probe.cpp"
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n)\n"
        "{ (void)d; (void)n; return 0; }\n")
    try_compile(ASHFALL_HAS_FUZZER
        "${CMAKE_BINARY_DIR}/ashfall_fuzz_probe"
        "${CMAKE_BINARY_DIR}/ashfall_fuzz_probe.cpp"
        CXX_STANDARD 20
        COMPILE_DEFINITIONS "-fsanitize=fuzzer"
        LINK_OPTIONS "-fsanitize=fuzzer"
        OUTPUT_VARIABLE ASHFALL_FUZZ_PROBE_LOG)
    if(NOT ASHFALL_HAS_FUZZER)
        message(FATAL_ERROR
            "ASHFALL_BUILD_FUZZ=ON but this compiler lacks -fsanitize=fuzzer "
            "(libFuzzer). On macOS use LLVM Clang: brew install llvm, then "
            "configure a SEPARATE dir, e.g. "
            "PATH=/opt/homebrew/opt/llvm/bin:$PATH "
            "cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ "
            "-DASHFALL_BUILD_FUZZ=ON. Probe log:\n${ASHFALL_FUZZ_PROBE_LOG}")
    endif()

    # Engine+targets must match sanitizers: build ashfall_sim itself with
    # fuzzer+ASan+UBSan so ODR/instrumentation agree at link time.
    target_compile_options(ashfall_sim PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
    target_link_options(ashfall_sim PUBLIC
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

    # libFuzzer executables (Round 2 sources; guarded so a missing source does
    # not break configure). Deliberately NOT added to ctest.
    set(ASHFALL_FUZZ_TARGETS
        fuzz_json_parse
        fuzz_material_load
        fuzz_reaction_load
        fuzz_material_struct
        fuzz_reaction_struct
        fuzz_save_crash
        fuzz_save_roundtrip)
    foreach(fuzz_name IN LISTS ASHFALL_FUZZ_TARGETS)
        set(fuzz_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/${fuzz_name}.cpp")
        if(EXISTS "${fuzz_src}")
            add_executable(${fuzz_name} "${fuzz_src}")
            target_link_libraries(${fuzz_name} PRIVATE ashfall_sim)
            if(MSVC)
                target_compile_options(${fuzz_name} PRIVATE /W4 /permissive-)
            else()
                target_compile_options(${fuzz_name} PRIVATE
                    -Wall -Wextra -Wpedantic -Werror
                    -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
            endif()
            target_link_options(${fuzz_name} PRIVATE
                -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)
        else()
            message(STATUS
                "Fuzz target ${fuzz_name} skipped: ${fuzz_src} not present yet (lands in Round 2)")
        endif()
    endforeach()

    # Stdin->verdict shim for the offline differential script
    # (tests/fuzz/differential_json.py). Diagnostic tool, NOT in ctest.
    set(json_verdict_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/json_verdict.cpp")
    if(EXISTS "${json_verdict_src}")
        add_executable(json_verdict "${json_verdict_src}")
        target_link_libraries(json_verdict PRIVATE ashfall_sim)
        if(MSVC)
            target_compile_options(json_verdict PRIVATE /W4 /permissive-)
        else()
            target_compile_options(json_verdict PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
endif()

if(ASHFALL_BUILD_TESTS)
    enable_testing()
    add_executable(ashfall_sim_tests tests/simulation_tests.cpp)
    target_link_libraries(ashfall_sim_tests PRIVATE ashfall_sim)
    target_include_directories(ashfall_sim_tests PRIVATE tests)
    add_test(NAME ashfall_sim_tests COMMAND ashfall_sim_tests)
    add_executable(ashfall_flat_tests tests/flat_map_tests.cpp)
    target_link_libraries(ashfall_flat_tests PRIVATE ashfall_sim)
    add_test(NAME ashfall_flat_tests COMMAND ashfall_flat_tests)
    # Step benchmark harness (Group 1): manual tool, not part of ctest.
    add_executable(ashfall_bench bench/bench_step.cpp)
    target_link_libraries(ashfall_bench PRIVATE ashfall_sim)
    # Oracle recorder (Group 4, O0): regenerates tests/oracle/*.hash; manual tool.
    add_executable(oracle_record bench/oracle_record.cpp)
    target_link_libraries(oracle_record PRIVATE ashfall_sim)
    target_include_directories(oracle_record PRIVATE tests)
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

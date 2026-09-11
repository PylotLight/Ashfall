// Task F3 (Group 6): save/load crash/DoS fuzz target.
//
// Contract: arbitrary bytes -> SimulationWorld::load_with_diagnostic (default
// LoadOptions). Asserts: the loader RETURNS (never throws), respects the load
// caps (no large allocation; -rss_limit_mb=512 is the backstop), and every
// failure names a LoadError (never a silent false). Temps-on-air, duplicates,
// unsorted cells/temps, and CRC paths are all in scope -- the fuzzer WILL
// generate them; only crashes, hangs, or throws are findings.
// Run budget: -max_len=8192 -timeout=5 -rss_limit_ms=512 (libFuzzer flags).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "ashfall/simulation.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // std::span needs a valid pointer even for an empty range; libFuzzer does
    // not pass null in practice, but the substitute keeps UBSan quiet.
    static const uint8_t kDummy = 0;
    if (data == nullptr) {
        if (size != 0) return 0;
        data = &kDummy;
    }
    const std::span<const uint8_t> bytes(data, size);

    ashfall::SimulationWorld world; // default material table (air..oil 0..9)
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    bool ok = false;
    try {
        ok = world.load_with_diagnostic(bytes, error, ashfall::LoadOptions{});
    } catch (...) {
        std::fprintf(stderr, "fuzz_save_crash: load_with_diagnostic threw on %zu input bytes\n", size);
        std::abort(); // loaders must be total: a throw is a finding
    }
    if (!ok && error == ashfall::SimulationWorld::LoadError::Ok) {
        std::fprintf(stderr, "fuzz_save_crash: silent failure (no error named) on %zu input bytes\n", size);
        std::abort(); // failures must name an error: silence is a finding
    }
    return 0;
}

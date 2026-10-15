// Task F3 (Group 6): save/load round-trip differential fuzz target.
//
// Contract (reviewer constraint: NEVER full trajectories -- the bounds below
// are the whole budget): bytes -> build a SMALL world (cell count =
// data[0] % 65, i.e. <= 64 cells; coords from byte pairs mapped to [-32,32];
// material byte -> id % 11 with set_cell gating validity naturally -- id 10
// is unknown and skipped, which is itself correct behavior; temps from slices
// via set_temperature, exercising the bool air-reject path; setup ticks =
// data[1] % 6, i.e. <= 5) -> step -> save() -> twin load() -> assert
// byte-equal saves -> step both 3 ticks -> assert per-tick byte equality.
// ANY inequality is a real invariant violation: the target prints the repro
// bytes to stderr and aborts.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ashfall/cell_types.hpp"
#include "ashfall/simulation.hpp"

namespace {

void fail_with_repro(const char* stage, const uint8_t* data, size_t size) {
    std::fprintf(stderr, "fuzz_save_roundtrip INVARIANT VIOLATION at %s (input %zu bytes):", stage, size);
    for (size_t i = 0; i < size; ++i) std::fprintf(stderr, " %02x", data[i]);
    std::fprintf(stderr, "\n");
    std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 3) return 0;
    const size_t cell_count = static_cast<size_t>(data[0]) % 65; // <= 64 cells
    const unsigned setup_ticks = static_cast<unsigned>(data[1]) % 6; // <= 5 ticks
    const uint8_t* body = data + 2;
    const size_t body_len = size - 2;

    ashfall::SimulationWorld world; // default material table (air..oil 0..9)
    for (size_t i = 0; i < cell_count; ++i) {
        // 5-byte stride per cell (x, y, material, temp_lo, temp_hi), wrapped
        // so any input length decodes deterministically.
        const uint8_t xb = body[(5 * i) % body_len];
        const uint8_t yb = body[(5 * i + 1) % body_len];
        const uint8_t mb = body[(5 * i + 2) % body_len];
        const uint8_t t0 = body[(5 * i + 3) % body_len];
        const uint8_t t1 = body[(5 * i + 4) % body_len];
        const ashfall::CellPosition pos{
            static_cast<int>(xb % 65) - 32, static_cast<int>(yb % 65) - 32};
        const ashfall::MaterialId mat = static_cast<ashfall::MaterialId>(mb % 11);
        world.set_cell(pos, mat); // id 10 is unknown: set_cell skips it
        const int temp = static_cast<int>(static_cast<int16_t>(
            static_cast<uint16_t>(t0) | (static_cast<uint16_t>(t1) << 8)));
        (void)world.set_temperature(pos, temp); // false on air: reject path
    }
    for (unsigned t = 0; t < setup_ticks; ++t) world.step();

    const std::vector<uint8_t> saved = world.save();
    ashfall::SimulationWorld twin;
    if (!twin.load(saved)) fail_with_repro("save/load", data, size);
    if (twin.save() != saved) fail_with_repro("save byte-equality", data, size);
    for (int t = 0; t < 3; ++t) {
        world.step();
        twin.step();
        if (twin.save() != world.save()) fail_with_repro("per-tick lockstep", data, size);
    }
    return 0;
}

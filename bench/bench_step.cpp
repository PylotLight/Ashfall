// In-repo step() benchmark harness (Group 1, Task A).
//
// Port of the throwaway /tmp/bench_step.cpp scenarios A-D (same geometry,
// same settle ticks, same 200-tick default measure). Standalone tool; the
// integrator wires it into CMake. Includes only ashfall/simulation.hpp.
//
// CLI:
//   --scenario [A|B|C|D|E|all]  (default all; all = A-D, E is opt-in only —
//                                 the 1024² world would dominate suite time)
//   --ticks N                 (default 200) measured step() calls per repeat
//   --repeats R               (default 5)
//   --warmup W                (default 1) warmup ticks discarded per repeat
//                             (repeat mode only; trajectory mode measures the
//                             continuing world directly)
//   --mode [trajectory|repeat]
//     trajectory: settle once, measure R repeats of N ticks on the
//                 continuing world.
//     repeat:     rebuild the scenario fresh per repeat (including its
//                 canonical settle ticks), discard W warmup ticks, measure N.
//   --help
//
// Report per scenario: median and p95 ms/step over repeats, occupied/temps,
// plus phase-timing and counter summary lines reading the Task C API
// (last_step_timings(), last_step_counters()) guarded by
// #ifdef ASHFALL_STEP_INSTRUMENT so this compiles both before Task C lands
// (prints "instrumentation: absent") and after (prints the breakdown).
//
// Compile (Task C absent path):
//   c++ -std=c++20 -O3 -I engine/include bench/bench_step.cpp \
//       build/libashfall_sim.a -o /tmp/bench_verify_a
// With Task C instrumentation:
//   c++ -std=c++20 -O3 -DASHFALL_STEP_INSTRUMENT -I engine/include \
//       bench/bench_step.cpp build/libashfall_sim.a -o /tmp/bench_verify_a
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ashfall/simulation.hpp"

namespace {

struct Options {
    std::string scenario = "all";
    int ticks = 200;
    int repeats = 5;
    int warmup = 1;
    std::string mode = "trajectory";
};

bool parse_int(const char* s, int& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || value < 0 || value > 1000000) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

void print_usage() {
    std::printf("usage: bench_step [--scenario A|B|C|D|E|all] [--ticks N] "
                "[--repeats R] [--warmup W] [--mode trajectory|repeat]\n");
}

// Scenario builders: geometry + canonical settle ticks verbatim from
// /tmp/bench_step.cpp. A settles 200 ticks, D settles 10, B/C measure from
// the freshly built state. E (Group 5, Task M3: 1024² water basin, no
// settle) is additive — A-D geometry untouched.
bool build_scenario(char id, ashfall::SimulationWorld& world) {
    switch (id) {
        case 'A':
            world.build_play_level();
            for (int i = 0; i < 200; ++i) {
                world.step();
            }
            return true;
        case 'B':
            world.fill_rect({156, 320}, {356, 320}, ashfall::kStone);
            world.fill_rect({156, 200}, {156, 319}, ashfall::kStone);
            world.fill_rect({356, 200}, {356, 319}, ashfall::kStone);
            world.fill_rect({157, 200}, {355, 260}, ashfall::kWater);
            return true;
        case 'C': {
            world.fill_rect({100, 400}, {400, 400}, ashfall::kStone);
            world.fill_rect({100, 380}, {400, 399}, ashfall::kWood);
            world.fill_rect({100, 370}, {400, 379}, ashfall::kFire);
            ashfall::ReactionTable table;
            std::string error;
            if (!ashfall::parse_reaction_data(
                    R"JSON({"version":1,"reactions":[
                      {"name":"spread","probability":40,
                       "reactants":[{"material":"fire"},{"tag":"burnable"}],
                       "products":[{"material":"fire"},{"material":"fire"}]},
                      {"name":"burnout","probability":8,
                       "reactants":[{"material":"fire"}],
                       "products":[{"material":"air"}]}]})JSON",
                    table, error)) {
                std::printf("rules parse failed: %s\n", error.c_str());
                return false;
            }
            if (!world.load_reactions(table)) {
                std::printf("rules load failed\n");
                return false;
            }
            return true;
        }
        case 'D':
            world.fill_rect({100, 100}, {400, 400}, ashfall::kStone);
            for (int i = 0; i < 10; ++i) {
                world.step();
            }
            return true;
        case 'E': {
            // 1024² scaling micro (Group 5, Task M3): B-class water basin
            // scaled to the full 1024² extent — floor + side walls + a deep
            // water body (~205k cells over a 16x7-chunk span). No settle:
            // measures from the freshly built state, like B. Tracks the
            // LLC-cliff scaling ratio vs B (the migration's raison d'être).
            world.fill_rect({0, 1023}, {1023, 1023}, ashfall::kStone);
            world.fill_rect({0, 800}, {0, 1023}, ashfall::kStone);
            world.fill_rect({1023, 800}, {1023, 1023}, ashfall::kStone);
            world.fill_rect({1, 823}, {1022, 1022}, ashfall::kWater);
            return true;
        }
        default:
            return false;
    }
}

const char* scenario_label(char id) {
    switch (id) {
        case 'A':
            return "A settled-level";
        case 'B':
            return "B water-resolve";
        case 'C':
            return "C wood-fire   ";
        case 'D':
            return "D static-box  ";
        case 'E':
            return "E water-1024  ";
        default:
            return "?             ";
    }
}

double measure_ms_per_step(ashfall::SimulationWorld& world, int ticks) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ticks; ++i) {
        world.step();
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count() /
           static_cast<double>(ticks);
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

double p95_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    std::size_t idx =
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    idx = idx == 0 ? 0 : idx - 1;
    if (idx >= n) {
        idx = n - 1;
    }
    return values[idx];
}

void print_instrumentation(const ashfall::SimulationWorld& world) {
#ifdef ASHFALL_STEP_INSTRUMENT
    const auto timings = world.last_step_timings();
    const auto counters = world.last_step_counters();
    std::printf("    phases ms/step: reactions=%.3f clear_reserve=%.3f classify=%.3f "
                "ordering=%.3f powder=%.3f liquid=%.3f gas=%.3f reconcile=%.3f "
                "heat_carry=%.3f conduct=%.3f total=%.3f\n",
                timings.reactions_ms, timings.clear_reserve_ms, timings.classify_ms,
                timings.ordering_ms, timings.powder_ms, timings.liquid_ms,
                timings.gas_ms, timings.reconcile_ms, timings.heat_carry_ms,
                timings.conduct_ms, timings.total_ms);
    std::printf("    counters: occupied=%zu static=%zu dynamic=%zu powder_moves=%zu "
                "liquid_moves=%zu gas_moves=%zu swaps=%zu reaction_cells=%zu "
                "reaction_hits=%zu nonzero_temps=%zu carried_heat=%zu "
                "changed_positions=%zu dirty_chunk_marks=%zu\n",
                counters.occupied, counters.static_cells, counters.dynamic_cells,
                counters.powder_moves, counters.liquid_moves, counters.gas_moves,
                counters.swaps, counters.reaction_cells, counters.reaction_hits,
                counters.nonzero_temps, counters.carried_heat,
                counters.changed_positions, counters.dirty_chunk_marks);
#else
    (void)world;
    std::printf("    instrumentation: absent\n");
#endif
}

int run_scenario(char id, const Options& opts) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(opts.repeats));
    // Keep the last measured world for occupied/temps + instrumentation reads.
    ashfall::SimulationWorld last_world;
    bool have_world = false;

    if (opts.mode == "trajectory") {
        ashfall::SimulationWorld world;
        if (!build_scenario(id, world)) {
            return 1;
        }
        for (int r = 0; r < opts.repeats; ++r) {
            samples.push_back(measure_ms_per_step(world, opts.ticks));
        }
        last_world = std::move(world);
        have_world = true;
    } else {
        for (int r = 0; r < opts.repeats; ++r) {
            ashfall::SimulationWorld world;
            if (!build_scenario(id, world)) {
                return 1;
            }
            for (int w = 0; w < opts.warmup; ++w) {
                world.step();
            }
            samples.push_back(measure_ms_per_step(world, opts.ticks));
            last_world = std::move(world);
            have_world = true;
        }
    }

    if (!have_world) {
        return 1;
    }
    std::printf("%s: median %8.3f ms/step p95 %8.3f ms/step "
                "(ticks=%d repeats=%d mode=%s) occupied=%zu temps=%zu\n",
                scenario_label(id), median_of(samples), p95_of(samples), opts.ticks,
                opts.repeats, opts.mode.c_str(), last_world.occupied_cell_count(),
                last_world.temperature_count());
    print_instrumentation(last_world);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--scenario" && i + 1 < argc) {
            opts.scenario = argv[++i];
        } else if (arg == "--ticks" && i + 1 < argc) {
            if (!parse_int(argv[++i], opts.ticks) || opts.ticks <= 0) {
                std::printf("invalid --ticks (need positive int)\n");
                return 1;
            }
        } else if (arg == "--repeats" && i + 1 < argc) {
            if (!parse_int(argv[++i], opts.repeats) || opts.repeats <= 0) {
                std::printf("invalid --repeats (need positive int)\n");
                return 1;
            }
        } else if (arg == "--warmup" && i + 1 < argc) {
            if (!parse_int(argv[++i], opts.warmup) || opts.warmup < 0) {
                std::printf("invalid --warmup (need non-negative int)\n");
                return 1;
            }
        } else if (arg == "--mode" && i + 1 < argc) {
            opts.mode = argv[++i];
        } else {
            std::printf("unknown arg: %s\n", arg.c_str());
            print_usage();
            return 1;
        }
    }
    if (opts.scenario != "A" && opts.scenario != "B" && opts.scenario != "C" &&
        opts.scenario != "D" && opts.scenario != "E" && opts.scenario != "all") {
        std::printf("invalid --scenario (need A|B|C|D|E|all)\n");
        return 1;
    }
    if (opts.mode != "trajectory" && opts.mode != "repeat") {
        std::printf("invalid --mode (need trajectory|repeat)\n");
        return 1;
    }

    std::vector<char> ids;
    if (opts.scenario == "all") {
        ids = {'A', 'B', 'C', 'D'};
    } else {
        ids = {opts.scenario[0]};
    }
    for (char id : ids) {
        if (run_scenario(id, opts) != 0) {
            return 1;
        }
    }
    return 0;
}

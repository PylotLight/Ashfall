// E1 benchmark worlds (Slice E, task E1; review §10.2).
//
// Eight runnable sim benchmark worlds on this machine (Apple Silicon, native
// release): empty, static terrain, falling-sand volume, moving-liquid volume,
// broad combustion, broad temperature, repeated explosions, chunk-boundary
// traversal. Follows the bench_step --scenario pattern (additive: scenarios
// A-E there are untouched; this is a separate tool so the D3 worker's
// dirty/lighting proof scripts and the bench_step file stay out of the way).
//
// What this measures: world_.step() ONLY (sim, no present) — the same span
// the HUD sim ms shows, so HUD figures are directly comparable. Per-tick
// sampling: every measured tick is timed individually and pooled across
// repeats, giving median/p95/worst-spike (max) over real tick samples, plus
// mean phase splits via the Task C API (last_step_timings /
// last_step_counters, always on in-tree since E1 enabled
// ASHFALL_STEP_INSTRUMENT on ashfall_sim).
//
// §10 mapping of the reported phases:
//   reactions      = reactions_ms
//   powder/liquid/gas movement = powder_ms / liquid_ms / gas_ms
//   temperature    = heat_carry_ms + conduct_ms
//   active-region maintenance = clear_reserve_ms + classify_ms +
//                    ordering_ms + reconcile_ms (scratch / dynamic-set /
//                    chunk-reconcile work; the engine has no separate
//                    active-region system)
//   snapshot extraction / image prep / upload submission = present-side
//                    phases (bridge PresentPhases: material_read / paint +
//                    set_data / submit) — NOT in this sim-only tool; covered
//                    by the cave headless splits run (see docs/perf output).
//   entities       = N/A: the engine owns no entities (avatar is host-side
//                    GDScript, outside the deterministic sim contract).
// GPU splits (world / liquid / lighting / bloom / particles / composite)
// are not measurable from any engine/bridge/GDScript surface in this build
// (no per-pass GPU timestamps; headless runs on the dummy driver anyway).
// They are reported as uninstrumented in the E1 numbers table with the §17
// budgets used for validation only.
//
// CLI:
//   --scenario [empty|static|sand|liquid|fire|temp|boom|xchunk|all]
//   --ticks N     (default 300) measured ticks per repeat (per-tick samples)
//   --repeats R   (default 3) world rebuilds (samples pool across repeats)
//   --help
//
// All builders are deterministic (fixed geometry, fixed settle counts, no
// RNG — sim randomness is tick/cell-hashed). No sim rules are touched.

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
    int ticks = 300;
    int repeats = 3;
};

bool parse_int(const char* s, int& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end = nullptr;
    const long value = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || value < 0 || value > 100000) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

void print_usage() {
    std::printf("usage: bench_e1 [--scenario empty|static|sand|liquid|fire|temp|boom|xchunk|all] "
                "[--ticks N] [--repeats R]\n");
}

// Broad-combustion rule set (inline, scenario-C style): fire spreads into
// burnables (wood/oil), hot burnables auto-ignite, fire burns out. Hot fire
// (product_temperature 150) trips the heat-ignition gate (min 25), so the
// front is self-sustaining across the wood bed + oil pools.
bool load_combustion_rules(ashfall::SimulationWorld& world) {
    ashfall::ReactionTable table;
    std::string error;
    if (!ashfall::parse_reaction_data(
            R"JSON({"version":1,"reactions":[
              {"name":"spread","probability":40,
               "reactants":[{"material":"fire"},{"tag":"burnable"}],
               "products":[{"material":"fire"},{"material":"fire"}],
               "product_temperature":150},
              {"name":"oil-flash","probability":60,
               "reactants":[{"material":"fire"},{"material":"oil"}],
               "products":[{"material":"fire"},{"material":"fire"}],
               "product_temperature":150},
              {"name":"heat-ignition","probability":100,
               "reactants":[{"tag":"burnable"}],
               "products":[{"material":"fire"}],
               "temperature":{"min_temperature":25},
               "product_temperature":150},
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

// Scenario builders: geometry + canonical settle ticks. All 512^2.
bool build_scenario(const std::string& id, ashfall::SimulationWorld& world) {
    if (id == "empty") {
        // Mostly empty: fresh world, nothing placed. Exercises the
        // static fast path (non_static_count_ == 0).
        return true;
    }
    if (id == "static") {
        // Mostly static terrain: room + pillars + arch, zero dynamics.
        world.fill_rect({0, 500}, {511, 511}, ashfall::kStone);
        world.fill_rect({0, 100}, {16, 499}, ashfall::kStone);
        world.fill_rect({495, 100}, {511, 499}, ashfall::kStone);
        world.fill_rect({100, 300}, {140, 499}, ashfall::kStone);
        world.fill_rect({371, 300}, {411, 499}, ashfall::kStone);
        world.fill_rect({100, 260}, {411, 299}, ashfall::kStone);
        world.fill_rect({230, 350}, {281, 499}, ashfall::kRock);
        for (int i = 0; i < 10; ++i) {
            world.step();
        }
        return true;
    }
    if (id == "sand") {
        // Large falling-sand volume: 200x200 block suspended over a floor,
        // measured from the fresh collapse (no settle = worst case).
        world.fill_rect({0, 480}, {511, 511}, ashfall::kStone);
        world.fill_rect({156, 60}, {355, 259}, ashfall::kSand);
        return true;
    }
    if (id == "liquid") {
        // Large moving-liquid volume: B-class basin scaled up (~66k water),
        // measured fresh before it settles.
        world.fill_rect({0, 480}, {511, 511}, ashfall::kStone);
        world.fill_rect({40, 200}, {60, 479}, ashfall::kStone);
        world.fill_rect({451, 200}, {471, 479}, ashfall::kStone);
        world.fill_rect({61, 290}, {450, 470}, ashfall::kWater);
        return true;
    }
    if (id == "fire") {
        // Broad combustion: wood bed + oil pools + ignition strip, full
        // rule set above. No settle: the front spreads during measurement.
        if (!load_combustion_rules(world)) {
            return false;
        }
        world.fill_rect({0, 460}, {511, 511}, ashfall::kStone);
        world.fill_rect({100, 410}, {400, 450}, ashfall::kWood);
        world.fill_rect({120, 451}, {200, 459}, ashfall::kOil);
        world.fill_rect({310, 451}, {390, 459}, ashfall::kOil);
        world.fill_rect({100, 430}, {180, 440}, ashfall::kWood);
        world.fill_rect({100, 400}, {400, 409}, ashfall::kFire);
        return true;
    }
    if (id == "temp") {
        // Broad temperature activity: 300x300 stone mass with a 200x200
        // block heated to 500 (int16-clamped cell heat), so conduction
        // works a large region every tick. All static otherwise.
        world.fill_rect({100, 100}, {400, 400}, ashfall::kStone);
        for (int y = 150; y <= 349; ++y) {
            for (int x = 150; x <= 349; ++x) {
                if (!world.set_temperature({x, y}, 500)) {
                    std::printf("set_temperature rejected at (%d,%d)\n", x, y);
                    return false;
                }
            }
        }
        return true;
    }
    if (id == "boom") {
        // Repeated-explosion ground: stone terrain with wood/oil pockets;
        // the driver (run_scenario, "boom" only) carves an air sphere and
        // injects fire + heat at one of 4 sites every 20 ticks, outside the
        // timed span. Debris surge shows up as post-blast step spikes.
        if (!load_combustion_rules(world)) {
            return false;
        }
        world.fill_rect({0, 420}, {511, 511}, ashfall::kStone);
        world.fill_rect({80, 380}, {180, 419}, ashfall::kWood);
        world.fill_rect({330, 380}, {430, 419}, ashfall::kWood);
        world.fill_rect({220, 400}, {290, 419}, ashfall::kOil);
        world.fill_rect({200, 300}, {310, 399}, ashfall::kSand);
        for (int i = 0; i < 30; ++i) {
            world.step();
        }
        return true;
    }
    if (id == "xchunk") {
        // Chunk-boundary traversal: a full-width trough at y=300 with gaps
        // at x 100..130 / 250..280 / 380..410 forces sand + water to migrate
        // across many 64-cell chunk columns during measurement. The driver
        // tops up the left hopper every 60 ticks (outside the timed span).
        world.fill_rect({0, 500}, {511, 511}, ashfall::kStone);
        world.fill_rect({0, 300}, {99, 305}, ashfall::kStone);
        world.fill_rect({131, 300}, {249, 305}, ashfall::kStone);
        world.fill_rect({281, 300}, {379, 305}, ashfall::kStone);
        world.fill_rect({411, 300}, {511, 305}, ashfall::kStone);
        world.fill_rect({0, 100}, {20, 299}, ashfall::kStone);
        world.fill_rect({491, 100}, {511, 299}, ashfall::kStone);
        world.fill_rect({30, 40}, {90, 140}, ashfall::kSand);
        world.fill_rect({300, 310}, {480, 490}, ashfall::kWater);
        return true;
    }
    return false;
}

// Explosion injection (scenario "boom" only): deterministic cycling sites,
// air-sphere carve r=12 + fire ring + heat injection. Runs BETWEEN timed
// steps, never inside the measured span.
void inject_blast(ashfall::SimulationWorld& world, int site) {
    const int cx[4] = {130, 381, 255, 255};
    const int cy[4] = {400, 400, 410, 350};
    const int x0 = cx[site % 4], y0 = cy[site % 4];
    for (int y = y0 - 12; y <= y0 + 12; ++y) {
        for (int x = x0 - 12; x <= x0 + 12; ++x) {
            const int dx = x - x0, dy = y - y0;
            if (dx * dx + dy * dy > 144) {
                continue;
            }
            world.set_cell({x, y}, ashfall::kAir);
        }
    }
    for (int y = y0 - 14; y <= y0 + 14; ++y) {
        for (int x = x0 - 14; x <= x0 + 14; ++x) {
            const int dx = x - x0, dy = y - y0;
            const int d2 = dx * dx + dy * dy;
            if (d2 <= 144 || d2 > 196) {
                continue;
            }
            world.set_cell({x, y}, ashfall::kFire);
            world.set_temperature({x, y}, 300);
        }
    }
}

void top_up_hopper(ashfall::SimulationWorld& world) {
    world.fill_rect({30, 40}, {90, 90}, ashfall::kSand);
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n == 0) {
        return 0.0;
    }
    if (n % 2 == 1) {
        return values[n / 2];
    }
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

double p95_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    if (n == 0) {
        return 0.0;
    }
    std::size_t idx =
        static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    idx = idx == 0 ? 0 : idx - 1;
    if (idx >= n) {
        idx = n - 1;
    }
    return values[idx];
}

int run_scenario(const std::string& id, const Options& opts) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(opts.ticks * opts.repeats));
#ifdef ASHFALL_STEP_INSTRUMENT
    ashfall::StepTimings phase_sum;
    long phase_n = 0;
    double phase_max_total = 0.0;
#endif
    ashfall::SimulationWorld last_world;
    bool have_world = false;

    for (int r = 0; r < opts.repeats; ++r) {
        ashfall::SimulationWorld world;
        if (!build_scenario(id, world)) {
            return 1;
        }
        int blasts = 0;
        for (int t = 0; t < opts.ticks; ++t) {
            // Drivers run outside the timed span (edits are not step()).
            if (id == "boom" && t > 0 && t % 20 == 0) {
                inject_blast(world, blasts++);
            }
            if (id == "xchunk" && t > 0 && t % 60 == 0) {
                top_up_hopper(world);
            }
            const auto start = std::chrono::steady_clock::now();
            world.step();
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
#ifdef ASHFALL_STEP_INSTRUMENT
            const ashfall::StepTimings tm = world.last_step_timings();
            phase_sum.reactions_ms += tm.reactions_ms;
            phase_sum.clear_reserve_ms += tm.clear_reserve_ms;
            phase_sum.classify_ms += tm.classify_ms;
            phase_sum.ordering_ms += tm.ordering_ms;
            phase_sum.powder_ms += tm.powder_ms;
            phase_sum.liquid_ms += tm.liquid_ms;
            phase_sum.gas_ms += tm.gas_ms;
            phase_sum.reconcile_ms += tm.reconcile_ms;
            phase_sum.heat_carry_ms += tm.heat_carry_ms;
            phase_sum.conduct_ms += tm.conduct_ms;
            phase_sum.total_ms += tm.total_ms;
            ++phase_n;
            phase_max_total = std::max(phase_max_total, tm.total_ms);
#endif
        }
        last_world = std::move(world);
        have_world = true;
    }

    if (!have_world || samples.empty()) {
        return 1;
    }
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::printf("%-7s: median %8.3f ms/step p95 %8.3f worst %8.3f mean %8.3f "
                "(samples=%zu ticks=%d repeats=%d) occupied=%zu temps=%zu\n",
                id.c_str(), median_of(samples), p95_of(samples),
                sorted.back(),
                [&] {
                    double s = 0.0;
                    for (double v : samples) s += v;
                    return s / static_cast<double>(samples.size());
                }(),
                samples.size(), opts.ticks, opts.repeats,
                last_world.occupied_cell_count(), last_world.temperature_count());
#ifdef ASHFALL_STEP_INSTRUMENT
    const double n = static_cast<double>(phase_n);
    std::printf("    phases mean ms/step: reactions=%.3f clear_reserve=%.3f classify=%.3f "
                "ordering=%.3f powder=%.3f liquid=%.3f gas=%.3f reconcile=%.3f "
                "heat_carry=%.3f conduct=%.3f instrumented_total=%.3f (max %.3f)\n",
                phase_sum.reactions_ms / n, phase_sum.clear_reserve_ms / n,
                phase_sum.classify_ms / n, phase_sum.ordering_ms / n,
                phase_sum.powder_ms / n, phase_sum.liquid_ms / n,
                phase_sum.gas_ms / n, phase_sum.reconcile_ms / n,
                phase_sum.heat_carry_ms / n, phase_sum.conduct_ms / n,
                phase_sum.total_ms / n, phase_max_total);
    const ashfall::StepCounters c = last_world.last_step_counters();
    std::printf("    counters (last tick): occupied=%zu static=%zu dynamic=%zu powder_moves=%zu "
                "liquid_moves=%zu gas_moves=%zu swaps=%zu reaction_cells=%zu "
                "reaction_hits=%zu nonzero_temps=%zu carried_heat=%zu "
                "changed_positions=%zu dirty_chunk_marks=%zu\n",
                c.occupied, c.static_cells, c.dynamic_cells, c.powder_moves,
                c.liquid_moves, c.gas_moves, c.swaps, c.reaction_cells,
                c.reaction_hits, c.nonzero_temps, c.carried_heat,
                c.changed_positions, c.dirty_chunk_marks);
#else
    std::printf("    instrumentation: absent\n");
#endif
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
        } else {
            std::printf("unknown arg: %s\n", arg.c_str());
            print_usage();
            return 1;
        }
    }
    const std::vector<std::string> known = {"empty", "static", "sand", "liquid",
                                            "fire", "temp", "boom", "xchunk"};
    if (opts.scenario != "all" &&
        std::find(known.begin(), known.end(), opts.scenario) == known.end()) {
        std::printf("invalid --scenario (need one of empty|static|sand|liquid|fire|temp|boom|xchunk|all)\n");
        return 1;
    }

    std::vector<std::string> ids;
    if (opts.scenario == "all") {
        ids = known;
    } else {
        ids = {opts.scenario};
    }
    for (const std::string& id : ids) {
        if (run_scenario(id, opts) != 0) {
            return 1;
        }
    }
    return 0;
}

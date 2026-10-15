// Identity oracle scenario builders (Group 4, Task O0).
//
// Replicates bench/bench_step.cpp scenarios EXACTLY: same geometry, same
// settle ticks, same C reaction JSON literal. Setup + settle only, ready to
// measure — the record tool (bench/oracle_record.cpp) and the replay test
// (test_oracle_replay in tests/simulation_tests.cpp) share this header so
// the three can never drift apart. Pure setup code, no engine change.
//
// A: settled play level (settle 200). B: water basin (no settle).
// C: wood-fire + spread/burnout rules (no settle). D: static stone box
// (settle 10). E: hot-sand/cold-water swap pair, no settle (mirrors the
// test_heat_hot_powder_swaps_cold_liquid fixture: heated sand above settled
// water over stone — stresses heat advection + conduction). F:
// shared-reactant arbitration pair, no settle (mirrors the
// test_reaction_shared_reactant_double_transform fixture: two primaries
// sharing one neighbor reactant — stresses conflict resolution).
#pragma once

#include <string>

#include "ashfall/simulation.hpp"

namespace oracle {

// Measured ticks per scenario (record tool + replay test must agree).
constexpr int kMeasureTicksA = 200;
constexpr int kMeasureTicksB = 200;
constexpr int kMeasureTicksC = 200;
constexpr int kMeasureTicksD = 200;
constexpr int kMeasureTicksE = 20;
constexpr int kMeasureTicksF = 10;

inline bool build_oracle_A(ashfall::SimulationWorld& world) {
    world.build_play_level();
    for (int i = 0; i < 200; ++i) {
        world.step();
    }
    return true;
}

inline bool build_oracle_B(ashfall::SimulationWorld& world) {
    world.fill_rect({156, 320}, {356, 320}, ashfall::kStone);
    world.fill_rect({156, 200}, {156, 319}, ashfall::kStone);
    world.fill_rect({356, 200}, {356, 319}, ashfall::kStone);
    world.fill_rect({157, 200}, {355, 260}, ashfall::kWater);
    return true;
}

inline bool build_oracle_C(ashfall::SimulationWorld& world) {
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
        return false;
    }
    if (!world.load_reactions(table)) {
        return false;
    }
    return true;
}

inline bool build_oracle_D(ashfall::SimulationWorld& world) {
    world.fill_rect({100, 100}, {400, 400}, ashfall::kStone);
    for (int i = 0; i < 10; ++i) {
        world.step();
    }
    return true;
}

inline bool build_oracle_E(ashfall::SimulationWorld& world) {
    world.fill_rect({-1, 1}, {1, 1}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({0, -1}, ashfall::kSand);
    world.set_temperature({0, -1}, 100);
    return true;
}

inline bool build_oracle_F(ashfall::SimulationWorld& world) {
    world.set_cell({0, 0}, ashfall::kWood);
    world.set_cell({1, 0}, ashfall::kStone);
    world.set_cell({2, 0}, ashfall::kRock);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec left;
    left.name = "left-claim";
    left.probability = 100;
    left.reactants = {{false, "wood"}, {false, "stone"}};
    left.products = {{"rock"}, {"rock"}};
    ashfall::ReactionSpec right;
    right.name = "right-claim";
    right.probability = 100;
    right.reactants = {{false, "rock"}, {false, "stone"}};
    right.products = {{"wood"}, {"wood"}};
    table.reactions = {left, right};
    if (!world.load_reactions(table)) {
        return false;
    }
    return true;
}

} // namespace oracle

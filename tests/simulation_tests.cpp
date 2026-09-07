#include "ashfall/simulation.hpp"

#include <cstdlib>
#include <iostream>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

void test_sand_falls_and_stops() {
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 3}, {1, 3}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kSand);
    world.step(); expect(world.cell({0, 1}) == ashfall::kSand, "sand falls one cell per tick");
    world.step(); expect(world.cell({0, 2}) == ashfall::kSand, "sand reaches floor");
    world.step(); expect(world.cell({0, 2}) == ashfall::kSand, "sand remains supported");
}

void test_water_spreads_when_supported() {
    ashfall::SimulationWorld world;
    world.fill_rect({-2, 2}, {2, 2}, ashfall::kStone);
    world.set_cell({0, 1}, ashfall::kWater);
    world.step();
    expect(world.cell({-1, 1}) == ashfall::kWater || world.cell({1, 1}) == ashfall::kWater, "water spreads sideways when supported");
}

void test_chunks_work_across_negative_boundary() {
    ashfall::SimulationWorld world;
    world.set_cell({-64, 2}, ashfall::kStone);
    world.set_cell({-64, 0}, ashfall::kSand);
    world.step();
    expect(world.cell({-64, 1}) == ashfall::kSand, "negative chunk coordinates preserve placement");
}

void test_save_load_is_exact() {
    ashfall::SimulationWorld source;
    source.fill_rect({-1, 4}, {1, 4}, ashfall::kStone);
    source.set_cell({0, 0}, ashfall::kSand);
    source.set_cell({1, 1}, ashfall::kWater);
    source.step();
    const auto saved = source.save();
    ashfall::SimulationWorld restored;
    expect(restored.load(saved), "valid save loads");
    expect(restored.save() == saved, "save/load preserves exact state");
    expect(!restored.load({}), "invalid save is rejected");
}
} // namespace

int main() {
    test_sand_falls_and_stops();
    test_water_spreads_when_supported();
    test_chunks_work_across_negative_boundary();
    test_save_load_is_exact();
    std::cout << "All simulation tests passed.\n";
}

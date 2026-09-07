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

void test_chunk_and_rect_reading() {
    ashfall::SimulationWorld world;
    expect(!world.has_chunk({0, 0}), "unallocated chunk returns false");
    expect(world.active_chunks().empty(), "initially no active chunks");

    world.set_cell({5, 10}, ashfall::kSand);
    expect(world.has_chunk({0, 0}), "allocated chunk is recognized");
    const auto active = world.active_chunks();
    expect(active.size() == 1 && active[0].x == 0 && active[0].y == 0, "active chunks lists chunk {0, 0}");

    std::vector<ashfall::MaterialId> chunk_buffer(ashfall::kChunkSize * ashfall::kChunkSize, ashfall::kWater);
    expect(world.read_chunk({0, 0}, chunk_buffer), "read_chunk succeeds");
    expect(chunk_buffer[10 * ashfall::kChunkSize + 5] == ashfall::kSand, "cell value read in chunk buffer");
    expect(chunk_buffer[0] == ashfall::kAir, "unmodified cells in chunk are air");

    // Reading unallocated chunk fills buffer with air
    std::vector<ashfall::MaterialId> empty_chunk(ashfall::kChunkSize * ashfall::kChunkSize, ashfall::kSand);
    expect(world.read_chunk({1, 1}, empty_chunk), "read unallocated chunk succeeds");
    expect(empty_chunk[0] == ashfall::kAir, "unallocated chunk filled with air");

    // copy_rect test across a 4x4 area
    std::vector<ashfall::MaterialId> rect_buffer(16, ashfall::kAir);
    world.fill_rect({0, 0}, {3, 3}, ashfall::kStone);
    world.copy_rect({0, 0}, {3, 3}, rect_buffer);
    for (std::size_t i = 0; i < 16; ++i) {
        expect(rect_buffer[i] == ashfall::kStone, "rect cells contain stone");
    }
}
} // namespace

int main() {
    test_sand_falls_and_stops();
    test_water_spreads_when_supported();
    test_chunks_work_across_negative_boundary();
    test_save_load_is_exact();
    test_chunk_and_rect_reading();
    std::cout << "All simulation tests passed.\n";
}

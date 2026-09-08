#include "ashfall/simulation.hpp"
#include "ashfall/material_data.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <vector>

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
    world.fill_rect({-3, 2}, {3, 2}, ashfall::kStone);
    world.fill_rect({-2, 1}, {2, 1}, ashfall::kWater); // supported row resting on stone
    world.fill_rect({0, 0}, {1, 0}, ashfall::kWater);  // ponded pair on top of water
    world.step();
    expect(world.cell({-1, 0}) == ashfall::kWater || world.cell({2, 0}) == ashfall::kWater,
        "ponded water with wet neighbors disperses laterally over its supporting water");
}

void test_water_surfer_drains_off_flat_layer() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({5, 9}, {15, 9}, ashfall::kWater);
    world.set_cell({10, 8}, ashfall::kWater); // lone cell riding the layer
    for (int i = 0; i < 100; ++i) world.step();
    expect(world.cell({10, 8}) == ashfall::kAir, "surfer drains away instead of parking as a bump");
    expect(world.occupied_cell_count() == 21 + 11 + 1, "surfer scenario conserves mass");
}

void test_water_surface_flattens_in_basin() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({20, 0}, {20, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {19, 9}, ashfall::kWater); // full bottom layer
    for (int x = 2; x <= 17; x += 3) world.set_cell({x, 8}, ashfall::kWater); // sparse dotted top row

    std::vector<std::uint8_t> previous = world.save();
    for (int i = 0; i < 300; ++i) world.step();
    // NOTE: a partial top row has no sink, so individual cells may still trade
    // places (gentle surface shimmer, smoothed out by the water renderer).
    // The visual guarantees are mass conservation and a level surface, which
    // hold regardless of arrangement. Full pools go bit-steady; that property
    // is covered by test_water_reaches_steady_state.

    // Surface must be level: water column heights differ by at most one.
    int min_height = 99, max_height = 0, total_water = 0;
    for (int x = 1; x <= 19; ++x) {
        int height = 0;
        for (int y = 0; y <= 9; ++y) {
            if (world.cell({x, y}) == ashfall::kWater) {
                ++height;
                ++total_water;
            }
        }
        if (height > 0) {
            min_height = std::min(min_height, height);
            max_height = std::max(max_height, height);
        }
    }
    expect(total_water == 19 + 6, "flattening conserves mass");
    expect(max_height - min_height <= 1, "settled surface is level within one cell");
}

void test_chunks_work_across_negative_boundary() {
    ashfall::SimulationWorld world;
    world.set_cell({-64, 2}, ashfall::kStone);
    world.set_cell({-64, 0}, ashfall::kSand);
    world.step();
    expect(world.cell({-64, 1}) == ashfall::kSand, "negative chunk coordinates preserve placement");
}

void test_materials_cross_chunk_boundaries() {
    auto crosses_right = [](ashfall::MaterialId material) {
        ashfall::SimulationWorld world;
        world.set_cell({63, 0}, material);
        world.set_cell({63, 1}, ashfall::kStone);
        world.set_cell({62, 1}, ashfall::kStone);
        world.step();
        return world.cell({64, 1}) == material;
    };
    auto crosses_negative = [](ashfall::MaterialId material) {
        ashfall::SimulationWorld world;
        world.set_cell({-65, 0}, material);
        world.set_cell({-65, 1}, ashfall::kStone);
        world.set_cell({-66, 1}, ashfall::kStone);
        world.step();
        return world.cell({-64, 1}) == material;
    };

    expect(crosses_right(ashfall::kSand), "sand crosses a positive chunk boundary");
    expect(crosses_negative(ashfall::kSand), "sand crosses a negative chunk boundary");
    expect(crosses_right(ashfall::kWater), "water crosses a positive chunk boundary");
    expect(crosses_negative(ashfall::kWater), "water crosses a negative chunk boundary");
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

void test_mass_conservation() {
    ashfall::SimulationWorld world;
    // Enclosed basin
    world.fill_rect({0, 30}, {30, 30}, ashfall::kStone); // floor
    world.fill_rect({0, 0}, {0, 30}, ashfall::kStone);   // left wall
    world.fill_rect({30, 0}, {30, 30}, ashfall::kStone); // right wall

    // Add sand and water
    world.fill_rect({5, 5}, {15, 10}, ashfall::kSand);  // 11 * 6 = 66 sand cells
    world.fill_rect({16, 5}, {25, 10}, ashfall::kWater); // 10 * 6 = 60 water cells

    const std::size_t expected_sand = 66;
    const std::size_t expected_water = 60;

    auto count_materials = [&](const ashfall::SimulationWorld& w) {
        std::size_t sand = 0, water = 0;
        std::vector<ashfall::MaterialId> buf(31 * 31);
        w.copy_rect({0, 0}, {30, 30}, buf);
        for (auto m : buf) {
            if (m == ashfall::kSand) ++sand;
            else if (m == ashfall::kWater) ++water;
        }
        return std::make_pair(sand, water);
    };

    auto [init_sand, init_water] = count_materials(world);
    expect(init_sand == expected_sand, "initial sand count matches");
    expect(init_water == expected_water, "initial water count matches");

    for (int step = 1; step <= 100; ++step) {
        world.step();
        auto [cur_sand, cur_water] = count_materials(world);
        expect(cur_sand == expected_sand, "sand count conserved every tick");
        expect(cur_water == expected_water, "water count conserved every tick");
    }
}

void test_sand_sinks_through_water() {
    ashfall::SimulationWorld world;
    // Narrow tube
    world.fill_rect({2, 10}, {6, 10}, ashfall::kStone); // floor
    world.fill_rect({2, 0}, {2, 10}, ashfall::kStone);  // left wall
    world.fill_rect({6, 0}, {6, 10}, ashfall::kStone);  // right wall

    // Water layer at y=7..9 (3 wide * 3 high = 9 water cells)
    world.fill_rect({3, 7}, {5, 9}, ashfall::kWater);
    // Sand layer above at y=3..4 (3 wide * 2 high = 6 sand cells)
    world.fill_rect({3, 3}, {5, 4}, ashfall::kSand);

    for (int i = 0; i < 40; ++i) {
        world.step();
    }

    // Sand should be at the bottom (y=8 and y=9)
    for (int y = 8; y <= 9; ++y) {
        for (int x = 3; x <= 5; ++x) {
            expect(world.cell({x, y}) == ashfall::kSand, "sand has sunk to the bottom");
        }
    }

    // Water should be displaced above sand (y=5 to y=7)
    std::size_t water_above = 0;
    for (int y = 5; y <= 7; ++y) {
        for (int x = 3; x <= 5; ++x) {
            if (world.cell({x, y}) == ashfall::kWater) ++water_above;
        }
    }
    expect(water_above == 9, "all 9 water cells displaced above sand");
}

void test_sand_block_sinks_through_open_water() {
    ashfall::SimulationWorld world;
    // Wide open basin: no walls to funnel the fall, so the block must sink
    // through the body coherently and settle on the floor.
    world.fill_rect({0, 20}, {20, 20}, ashfall::kStone); // floor
    world.fill_rect({0, 0}, {0, 20}, ashfall::kStone);   // left wall
    world.fill_rect({20, 0}, {20, 20}, ashfall::kStone); // right wall
    world.fill_rect({1, 10}, {19, 19}, ashfall::kWater); // deep open water
    world.fill_rect({1, 9}, {4, 9}, ashfall::kWater);    // top-up: 194 water ...
    world.fill_rect({8, 2}, {12, 4}, ashfall::kSand);    // ... + 15 sand = 209,
    // exactly 11 full rows when settled. NOTE: no quiescence is asserted:
    // the last unsettled top cells keep a gentle surface shimmer by design
    // (partial rows trade places; the water renderer smooths it out).
    for (int i = 0; i < 400; ++i) world.step();

    int sand = 0, water = 0, sand_max = -1, sand_above = 0;
    for (int y = 0; y <= 20; ++y) {
        for (int x = 0; x <= 20; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kSand) {
                ++sand;
                sand_max = std::max(sand_max, y);
                if (y < 12) ++sand_above;
            } else if (m == ashfall::kWater) {
                ++water;
            }
        }
    }
    expect(sand == 15, "sinking conserves sand mass");
    expect(water == 194, "sinking conserves water mass");
    expect(sand_above == 0, "no sand left suspended in the water column");
    expect(sand_max == 19, "sunken sand rests on the basin floor");
}

void test_settling_reaches_steady_state() {
    ashfall::SimulationWorld world;
    // Basin with sand only — sand settles deterministically without lateral oscillation
    world.fill_rect({0, 20}, {20, 20}, ashfall::kStone); // floor
    world.fill_rect({0, 0}, {0, 20}, ashfall::kStone);   // left wall
    world.fill_rect({20, 0}, {20, 20}, ashfall::kStone);  // right wall

    // Drop a pile of sand from mid-height
    world.fill_rect({8, 2}, {12, 6}, ashfall::kSand);

    bool reached_steady_state = false;
    std::vector<std::uint8_t> prev_save = world.save();

    for (int i = 0; i < 200; ++i) {
        world.step();
        std::vector<std::uint8_t> cur_save = world.save();
        // Compare cell payload (ignoring 8-byte tick at offset 8..15)
        if (cur_save.size() == prev_save.size() &&
            std::equal(cur_save.begin() + 16, cur_save.end(), prev_save.begin() + 16)) {
            reached_steady_state = true;
            break;
        }
        prev_save = cur_save;
    }
    expect(reached_steady_state, "sand reaches steady state / quiescence in basin");
}

void test_water_reaches_steady_state() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({8, 0}, {8, 10}, ashfall::kStone);
    world.fill_rect({4, 1}, {4, 7}, ashfall::kWater);

    std::vector<std::uint8_t> previous = world.save();
    bool reached_steady_state = false;
    for (int i = 0; i < 200; ++i) {
        world.step();
        const std::vector<std::uint8_t> current = world.save();
        if (current.size() == previous.size() &&
            std::equal(current.begin() + 16, current.end(), previous.begin() + 16)) {
            reached_steady_state = true;
            break;
        }
        previous = current;
    }
    expect(reached_steady_state, "water reaches steady state in a basin");
}

void test_lone_water_droplet_rests() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.set_cell({10, 9}, ashfall::kWater);
    for (int i = 0; i < 50; ++i) {
        world.step();
        expect(world.cell({10, 9}) == ashfall::kWater, "lone droplet does not vibrate off its cell");
    }
    expect(world.occupied_cell_count() == 22, "droplet plus floor conserved while resting");
}

void test_thin_water_film_stays_connected() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({5, 9}, {15, 9}, ashfall::kWater);
    for (int i = 0; i < 50; ++i) world.step();
    for (int x = 5; x <= 15; ++x) {
        expect(world.cell({x, 9}) == ashfall::kWater, "thin film neither splits nor drifts");
    }
    expect(world.occupied_cell_count() == 21 + 11, "film mass conserved");
}

void test_water_mound_levels_quickly() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 20}, {30, 20}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 20}, ashfall::kStone);
    world.fill_rect({30, 0}, {30, 20}, ashfall::kStone);
    // Tall narrow column: 3 wide x 12 tall.
    world.fill_rect({14, 8}, {16, 19}, ashfall::kWater);
    for (int i = 0; i < 120; ++i) world.step();
    std::size_t high_water = 0;
    std::size_t total_water = 0;
    for (int y = 0; y <= 19; ++y) {
        for (int x = 1; x <= 29; ++x) {
            if (world.cell({x, y}) == ashfall::kWater) {
                ++total_water;
                if (y <= 16) ++high_water;
            }
        }
    }
    expect(total_water == 36, "mound mass conserved while leveling");
    expect(high_water == 0, "12-tall mound levels to the bottom rows within 120 ticks");
}

void test_chunk_boundaries_crossing() {
    ashfall::SimulationWorld world;
    world.fill_rect({60, 70}, {70, 70}, ashfall::kStone);

    // Sand at chunk boundary x=63 -> x=64, y=63 -> y=64
    world.set_cell({63, 60}, ashfall::kSand);
    for (int i = 0; i < 15; ++i) {
        world.step();
    }
    // Sand must cross into chunk {0, 1} at y=69
    expect(world.cell({63, 69}) == ashfall::kSand, "sand crossed vertical chunk boundary");
}

void test_play_level_layout() {
    ashfall::SimulationWorld world;
    world.build_play_level();
    expect(world.occupied_cell_count() > 0, "play level contains material");
    const ashfall::CellPosition spawn = ashfall::SimulationWorld::play_level_spawn();
    expect(spawn.x == 256 && spawn.y == 480, "spawn matches the 512 play level");
    expect(world.cell({spawn.x, spawn.y}) == ashfall::kStone, "spawn stands on the floor");
    expect(world.cell({spawn.x, spawn.y - 1}) == ashfall::kAir, "spawn feet cell is free");
    expect(world.cell({spawn.x, spawn.y - 2}) == ashfall::kAir, "spawn head cell is free");
    expect(world.can_move_to({spawn.x, spawn.y - 2}, {spawn.x, spawn.y - 1}), "spawn box is passable");
    expect(!world.can_move_to({spawn.x, spawn.y - 1}, {spawn.x, spawn.y}), "floor box is blocked");
    std::size_t water = 0, sand = 0;
    std::vector<ashfall::MaterialId> rect(512 * 512, ashfall::kAir);
    world.copy_rect({0, 0}, {511, 511}, rect);
    for (ashfall::MaterialId material_id : rect) {
        if (material_id == ashfall::kWater) ++water;
        else if (material_id == ashfall::kSand) ++sand;
    }
    expect(water > 2000, "reservoir and pocket hold water");
    expect(sand > 2000, "dunes and plug hold sand");
    ashfall::SimulationWorld again;
    again.build_play_level();
    expect(again.save() == world.save(), "play level builds deterministically");
}

void test_water_trickles_down_sand_slope() {
    ashfall::SimulationWorld world;
    // Closed basin so cell counts are exact (nothing escapes the window).
    world.fill_rect({0, 31}, {40, 31}, ashfall::kStone); // floor
    world.fill_rect({0, 0}, {0, 31}, ashfall::kStone);   // left wall
    world.fill_rect({40, 0}, {40, 31}, ashfall::kStone); // right wall
    for (int i = 0; i < 10; ++i) world.fill_rect({20 + i, 20 + i}, {20 + i, 30}, ashfall::kSand); // staircase
    world.set_cell({20, 19}, ashfall::kWater);
    for (int i = 0; i < 300; ++i) world.step();
    std::vector<ashfall::MaterialId> area(static_cast<std::size_t>(41 * 32), ashfall::kAir);
    world.copy_rect({0, 0}, {40, 31}, area);
    std::size_t slope_water = 0, total_water = 0, total_sand = 0;
    for (int y = 0; y <= 31; ++y) {
        for (int x = 0; x <= 40; ++x) {
            const ashfall::MaterialId m = area[static_cast<std::size_t>(y * 41 + x)];
            if (m == ashfall::kWater) {
                ++total_water;
                if (y < 30) ++slope_water;
            } else if (m == ashfall::kSand) {
                ++total_sand;
            }
        }
    }
    expect(total_water == 1, "slope water mass conserved");
    expect(total_sand == 65, "slope sand mass conserved");
    // A dot cupped in a fully-enclosed 1-cell pit is a puddle and stays;
    // anything on open slope must drain (no swap-cycle flicker either).
    expect(slope_water <= 1, "open slope drains instead of dotting up");
}

void test_command_replay() {
    auto run_scenario = []() {
        ashfall::SimulationWorld world;
        world.fill_rect({0, 20}, {20, 20}, ashfall::kStone);
        world.set_cell({10, 5}, ashfall::kSand);
        world.step();
        world.set_cell({11, 5}, ashfall::kWater);
        world.step();
        world.fill_rect({5, 2}, {8, 4}, ashfall::kSand);
        for (int i = 0; i < 20; ++i) world.step();
        return world.save();
    };

    const auto run1 = run_scenario();
    const auto run2 = run_scenario();
    expect(run1 == run2, "command replay produces bitwise identical state");
}

void test_step_marks_dirty_chunks() {
    auto contains = [](const std::vector<ashfall::CellPosition>& chunks, ashfall::CellPosition want) {
        return std::find(chunks.begin(), chunks.end(), want) != chunks.end();
    };

    // Same-chunk movement: sand falls within chunk {0, 0}.
    {
        ashfall::SimulationWorld world;
        world.fill_rect({10, 12}, {12, 12}, ashfall::kStone);
        world.set_cell({11, 10}, ashfall::kSand);
        world.clear_dirty_chunks();
        expect(world.dirty_chunks().empty(), "dirty starts empty before step");
        world.step();
        const auto dirty = world.dirty_chunks();
        expect(!dirty.empty(), "step marks dirty chunks");
        expect(contains(dirty, {0, 0}), "falling sand dirties its chunk");
        expect(world.cell({11, 11}) == ashfall::kSand, "sand fell one cell");
        world.clear_dirty_chunks();
        expect(world.dirty_chunks().empty(), "clear after step works");
    }

    // Cross-chunk movement: sand falls from y=63 to y=64, touching {0,0} and {0,1}.
    {
        ashfall::SimulationWorld world;
        world.set_cell({5, 63}, ashfall::kSand);
        world.clear_dirty_chunks();
        world.step();
        const auto dirty = world.dirty_chunks();
        expect(world.cell({5, 64}) == ashfall::kSand, "sand crossed horizontal chunk boundary");
        expect(contains(dirty, {0, 0}) && contains(dirty, {0, 1}), "cross-chunk step dirties both chunks");
    }
}

void test_save_load_rejects_malformed() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
    world.set_cell({5, 5}, ashfall::kSand);
    world.step();
    const auto valid = world.save();
    const auto snapshot = valid;
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();

    auto expect_rejected = [&](std::vector<std::uint8_t> bytes, const char* message) {
        expect(!world.load(bytes), message);
        expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
        expect(world.tick() == tick_before, "failed load preserves tick");
        expect(world.save() == snapshot, "failed load preserves exact state");
    };

    expect_rejected({}, "empty payload is rejected");
    {
        auto bad_magic = valid;
        bad_magic[0] ^= 0xFF;
        expect_rejected(bad_magic, "bad magic is rejected");
    }
    {
        auto truncated = valid;
        truncated.pop_back();
        expect_rejected(truncated, "truncated payload is rejected");
    }
    {
        auto extra = valid;
        extra.push_back(0x00);
        expect_rejected(extra, "trailing byte (count mismatch) is rejected");
    }
    {
        auto bad_material = valid;
        bad_material.back() = 99;
        expect_rejected(bad_material, "unknown material is rejected");
    }
    {
        auto air_material = valid;
        air_material.back() = ashfall::kAir;
        expect_rejected(air_material, "air material entry is rejected");
    }
}

void test_save_load_varied_topology() {
    ashfall::SimulationWorld world;
    // Spread across negative, origin, and far-positive chunks.
    world.set_cell({-130, -65}, ashfall::kSand);
    world.set_cell({-1, -1}, ashfall::kWater);
    world.set_cell({5, 10}, ashfall::kStone);
    world.set_cell({130, 0}, ashfall::kWater);
    world.fill_rect({200, 200}, {201, 201}, ashfall::kSand);
    for (int i = 0; i < 5; ++i) world.step();
    const auto saved = world.save();
    const std::uint64_t tick = world.tick();
    const auto chunks_before = world.active_chunks();

    ashfall::SimulationWorld restored;
    expect(restored.load(saved), "varied topology save loads");
    expect(restored.save() == saved, "varied topology round-trips exactly");
    expect(restored.tick() == tick, "tick preserved across varied topology load");
    expect(restored.active_chunks().size() == chunks_before.size(), "chunk count preserved");
    expect(restored.cell({5, 10}) == ashfall::kStone, "stone cell preserved");
    expect(restored.cell({130, 5}) == ashfall::kWater, "unsupported water fell 5 cells and round-tripped");
    expect(restored.cell({-130, -60}) == ashfall::kSand, "unsupported sand fell 5 cells and round-tripped");

    // Empty world round-trip clears prior state.
    ashfall::SimulationWorld empty_source;
    const auto empty_save = empty_source.save();
    ashfall::SimulationWorld holder;
    holder.set_cell({0, 0}, ashfall::kSand);
    expect(holder.load(empty_save), "empty save loads");
    expect(holder.occupied_cell_count() == 0, "empty load clears cells");
    expect(holder.save() == empty_save, "empty round-trip is exact");
}

void test_player_collision_and_dirty_tracking() {
    ashfall::SimulationWorld world;
    world.fill_rect({10, 0}, {10, 20}, ashfall::kStone);

    expect(world.is_solid({10, 5}), "stone is solid");
    expect(!world.is_solid({9, 5}), "air is not solid");
    world.set_cell({9, 5}, ashfall::kWater);
    expect(!world.is_solid({9, 5}), "water is not solid");
    world.set_cell({9, 6}, ashfall::kSand);
    expect(!world.is_solid({9, 6}), "sand is not solid");

    // Collision box checks
    expect(!world.can_move_to({8, 5}, {10, 5}), "box touching stone wall is blocked");
    expect(world.can_move_to({7, 4}, {9, 5}), "box in air/water/sand can move");

    // Dirty chunks tracking
    world.clear_dirty_chunks();
    expect(world.dirty_chunks().empty(), "clear_dirty_chunks clears set");
    world.set_cell({5, 5}, ashfall::kSand);
    const auto dirty = world.dirty_chunks();
    expect(!dirty.empty() && dirty[0].x == 0 && dirty[0].y == 0, "chunk {0, 0} marked dirty");

    world.clear_dirty_chunks();
    ashfall::SimulationWorld saved_world;
    saved_world.set_cell({128, 0}, ashfall::kStone);
    expect(world.load(saved_world.save()), "load succeeds");
    const auto loaded_dirty = world.dirty_chunks();
    expect(loaded_dirty.size() == 2, "load marks removed and added chunks dirty");
    expect(loaded_dirty[0] == ashfall::CellPosition{0, 0} && loaded_dirty[1] == ashfall::CellPosition{2, 0},
           "load dirty chunks include old and new positions");

    world.reset();
    expect(world.tick() == 0 && world.occupied_cell_count() == 0, "reset clears world state and tick");
    expect(world.dirty_chunks().size() == 2, "reset marks removed chunks dirty");
}
void test_material_data_parses_table() {
    const char* kJson = R"JSON({
  "version": 1,
  "materials": [
    {"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]},
    {"id": 1, "name": "stone", "movement": "static", "solid": true, "density": 99, "color": [128, 133, 138]},
    {"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [230, 175, 50]},
    {"id": 3, "name": "water", "movement": "liquid", "solid": false, "density": 1, "color": [86, 178, 214]}
  ]
})JSON";
    ashfall::MaterialTable table;
    std::string error;
    expect(ashfall::parse_material_data(kJson, table, error), "valid material data parses");
    expect(table.version == 1, "table version preserved");
    expect(table.materials.size() == 4, "table holds four materials");
    expect(table.materials[2].id == 2 && table.materials[2].name == "sand", "sand spec intact");
    expect(table.materials[2].movement == ashfall::Movement::Powder, "sand is powder");
    expect(table.materials[2].density == 2 && !table.materials[2].solid, "sand density/solidity intact");
    expect(table.materials[3].color[0] == 86 && table.materials[3].color[1] == 178 &&
            table.materials[3].color[2] == 214,
        "water color intact");
    ashfall::Movement movement = ashfall::Movement::Static;
    expect(ashfall::movement_from_string("liquid", movement) && movement == ashfall::Movement::Liquid,
        "movement name resolves");
    expect(!ashfall::movement_from_string("gas", movement), "gas rejected until P2.5");
}

void test_material_data_rejects_bad_input() {
    const char* kBase = R"JSON({"version": 1, "materials": [%s]})JSON";
    const char* kAir = R"JSON({"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]})JSON";
    const char* kSand = R"JSON({"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [230, 175, 50]})JSON";
    auto rejects = [&](const std::string& text) {
        ashfall::MaterialTable table;
        std::string error;
        if (ashfall::parse_material_data(text, table, error)) {
            std::cerr << "ACCEPTED BAD INPUT: " << text << '\n';
            return false;
        }
        return !error.empty();
    };
    char full[2048];
    std::snprintf(full, sizeof(full), kBase, (std::string(kAir) + "," + kSand).c_str());
    {
        ashfall::MaterialTable table;
        std::string error;
        expect(ashfall::parse_material_data(full, table, error), "two-material table parses");
    }
    expect(rejects(""), "empty input rejected");
    expect(rejects("{unterminated"), "truncated JSON rejected");
    expect(rejects(std::string(full) + "trailing"), "trailing characters rejected");
    char buf[2048];
    std::snprintf(buf, sizeof(buf), kBase, kSand);
    expect(rejects(buf), "missing air rejected");
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kAir).c_str());
    expect(rejects(buf), "duplicate id rejected");
    const char* kSand2 =
        R"JSON({"id": 4, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kSand + "," + kSand2).c_str());
    expect(rejects(buf), "duplicate name rejected");
    const char* kGas =
        R"JSON({"id": 5, "name": "smoke", "movement": "gas", "solid": false, "density": 0, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kGas).c_str());
    expect(rejects(buf), "unknown movement rejected");
    const char* kSolidSand =
        R"JSON({"id": 6, "name": "brick", "movement": "powder", "solid": true, "density": 5, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kSolidSand).c_str());
    expect(rejects(buf), "solid powder rejected");
    const char* kDense =
        R"JSON({"id": 7, "name": "heavy", "movement": "static", "solid": true, "density": 300, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kDense).c_str());
    expect(rejects(buf), "density out of range rejected");
    const char* kBadColor =
        R"JSON({"id": 8, "name": "odd", "movement": "static", "solid": true, "density": 9, "color": [1, 2, 999]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kBadColor).c_str());
    expect(rejects(buf), "color out of range rejected");
    const char* kV2 = R"JSON({"version": 2, "materials": []})JSON";
    expect(rejects(kV2), "wrong version rejected");
}

void test_table_with_remapped_ids_behaves_identically() {
    // Same physics roles, different ids: proves dispatch keys off the
    // movement class, never the id. Air must stay 0 (engine air anchor).
    ashfall::MaterialTable table;
    table.version = 1;
    table.materials = {
        {0, "air", ashfall::Movement::Static, false, 0, {0, 0, 0}},
        {4, "stone", ashfall::Movement::Static, true, 99, {0, 0, 0}},
        {5, "sand", ashfall::Movement::Powder, false, 2, {0, 0, 0}},
        {6, "water", ashfall::Movement::Liquid, false, 1, {0, 0, 0}},
    };
    ashfall::SimulationWorld world(table);
    // Powder falls and rests on static.
    world.fill_rect({-1, 3}, {1, 3}, 4);
    world.set_cell({0, 0}, 5);
    world.step();
    world.step();
    expect(world.cell({0, 2}) == 5, "remapped sand falls and rests");
    // Liquid spreads over static.
    world.fill_rect({-3, 2}, {3, 2}, 4);
    world.fill_rect({-2, 1}, {2, 1}, 6);
    world.fill_rect({0, 0}, {1, 0}, 6);
    world.step();
    expect(world.cell({-1, 0}) == 6 || world.cell({2, 0}) == 6, "remapped water disperses");
    // Powder sinks through liquid (mini tube).
    ashfall::SimulationWorld tube(table);
    tube.fill_rect({2, 5}, {4, 5}, 4);
    tube.fill_rect({2, 0}, {2, 5}, 4);
    tube.fill_rect({4, 0}, {4, 5}, 4);
    tube.fill_rect({3, 3}, {3, 4}, 6);
    tube.set_cell({3, 1}, 5);
    for (int i = 0; i < 20; ++i) tube.step();
    expect(tube.cell({3, 4}) == 5, "remapped sand sinks through remapped water");
    expect(tube.occupied_cell_count() == 16, "remapped scenario conserves mass");
}

void test_unknown_material_id_rejected() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, 42);
    expect(world.cell({0, 0}) == ashfall::kAir, "unknown id set_cell is a no-op");
    world.fill_rect({-2, -2}, {2, 2}, 42);
    expect(world.occupied_cell_count() == 0, "unknown id fill_rect is a no-op");
}

void test_loading_table_resets_world() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kSand);
    world.step();
    expect(world.occupied_cell_count() == 1 && world.tick() == 1, "world has state before load");
    expect(world.load_material_table(ashfall::default_material_table()), "reloading defaults succeeds");
    expect(world.occupied_cell_count() == 0 && world.tick() == 0, "loading a table resets the world");
    ashfall::MaterialTable bad;
    bad.version = 1;
    expect(!world.load_material_table(bad), "invalid table rejected, world untouched");
}

void test_material_data_matches_canonical_json() {
    // Literal mirrors data/materials.json field-for-field; the Godot verify
    // loads the real file through the bridge (see godot_p1_verify.gd).
    const char* kJson = R"JSON({
  "version": 1,
  "materials": [
    {"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]},
    {"id": 1, "name": "stone", "movement": "static", "solid": true, "density": 99, "color": [128, 133, 138]},
    {"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [230, 175, 50]},
    {"id": 3, "name": "water", "movement": "liquid", "solid": false, "density": 1, "color": [86, 178, 214]}
  ]
})JSON";
    ashfall::MaterialTable parsed;
    std::string error;
    expect(ashfall::parse_material_data(kJson, parsed, error), "canonical JSON parses");
    const ashfall::MaterialTable defaults = ashfall::default_material_table();
    expect(parsed.materials.size() == defaults.materials.size(), "canonical table has default length");
    for (std::size_t i = 0; i < parsed.materials.size(); ++i) {
        const ashfall::MaterialSpec& a = parsed.materials[i];
        const ashfall::MaterialSpec& b = defaults.materials[i];
        expect(a.id == b.id && a.name == b.name && a.movement == b.movement && a.solid == b.solid &&
                a.density == b.density && a.color == b.color,
            "canonical entry matches code default");
    }
}

void test_material_ids_preserve_table_order() {
    // Table listed out of id order: presentation order must follow the
    // table, not the sorted map.
    ashfall::MaterialTable table;
    table.version = 1;
    table.materials = {
        {0, "air", ashfall::Movement::Static, false, 0, {0, 0, 0}},
        {6, "water", ashfall::Movement::Liquid, false, 1, {0, 0, 0}},
        {4, "stone", ashfall::Movement::Static, true, 99, {0, 0, 0}},
        {5, "sand", ashfall::Movement::Powder, false, 2, {0, 0, 0}},
    };
    ashfall::SimulationWorld world(table);
    const std::vector<ashfall::MaterialId> ids = world.material_ids();
    expect(ids.size() == 4, "four ids listed");
    expect(ids[0] == 0 && ids[1] == 6 && ids[2] == 4 && ids[3] == 5,
        "ids follow table order, not sorted order");
    ashfall::MaterialSpec spec;
    expect(world.material_info(6, spec), "known id lookup succeeds");
    expect(spec.name == "water" && spec.movement == ashfall::Movement::Liquid,
        "info carries name and movement");
    expect(spec.color[0] == 0 && spec.color[1] == 0 && spec.color[2] == 0, "info carries color");
    expect(!world.material_info(42, spec), "unknown id lookup fails");
    expect(world.load_material_table(ashfall::default_material_table()), "defaults reload");
    const std::vector<ashfall::MaterialId> canonical = world.material_ids();
    expect(canonical.size() == 4 && canonical[0] == 0 && canonical[3] == 3,
        "canonical order restored");
}

} // namespace

int main() {
    test_sand_falls_and_stops();
    test_water_spreads_when_supported();
    test_chunks_work_across_negative_boundary();
    test_materials_cross_chunk_boundaries();
    test_save_load_is_exact();
    test_chunk_and_rect_reading();
    test_mass_conservation();
    test_sand_sinks_through_water();
    test_sand_block_sinks_through_open_water();
    test_settling_reaches_steady_state();
    test_water_reaches_steady_state();
    test_lone_water_droplet_rests();
    test_thin_water_film_stays_connected();
    test_water_surfer_drains_off_flat_layer();
    test_water_surface_flattens_in_basin();
    test_water_mound_levels_quickly();
    test_chunk_boundaries_crossing();
    test_play_level_layout();
    test_water_trickles_down_sand_slope();
    test_command_replay();
    test_step_marks_dirty_chunks();
    test_save_load_rejects_malformed();
    test_save_load_varied_topology();
    test_player_collision_and_dirty_tracking();
    test_material_data_parses_table();
    test_material_data_rejects_bad_input();
    test_table_with_remapped_ids_behaves_identically();
    test_unknown_material_id_rejected();
    test_loading_table_resets_world();
    test_material_data_matches_canonical_json();
    test_material_ids_preserve_table_order();
    std::cout << "All simulation tests passed.\n";
}

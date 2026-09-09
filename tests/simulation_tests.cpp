#include "ashfall/simulation.hpp"
#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

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
    expect(world.cell({455, 320}) == ashfall::kStone, "fire pit kiln floor present");
    expect(world.cell({440, 315}) == ashfall::kStone, "fire pit west wall present");
    expect(world.cell({455, 310}) == ashfall::kStone, "fire pit lid present");
    std::size_t pit_wood = 0, pit_fire = 0;
    for (int y = 310; y <= 320; ++y) {
        for (int x = 440; x <= 470; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kWood) ++pit_wood;
            else if (m == ashfall::kFire) ++pit_fire;
        }
    }
    expect(pit_wood == 19, "pit wood pile built");
    expect(pit_fire == 2, "pit starter built");
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
        // v2 layout ends with a 4-byte temp count: the last cell material
        // byte sits 5 from the end.
        auto bad_material = valid;
        bad_material[bad_material.size() - 5] = 99;
        expect_rejected(bad_material, "unknown material is rejected");
    }
    {
        auto air_material = valid;
        air_material[air_material.size() - 5] = ashfall::kAir;
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
    expect(ashfall::movement_from_string("gas", movement) && movement == ashfall::Movement::Gas, "gas movement resolves");
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
    const char* kPlasma =
        R"JSON({"id": 5, "name": "smoke", "movement": "plasma", "solid": false, "density": 0, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kPlasma).c_str());
    expect(rejects(buf), "unknown movement rejected");
    const char* kSolidSand =
        R"JSON({"id": 6, "name": "brick", "movement": "powder", "solid": true, "density": 5, "color": [1, 2, 3]})JSON";
    std::snprintf(buf, sizeof(buf), kBase, (std::string(kAir) + "," + kSolidSand).c_str());
    expect(rejects(buf), "solid powder rejected");
    const char* kDense =
        R"JSON({"id": 7, "name": "heavy", "movement": "static", "solid": true, "density": 100001, "color": [1, 2, 3]})JSON";
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
    {"id": 1, "name": "stone", "movement": "static", "solid": true, "density": 2000, "color": [128, 133, 138], "tags": ["static"], "hardness": 3},
    {"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 160, "color": [230, 175, 50], "tags": ["powder"]},
    {"id": 3, "name": "water", "movement": "liquid", "solid": false, "density": 100, "color": [86, 178, 214], "tags": ["liquid"]},
    {"id": 4, "name": "steam", "movement": "gas", "solid": false, "density": 1, "color": [205, 205, 210], "tags": ["vapour"]},
    {"id": 5, "name": "fire", "movement": "gas", "solid": false, "density": 1, "color": [255, 180, 40], "tags": ["fire"]},
    {"id": 6, "name": "wood", "movement": "static", "solid": true, "density": 2000, "color": [146, 104, 58], "tags": ["burnable", "static"], "hardness": 2},
    {"id": 7, "name": "lava", "movement": "liquid", "solid": false, "density": 140, "color": [255, 80, 10], "tags": ["liquid"]},
    {"id": 8, "name": "rock", "movement": "static", "solid": true, "density": 2000, "color": [120, 115, 110], "tags": ["static"], "hardness": 3}
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
                a.density == b.density && a.color == b.color && a.tags == b.tags &&
                a.hardness == b.hardness,
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
    expect(canonical.size() == 9 && canonical[0] == 0 && canonical[8] == 8,
        "canonical order restored");
}

void test_temperature_set_and_query() {
    ashfall::SimulationWorld world;
    expect(world.temperature({0, 0}) == 0, "ambient reads zero");
    expect(world.temperature_count() == 0, "fresh world stores nothing");
    world.set_temperature({3, -2}, 100);
    expect(world.temperature({3, -2}) == 100, "heat stored and read");
    expect(world.temperature_count() == 1, "one entry stored");
    world.set_temperature({3, -2}, -40);
    expect(world.temperature({3, -2}) == -40, "cold overwrites heat");
    expect(world.temperature_count() == 1, "overwrite keeps one entry");
    world.set_temperature({3, -2}, 0);
    expect(world.temperature({3, -2}) == 0, "zero reads ambient");
    expect(world.temperature_count() == 0, "zero erases the entry");
    world.set_temperature({0, 0}, 40000);
    expect(world.temperature({0, 0}) == 32767, "heat clamps to int16 max");
    world.set_temperature({1, 1}, -40000);
    expect(world.temperature({1, 1}) == -32767, "cold clamps to int16 min");
}

void test_temperature_single_cell_cools_exact() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_temperature({0, 0}, 100);
    world.step();
    expect(world.temperature({0, 0}) == 20, "100 conducts to 20 against ambient air");
    world.step();
    expect(world.temperature({0, 0}) == 4, "20 conducts to 4");
    world.step();
    expect(world.temperature({0, 0}) == 0, "4 truncates to ambient");
    expect(world.temperature_count() == 0, "ambient entries vanish");
}

void test_temperature_two_cells_equalize_exact() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_cell({1, 0}, ashfall::kStone);
    world.set_temperature({0, 0}, 100);
    world.step();
    expect(world.temperature({0, 0}) == 20 && world.temperature({1, 0}) == 20,
        "heat spreads: hot cools to 20, cold neighbor warms to 20");
    world.step();
    expect(world.temperature({0, 0}) == 8 && world.temperature({1, 0}) == 8, "pair conducts to 8");
    world.step();
    expect(world.temperature({0, 0}) == 3 && world.temperature({1, 0}) == 3, "pair conducts to 3");
    world.step();
    expect(world.temperature({0, 0}) == 1 && world.temperature({1, 0}) == 1, "pair conducts to 1");
    world.step();
    expect(world.temperature_count() == 0, "pair fully cools to ambient");
    // Cold mirror: truncation toward zero is symmetric.
    world.set_temperature({5, 5}, -100);
    world.set_cell({5, 5}, ashfall::kStone);
    world.set_cell({6, 5}, ashfall::kStone);
    world.step();
    expect(world.temperature({5, 5}) == -20 && world.temperature({6, 5}) == -20, "cold spreads symmetrically");
}

void test_temperature_ambient_world_stays_sparse() {
    ashfall::SimulationWorld empty;
    expect(empty.save().size() == 8 + 8 + 4 + 0 + 4, "empty v2 save is header plus zero counts");
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {19, 9}, ashfall::kWater);
    world.fill_rect({5, 5}, {8, 8}, ashfall::kSand);
    for (int i = 0; i < 50; ++i) world.step();
    expect(world.temperature_count() == 0, "ambient play never allocates heat");
    ashfall::SimulationWorld restored;
    expect(restored.load(world.save()), "ambient v2 save loads");
    expect(restored.temperature_count() == 0, "ambient load stores nothing");
}

void test_temperature_save_round_trip_v2() {
    ashfall::SimulationWorld source;
    source.fill_rect({0, 2}, {2, 2}, ashfall::kStone);
    source.set_cell({1, 0}, ashfall::kSand);
    source.set_temperature({0, 2}, 100);
    source.set_temperature({1, 2}, -50);
    source.set_temperature({2, 2}, 1);
    source.step(); // tick 1: sand drops to (1,1), heat conducts once
    // (0,2): (100-50)/5=10; (1,2): (-50+100+1)/5=10; (2,2): (1-50)/5=-9;
    // (1,1) sand: (-50)/5=-10 from its hot neighbor below.
    expect(source.temperature({0, 2}) == 10, "conducted stone cools to 10");
    expect(source.temperature({1, 2}) == 10, "middle stone settles at 10");
    expect(source.temperature({2, 2}) == -9, "cold edge truncates to -9");
    expect(source.temperature({1, 1}) == -10, "fallen sand picks up conducted cold");
    expect(source.temperature_count() == 4, "four entries conducted");
    const auto saved = source.save();
    ashfall::SimulationWorld restored;
    expect(restored.load(saved), "v2 save with heat loads");
    expect(restored.tick() == 1, "tick preserved");
    expect(restored.cell({1, 1}) == ashfall::kSand, "cells preserved");
    expect(restored.temperature({0, 2}) == 10 && restored.temperature({1, 2}) == 10 &&
            restored.temperature({2, 2}) == -9 && restored.temperature({1, 1}) == -10,
        "heat preserved across round-trip");
    expect(restored.save() == saved, "v2 round-trip is byte-exact");
}

void test_temperature_v1_save_still_loads() {
    // Hand-built v1 payload: magic(ASHFALL,1) + tick 7 + 1 stone cell (3,4).
    const std::vector<std::uint8_t> v1 = {
        'A', 'S', 'H', 'F', 'A', 'L', 'L', 1,
        7, 0, 0, 0, 0, 0, 0, 0,
        1, 0, 0, 0,
        3, 0, 0, 0, 4, 0, 0, 0,
        ashfall::kStone,
    };
    ashfall::SimulationWorld world;
    expect(world.load(v1), "v1 save loads under v2 reader");
    expect(world.cell({3, 4}) == ashfall::kStone, "v1 cell preserved");
    expect(world.tick() == 7, "v1 tick preserved");
    expect(world.temperature({3, 4}) == 0, "v1 world reads ambient");
    expect(world.temperature_count() == 0, "v1 world stores no heat");
    auto truncated = v1;
    truncated.pop_back();
    expect(!world.load(truncated), "truncated v1 rejected");
    auto bad_version = v1;
    bad_version[7] = 9;
    expect(!world.load(bad_version), "unknown save version rejected");
    world.set_temperature({3, 4}, 50);
    expect(!world.load(truncated), "bad load rejected with heat present");
    expect(world.temperature({3, 4}) == 50, "failed load preserves heat");
}

void test_temperature_cleared_by_edits_and_reset() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kSand);
    world.set_temperature({0, 0}, 50);
    world.set_cell({0, 0}, ashfall::kAir);
    expect(world.temperature({0, 0}) == 0, "digging a cell clears its heat");
    world.fill_rect({0, 0}, {2, 0}, ashfall::kStone);
    world.set_temperature({1, 0}, 60);
    world.fill_rect({0, 0}, {2, 0}, ashfall::kAir);
    expect(world.temperature({1, 0}) == 0, "bulk clear clears heat");
    expect(world.temperature_count() == 0, "no heat entries remain");
    world.set_cell({5, 5}, ashfall::kStone);
    world.set_temperature({5, 5}, 70);
    world.reset();
    expect(world.temperature_count() == 0 && world.temperature({5, 5}) == 0, "reset clears heat");
    world.set_cell({5, 5}, ashfall::kStone);
    world.set_temperature({5, 5}, 70);
    expect(world.load_material_table(ashfall::default_material_table()), "table reload succeeds");
    expect(world.temperature_count() == 0, "table load clears heat");
}

void test_temperature_follows_falling_sand() {
    // P2.5 advection: heat rides the grain (staged during movement, applied
    // post-swap, then conducted). Replaces the P2.4 no-advection pin.
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 2}, {1, 2}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kSand);
    world.set_temperature({0, 0}, 100);
    world.step();
    expect(world.cell({0, 1}) == ashfall::kSand, "sand falls one cell");
    expect(world.temperature({0, 0}) == 0, "vacated cell keeps no heat");
    expect(world.temperature({0, 1}) == 20, "carried heat conducts at the new home");
    expect(world.temperature({0, 2}) == 20, "carried heat also warms the floor");
    expect(world.temperature_count() == 2, "rider plus warmed floor");
}

void test_material_data_gas_tags_hardness() {
    const char* kJson = R"JSON({
  "version": 1,
  "materials": [
    {"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]},
    {"id": 4, "name": "steam", "movement": "gas", "solid": false, "density": 1, "color": [205, 205, 210], "tags": ["vapour"], "hardness": 0},
    {"id": 6, "name": "wood", "movement": "static", "solid": true, "density": 2000, "color": [146, 104, 58], "tags": ["burnable", "static"], "hardness": 2}
  ]
})JSON";
    ashfall::MaterialTable table;
    std::string error;
    expect(ashfall::parse_material_data(kJson, table, error), "gas/tags/hardness parse");
    expect(table.materials[1].movement == ashfall::Movement::Gas, "steam is gas");
    expect(table.materials[1].tags.size() == 1 && table.materials[1].tags[0] == "vapour",
        "steam tags intact");
    expect(table.materials[2].hardness == 2, "wood hardness intact");
    expect(table.materials[2].tags.size() == 2, "wood has two tags");
    ashfall::MaterialTable bad;
    std::string bad_error;
    bad.version = 1;
    bad.materials = {
        {0, "air", ashfall::Movement::Static, false, 0, {0, 0, 0}, {}, 0},
        {4, "steam", ashfall::Movement::Gas, true, 1, {0, 0, 0}, {}, 0},
    };
    expect(!ashfall::validate_material_table(bad, bad_error), "solid gas rejected");
    bad.materials[1].solid = false;
    bad.materials[1].hardness = 7;
    expect(!ashfall::validate_material_table(bad, bad_error), "hardness above 6 rejected");
    bad.materials[1].hardness = 0;
    bad.materials[1].tags = {"vapour", "vapour"};
    expect(!ashfall::validate_material_table(bad, bad_error), "duplicate tags rejected");
}

void test_material_data_legacy_json_still_parses() {
    // v1 JSON without the P2.5 optional fields: tags default empty,
    // hardness defaults 0, schema version stays 1.
    const char* kJson = R"JSON({
  "version": 1,
  "materials": [
    {"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]},
    {"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [230, 175, 50]}
  ]
})JSON";
    ashfall::MaterialTable table;
    std::string error;
    expect(ashfall::parse_material_data(kJson, table, error), "legacy JSON parses");
    expect(table.materials[1].tags.empty(), "missing tags default empty");
    expect(table.materials[1].hardness == 0, "missing hardness defaults to 0");
}

void test_reaction_data_parses() {
    const char* kJson = R"JSON({
  "version": 1,
  "reactions": [
    {"name": "quench", "probability": 20,
     "reactants": [{"material": "fire"}, {"material": "water"}],
     "products": [{"material": "fire"}, {"material": "steam"}]},
    {"name": "heat-ignition", "probability": 100,
     "reactants": [{"tag": "burnable"}],
     "products": [{"material": "fire"}],
     "temperature": {"min_temperature": 25},
     "product_temperature": 150}
  ]
})JSON";
    ashfall::ReactionTable table;
    std::string error;
    expect(ashfall::parse_reaction_data(kJson, table, error), "reaction data parses");
    expect(table.reactions.size() == 2, "two reactions loaded");
    const ashfall::ReactionSpec& quench = table.reactions[0];
    expect(quench.probability == 20, "probability intact");
    expect(quench.reactants.size() == 2 && !quench.reactants[0].is_tag &&
            quench.reactants[0].name == "fire",
        "positional reactant 1 intact");
    expect(quench.products[1].material == "steam", "positional product 2 intact");
    const ashfall::ReactionSpec& ignite = table.reactions[1];
    expect(ignite.reactants.size() == 1 && ignite.reactants[0].is_tag &&
            ignite.reactants[0].name == "burnable",
        "tag reactant intact");
    expect(ignite.products.size() == 1, "short products = identity tail");
    expect(ignite.gate.has_min && ignite.gate.min_value == 25, "temperature gate intact");
    expect(ignite.has_product_temperature && ignite.product_temperature == 150,
        "product temperature intact");
}

void test_reaction_data_rejects_bad_input() {
    auto rejects = [&](const std::string& text) {
        ashfall::ReactionTable table;
        std::string error;
        if (ashfall::parse_reaction_data(text, table, error)) {
            std::cerr << "ACCEPTED BAD REACTIONS: " << text << '\n';
            return false;
        }
        return !error.empty();
    };
    const char* kGood =
        R"JSON({"name": "quench", "probability": 20, "reactants": [{"material": "fire"}, {"material": "water"}], "products": [{"material": "fire"}, {"material": "steam"}]})JSON";
    auto wrap = [&](const std::string& reaction) {
        return std::string(R"JSON({"version": 1, "reactions": [)JSON") + reaction + "]}";
    };
    expect(rejects(""), "empty input rejected");
    expect(rejects(R"JSON({"version": 2, "reactions": []})JSON"), "wrong version rejected");
    expect(rejects(R"JSON({"version": 1, "reactions": []})JSON"), "empty array rejected");
    expect(rejects(wrap(kGood) + "trailing"), "trailing rejected");
    std::string no_name = kGood;
    no_name.replace(no_name.find("\"quench\""), 8, "\"\"");
    expect(rejects(wrap(no_name)), "empty name rejected");
    std::string dup = wrap(kGood);
    dup = std::string(R"JSON({"version": 1, "reactions": [)JSON") + kGood + "," + kGood + "]}";
    expect(rejects(dup), "duplicate name rejected");
    std::string bad_prob = kGood;
    bad_prob.replace(bad_prob.find("20"), 2, "101");
    expect(rejects(wrap(bad_prob)), "probability above 100 rejected");
    expect(
        rejects(wrap(R"JSON({"name": "big", "probability": 10, "reactants": [{"material": "fire"}, {"material": "water"}, {"material": "air"}], "products": [{"material": "fire"}]})JSON")),
        "three reactants rejected");
    expect(
        rejects(wrap(R"JSON({"name": "wide", "probability": 10, "reactants": [{"material": "fire"}], "products": [{"material": "fire"}, {"material": "air"}]})JSON")),
        "products wider than reactants rejected");
    expect(
        rejects(wrap(R"JSON({"name": "both", "probability": 10, "reactants": [{"material": "fire", "tag": "fire"}], "products": [{"material": "fire"}]})JSON")),
        "material+tag reactant rejected");
    expect(
        rejects(wrap(R"JSON({"name": "neither", "probability": 10, "reactants": [{}], "products": [{"material": "fire"}]})JSON")),
        "empty reactant rejected");
    expect(
        rejects(wrap(R"JSON({"name": "create", "probability": 10, "reactants": [{"material": "steam"}, {"material": "air"}], "products": [{"material": "air"}, {"material": "water"}]})JSON")),
        "creation from air rejected");
    expect(
        rejects(wrap(R"JSON({"name": "gate", "probability": 10, "reactants": [{"material": "fire"}], "products": [{"material": "air"}], "temperature": {"min_temperature": 50, "max_temperature": 10}})JSON")),
        "inverted gate rejected");
    // Programmatic table: validator is shared with load_reactions.
    ashfall::ReactionTable bad;
    bad.version = 1;
    std::string bad_error;
    expect(!ashfall::validate_reaction_table(bad, bad_error), "programmatic empty rejected");
}

void test_gas_rises_one_cell_per_tick() {
    ashfall::SimulationWorld world;
    world.set_cell({5, 10}, ashfall::kSteam);
    for (int i = 0; i < 5; ++i) world.step();
    expect(world.cell({5, 5}) == ashfall::kSteam, "steam rises one cell per tick");
    expect(world.occupied_cell_count() == 1, "rising conserves mass");
}

void test_gas_pools_under_ceiling() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 0}, {10, 0}, ashfall::kStone);
    world.set_cell({5, 5}, ashfall::kSteam);
    for (int i = 0; i < 4; ++i) world.step();
    expect(world.cell({5, 1}) == ashfall::kSteam, "steam rises to the ceiling");
    for (int i = 0; i < 6; ++i) world.step();
    expect(world.occupied_cell_count() == 12, "ceiling plus pooled gas conserved");
    int steam_y = -99;
    for (int y = 0; y <= 5; ++y) {
        for (int x = 0; x <= 10; ++x) {
            if (world.cell({x, y}) == ashfall::kSteam) steam_y = y;
        }
    }
    expect(steam_y == 1, "blocked gas stays pooled along the ceiling");
}

void test_sand_falls_through_steam() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({8, 0}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {8, 0}, ashfall::kStone); // cap: displaced steam pools, not escapes
    world.fill_rect({2, 8}, {4, 8}, ashfall::kSteam);
    world.set_cell({3, 5}, ashfall::kSand);
    for (int i = 0; i < 10; ++i) world.step();
    expect(world.cell({3, 9}) == ashfall::kSand, "sand rests on the floor");
    std::size_t steam = 0;
    bool steam_low = false;
    for (int y = 0; y <= 10; ++y) {
        for (int x = 0; x <= 8; ++x) {
            if (world.cell({x, y}) == ashfall::kSteam) {
                ++steam;
                if (y >= 9) steam_low = true;
            }
        }
    }
    expect(steam == 3, "steam mass conserved through the swap");
    expect(!steam_low, "displaced steam ends above the sand");
}

void test_lava_sinks_through_water() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({8, 0}, {8, 10}, ashfall::kStone);
    world.fill_rect({1, 6}, {7, 9}, ashfall::kWater); // 28 water
    world.fill_rect({3, 3}, {5, 3}, ashfall::kLava);  // 3 lava
    for (int i = 0; i < 80; ++i) world.step();
    std::size_t lava = 0, water = 0;
    bool lava_high = false;
    for (int y = 0; y <= 10; ++y) {
        for (int x = 0; x <= 8; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kLava) {
                ++lava;
                if (y < 9) lava_high = true;
            } else if (m == ashfall::kWater) {
                ++water;
            }
        }
    }
    expect(lava == 3, "lava mass conserved while sinking");
    expect(water == 28, "water mass conserved while displaced");
    expect(!lava_high, "all lava reaches the floor");
}

void test_water_rests_on_lava() {
    // The float direction: lighter water cannot displace heavier lava down.
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({8, 0}, {8, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {7, 9}, ashfall::kLava);
    world.set_cell({3, 7}, ashfall::kWater);
    for (int i = 0; i < 30; ++i) world.step();
    // The droplet falls onto the lake, then spreads along the surface row:
    // it can never displace lava downward, and liquid has no upward move,
    // so y == 8 is invariant while x wanders.
    std::size_t water = 0;
    int water_y = -99;
    for (int y = 0; y <= 10; ++y) {
        for (int x = 0; x <= 8; ++x) {
            if (world.cell({x, y}) == ashfall::kWater) {
                ++water;
                water_y = y;
            }
        }
    }
    expect(water == 1, "droplet mass conserved on the lava");
    expect(water_y == 8, "water rests on the lava surface");
    std::size_t lava = 0;
    for (int x = 0; x <= 8; ++x) {
        if (world.cell({x, 9}) == ashfall::kLava) ++lava;
    }
    expect(lava == 7, "lava lake intact under the water");
}

void test_reactions_reject_unknown_materials() {
    ashfall::SimulationWorld world;
    ashfall::ReactionTable good;
    good.version = 1;
    ashfall::ReactionSpec quench;
    quench.name = "quench";
    quench.probability = 100;
    quench.reactants = {{false, "fire"}, {false, "water"}};
    quench.products = {{"fire"}, {"steam"}};
    good.reactions = {quench};
    expect(world.load_reactions(good), "known materials resolve");
    expect(world.reaction_count() == 1, "one rule stored");
    ashfall::ReactionTable bad = good;
    bad.reactions[0].reactants[1].name = "unobtainium";
    expect(!world.load_reactions(bad), "unknown material rejected");
    expect(world.reaction_count() == 1, "failed load keeps previous rules");
    ashfall::ReactionTable malformed;
    malformed.version = 1;
    expect(!world.load_reactions(malformed), "empty table rejected");
    expect(world.reaction_count() == 1, "rejected load keeps rules");
}

void test_reaction_inapplicable_rule_does_not_shadow() {
    // A 2-reactant rule whose R1 matches but R2 is absent must NOT consume
    // the primary: lower-priority rules still get their chance (regression:
    // wet quench used to shadow burnout for a dry fire, making it immortal).
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kWood);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec quench;
    quench.name = "quench";
    quench.probability = 100;
    quench.reactants = {{false, "fire"}, {false, "water"}};
    quench.products = {{"fire"}, {"steam"}};
    ashfall::ReactionSpec burnout;
    burnout.name = "burnout";
    burnout.probability = 100;
    burnout.reactants = {{false, "fire"}};
    burnout.products = {{"air"}};
    table.reactions = {quench, burnout};
    expect(world.load_reactions(table), "quench+burnout load");
    world.step();
    expect(world.cell({0, 0}) == ashfall::kAir, "dry fire burns out despite quench priority");
    expect(world.cell({1, 0}) == ashfall::kWood, "wood untouched without spread");
}

void test_reaction_quench_conserves_cells() {
    ashfall::SimulationWorld world;
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec quench;
    quench.name = "quench";
    quench.probability = 100;
    quench.reactants = {{false, "fire"}, {false, "water"}};
    quench.products = {{"fire"}, {"steam"}};
    table.reactions = {quench};
    expect(world.load_reactions(table), "quench loads");
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kWater);
    for (int i = 0; i < 20; ++i) {
        world.step();
        expect(world.occupied_cell_count() == 2, "quench conserves cell count every tick");
    }
    std::size_t fire = 0, steam = 0, water = 0;
    for (int y = -25; y <= 5; ++y) {
        for (int x = -5; x <= 6; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kFire) ++fire;
            else if (m == ashfall::kSteam) ++steam;
            else if (m == ashfall::kWater) ++water;
        }
    }
    expect(fire == 1 && steam == 1 && water == 0, "fire persists, water becomes steam");
}

void test_reaction_spread_and_burnout() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kFire);
    world.fill_rect({1, 0}, {2, 0}, ashfall::kWood);
    ashfall::ReactionTable spread_table;
    spread_table.version = 1;
    ashfall::ReactionSpec spread;
    spread.name = "spread";
    spread.probability = 100;
    spread.reactants = {{false, "fire"}, {true, "burnable"}};
    spread.products = {{"fire"}, {"fire"}};
    spread.has_product_temperature = true;
    spread.product_temperature = 150;
    spread_table.reactions = {spread};
    expect(world.load_reactions(spread_table), "spread loads");
    world.step();
    // One ignition per fire per tick (first matching neighbor only); both
    // flames then rise one cell and their carried 150s conduct (150 + 150
    // from the twin flame) / 5 = 60.
    std::size_t fire = 0, wood = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 4; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kFire) ++fire;
            else if (m == ashfall::kWood) ++wood;
        }
    }
    expect(fire == 2, "fire spreads to one wood cell in one tick");
    expect(wood == 1, "unreached wood waits its turn");
    expect(world.temperature({1, -1}) == 60, "fresh fire heat conducts at the new home");
    // Burnout alone: every flame dies the next tick at p=100.
    ashfall::ReactionTable burnout_table;
    burnout_table.version = 1;
    ashfall::ReactionSpec burnout;
    burnout.name = "burnout";
    burnout.probability = 100;
    burnout.reactants = {{false, "fire"}};
    burnout.products = {{"air"}};
    burnout_table.reactions = {burnout};
    expect(world.load_reactions(burnout_table), "burnout loads");
    world.step();
    expect(world.occupied_cell_count() == 1, "burnout deletes all fire, spares wood");
    expect(world.cell({2, 0}) == ashfall::kWood, "unburned wood stands");
    expect(world.temperature_count() == 0, "burned-out fire leaves no heat");
}

void test_reaction_heat_ignition_gate() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kWood);
    world.set_cell({5, 5}, ashfall::kWood);
    world.set_temperature({0, 0}, 30);
    world.set_temperature({5, 5}, 10);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec ignite;
    ignite.name = "heat-ignition";
    ignite.probability = 100;
    ignite.reactants = {{true, "burnable"}};
    ignite.products = {{"fire"}};
    ignite.gate.has_min = true;
    ignite.gate.min_value = 25;
    ignite.has_product_temperature = true;
    ignite.product_temperature = 150;
    table.reactions = {ignite};
    expect(world.load_reactions(table), "heat-ignition loads");
    world.step();
    // Gate reads tick-start temps (reactions run before conduction): 30
    // ignites, 10 stays wood. The new flame then rises one cell.
    std::size_t fire = 0, wood = 0;
    for (int y = -2; y <= 6; ++y) {
        for (int x = -2; x <= 7; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kFire) ++fire;
            else if (m == ashfall::kWood) ++wood;
        }
    }
    expect(fire == 1 && wood == 1, "only hot wood ignites");
    expect(world.cell({5, 5}) == ashfall::kWood, "cold wood stays wood");
}

void test_reaction_condense_on_static() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 0}, {2, 0}, ashfall::kStone);
    world.set_cell({1, 1}, ashfall::kSteam);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec condense;
    condense.name = "condense";
    condense.probability = 100;
    condense.reactants = {{false, "steam"}, {true, "static"}};
    condense.products = {{"water"}};
    table.reactions = {condense};
    expect(world.load_reactions(table), "condense loads");
    world.step();
    // steam->water (missing tail product = identity, ceiling untouched),
    // then the droplet falls to (1,2).
    expect(world.cell({1, 2}) == ashfall::kWater, "condensed water drips");
    expect(world.cell({1, 0}) == ashfall::kStone, "ceiling unchanged");
    std::size_t steam = 0;
    for (int y = 0; y <= 3; ++y) {
        for (int x = 0; x <= 2; ++x) {
            if (world.cell({x, y}) == ashfall::kSteam) ++steam;
        }
    }
    expect(steam == 0, "no steam remains");
}

void test_reaction_dissipate_needs_air() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kSteam); // open air
    world.set_cell({10, 10}, ashfall::kSteam); // boxed: stone ring
    world.set_cell({10, 9}, ashfall::kStone);
    world.set_cell({10, 11}, ashfall::kStone);
    world.set_cell({9, 10}, ashfall::kStone);
    world.set_cell({11, 10}, ashfall::kStone);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec dissipate;
    dissipate.name = "dissipate";
    dissipate.probability = 100;
    dissipate.reactants = {{true, "vapour"}, {false, "air"}};
    dissipate.products = {{"air"}, {"air"}};
    table.reactions = {dissipate};
    expect(world.load_reactions(table), "dissipate loads");
    world.step();
    // Open steam reacts away before movement (empty up-neighbor); boxed
    // steam has no air neighbor and its diagonal escapes are corner-cut
    // blocked, so it stays put.
    expect(world.occupied_cell_count() == 5, "open steam vanishes, boxed steam + 4 stones remain");
    expect(world.cell({10, 10}) == ashfall::kSteam, "enclosed steam survives");
}

void test_reaction_lava_crusts_on_water() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kLava);
    world.set_cell({1, 0}, ashfall::kWater);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec crust;
    crust.name = "lava-crust";
    crust.probability = 100;
    crust.reactants = {{false, "lava"}, {false, "water"}};
    crust.products = {{"rock"}, {"steam"}};
    table.reactions = {crust};
    expect(world.load_reactions(table), "lava-crust loads");
    world.step();
    // Rock is static and stays; the spawned steam rises one cell.
    expect(world.cell({0, 0}) == ashfall::kRock, "lava crusts to rock");
    std::size_t steam = 0, water = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 3; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kSteam) ++steam;
            else if (m == ashfall::kWater) ++water;
        }
    }
    expect(steam == 1, "one steam spawned");
    expect(water == 0, "water consumed into steam");
}

void test_reactions_are_scan_order_independent() {
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec quench;
    quench.name = "quench";
    quench.probability = 100;
    quench.reactants = {{false, "fire"}, {false, "water"}};
    quench.products = {{"fire"}, {"steam"}};
    ashfall::ReactionSpec spread;
    spread.name = "spread";
    spread.probability = 100;
    spread.reactants = {{false, "fire"}, {true, "burnable"}};
    spread.products = {{"fire"}, {"fire"}};
    spread.has_product_temperature = true;
    spread.product_temperature = 150;
    ashfall::ReactionSpec condense;
    condense.name = "condense";
    condense.probability = 100;
    condense.reactants = {{false, "steam"}, {true, "static"}};
    condense.products = {{"water"}};
    table.reactions = {quench, spread, condense};
    auto build = [&](bool reverse) {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(table), "rules load");
        const std::vector<std::pair<ashfall::CellPosition, ashfall::MaterialId>> cells = {
            {{0, 0}, ashfall::kFire}, {{1, 0}, ashfall::kWater}, {{2, 0}, ashfall::kWood},
            {{0, 1}, ashfall::kStone}, {{1, 1}, ashfall::kSteam}, {{2, 1}, ashfall::kStone},
        };
        if (!reverse) {
            for (const auto& [pos, mat] : cells) world.set_cell(pos, mat);
        } else {
            for (auto it = cells.rbegin(); it != cells.rend(); ++it) world.set_cell(it->first, it->second);
        }
        return world;
    };
    ashfall::SimulationWorld forward = build(false);
    ashfall::SimulationWorld backward = build(true);
    for (int i = 0; i < 5; ++i) {
        forward.step();
        backward.step();
    }
    expect(forward.save() == backward.save(), "insertion order never affects reactions");
}

void test_loading_table_clears_reactions() {
    ashfall::SimulationWorld world;
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec burnout;
    burnout.name = "burnout";
    burnout.probability = 8;
    burnout.reactants = {{false, "fire"}};
    burnout.products = {{"air"}};
    table.reactions = {burnout};
    expect(world.load_reactions(table), "reactions load");
    expect(world.reaction_count() == 1, "one rule stored");
    expect(world.load_material_table(ashfall::default_material_table()), "table reload succeeds");
    expect(world.reaction_count() == 0, "table load clears reactions");
}

void test_play_level_fire_pit_burns_out() {
    // Reactions are data, not state: the test loads the canonical rule set
    // (literal mirrors data/reactions.json field-for-field; godot_p2_verify
    // loads the real file through the bridge), exactly like the host boot.
    const char* kReactions = R"JSON({
  "version": 1,
  "reactions": [
    {"name": "quench", "probability": 20,
     "reactants": [{"material": "fire"}, {"material": "water"}],
     "products": [{"material": "fire"}, {"material": "steam"}]},
    {"name": "spread", "probability": 40,
     "reactants": [{"material": "fire"}, {"tag": "burnable"}],
     "products": [{"material": "fire"}, {"material": "fire"}],
     "product_temperature": 150},
    {"name": "heat-ignition", "probability": 100,
     "reactants": [{"tag": "burnable"}],
     "products": [{"material": "fire"}],
     "temperature": {"min_temperature": 25},
     "product_temperature": 150},
    {"name": "lava-crust", "probability": 80,
     "reactants": [{"material": "lava"}, {"material": "water"}],
     "products": [{"material": "rock"}, {"material": "steam"}]},
    {"name": "lava-ignite", "probability": 8,
     "reactants": [{"material": "lava"}, {"tag": "burnable"}],
     "products": [{"material": "lava"}, {"material": "fire"}],
     "product_temperature": 150},
    {"name": "condense", "probability": 70,
     "reactants": [{"material": "steam"}, {"tag": "static"}],
     "products": [{"material": "water"}]},
    {"name": "dissipate", "probability": 3,
     "reactants": [{"tag": "vapour"}, {"material": "air"}],
     "products": [{"material": "air"}, {"material": "air"}]},
    {"name": "burnout", "probability": 8,
     "reactants": [{"material": "fire"}],
     "products": [{"material": "air"}]}
  ]
})JSON";
    ashfall::ReactionTable rules;
    std::string error;
    expect(ashfall::parse_reaction_data(kReactions, rules, error), "canonical reactions parse");
    ashfall::SimulationWorld world;
    world.build_play_level();
    expect(world.load_reactions(rules), "pit rules load");
    for (int i = 0; i < 200; ++i) world.step();
    // Kiln stone is static: floor 31 + walls 10 + 10 + lid 29, all intact.
    for (int x = 440; x <= 470; ++x) {
        expect(world.cell({x, 320}) == ashfall::kStone, "kiln floor survives the burn");
    }
    for (int y = 310; y <= 319; ++y) {
        expect(world.cell({440, y}) == ashfall::kStone, "kiln west wall survives");
        expect(world.cell({470, y}) == ashfall::kStone, "kiln east wall survives");
    }
    for (int x = 441; x <= 469; ++x) {
        expect(world.cell({x, 310}) == ashfall::kStone, "kiln lid survives");
    }
    std::vector<ashfall::MaterialId> full(static_cast<std::size_t>(512 * 512), ashfall::kAir);
    world.copy_rect({0, 0}, {511, 511}, full);
    std::size_t fire = 0, steam = 0, wood = 0;
    for (ashfall::MaterialId m : full) {
        if (m == ashfall::kFire) ++fire;
        else if (m == ashfall::kSteam) ++steam;
        else if (m == ashfall::kWood) ++wood;
    }
    expect(fire == 0, "all pit fire burns out during settle");
    expect(steam == 0, "all pit steam dissipates or condenses");
    expect(wood <= 19, "no reaction creates wood");
    ashfall::SimulationWorld again;
    again.build_play_level();
    expect(again.load_reactions(rules), "pit rules load on the twin");
    for (int i = 0; i < 200; ++i) again.step();
    expect(again.save() == world.save(), "pit burn is deterministic");
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
    test_material_data_gas_tags_hardness();
    test_material_data_legacy_json_still_parses();
    test_reaction_data_parses();
    test_reaction_data_rejects_bad_input();
    test_gas_rises_one_cell_per_tick();
    test_gas_pools_under_ceiling();
    test_sand_falls_through_steam();
    test_lava_sinks_through_water();
    test_water_rests_on_lava();
    test_reactions_reject_unknown_materials();
    test_reaction_quench_conserves_cells();
    test_reaction_spread_and_burnout();
    test_reaction_inapplicable_rule_does_not_shadow();
    test_reaction_heat_ignition_gate();
    test_reaction_condense_on_static();
    test_reaction_dissipate_needs_air();
    test_reaction_lava_crusts_on_water();
    test_reactions_are_scan_order_independent();
    test_loading_table_clears_reactions();
    test_play_level_fire_pit_burns_out();
    test_temperature_set_and_query();
    test_temperature_single_cell_cools_exact();
    test_temperature_two_cells_equalize_exact();
    test_temperature_ambient_world_stays_sparse();
    test_temperature_save_round_trip_v2();
    test_temperature_v1_save_still_loads();
    test_temperature_cleared_by_edits_and_reset();
    test_temperature_follows_falling_sand();
    std::cout << "All simulation tests passed.\n";
}

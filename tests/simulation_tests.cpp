#include "ashfall/simulation.hpp"
#include "ashfall/json_subset.hpp"
#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"
#include "oracle_scenarios.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

// Group 3 (Task T) v3 helpers: IEEE CRC32 (polynomial 0xEDB88320, matching
// engine/src/simulation.cpp) + minimal v3 section walkers. The walkers parse
// only the roster framing (no semantic checks), so mutated-payload tests can
// locate the cell_count/cell/temp fields of a save() snapshot. Semantic
// mutants must re-seal the CRC (verified BEFORE parsing) via g3_fix_v3_crc,
// otherwise the loader reports BadCrc instead of the targeted fault.
std::uint32_t g3_crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) != 0u ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t g3_read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void g3_write_u32_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

// v3 layout recap (writer emits v3 ONLY): prefix(7) ver(1) tick(8)
// roster_count(4) roster(old_id u8, name_len u8, name) reaction_count(4)
// hash(4) extents(16) cell_count(4) cells(x u32, y u32, old_id u8)
// temp_count(4) temps(x u32, y u32, temp i16) crc32(4).
std::size_t g3_v3_cell_count_offset(const std::vector<std::uint8_t>& bytes) {
    std::size_t off = 16; // prefix + version + tick
    const std::uint32_t roster = g3_read_u32_le(bytes, off);
    off += 4;
    for (std::uint32_t i = 0; i < roster; ++i) off += 2 + bytes[off + 1];
    return off + 4 + 4 + 16; // skip reaction_count + hash + extents
}

void g3_fix_v3_crc(std::vector<std::uint8_t>& bytes) {
    g3_write_u32_le(bytes, bytes.size() - 4, g3_crc32(bytes.data(), bytes.size() - 4));
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

void test_partial_surface_remains_level() {
    // Honest name for the old flattening fixture: its sparse top row already
    // differs by at most one per column before stepping, so it pins level
    // preservation, not genuine flattening (see the uneven test below).
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({20, 0}, {20, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {19, 9}, ashfall::kWater); // full bottom layer
    for (int x = 2; x <= 17; x += 3) world.set_cell({x, 8}, ashfall::kWater); // sparse dotted top row

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

void test_uneven_water_mound_flattens_in_basin() {
    // Genuinely uneven start: a tall mound on one side (columns differ by 6
    // up front), so the oracle measures real flattening — topmost-water
    // descent plus bottom-contiguous columns, with empty columns included
    // in the height compare (excluding them hides split surfaces).
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({20, 0}, {20, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {19, 9}, ashfall::kWater); // full bottom layer (19)
    world.fill_rect({1, 3}, {3, 8}, ashfall::kWater);  // mound on the left (3x6=18)
    const int kTotalWater = 19 + 18;

    int initial_top = 99;
    for (int y = 0; y <= 9; ++y) {
        for (int x = 1; x <= 19; ++x) {
            if (world.cell({x, y}) == ashfall::kWater) initial_top = std::min(initial_top, y);
        }
    }
    expect(initial_top == 3, "mound starts genuinely uneven");

    for (int i = 0; i < 300; ++i) world.step();

    int top = 99, min_height = 99, max_height = 0, total_water = 0;
    for (int x = 1; x <= 19; ++x) {
        int height = 0;
        for (int y = 0; y <= 9; ++y) {
            if (world.cell({x, y}) == ashfall::kWater) {
                ++height;
                ++total_water;
                top = std::min(top, y);
            }
        }
        // Bottom-contiguous: no floating water or internal air gaps — every
        // column is a solid stack rising from the floor row.
        for (int y = 9; y > 9 - height; --y) {
            expect(world.cell({x, y}) == ashfall::kWater, "settled columns are bottom-contiguous");
        }
        min_height = std::min(min_height, height);
        max_height = std::max(max_height, height);
    }
    expect(total_water == kTotalWater, "mound flattening conserves mass");
    expect(top >= 7, "mound top descends as the surface levels");
    expect(max_height - min_height <= 1, "leveled heights agree within one cell, empty columns included");
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

    // Reading a missing chunk succeeds and fills the WHOLE output with air.
    std::vector<ashfall::MaterialId> empty_chunk(ashfall::kChunkSize * ashfall::kChunkSize, ashfall::kSand);
    expect(world.read_chunk({1, 1}, empty_chunk), "read unallocated chunk succeeds");
    expect(empty_chunk.size() == 4096, "chunk buffer holds 64x64 cells");
    expect(std::all_of(empty_chunk.begin(), empty_chunk.end(),
                       [](ashfall::MaterialId m) { return m == ashfall::kAir; }),
        "unallocated chunk filled with air");
    // copy_rect over a mixed populated/missing region with a non-air
    // prefill: missing cells must read back as air, never prefill residue.
    world.set_cell({63, 63}, ashfall::kStone);
    std::vector<ashfall::MaterialId> mixed(64, static_cast<ashfall::MaterialId>(0xFF));
    world.copy_rect({60, 60}, {67, 67}, mixed);
    for (int y = 60; y <= 67; ++y) {
        for (int x = 60; x <= 67; ++x) {
            const ashfall::MaterialId want = (x == 63 && y == 63) ? ashfall::kStone : ashfall::kAir;
            const std::size_t index = static_cast<std::size_t>((y - 60) * 8 + (x - 60));
            expect(mixed[index] == want, "mixed populated/missing rect reads stone amid air");
        }
    }

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

    // Semantic cell-state compare over the basin (never sliced save bytes:
    // the tick header and any future framing would couple the oracle to
    // the serialization layout).
    auto snapshot = [&]() {
        std::vector<ashfall::MaterialId> buf(static_cast<std::size_t>(21 * 21), ashfall::kAir);
        world.copy_rect({0, 0}, {20, 20}, buf);
        return buf;
    };
    // Bounded evidence, not proof: one unchanged tick is a pause (arbitration
    // can resume motion later), so demand 10 consecutive unchanged ticks.
    bool reached_steady_state = false;
    std::vector<ashfall::MaterialId> previous = snapshot();
    for (int i = 0; i < 200 && !reached_steady_state; ++i) {
        world.step();
        const std::vector<ashfall::MaterialId> current = snapshot();
        if (current == previous) {
            reached_steady_state = true;
            for (int w = 0; w < 10; ++w) {
                world.step();
                if (snapshot() != previous) {
                    reached_steady_state = false;
                    previous = snapshot();
                    break;
                }
            }
        } else {
            previous = current;
        }
    }
    expect(reached_steady_state, "sand reaches steady state / quiescence in basin");
    // Settled pile fills its footprint row directly above the stone floor.
    for (int x = 8; x <= 12; ++x) {
        expect(world.cell({x, 19}) == ashfall::kSand, "settled sand fills the floor footprint row");
    }
}

void test_water_reaches_steady_state() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {8, 10}, ashfall::kStone);
    world.fill_rect({0, 0}, {0, 10}, ashfall::kStone);
    world.fill_rect({8, 0}, {8, 10}, ashfall::kStone);
    world.fill_rect({4, 1}, {4, 7}, ashfall::kWater);

    // Semantic cell-state compare over the basin (never sliced save bytes).
    auto snapshot = [&]() {
        std::vector<ashfall::MaterialId> buf(static_cast<std::size_t>(9 * 11), ashfall::kAir);
        world.copy_rect({0, 0}, {8, 10}, buf);
        return buf;
    };
    // Bounded evidence, not proof: demand 10 consecutive unchanged ticks.
    std::vector<ashfall::MaterialId> previous = snapshot();
    bool reached_steady_state = false;
    for (int i = 0; i < 200 && !reached_steady_state; ++i) {
        world.step();
        const std::vector<ashfall::MaterialId> current = snapshot();
        if (current == previous) {
            reached_steady_state = true;
            for (int w = 0; w < 10; ++w) {
                world.step();
                if (snapshot() != previous) {
                    reached_steady_state = false;
                    previous = snapshot();
                    break;
                }
            }
        } else {
            previous = current;
        }
    }
    expect(reached_steady_state, "water reaches steady state in a basin");
    // The 7-cell column settles into the 7-wide bottom row exactly.
    for (int x = 1; x <= 7; ++x) {
        expect(world.cell({x, 9}) == ashfall::kWater, "settled water fills the bottom row");
    }
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
    // CONTENT: pins authored spawn {256,480} — level tuning, not physics.
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
    // CONTENT: authored kiln geometry pin — level tuning, not physics.
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
    // CONTENT: pit holds exactly 19 wood + 2 fire — authored content pin.
    ashfall::SimulationWorld again;
    again.build_play_level();
    expect(again.save() == world.save(), "play level builds deterministically");
}

void test_water_trickles_down_sand_slope() {
    // (a) Open slope with an escape route: a sand staircase pierced by a
    // stone-lined shaft open to the floor. The oracle asserts POSITION (the
    // droplet reaches the shaft floor within budget), not counts:
    // slope_water is a subset of total_water, so a comparison like
    // `slope_water <= 1` passes vacuously once mass is conserved. A bare
    // `== 0` would be equally dishonest — settled sand can cup a droplet
    // into a legitimate 1-cell puddle — so absence claims always need
    // pocket analysis (see fixture (b)). The route is a fall path on
    // purpose: lone droplets rest on flat support by design (see
    // test_lone_water_droplet_rests), so a lateral-only slope cannot oracle
    // lone-droplet drainage; only collective water or a fall path moves it.
    {
        ashfall::SimulationWorld world;
        world.fill_rect({0, 31}, {40, 31}, ashfall::kStone); // floor (41)
        world.fill_rect({0, 0}, {0, 31}, ashfall::kStone);   // left wall (+31)
        world.fill_rect({40, 0}, {40, 31}, ashfall::kStone); // right wall (+31)
        world.fill_rect({24, 20}, {24, 30}, ashfall::kStone); // shaft west lining (+11)
        world.fill_rect({26, 20}, {26, 30}, ashfall::kStone); // shaft east lining (+11)
        for (int x : {20, 21, 22, 23, 27, 28, 29}) world.fill_rect({x, x}, {x, 30}, ashfall::kSand); // split staircase (47)
        world.set_cell({25, 19}, ashfall::kWater); // over the shaft mouth
        for (int i = 0; i < 100; ++i) world.step();
        expect(world.occupied_cell_count() == 125 + 47 + 1, "open slope conserves mass");
        expect(world.cell({25, 30}) == ashfall::kWater, "droplet drains through the slope shaft to the floor");
    }
    // (b) Fully enclosed one-cell pocket: the droplet has no legal move,
    // so it must remain. This is the pocket analysis absence claims need.
    {
        ashfall::SimulationWorld world;
        world.fill_rect({4, 4}, {6, 4}, ashfall::kStone); // lid
        world.set_cell({4, 5}, ashfall::kStone);          // west wall
        world.set_cell({6, 5}, ashfall::kStone);          // east wall
        world.fill_rect({4, 6}, {6, 6}, ashfall::kStone); // floor
        world.set_cell({5, 5}, ashfall::kWater);
        for (int i = 0; i < 100; ++i) world.step();
        expect(world.cell({5, 5}) == ashfall::kWater, "pocketed droplet cannot leave its cell");
        expect(world.occupied_cell_count() == 8 + 1, "pocket fixture conserves mass");
    }
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
    expect(valid.size() > 8 && valid[7] == ashfall::SimulationWorld::kSaveVersion3,
        "writer emits v3");
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
        // v3 seals the payload with CRC32 (verified BEFORE parsing), so a
        // semantic mutant must re-seal first: corrupt the first cell record's
        // old-id byte (cells start 4 past the cell_count field; the id is the
        // 9th byte of the record), fix the CRC, then load.
        auto bad_material = valid;
        const std::size_t cells = g3_v3_cell_count_offset(bad_material) + 4;
        bad_material[cells + 8] = 99; // no roster row carries old-id 99
        g3_fix_v3_crc(bad_material);
        expect(!world.load(bad_material), "unknown material is rejected");
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(bad_material, error), "unknown material diagnosed");
        expect(error == ashfall::SimulationWorld::LoadError::RosterUnknownMaterial,
            "unknown old-id names RosterUnknownMaterial");
        expect(world.save() == snapshot, "failed load preserves exact state");
    }
    {
        auto air_material = valid;
        const std::size_t cells = g3_v3_cell_count_offset(air_material) + 4;
        air_material[cells + 8] = ashfall::kAir; // roster old-id 0 is air
        g3_fix_v3_crc(air_material);
        expect(!world.load(air_material), "air material entry is rejected");
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(air_material, error), "air entry diagnosed");
        expect(error == ashfall::SimulationWorld::LoadError::AirCellStored,
            "stored air names AirCellStored");
        expect(world.save() == snapshot, "failed load preserves exact state");
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

void test_save_golden_empty_v3() {
    // GOLDEN (v3): empty world, tick 0, default table (air..oil ids 0..9), 0
    // reactions. Layout: prefix(7) ver(1)=3 tick u64 roster_count=10 roster
    // (old_id u8, len u8, name) reaction_count=0 hash=0 extents(0,0,0,0)
    // cell_count=0 temp_count=0 crc32 = 16+65+8+16+4+0+4+4 = 117 bytes. The
    // CRC F6 BD C5 75 is engine-computed; the non-CRC prefix is hand-verified
    // (roster bytes spell the ten default names; all counts/extents zero).
    const std::vector<std::uint8_t> kEmptyV3 = {
        0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x61, 0x69,
        0x72, 0x01, 0x05, 0x73, 0x74, 0x6F, 0x6E, 0x65, 0x02, 0x04, 0x73, 0x61,
        0x6E, 0x64, 0x03, 0x05, 0x77, 0x61, 0x74, 0x65, 0x72, 0x04, 0x05, 0x73,
        0x74, 0x65, 0x61, 0x6D, 0x05, 0x04, 0x66, 0x69, 0x72, 0x65, 0x06, 0x04,
        0x77, 0x6F, 0x6F, 0x64, 0x07, 0x04, 0x6C, 0x61, 0x76, 0x61, 0x08, 0x04,
        0x72, 0x6F, 0x63, 0x6B, 0x09, 0x03, 0x6F, 0x69, 0x6C, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xBD, 0xC5, 0x75};
    expect(kEmptyV3.size() == 117, "empty v3 vector is 117 bytes");
    ashfall::SimulationWorld world;
    expect(world.save() == kEmptyV3, "empty world saves 117 golden v3 bytes");
    ashfall::SimulationWorld restored;
    expect(restored.load(kEmptyV3), "golden empty v3 payload loads");
    expect(restored.tick() == 0 && restored.occupied_cell_count() == 0, "golden empty round-trips state");
    expect(restored.save() == kEmptyV3, "golden empty re-save is byte-exact");
    // Self-check: the test-local CRC re-seal is identity on pristine bytes
    // (pins the helper against the engine's CRC32).
    auto probe = kEmptyV3;
    g3_fix_v3_crc(probe);
    expect(probe == kEmptyV3, "test CRC helper agrees with engine CRC");
}

void test_save_v2_reader_empty() {
    // READER fixture (v2): the Group-1/2 24-byte golden still loads under the
    // retained v1/v2 reader. Load-only: save() now emits v3, so no save pin.
    const std::vector<std::uint8_t> kEmptyV2 = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ashfall::SimulationWorld restored;
    expect(restored.load(kEmptyV2), "v2 empty payload loads");
    expect(restored.tick() == 0 && restored.occupied_cell_count() == 0, "v2 empty state intact");
}

void test_save_golden_single_stone_v3() {
    // GOLDEN (v3): one stone at (1,2), tick 0. Same header/roster as the empty
    // vector, then extents(1,1,2,2) cell_count=1 cell(1,2,old-id 1)
    // temp_count=0 CRC 91 3C 2D E3 = 126 bytes. Non-CRC prefix hand-verified
    // (extents match the lone cell; cell bytes are x=1 y=2 id=1).
    const std::vector<std::uint8_t> kOneStoneV3 = {
        0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x61, 0x69,
        0x72, 0x01, 0x05, 0x73, 0x74, 0x6F, 0x6E, 0x65, 0x02, 0x04, 0x73, 0x61,
        0x6E, 0x64, 0x03, 0x05, 0x77, 0x61, 0x74, 0x65, 0x72, 0x04, 0x05, 0x73,
        0x74, 0x65, 0x61, 0x6D, 0x05, 0x04, 0x66, 0x69, 0x72, 0x65, 0x06, 0x04,
        0x77, 0x6F, 0x6F, 0x64, 0x07, 0x04, 0x6C, 0x61, 0x76, 0x61, 0x08, 0x04,
        0x72, 0x6F, 0x63, 0x6B, 0x09, 0x03, 0x6F, 0x69, 0x6C, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x91, 0x3C, 0x2D, 0xE3};
    expect(kOneStoneV3.size() == 126, "single-stone v3 vector is 126 bytes");
    ashfall::SimulationWorld world;
    world.set_cell({1, 2}, ashfall::kStone);
    expect(world.save() == kOneStoneV3, "single stone saves 126 golden v3 bytes");
    ashfall::SimulationWorld restored;
    expect(restored.load(kOneStoneV3), "golden single-stone v3 payload loads");
    expect(restored.cell({1, 2}) == ashfall::kStone, "golden stone cell preserved");
    expect(restored.tick() == 0, "golden tick preserved");
    expect(restored.save() == kOneStoneV3, "golden single-stone re-save is byte-exact");
}

void test_save_v2_reader_single_stone() {
    // READER fixture (v2): Group-1/2 golden, load-only (no save pin).
    const std::vector<std::uint8_t> kOneStone = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0};
    ashfall::SimulationWorld restored;
    expect(restored.load(kOneStone), "v2 single-stone payload loads");
    expect(restored.cell({1, 2}) == ashfall::kStone, "v2 stone cell preserved");
    expect(restored.tick() == 0, "v2 tick preserved");
}

void test_save_golden_negative_coords_and_temps_v3() {
    // GOLDEN (v3): two stones ((-1,0) at -1 heat, (2,3) at max heat), tick 0.
    // Chunk-major order: (-1,0) lives in chunk {-1,0}, so it sorts first;
    // extents(-1,2,0,3); temps follow the same cell-walk order; CRC 6A C3 D0
    // F0 is engine-computed, non-CRC prefix hand-verified (little-endian
    // two's-complement coords, temp -1 = FF FF, 32767 = FF 7F). 155 bytes.
    const std::vector<std::uint8_t> kNegTempV3 = {
        0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x03, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x03, 0x61, 0x69,
        0x72, 0x01, 0x05, 0x73, 0x74, 0x6F, 0x6E, 0x65, 0x02, 0x04, 0x73, 0x61,
        0x6E, 0x64, 0x03, 0x05, 0x77, 0x61, 0x74, 0x65, 0x72, 0x04, 0x05, 0x73,
        0x74, 0x65, 0x61, 0x6D, 0x05, 0x04, 0x66, 0x69, 0x72, 0x65, 0x06, 0x04,
        0x77, 0x6F, 0x6F, 0x64, 0x07, 0x04, 0x6C, 0x61, 0x76, 0x61, 0x08, 0x04,
        0x72, 0x6F, 0x63, 0x6B, 0x09, 0x03, 0x6F, 0x69, 0x6C, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00,
        0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0xFF,
        0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0xFF, 0x7F, 0x6A, 0xC3, 0xD0, 0xF0};
    expect(kNegTempV3.size() == 155, "negative/temp v3 vector is 155 bytes");
    ashfall::SimulationWorld world;
    world.set_cell({-1, 0}, ashfall::kStone);
    world.set_cell({2, 3}, ashfall::kStone);
    expect(world.set_temperature({-1, 0}, -1), "cold stored on stone");
    expect(world.set_temperature({2, 3}, 32767), "max heat stored on stone");
    expect(world.save() == kNegTempV3, "negative coords and temp edges save golden v3 bytes");
    ashfall::SimulationWorld restored;
    expect(restored.load(kNegTempV3), "golden negative/temp v3 payload loads");
    expect(restored.cell({-1, 0}) == ashfall::kStone && restored.cell({2, 3}) == ashfall::kStone,
        "golden negative-coord cells preserved");
    expect(restored.temperature({-1, 0}) == -1 && restored.temperature({2, 3}) == 32767,
        "golden temp edges preserved");
    expect(restored.save() == kNegTempV3, "golden negative/temp re-save is byte-exact");
}

void test_save_v2_reader_negative_coords_and_temps() {
    // READER fixture (v2): Group-1/2 golden, load-only (no save pin).
    const std::vector<std::uint8_t> kNegTemp = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C, 0x02,
        0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 1,
        2, 0, 0, 0, 3, 0, 0, 0, 1, 2, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0,
        0xFF, 0xFF, 2, 0, 0, 0, 3, 0, 0, 0, 0xFF, 0x7F};
    ashfall::SimulationWorld restored;
    expect(restored.load(kNegTemp), "v2 negative/temp payload loads");
    expect(restored.cell({-1, 0}) == ashfall::kStone && restored.cell({2, 3}) == ashfall::kStone,
        "v2 negative-coord cells preserved");
    expect(restored.temperature({-1, 0}) == -1 && restored.temperature({2, 3}) == 32767,
        "v2 temp edges preserved");
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

    // Membership, not exact equality: the contract only guarantees the
    // removed populated chunk is dirtied, so extra invalidations would be
    // legal. The clear first isolates reset's own dirtiness — without it a
    // do-nothing reset would still pass on stale load dirt.
    world.clear_dirty_chunks();
    world.reset();
    expect(world.tick() == 0 && world.occupied_cell_count() == 0, "reset clears world state and tick");
    const auto reset_dirty = world.dirty_chunks();
    expect(std::find(reset_dirty.begin(), reset_dirty.end(), ashfall::CellPosition{2, 0}) != reset_dirty.end(),
        "reset dirties the removed populated chunk");
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
    {"id": 8, "name": "rock", "movement": "static", "solid": true, "density": 2000, "color": [120, 115, 110], "tags": ["static"], "hardness": 3},
    {"id": 9, "name": "oil", "movement": "liquid", "solid": false, "density": 40, "color": [122, 72, 18], "tags": ["liquid", "burnable"]}
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
    expect(canonical.size() == 18 && canonical[0] == 0 && canonical[17] == 17,
        "canonical order restored");
}

void test_temperature_set_and_query() {
    ashfall::SimulationWorld world;
    expect(world.temperature({0, 0}) == 0, "ambient reads zero");
    expect(world.temperature_count() == 0, "fresh world stores nothing");
    // v3 cell-owned heat: setting heat on air is rejected (false, nothing
    // stored); every address below is occupied before it is heated.
    expect(!world.set_temperature({3, -2}, 100), "set-temperature on air rejects");
    expect(world.temperature_count() == 0, "rejected set stores nothing");
    world.set_cell({3, -2}, ashfall::kStone);
    expect(world.set_temperature({3, -2}, 100), "heat stored returns true");
    expect(world.temperature({3, -2}) == 100, "heat stored and read");
    expect(world.temperature_count() == 1, "one entry stored");
    expect(world.set_temperature({3, -2}, -40), "cold overwrite returns true");
    expect(world.temperature({3, -2}) == -40, "cold overwrites heat");
    expect(world.temperature_count() == 1, "overwrite keeps one entry");
    expect(world.set_temperature({3, -2}, 0), "zero erase returns true");
    expect(world.temperature({3, -2}) == 0, "zero reads ambient");
    expect(world.temperature_count() == 0, "zero erases the entry");
    world.set_cell({0, 0}, ashfall::kStone);
    expect(world.set_temperature({0, 0}, 40000), "clamped store returns true");
    expect(world.temperature({0, 0}) == 32767, "heat clamps to int16 max");
    world.set_cell({1, 1}, ashfall::kStone);
    expect(world.set_temperature({1, 1}, -40000), "clamped cold returns true");
    expect(world.temperature({1, 1}) == -32767, "cold clamps to symmetric min -32767 (keeps negation symmetry)");
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
    // Cold mirror: truncation toward zero is symmetric (cells first: heat on
    // air would reject and store nothing).
    world.set_cell({5, 5}, ashfall::kStone);
    world.set_cell({6, 5}, ashfall::kStone);
    expect(world.set_temperature({5, 5}, -100), "cold stored on stone");
    world.step();
    expect(world.temperature({5, 5}) == -20 && world.temperature({6, 5}) == -20, "cold spreads symmetrically");
}

void test_temperature_ambient_world_stays_sparse() {
    ashfall::SimulationWorld empty;
    expect(empty.save().size() == 117, "empty v3 save is roster framing plus zero counts");
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {20, 10}, ashfall::kStone);
    world.fill_rect({1, 9}, {19, 9}, ashfall::kWater);
    world.fill_rect({5, 5}, {8, 8}, ashfall::kSand);
    for (int i = 0; i < 50; ++i) world.step();
    expect(world.temperature_count() == 0, "ambient play never allocates heat");
    ashfall::SimulationWorld restored;
    expect(restored.load(world.save()), "ambient v3 save loads");
    expect(restored.temperature_count() == 0, "ambient load stores nothing");
}

void test_temperature_save_round_trip_v3() {
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
    expect(restored.load(saved), "v3 save with heat loads");
    expect(restored.tick() == 1, "tick preserved");
    expect(restored.cell({1, 1}) == ashfall::kSand, "cells preserved");
    expect(restored.temperature({0, 2}) == 10 && restored.temperature({1, 2}) == 10 &&
            restored.temperature({2, 2}) == -9 && restored.temperature({1, 1}) == -10,
        "heat preserved across round-trip");
    expect(restored.save() == saved, "v3 round-trip is byte-exact");
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
    expect(world.load(v1), "v1 save loads under the retained v1 reader");
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
    expect(world.occupied_cell_count() == 1,
        "burnout removes fire despite inapplicable higher-priority rule");
    expect(world.cell({0, 0}) == ashfall::kAir, "dry fire burns out despite quench priority");
    expect(world.cell({1, 0}) == ashfall::kWood, "wood survives");
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
    // Reactions are data, not state: the test loads a FROZEN 8-row copy of
    // the rule set — not production (production data/reactions.json has 9
    // rows including oil-flash); godot_p2_verify loads the real file
    // through the bridge. Exactly like the host boot otherwise.
    const char* kFrozenReactions = R"JSON({
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
    expect(ashfall::parse_reaction_data(kFrozenReactions, rules, error), "frozen reactions parse");
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
    // CONTENT: kiln geometry above survives the burn — authored layout pin.
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
    // GOLDEN: 200-tick extinction schedule under the frozen 8-row rules.
    expect(wood <= 19, "no reaction creates wood");
    ashfall::SimulationWorld again;
    again.build_play_level();
    expect(again.load_reactions(rules), "pit rules load on the twin");
    for (int i = 0; i < 200; ++i) again.step();
    expect(again.save() == world.save(), "pit burn is deterministic");
}

void test_oil_floats_on_water() {
    // Density data drives buoyancy: oil (40) rises through water (100) and
    // layers above it with no engine branch — inverted start proves active
    // rise, not just containment; the cup contains sideways dispersal.
    ashfall::SimulationWorld world;
    world.fill_rect({10, 20}, {20, 20}, ashfall::kStone);
    world.fill_rect({10, 10}, {10, 19}, ashfall::kStone);
    world.fill_rect({20, 10}, {20, 19}, ashfall::kStone);
    world.fill_rect({11, 11}, {19, 14}, ashfall::kWater);
    world.fill_rect({11, 15}, {19, 19}, ashfall::kOil);
    for (int i = 0; i < 200; ++i) world.step();
    std::size_t oil = 0, water = 0;
    int oil_bottom = -1, water_top = 512;
    for (int y = 10; y <= 20; ++y) {
        for (int x = 10; x <= 20; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kOil) {
                ++oil;
                oil_bottom = y;
            } else if (m == ashfall::kWater) {
                ++water;
                water_top = (y < water_top) ? y : water_top;
            }
        }
    }
    expect(oil == 45, "oil mass conserved while rising");
    expect(water == 36, "water mass conserved under oil");
    expect(oil_bottom < water_top, "oil rises and layers cleanly above water");
}

void test_oil_flash_reacts_and_heats() {
    // Generic oil-to-fire behavior: p=100 with the temperature flag set
    // explicitly (the declared flag-based API needs BOTH fields — setting
    // product_temperature alone requests no heat). Asserts products AND heat.
    // Single tick only: if ignition failed, fire would rise and oil slosh
    // apart, so later ticks would test movement eligibility, not reaction.
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kOil);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec flash;
    flash.name = "oil-flash";
    flash.probability = 100;
    flash.reactants = {{false, "fire"}, {false, "oil"}};
    flash.products = {{"fire"}, {"fire"}};
    flash.has_product_temperature = true;
    flash.product_temperature = 150;
    table.reactions = {flash};
    expect(world.load_reactions(table), "oil-flash loads");
    world.step();
    // Fire is gas: the products rise away the same tick they ignite, so
    // count the neighborhood (Task 6 spread style), not the origin cell.
    std::size_t oil = 0, fire = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kOil) ++oil;
            else if (m == ashfall::kFire) ++fire;
        }
    }
    expect(oil == 0, "oil flashes on the first tick");
    expect(fire == 2, "flash conserves the pair as fire");
    // Both flames carry 150 to their risen homes and conduct together:
    // (150 + 150 from the twin flame) / 5 = 60, mirroring spread heat.
    expect(world.temperature({1, -1}) == 60, "flash products carry heat to their new home");
}

void test_oil_flash_first_tick_golden() {
    // GOLDEN: production oil-flash row (p=90) ignites this exact adjacency
    // on tick 0. Pins the deterministic hash-roll schedule, not the general
    // reaction semantics (those are covered at p=100 above).
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kOil);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec flash;
    flash.name = "oil-flash";
    flash.probability = 90;
    flash.reactants = {{false, "fire"}, {false, "oil"}};
    flash.products = {{"fire"}, {"fire"}};
    flash.has_product_temperature = true;
    flash.product_temperature = 150;
    table.reactions = {flash};
    expect(world.load_reactions(table), "oil-flash loads");
    world.step();
    std::size_t oil = 0, fire = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kOil) ++oil;
            else if (m == ashfall::kFire) ++fire;
        }
    }
    expect(oil == 0, "p=90 golden ignites on the first tick");
    expect(fire == 2, "golden flash conserves the pair as fire");
}

void test_oil_flash_production_row() {
    // Production-content integration: loads the actual oil-flash row from
    // data/reactions.json (9 production rows) instead of a hand-built
    // literal, so data edits that break ignition fail here.
    // CWD assumption: tests run from the repo root (`./build/ashfall_sim_tests`)
    // or under ctest (CWD is `build/`); no existing C++ test reads data
    // files, so both relative candidates are tried. Single tick only —
    // eligibility moves (see above).
    std::string json_text;
    for (const char* candidate : {"data/reactions.json", "../data/reactions.json"}) {
        std::ifstream file(candidate);
        if (file) {
            std::ostringstream stream;
            stream << file.rdbuf();
            json_text = stream.str();
            break;
        }
    }
    expect(!json_text.empty(), "production reactions.json opens");
    ashfall::ReactionTable rules;
    std::string error;
    expect(ashfall::parse_reaction_data(json_text, rules, error), "production reactions parse");
    expect(rules.reactions.size() == 9, "production table holds 9 rows");
    expect(rules.reactions[0].name == "oil-flash", "oil-flash leads the production table");
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kOil);
    expect(world.load_reactions(rules), "production rules load");
    world.step();
    std::size_t oil = 0, fire = 0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
            const ashfall::MaterialId m = world.cell({x, y});
            if (m == ashfall::kOil) ++oil;
            else if (m == ashfall::kFire) ++fire;
        }
    }
    expect(oil == 0, "production oil-flash ignites on the first tick");
    expect(fire == 2, "production flash conserves the pair as fire");
}

void test_reaction_shared_reactant_double_transform() {
    // Option A case (a): two primaries sharing one neighbor reactant BOTH
    // transform while one neighbor claim wins (first hit per cell: same
    // position, both neighbor-imposed, lower rule index, then lower primary
    // x). Gateless p=100 rules on static materials only, so no temperature
    // state is read and the Round-2 heat-transport change cannot shift this
    // pin (reactions run before conduction; static products never move).
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kWood);  // left primary
    world.set_cell({1, 0}, ashfall::kStone); // shared neighbor reactant
    world.set_cell({2, 0}, ashfall::kRock);  // right primary
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
    expect(world.load_reactions(table), "shared-reactant rules load");
    world.step();
    expect(world.cell({0, 0}) == ashfall::kRock, "left primary transforms despite sharing its reactant");
    expect(world.cell({2, 0}) == ashfall::kWood, "right primary transforms despite sharing its reactant");
    expect(world.cell({1, 0}) == ashfall::kRock, "shared neighbor shows the lower-rule-index winner");
    expect(world.occupied_cell_count() == 3, "shared-reactant pin conserves mass");
}

void test_reaction_half_applied_instance() {
    // Option A case (b): the primary wins its own cell (own claim,
    // uncontested) while its neighbor product loses to the neighbor's own
    // reaction (own beats neighbor-imposed) — HALF the instance applies.
    // Gateless p=100 rules on static materials only: immune to the Round-2
    // heat-transport change (no temperature gates, no movement).
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kWood);
    world.set_cell({1, 0}, ashfall::kStone);
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec pair;
    pair.name = "pair";
    pair.probability = 100;
    pair.reactants = {{false, "wood"}, {false, "stone"}};
    pair.products = {{"rock"}, {"rock"}};
    ashfall::ReactionSpec self;
    self.name = "self";
    self.probability = 100;
    self.reactants = {{false, "stone"}};
    self.products = {{"wood"}};
    table.reactions = {pair, self};
    expect(world.load_reactions(table), "half-apply rules load");
    world.step();
    expect(world.cell({0, 0}) == ashfall::kRock, "primary half of the instance applies");
    expect(world.cell({1, 0}) == ashfall::kWood, "neighbor shows its own reaction, not the pair product");
    expect(world.occupied_cell_count() == 2, "half-applied pin conserves mass");
}

// Save byte-layout helpers for the v2-strict (S1) battery: magic(7) +
// version(1) + tick u64le + ncell u32le + cells x (x u32le, y u32le, id u8)
// + ntemp u32le + temps x (x u32le, y u32le, temp i16le).
constexpr std::size_t kSaveHeaderBytes = 8 + 8 + 4;
constexpr std::size_t kSaveCellRecord = 9;
constexpr std::size_t kSaveTempRecord = 10;

std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32_le(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

// BEHAVIOR(v2-heat): hot material carries its payload into air (cell-owned
// heat, Group 2 Task H). The old coordinate-sticky code left the tick-start
// entry behind AND staged the arrival, so heat duplicated; now the vacated
// cell reads ambient and exactly one staged entry survives.
void test_heat_hot_sand_falls_to_air() {
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kSand);
    world.set_temperature({0, 0}, 50);
    world.step();
    expect(world.cell({0, 1}) == ashfall::kSand, "hot sand falls one cell");
    expect(world.temperature({0, 0}) == 0, "vacated cell keeps no heat");
    // Conducted: the lone arrival sees only ambient neighbors, 50/5 = 10.
    expect(world.temperature({0, 1}) == 10, "carried heat conducts at the new home");
    expect(world.temperature_count() == 1, "no stale coordinate entry survives");
    world.step();
    expect(world.cell({0, 2}) == ashfall::kSand, "hot sand falls again");
    expect(world.temperature({0, 1}) == 0, "second vacated cell keeps no heat");
    expect(world.temperature({0, 2}) == 2, "carried heat conducts again (10/5)");
    expect(world.temperature_count() == 1, "heat still rides a single entry");
}

// BEHAVIOR(v2-heat): cold sand chasing hot sand inherits nothing (the
// cold-follows-hot duplication repro). The old code kept a stale 100 at the
// old coordinate AND staged 100 at the arrival, conducting 40/40; cell-owned
// heat conducts 20/20 with exactly two entries.
void test_heat_cold_follows_hot_no_duplication() {
    ashfall::SimulationWorld world;
    world.set_cell({0, -1}, ashfall::kSand);
    world.set_cell({0, 0}, ashfall::kSand);
    world.set_temperature({0, 0}, 100);
    world.step();
    expect(world.cell({0, 0}) == ashfall::kSand, "cold follower takes the old hot home");
    expect(world.cell({0, 1}) == ashfall::kSand, "hot leader falls one cell");
    expect(world.temperature({0, 0}) == 20, "follower warms only by conduction");
    expect(world.temperature({0, 1}) == 20, "leader carries its payload");
    expect(world.temperature({0, -1}) == 0, "departed cell keeps no heat");
    expect(world.temperature_count() == 2, "exactly rider plus warmed follower");
}

// BEHAVIOR(v2-heat): powder-arm swap exchanges heat payloads — hot sand
// sinks through settled cold water, so the sand keeps 100 and the risen
// water warms only by conduction. Conducted snapshot: sand (0,0) warms the
// water above and the stone below to 20 each; side stones stay ambient.
void test_heat_hot_powder_swaps_cold_liquid() {
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 1}, {1, 1}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({0, -1}, ashfall::kSand);
    world.set_temperature({0, -1}, 100);
    world.step();
    expect(world.cell({0, 0}) == ashfall::kSand, "hot sand sinks into the settled water");
    expect(world.cell({0, -1}) == ashfall::kWater, "cold water rises to the sand home");
    expect(world.temperature({0, 0}) == 20, "sunk sand keeps its payload");
    expect(world.temperature({0, -1}) == 20, "risen water warms by conduction only");
    expect(world.temperature({0, 1}) == 20, "floor warms under the hot sand");
    expect(world.temperature_count() == 3, "sand plus two conducted neighbors");
}

// BEHAVIOR(v2-heat): the reverse payload direction through the same
// powder-arm swap — cold sand sinks through settled hot water. Conducted:
// the risen water keeps its payload (20), the sunk sand warms by conduction
// (20), and the floor sees only ambient sand (stays 0).
void test_heat_cold_powder_swaps_hot_liquid() {
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 1}, {1, 1}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({0, -1}, ashfall::kSand);
    world.set_temperature({0, 0}, 100);
    world.step();
    expect(world.cell({0, 0}) == ashfall::kSand, "cold sand sinks into the settled water");
    expect(world.cell({0, -1}) == ashfall::kWater, "hot water rises to the sand home");
    expect(world.temperature({0, -1}) == 20, "risen water keeps its payload");
    expect(world.temperature({0, 0}) == 20, "sunk sand warms by conduction only");
    expect(world.temperature({0, 1}) == 0, "floor stays ambient under cold sand");
    expect(world.temperature_count() == 2, "water plus conducted sand");
}

// BEHAVIOR(v2-heat): liquid-arm density swap moves heat with the material
// too — hot lava (density 140) sinks through settled cold water (100).
// Same conducted geometry as the powder-arm case (20/20/20, three entries),
// reached through the liquid displacement path instead.
void test_heat_hot_liquid_sinks_through_cold_liquid() {
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 1}, {1, 1}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({0, -1}, ashfall::kLava);
    world.set_temperature({0, -1}, 100);
    world.step();
    expect(world.cell({0, 0}) == ashfall::kLava, "hot lava sinks through the settled water");
    expect(world.cell({0, -1}) == ashfall::kWater, "cold water rises to the lava home");
    expect(world.temperature({0, 0}) == 20, "sunk lava keeps its payload");
    expect(world.temperature({0, -1}) == 20, "risen water warms by conduction only");
    expect(world.temperature({0, 1}) == 20, "floor warms under the hot lava");
    expect(world.temperature_count() == 3, "lava plus two conducted neighbors");
}

// BEHAVIOR(v2-heat): heated in-flight powder displaced by infiltration keeps
// its payload (the §3.2 fix: heat_of reads staged-next first, so the
// arrival's staged temp wins over the old coordinate's temp). The old code
// staged the old coordinate's ambient temp instead, leaving the displaced
// sand cold (conducted zeros, zero entries).
void test_heat_infiltration_displaces_heated_arrival() {
    ashfall::SimulationWorld world;
    // Row y=-1: arrival lane (0,-1) flanked by stone (-1,-1)/(-2,-1)/(2,-1)
    // so the sand must arrive diagonally; settled water W2 at (1,-1) blocks
    // the droplet's right diag while staying escapable (liquid in current).
    world.set_cell({-2, -1}, ashfall::kStone);
    world.set_cell({-1, -1}, ashfall::kStone);
    world.set_cell({2, -1}, ashfall::kStone);
    world.set_cell({1, -1}, ashfall::kWater);
    // Row y=0: floor stones blocking every diag the droplet could take.
    world.fill_rect({-1, 0}, {2, 0}, ashfall::kStone);
    // Row y=-2: heated sand arrives first (x-asc order), then the droplet
    // above the arrival infiltrates down, swapping the pair.
    world.set_cell({-1, -2}, ashfall::kSand);
    world.set_cell({0, -2}, ashfall::kWater);
    world.set_temperature({-1, -2}, 100);
    world.step();
    expect(world.cell({0, -1}) == ashfall::kWater, "droplet seeps into the arrival lane");
    expect(world.cell({0, -2}) == ashfall::kSand, "heated arrival relocates to the droplet home");
    expect(world.cell({1, -1}) == ashfall::kWater, "blocking water stays settled");
    // Conducted: the relocated sand keeps 100 and warms the droplet above;
    // the blocker sees only ambient neighbors.
    expect(world.temperature({0, -2}) == 20, "displaced sand keeps its payload");
    expect(world.temperature({0, -1}) == 20, "seeped water warms by conduction only");
    expect(world.temperature({1, -1}) == 0, "settled blocker stays ambient");
    expect(world.temperature_count() == 2, "sand plus conducted droplet");
}

// BEHAVIOR(v2-heat): save/load twin continuation across the hot-swap case —
// build, one transport tick, twin via save/load, then per-tick save compare.
void test_heat_hot_swap_save_load_continuation() {
    ashfall::SimulationWorld world;
    world.fill_rect({-1, 1}, {1, 1}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({0, -1}, ashfall::kSand);
    world.set_temperature({0, -1}, 100);
    world.step();
    ashfall::SimulationWorld twin;
    expect(twin.load(world.save()), "hot-swap twin loads");
    expect(twin.save() == world.save(), "twin starts identical");
    for (int i = 0; i < 3; ++i) {
        world.step();
        twin.step();
        expect(twin.save() == world.save(), "twin tracks each post-swap tick");
        expect(twin.tick() == world.tick(), "twin tick tracks");
    }
}

// BEHAVIOR(v2-heat): save/load twin continuation across the infiltration
// case — same build/twin/per-tick-compare shape as the hot-swap twin.
void test_heat_infiltration_save_load_continuation() {
    ashfall::SimulationWorld world;
    world.set_cell({-2, -1}, ashfall::kStone);
    world.set_cell({-1, -1}, ashfall::kStone);
    world.set_cell({2, -1}, ashfall::kStone);
    world.set_cell({1, -1}, ashfall::kWater);
    world.fill_rect({-1, 0}, {2, 0}, ashfall::kStone);
    world.set_cell({-1, -2}, ashfall::kSand);
    world.set_cell({0, -2}, ashfall::kWater);
    world.set_temperature({-1, -2}, 100);
    world.step();
    ashfall::SimulationWorld twin;
    expect(twin.load(world.save()), "infiltration twin loads");
    expect(twin.save() == world.save(), "twin starts identical");
    for (int i = 0; i < 3; ++i) {
        world.step();
        twin.step();
        expect(twin.save() == world.save(), "twin tracks each post-seep tick");
        expect(twin.tick() == world.tick(), "twin tick tracks");
    }
}

void test_save_rejects_duplicate_cell() {
    // v3 policy: save() emits chunk-major ascending cells, so a repeated
    // coordinate (foreign/hand-corrupted) rejects with BadChunkOrder (the key
    // compare names dup and descent alike; v1/v2 readers keep the
    // DuplicateCell/UnsortedCells split). CRC re-sealed after the splice.
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_temperature({5, 5}, 40);
    world.step();
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    auto dup = snapshot;
    const std::size_t cells_off = g3_v3_cell_count_offset(dup) + 4;
    const std::uint32_t cell_count = g3_read_u32_le(dup, cells_off - 4);
    const std::vector<std::uint8_t> first(dup.begin() + cells_off, dup.begin() + cells_off + 9);
    dup.insert(dup.begin() + cells_off + 9, first.begin(), first.end());
    g3_write_u32_le(dup, cells_off - 4, cell_count + 1);
    g3_fix_v3_crc(dup);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(dup, error), "duplicate cell rejected");
    expect(error == ashfall::SimulationWorld::LoadError::BadChunkOrder, "duplicate cell names BadChunkOrder");
    expect(!world.load(dup), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_unsorted_cells() {
    // v3 policy: a descending adjacent pair (foreign/hand-corrupted) rejects
    // with BadChunkOrder. CRC re-sealed after the swap.
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_cell({2, 0}, ashfall::kStone);
    world.step();
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    auto unsorted = snapshot;
    const std::size_t cells_off = g3_v3_cell_count_offset(unsorted) + 4;
    std::vector<std::uint8_t> first(unsorted.begin() + cells_off, unsorted.begin() + cells_off + 9);
    std::copy(unsorted.begin() + cells_off + 9, unsorted.begin() + cells_off + 18,
        unsorted.begin() + cells_off);
    std::copy(first.begin(), first.end(), unsorted.begin() + cells_off + 9);
    g3_fix_v3_crc(unsorted);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(unsorted, error), "unsorted cells rejected");
    expect(error == ashfall::SimulationWorld::LoadError::BadChunkOrder, "descent names BadChunkOrder");
    expect(!world.load(unsorted), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_duplicate_temp() {
    // v3 policy: a repeated temp coordinate rejects with BadChunkOrder
    // (temps follow the same chunk-major key subsequence as cells).
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_cell({1, 0}, ashfall::kStone);
    world.set_temperature({0, 0}, 10);
    world.set_temperature({1, 0}, 20);
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    const std::size_t cells_off = g3_v3_cell_count_offset(snapshot) + 4;
    const std::uint32_t cell_count = g3_read_u32_le(snapshot, cells_off - 4);
    const std::size_t temp_count_off = cells_off + static_cast<std::size_t>(cell_count) * 9;
    auto dup = snapshot;
    const std::uint32_t temp_count = g3_read_u32_le(dup, temp_count_off);
    expect(temp_count == 2, "fixture holds two temps");
    const std::size_t first_off = temp_count_off + 4;
    const std::vector<std::uint8_t> first(dup.begin() + first_off, dup.begin() + first_off + 10);
    dup.insert(dup.begin() + first_off + 10, first.begin(), first.end());
    g3_write_u32_le(dup, temp_count_off, temp_count + 1);
    g3_fix_v3_crc(dup);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(dup, error), "duplicate temp rejected");
    expect(error == ashfall::SimulationWorld::LoadError::BadChunkOrder, "duplicate temp names BadChunkOrder");
    expect(!world.load(dup), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_unsorted_temps() {
    // v3 policy: a descending adjacent temp pair rejects with BadChunkOrder.
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_cell({1, 0}, ashfall::kStone);
    world.set_temperature({0, 0}, 10);
    world.set_temperature({1, 0}, 20);
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    const std::size_t cells_off = g3_v3_cell_count_offset(snapshot) + 4;
    const std::uint32_t cell_count = g3_read_u32_le(snapshot, cells_off - 4);
    const std::size_t temp_count_off = cells_off + static_cast<std::size_t>(cell_count) * 9;
    auto unsorted = snapshot;
    const std::size_t first_off = temp_count_off + 4;
    std::vector<std::uint8_t> first(unsorted.begin() + first_off, unsorted.begin() + first_off + 10);
    std::copy(unsorted.begin() + first_off + 10, unsorted.begin() + first_off + 20,
        unsorted.begin() + first_off);
    std::copy(first.begin(), first.end(), unsorted.begin() + first_off + 10);
    g3_fix_v3_crc(unsorted);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(unsorted, error), "unsorted temps rejected");
    expect(error == ashfall::SimulationWorld::LoadError::BadChunkOrder, "descent names BadChunkOrder");
    expect(!world.load(unsorted), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_ambient_temp() {
    // Canonical saves never store ambient zeros, so a stored zero rejects
    // with AmbientTempStored (v2 and v3 agree on the name).
    ashfall::SimulationWorld world;
    world.set_cell({3, 3}, ashfall::kStone);
    world.set_temperature({3, 3}, 50);
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    const std::size_t cells_off = g3_v3_cell_count_offset(snapshot) + 4;
    const std::uint32_t cell_count = g3_read_u32_le(snapshot, cells_off - 4);
    const std::size_t temp_count_off = cells_off + static_cast<std::size_t>(cell_count) * 9;
    auto zeroed = snapshot;
    expect(g3_read_u32_le(zeroed, temp_count_off) == 1, "fixture holds one temp");
    const std::size_t temp_off = temp_count_off + 4;
    zeroed[temp_off + 8] = 0;
    zeroed[temp_off + 9] = 0;
    g3_fix_v3_crc(zeroed);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(zeroed, error), "stored ambient temp rejected");
    expect(error == ashfall::SimulationWorld::LoadError::AmbientTempStored,
        "stored zero names AmbientTempStored");
    expect(!world.load(zeroed), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_oversize_count() {
    // The count cap fires BEFORE reserve_for, so a 5M cell_count in a small
    // file rejects with Oversize (no large alloc, fast). CRC re-sealed so the
    // cap — not the integrity check — is what fires.
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_temperature({5, 5}, 40);
    world.step();
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    auto huge = snapshot;
    g3_write_u32_le(huge, g3_v3_cell_count_offset(huge), 5000000u);
    g3_fix_v3_crc(huge);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(huge, error), "oversize count rejected");
    expect(error == ashfall::SimulationWorld::LoadError::Oversize, "oversize count names Oversize");
    expect(!world.load(huge), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_out_of_range_coord() {
    // |x|,|y| beyond kMaxSaveCoord (1<<24) reject with CoordOutOfRange (play
    // level is 0..511; x here is (1<<24)+1). CRC re-sealed.
    ashfall::SimulationWorld world;
    world.set_cell({5, 5}, ashfall::kStone);
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    auto far = snapshot;
    g3_write_u32_le(far, g3_v3_cell_count_offset(far) + 4, (1u << 24) + 1);
    g3_fix_v3_crc(far);
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(far, error), "out-of-range coord rejected");
    expect(error == ashfall::SimulationWorld::LoadError::CoordOutOfRange,
        "far coord names CoordOutOfRange");
    expect(!world.load(far), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_save_rejects_chunk_bomb() {
    // 4097 cells 64 apart land in 4097 distinct chunks (chunk size 64),
    // tripping the kMaxSaveChunks (4096) guard. save() emits them chunk-major
    // ascending by construction with a valid CRC, so only the bomb guard fires.
    // v3 file: 117-byte empty framing + 9 bytes per cell, no temps.
    ashfall::SimulationWorld bomb_source;
    for (int i = 0; i < 4097; ++i) bomb_source.set_cell({64 * i, 0}, ashfall::kStone);
    const auto bomb = bomb_source.save();
    expect(bomb.size() == 117 + 4097 * 9, "bomb file stays thin");
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_temperature({5, 5}, 40);
    world.step();
    const auto snapshot = world.save();
    const std::size_t occupied_before = world.occupied_cell_count();
    const std::uint64_t tick_before = world.tick();
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!world.load_with_diagnostic(bomb, error), "chunk bomb rejected");
    expect(error == ashfall::SimulationWorld::LoadError::TooManyChunks, "bomb names TooManyChunks");
    expect(!world.load(bomb), "load() agrees on rejection");
    expect(world.occupied_cell_count() == occupied_before, "failed load preserves cell count");
    expect(world.tick() == tick_before, "failed load preserves tick");
    expect(world.save() == snapshot, "failed load preserves exact state");
}

void test_loader_rejects_dual_key_reactant() {
    // P3 presence-based exclusivity: {"material":"water","tag":"liquid"}
    // must FAIL even though both values are individually valid; unknown slot
    // keys reject too. Each key alone still accepts (positive controls).
    auto wrap = [](const std::string& reaction) {
        return std::string(R"JSON({"version": 1, "reactions": [)JSON") + reaction + "]}";
    };
    const std::string dual = wrap(
        R"JSON({"name": "dual", "probability": 100, "reactants": [{"material": "water", "tag": "liquid"}], "products": [{"material": "water"}]})JSON");
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(!ashfall::parse_reaction_data(dual, table, error), "dual-key both-valid-values rejected");
        expect(!error.empty(), "dual-key failure names the fault");
    }
    const std::string typo = wrap(
        R"JSON({"name": "typo", "probability": 100, "reactants": [{"material": "fire", "materail": "x"}], "products": [{"material": "air"}]})JSON");
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(!ashfall::parse_reaction_data(typo, table, error), "unknown reactant key rejected");
        expect(!error.empty(), "unknown-key failure names the fault");
    }
    const std::string material_only = wrap(
        R"JSON({"name": "ok", "probability": 100, "reactants": [{"material": "fire"}], "products": [{"material": "air"}]})JSON");
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(ashfall::parse_reaction_data(material_only, table, error), "material-only reactant accepts");
    }
    const std::string tag_only = wrap(
        R"JSON({"name": "ok", "probability": 100, "reactants": [{"material": "fire"}, {"tag": "burnable"}], "products": [{"material": "fire"}, {"material": "fire"}]})JSON");
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(ashfall::parse_reaction_data(tag_only, table, error), "tag-only reactant accepts");
    }
}

void test_loader_rejects_forged_movement_enum() {
    // P5 parity: a forged programmatic enum (static_cast<Movement>(7)) must
    // reject — it used to reach step() and silently disappear (no branch
    // places the cell). The canonical default table still validates.
    ashfall::MaterialTable table = ashfall::default_material_table();
    table.materials[2].movement = static_cast<ashfall::Movement>(7);
    std::string error;
    expect(!ashfall::validate_material_table(table, error), "forged Movement rejected");
    expect(!error.empty(), "forged-enum failure names the fault");
    expect(ashfall::validate_material_table(ashfall::default_material_table(), error),
        "default table validates");
    expect(error.empty(), "error cleared on success");
}

void test_loader_rejects_out_of_range_density() {
    // P5 parity: the programmatic route mirrors the JSON route's 0..100000
    // density bound. Zero-density dynamic stays legal (no invented rules).
    for (int bad : {-1, 100001}) {
        ashfall::MaterialTable table = ashfall::default_material_table();
        table.materials[2].density = bad;
        std::string error;
        expect(!ashfall::validate_material_table(table, error), "out-of-range density rejected");
        expect(error == "material \"sand\" density must be 0..100000", "density bound names row and range");
    }
    {
        ashfall::MaterialTable table = ashfall::default_material_table();
        table.materials[2].density = 0;
        std::string error;
        expect(ashfall::validate_material_table(table, error), "zero-density sand stays legal");
    }
}

void test_loader_rejects_oversize_material_table() {
    // P5 parity: programmatic tables beyond 256 rows reject (count bound
    // fires before any per-row check, so duplicate ids below are harmless).
    ashfall::MaterialTable table;
    table.version = 1;
    table.materials.push_back({0, "air", ashfall::Movement::Static, false, 0, {0, 0, 0}, {}, 0});
    for (int i = 0; i < 256; ++i) {
        table.materials.push_back(
            {static_cast<ashfall::MaterialId>((i % 255) + 1), "m" + std::to_string(i),
                ashfall::Movement::Static, false, 0, {0, 0, 0}, {}, 0});
    }
    expect(table.materials.size() == 257, "fixture holds 257 rows");
    std::string error;
    expect(!ashfall::validate_material_table(table, error), "257-row table rejected");
    expect(error == "\"materials\" must hold at most 256 entries", "count bound names the limit");
}

void test_loader_failed_reload_preserves_world() {
    // P6 core: failed table/reaction reloads preserve cells, heat,
    // reactions, and tick — the world is populated BEFORE the bad loads (a
    // rejection on an empty world would prove nothing).
    ashfall::SimulationWorld world;
    world.fill_rect({0, 5}, {4, 5}, ashfall::kStone);
    world.set_cell({2, 3}, ashfall::kSand);
    world.set_temperature({2, 3}, 60);
    ashfall::ReactionTable good;
    good.version = 1;
    ashfall::ReactionSpec burnout;
    burnout.name = "burnout";
    burnout.probability = 100;
    burnout.reactants = {{false, "fire"}};
    burnout.products = {{"air"}};
    good.reactions = {burnout};
    expect(world.load_reactions(good), "good rules load");
    world.step();
    world.step();
    const auto snapshot = world.save();
    const std::uint64_t tick_before = world.tick();
    const std::size_t reactions_before = world.reaction_count();
    const std::size_t heat_before = world.temperature_count();
    auto expect_preserved = [&](const char* message) {
        expect(world.save() == snapshot, message);
        expect(world.tick() == tick_before, "failed reload preserves tick");
        expect(world.reaction_count() == reactions_before, "failed reload preserves reactions");
        expect(world.temperature_count() == heat_before, "failed reload preserves heat");
    };
    {
        ashfall::MaterialTable bad = ashfall::default_material_table();
        bad.materials[2].movement = static_cast<ashfall::Movement>(7);
        expect(!world.load_material_table(bad), "bad table rejected");
        expect_preserved("failed table load preserves cells");
    }
    {
        ashfall::ReactionTable bad = good;
        bad.reactions[0].reactants[0].name = "unobtainium";
        expect(!world.load_reactions(bad), "bad reactions rejected");
        expect_preserved("failed reaction load preserves cells");
    }
    {
        ashfall::ReactionTable malformed;
        malformed.version = 1;
        expect(!world.load_reactions(malformed), "empty table rejected");
        expect_preserved("malformed reaction load preserves cells");
    }
}

void test_loader_parse_output_is_transactional() {
    // P6 core: failed parses leave `out` untouched and clear `error` at the
    // start of every public parse call (commit only on success).
    const char* kMaterials = R"JSON({
  "version": 1,
  "materials": [
    {"id": 0, "name": "air", "movement": "static", "solid": false, "density": 0, "color": [18, 20, 24]},
    {"id": 2, "name": "sand", "movement": "powder", "solid": false, "density": 2, "color": [230, 175, 50]}
  ]
})JSON";
    ashfall::MaterialTable held;
    std::string error;
    expect(ashfall::parse_material_data(kMaterials, held, error), "good material parse");
    expect(error.empty(), "error cleared on successful parse");
    expect(held.materials.size() == 2, "good parse stores two rows");
    expect(!ashfall::parse_material_data("{unterminated", held, error), "bad material parse fails");
    expect(held.materials.size() == 2 && held.materials[1].name == "sand",
        "failed material parse preserves out");
    expect(!error.empty(), "failed parse names the fault");
    const std::string kGoodReaction =
        R"JSON({"version": 1, "reactions": [{"name": "ok", "probability": 100, "reactants": [{"material": "fire"}], "products": [{"material": "air"}]}]})JSON";
    ashfall::ReactionTable rules;
    expect(ashfall::parse_reaction_data(kGoodReaction, rules, error), "good reaction parse");
    expect(error.empty(), "error cleared on successful reaction parse");
    expect(rules.reactions.size() == 1, "good parse stores one rule");
    expect(!ashfall::parse_reaction_data(kGoodReaction + "trailing", rules, error),
        "bad reaction parse fails");
    expect(rules.reactions.size() == 1 && rules.reactions[0].name == "ok",
        "failed reaction parse preserves out");
    expect(!error.empty(), "failed reaction parse names the fault");
}

// Group 3 (Task T) JSON corpus: syntax table + material list + reaction list
// (backlog §12). Table-driven where mechanical; every case asserts
// accept/reject, rejects spot-check path-prefixed messages using CURRENT
// code strings (identifier class `[a-z][a-z0-9_-]*` — hyphen allowed).
void test_json_syntax_corpus() {
    struct Case {
        const char* name;
        std::string input;
        bool accept;
        const char* want;
    };
    const std::vector<Case> cases = {
        {"empty", "", false, "unexpected end of input"},
        {"garbage", "{unterminated", false, "expected string key"},
        {"trailing-comma-object", "{\"a\": 1,}", false, "expected string key"},
        {"trailing-comma-array", "[1, 2,]", false, "unexpected character"},
        {"dup-key", "{\"a\": 1, \"a\": 2}", false, "duplicate key \"a\""},
        {"unterminated-string", "\"abc", false, "unterminated string"},
        {"bad-escape-n", "\"a\\nb\"", false, "unsupported escape"},
        {"bad-escape-u", "\"\\u0041\"", false, "unsupported escape"},
        {"good-escapes", "\"a\\\"b\\\\c\"", true, ""},
        {"truex", "truex", false, "trailing characters after top-level value"},
        {"bare-true-suffix", "tru", false, "expected 'true' or 'false'"},
        {"null", "null", false, "unexpected character 'n'"},
        {"leading-zero", "01", false, "leading zeros are not allowed"},
        {"negative-leading-zero", "-01", false, "leading zeros are not allowed"},
        {"double-zero", "00", false, "leading zeros are not allowed"},
        {"zero", "0", true, ""},
        {"negative-zero", "-0", true, ""},
        {"max-int", "1000000000", true, ""},
        {"min-int", "-1000000000", true, ""},
        {"over-max", "1000000001", false, "limit: integer magnitude exceeds"},
        {"under-min", "-1000000001", false, "limit: integer magnitude exceeds"},
        {"float-dot", "1.5", false, "unsupported number continuation '.'"},
        {"float-exp", "1e3", false, "unsupported number continuation 'e'"},
        {"float-Exp", "1E3", false, "unsupported number continuation 'E'"},
        {"control-char", std::string("\"a\x01b\""), false, "unescaped control character in string"},
        {"utf8-passthrough", "\"caf\xc3\xa9\"", true, ""},
        {"trailing-ws", "1 ", true, ""},
        {"nested", "{\"a\": {\"b\": [1, {\"c\": true}]}}", true, ""},
        {"top-array", "[1]", true, ""},
    };
    for (const Case& c : cases) {
        ashfall::JsonValue root;
        ashfall::JsonParser parser(c.input);
        const bool ok = parser.run(root);
        if (c.accept) {
            if (!ok) std::cerr << "SYNTAX ACCEPT-FAIL [" << c.name << "]: " << parser.error() << '\n';
            expect(ok, c.name);
            expect(parser.error().empty(), c.name);
        } else {
            if (ok) std::cerr << "SYNTAX ACCEPTED BAD [" << c.name << "]: " << c.input << '\n';
            expect(!ok, c.name);
            expect(parser.error().find(c.want) != std::string::npos, c.name);
        }
    }
}

void test_json_parser_limits() {
    // Default input-byte boundary: 1MiB+1 rejects fast, before any parsing.
    {
        ashfall::JsonValue root;
        ashfall::JsonParser parser(std::string((1u << 20) + 1, 'x'));
        expect(!parser.run(root), "1MiB+1 input rejects");
        expect(parser.error().find("limit: input exceeds 1048576 bytes") != std::string::npos,
            "input cap names the limit");
    }
    // Depth ±1 around max_depth 64 (nested arrays).
    {
        std::string deep64, deep65;
        for (int i = 0; i < 64; ++i) deep64 += '[';
        deep64 += '0';
        for (int i = 0; i < 64; ++i) deep64 += ']';
        deep65 = '[' + deep64 + ']';
        ashfall::JsonValue root;
        ashfall::JsonParser ok_parser(deep64);
        expect(ok_parser.run(root), "depth 64 accepts");
        ashfall::JsonParser bad_parser(deep65);
        expect(!bad_parser.run(root), "depth 65 rejects");
        expect(bad_parser.error().find("limit: nesting depth exceeds 64") != std::string::npos,
            "depth cap names the limit");
    }
    // Custom small budgets pin each pre-allocation check directly.
    {
        ashfall::JsonLimits limits;
        limits.max_nodes = 3; // "[1, 2, 3]" needs 4 (array + 3 ints)
        ashfall::JsonValue root;
        ashfall::JsonParser parser("[1, 2, 3]", limits);
        expect(!parser.run(root), "tiny node budget rejects");
        expect(parser.error().find("limit: node count exceeds 3") != std::string::npos,
            "node cap names the limit");
    }
    {
        ashfall::JsonLimits limits;
        limits.max_array_items = 2;
        ashfall::JsonValue root;
        ashfall::JsonParser parser("[1, 2, 3]", limits);
        expect(!parser.run(root), "tiny array budget rejects");
        expect(parser.error().find("limit: array exceeds 2 items") != std::string::npos,
            "array cap names the limit");
    }
    {
        ashfall::JsonLimits limits;
        limits.max_object_fields = 1;
        ashfall::JsonValue root;
        ashfall::JsonParser parser("{\"a\": 1, \"b\": 2}", limits);
        expect(!parser.run(root), "tiny object budget rejects");
        expect(parser.error().find("limit: object exceeds 1 fields") != std::string::npos,
            "object cap names the limit");
    }
    {
        ashfall::JsonLimits limits;
        limits.max_string_bytes = 2;
        ashfall::JsonValue root;
        ashfall::JsonParser parser("\"abcdef\"", limits);
        expect(!parser.run(root), "tiny string budget rejects");
        expect(parser.error().find("limit: string data exceeds 2 bytes") != std::string::npos,
            "string cap names the limit");
    }
    // Schema-level caps (checked before row decoding allocates any spec).
    {
        std::string materials = "{\"version\": 1, \"materials\": [";
        for (int i = 0; i < 257; ++i) {
            if (i > 0) materials += ',';
            materials += "{\"id\": " + std::to_string(i % 256) + ", \"name\": \"m" +
                         std::to_string(i) +
                         "\", \"movement\": \"static\", \"solid\": false, \"density\": 0, "
                         "\"color\": [1, 2, 3]}";
        }
        materials += "]}";
        ashfall::MaterialTable table;
        std::string error;
        expect(!ashfall::parse_material_data(materials, table, error), "257 materials reject");
        expect(error == "limit: materials array exceeds 256 entries", "material cap names the limit");
    }
    {
        std::string reactions = "{\"version\": 1, \"reactions\": [";
        for (int i = 0; i < 4097; ++i) {
            if (i > 0) reactions += ',';
            reactions += "{\"name\": \"r" + std::to_string(i) +
                         "\", \"probability\": 100, \"reactants\": [{\"material\": \"fire\"}], "
                         "\"products\": [{\"material\": \"air\"}]}";
        }
        reactions += "]}";
        ashfall::ReactionTable table;
        std::string error;
        expect(!ashfall::parse_reaction_data(reactions, table, error), "4097 reactions reject");
        expect(error == "limit: reactions array exceeds 4096 entries", "reaction cap names the limit");
    }
}

void test_json_material_corpus() {
    const char* kAir =
        "{\"id\": 0, \"name\": \"air\", \"movement\": \"static\", \"solid\": false, \"density\": 0, "
        "\"color\": [18, 20, 24]}";
    const char* kStone =
        "{\"id\": 1, \"name\": \"stone\", \"movement\": \"static\", \"solid\": true, \"density\": 2000, "
        "\"color\": [128, 133, 138], \"tags\": [\"static\"], \"hardness\": 3}";
    auto doc = [&](const std::string& rows) {
        return std::string("{\"version\": 1, \"materials\": [") + rows + "]}";
    };
    struct Case {
        const char* name;
        std::string input;
        bool accept;
        const char* want;
    };
    const std::vector<Case> cases = {
        {"minimal", doc(std::string(kAir) + "," + kStone), true, ""},
        {"hyphen-names", doc(std::string(kAir) +
                              ",{\"id\": 1, \"name\": \"slag-heap\", \"movement\": \"static\", "
                              "\"solid\": true, \"density\": 9, \"color\": [1, 2, 3], "
                              "\"tags\": [\"heavy-stuff\"], \"hardness\": 1}"),
            true, ""},
        {"dup-id", doc(std::string(kAir) + "," + kAir), false, "materials[1]: duplicate material id 0"},
        {"dup-name",
            doc(std::string(kAir) + "," + kStone +
                ",{\"id\": 2, \"name\": \"stone\", \"movement\": \"static\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2, 3]}"),
            false, "materials[2]: duplicate material name \"stone\""},
        {"missing-air", doc(kStone), false, "material id 0 (air) is required"},
        {"bad-air",
            doc("{\"id\": 0, \"name\": \"void\", \"movement\": \"static\", \"solid\": false, "
                "\"density\": 0, \"color\": [1, 2, 3]}"),
            false, "material id 0 must be non-solid static air with density 0"},
        {"bad-movement",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"smoke\", \"movement\": \"plasma\", \"solid\": false, "
                "\"density\": 0, \"color\": [1, 2, 3]}"),
            false,
            "materials[1].movement: unknown movement \"plasma\" (want static/powder/liquid/gas)"},
        {"density-high",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"heavy\", \"movement\": \"static\", \"solid\": true, "
                "\"density\": 100001, \"color\": [1, 2, 3]}"),
            false, "materials[1].density: field \"density\" out of range"},
        {"density-negative",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"light\", \"movement\": \"static\", \"solid\": false, "
                "\"density\": -1, \"color\": [1, 2, 3]}"),
            false, "materials[1].density: field \"density\" out of range"},
        {"hardness-high",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"hard\", \"movement\": \"static\", \"solid\": true, "
                "\"density\": 9, \"color\": [1, 2, 3], \"hardness\": 7}"),
            false, "materials[1].hardness: field \"hardness\" out of range"},
        {"dup-tags",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"gassy\", \"movement\": \"gas\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2, 3], \"tags\": [\"vapour\", \"vapour\"]}"),
            false, "material \"gassy\" has a duplicate tag"},
        {"empty-tag",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"gassy\", \"movement\": \"gas\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2, 3], \"tags\": [\"\"]}"),
            false, "material \"tags\" must be non-empty strings"},
        {"unknown-field",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"stone\", \"movement\": \"static\", \"solid\": true, "
                "\"density\": 9, \"color\": [1, 2, 3], \"hardnes\": 3}"),
            false, "materials[1]: unknown key \"hardnes\""},
        {"root-metadata", "{\"version\": 1, \"materials\": [], \"metadata\": {}}", false,
            "(root): unknown key \"metadata\""},
        {"bad-identifier",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"Sand\", \"movement\": \"static\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2, 3]}"),
            false,
            "materials[1].name: \"Sand\" is not a valid identifier (want [a-z][a-z0-9_-]*)"},
        {"bad-tag-identifier",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"gassy\", \"movement\": \"gas\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2, 3], \"tags\": [\"Bad Tag\"]}"),
            false, "\"Bad Tag\" is not a valid identifier (want [a-z][a-z0-9_-]*)"},
        {"color-short",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"odd\", \"movement\": \"static\", \"solid\": false, "
                "\"density\": 1, \"color\": [1, 2]}"),
            false, "materials[1].color: material \"color\" must be an array of 3 integers"},
        {"solid-powder",
            doc(std::string(kAir) +
                ",{\"id\": 1, \"name\": \"brick\", \"movement\": \"powder\", \"solid\": true, "
                "\"density\": 5, \"color\": [1, 2, 3]}"),
            false, "materials[1]: material \"brick\" is solid but not static"},
        {"version", "{\"version\": 2, \"materials\": []}", false, "unsupported data version (want 1)"},
        {"empty-array", "{\"version\": 1, \"materials\": []}", false,
            "\"materials\" must be a non-empty array"},
    };
    for (const Case& c : cases) {
        ashfall::MaterialTable table;
        std::string error;
        const bool ok = ashfall::parse_material_data(c.input, table, error);
        if (c.accept) {
            if (!ok) std::cerr << "MATERIAL ACCEPT-FAIL [" << c.name << "]: " << error << '\n';
            expect(ok, c.name);
            expect(error.empty(), c.name);
        } else {
            if (ok) std::cerr << "MATERIAL ACCEPTED BAD [" << c.name << "]\n";
            expect(!ok, c.name);
            expect(error.find(c.want) != std::string::npos, c.name);
        }
    }
}

void test_json_reaction_corpus() {
    const char* kQuench =
        "{\"name\": \"quench\", \"probability\": 20, \"reactants\": [{\"material\": \"fire\"}, "
        "{\"material\": \"water\"}], \"products\": [{\"material\": \"fire\"}, "
        "{\"material\": \"steam\"}]}";
    auto doc = [&](const std::string& rows) {
        return std::string("{\"version\": 1, \"reactions\": [") + rows + "]}";
    };
    struct Case {
        const char* name;
        std::string input;
        bool accept;
        const char* want;
    };
    const std::vector<Case> cases = {
        {"valid", doc(kQuench), true, ""},
        {"hyphen-name",
            doc("{\"name\": \"heat-ignition\", \"probability\": 100, \"reactants\": "
                "[{\"tag\": \"burnable\"}], \"products\": [{\"material\": \"fire\"}]}"),
            true, ""},
        {"dual-both-present",
            doc("{\"name\": \"dual\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"water\", \"tag\": \"liquid\"}], \"products\": "
                "[{\"material\": \"water\"}]}"),
            false, "reactions[0].reactants[0]: specify exactly one of \"material\" or \"tag\""},
        {"dual-neither",
            doc("{\"name\": \"empty\", \"probability\": 100, \"reactants\": [{}], \"products\": "
                "[{\"material\": \"fire\"}]}"),
            false, "reactions[0].reactants[0]: specify exactly one of \"material\" or \"tag\""},
        {"dual-unknown-key",
            doc("{\"name\": \"typo\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\", \"materail\": \"x\"}], \"products\": "
                "[{\"material\": \"air\"}]}"),
            false, "reactions[0].reactants[0]: unknown key \"materail\""},
        {"dual-tag-unknown-key",
            doc("{\"name\": \"typo\", \"probability\": 100, \"reactants\": "
                "[{\"tag\": \"burnable\", \"tag2\": \"x\"}], \"products\": "
                "[{\"material\": \"fire\"}]}"),
            false, "reactions[0].reactants[0]: unknown key \"tag2\""},
        {"dual-empty-value",
            doc("{\"name\": \"empty\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"\"}], \"products\": [{\"material\": \"fire\"}]}"),
            false, "reactions[0].reactants[0]: specify exactly one of \"material\" or \"tag\""},
        {"product-temp-wrong-type",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"product_temperature\": \"hot\"}"),
            false,
            "reactions[0].product_temperature: field \"product_temperature\" must be an integer"},
        {"temperature-wrong-type",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"temperature\": 5}"),
            false, "reactions[0].temperature: temperature must be an object"},
        {"probability-wrong-type",
            doc("{\"name\": \"hot\", \"probability\": \"20\", \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}]}"),
            false, "reactions[0].probability: field \"probability\" must be an integer"},
        {"root-metadata", "{\"version\": 1, \"reactions\": [], \"metadata\": {}}", false,
            "(root): unknown key \"metadata\""},
        {"reaction-unknown-key",
            doc("{\"name\": \"hot\", \"probablity\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}]}"),
            false, "reactions[0]: unknown key \"probablity\""},
        {"product-unknown-key",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\", "
                "\"extra\": 1}]}"),
            false, "reactions[0].products[0]: unknown key \"extra\""},
        {"gate-typo",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"temperature\": {\"min_temperatue\": 25}}"),
            false, "reactions[0].temperature: unknown key \"min_temperatue\""},
        {"empty-temp-object",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"temperature\": {}}"),
            false, "reactions[0].temperature: empty temperature object is not allowed"},
        {"inverted-gate",
            doc("{\"name\": \"gate\", \"probability\": 10, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"air\"}], "
                "\"temperature\": {\"min_temperature\": 50, \"max_temperature\": 10}}"),
            false, "reactions[0] \"gate\": temperature min exceeds max"},
        {"temp-max-bound",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"temperature\": {\"min_temperature\": -32767, \"max_temperature\": 32767}}"),
            true, ""},
        {"temp-out-of-range",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"temperature\": {\"min_temperature\": -32768}}"),
            false,
            "reactions[0].temperature.min_temperature: field \"min_temperature\" out of range"},
        {"product-temp-out-of-range",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}], "
                "\"product_temperature\": 32768}"),
            false,
            "reactions[0].product_temperature: field \"product_temperature\" out of range"},
        {"too-many-products",
            doc("{\"name\": \"wide\", \"probability\": 10, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}, "
                "{\"material\": \"air\"}]}"),
            false, "reactions[0].products: need 1 product per reactant slot (missing tail = identity)"},
        {"dup-names", doc(std::string(kQuench) + "," + kQuench), false,
            "duplicate reaction name \"quench\""},
        {"unresolved-reactant-parses",
            doc("{\"name\": \"weird\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"unobtainium\"}], \"products\": [{\"material\": \"air\"}]}"),
            true, ""},
        {"unresolved-product-parses",
            doc("{\"name\": \"weird\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"unobtainium\"}]}"),
            true, ""},
        {"primary-air",
            doc("{\"name\": \"suck\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"air\"}], \"products\": [{\"material\": \"air\"}]}"),
            false, "reactions[0] \"suck\": primary reactant cannot be \"air\""},
        {"air-to-occupied",
            doc("{\"name\": \"create\", \"probability\": 10, \"reactants\": "
                "[{\"material\": \"steam\"}, {\"material\": \"air\"}], \"products\": "
                "[{\"material\": \"air\"}, {\"material\": \"water\"}]}"),
            false, "reactions[0] \"create\": air reactant cannot yield occupied product"},
        {"probability-range",
            doc("{\"name\": \"hot\", \"probability\": 101, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"fire\"}]}"),
            false, "reactions[0].probability: field \"probability\" out of range"},
        {"version", "{\"version\": 2, \"reactions\": []}", false,
            "unsupported reaction data version (want 1)"},
        {"empty-array", "{\"version\": 1, \"reactions\": []}", false,
            "\"reactions\" must be a non-empty array"},
        {"bad-name-identifier",
            doc("{\"name\": \"Bad Name\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"air\"}]}"),
            false,
            "reactions[0].name: \"Bad Name\" is not a valid identifier (want [a-z][a-z0-9_-]*)"},
        {"bad-reactant-identifier",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"Bad Name\"}], \"products\": [{\"material\": \"air\"}]}"),
            false,
            "reactions[0].reactants[0]: \"Bad Name\" is not a valid identifier "
            "(want [a-z][a-z0-9_-]*)"},
        {"bad-product-identifier",
            doc("{\"name\": \"hot\", \"probability\": 100, \"reactants\": "
                "[{\"material\": \"fire\"}], \"products\": [{\"material\": \"-lead\"}]}"),
            false,
            "reactions[0].products[0]: \"-lead\" is not a valid identifier (want [a-z][a-z0-9_-]*)"},
    };
    for (const Case& c : cases) {
        ashfall::ReactionTable table;
        std::string error;
        const bool ok = ashfall::parse_reaction_data(c.input, table, error);
        if (c.accept) {
            if (!ok) std::cerr << "REACTION ACCEPT-FAIL [" << c.name << "]: " << error << '\n';
            expect(ok, c.name);
            expect(error.empty(), c.name);
        } else {
            if (ok) std::cerr << "REACTION ACCEPTED BAD [" << c.name << "]\n";
            expect(!ok, c.name);
            expect(error.find(c.want) != std::string::npos, c.name);
        }
    }
}

void test_json_identifier_policy() {
    // Owner decision: `[a-z][a-z0-9_-]*` (hyphen admitted for shipped names
    // like "oil-flash"). Reaction names ARE identifier-checked.
    expect(ashfall::is_valid_identifier("oil-flash"), "hyphenated name valid");
    expect(ashfall::is_valid_identifier("heat-ignition"), "hyphenated name valid");
    expect(!ashfall::is_valid_identifier("Bad Name"), "caps+space invalid");
    expect(!ashfall::is_valid_identifier("-lead"), "leading hyphen invalid");
    expect(!ashfall::is_valid_identifier(""), "empty invalid");
    expect(!ashfall::is_valid_identifier("0abc"), "leading digit invalid");
    auto reaction_doc = [](const std::string& name) {
        return std::string("{\"version\": 1, \"reactions\": [{\"name\": \"") + name +
               "\", \"probability\": 100, \"reactants\": [{\"material\": \"fire\"}], "
               "\"products\": [{\"material\": \"air\"}]}]}";
    };
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(ashfall::parse_reaction_data(reaction_doc("oil-flash"), table, error),
            "hyphenated reaction name accepts");
    }
    for (const char* bad : {"Bad Name", "-lead"}) {
        ashfall::ReactionTable table;
        std::string error;
        expect(!ashfall::parse_reaction_data(reaction_doc(bad), table, error),
            "invalid reaction name rejects");
        expect(error.find("is not a valid identifier (want [a-z][a-z0-9_-]*)") != std::string::npos,
            "reaction-name fault names the identifier class");
    }
    {
        ashfall::ReactionTable table;
        std::string error;
        expect(!ashfall::parse_reaction_data(reaction_doc(""), table, error),
            "empty reaction name rejects");
        expect(error.find("must be a non-empty string") != std::string::npos,
            "empty name names the fault");
    }
}

void test_reaction_link_stage() {
    // Vocabulary straight from the default table (no hand-listed copy to rot).
    const ashfall::MaterialTable defaults = ashfall::default_material_table();
    std::vector<std::string> names;
    std::vector<std::string> tags;
    for (const ashfall::MaterialSpec& spec : defaults.materials) {
        names.push_back(spec.name);
        for (const std::string& tag : spec.tags) {
            if (std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
        }
    }
    auto rule = [](const char* name, ashfall::ReactantSpec reactant, const char* product) {
        ashfall::ReactionTable table;
        table.version = 1;
        ashfall::ReactionSpec spec;
        spec.name = name;
        spec.probability = 100;
        spec.reactants = {reactant};
        spec.products = {{product}};
        table.reactions = {spec};
        return table;
    };
    {
        ashfall::ReactionTable table = rule("quench", {false, "fire"}, "fire");
        table.reactions[0].reactants.push_back({false, "water"});
        table.reactions[0].products.push_back({"steam"});
        std::string error;
        expect(ashfall::validate_reaction_links(table, names, tags, error), "known names link");
        expect(error.empty(), "link error cleared on success");
    }
    {
        ashfall::ReactionTable table = rule("quench", {false, "fire"}, "fire");
        table.reactions[0].reactants.push_back({false, "unobtainium"});
        table.reactions[0].products.push_back({"steam"});
        std::string error;
        expect(!ashfall::validate_reaction_links(table, names, tags, error),
            "unknown reactant material rejected");
        expect(error ==
                "reactions[0] \"quench\": reactants[1]: unknown material \"unobtainium\"",
            "unknown reactant names rule+field");
    }
    {
        ashfall::ReactionTable table = rule("quench", {false, "fire"}, "unobtainium");
        std::string error;
        expect(!ashfall::validate_reaction_links(table, names, tags, error),
            "unknown product material rejected");
        expect(error ==
                "reactions[0] \"quench\": products[0]: unknown material \"unobtainium\"",
            "unknown product names rule+field");
    }
    {
        // Dead tag ("vapor" vs the shipped "vapour"): a config bug, rejected.
        ashfall::ReactionTable table = rule("dry", {true, "vapor"}, "air");
        std::string error;
        expect(!ashfall::validate_reaction_links(table, names, tags, error), "dead tag rejected");
        expect(error == "reactions[0] \"dry\": reactants[0]: tag \"vapor\" matches no material",
            "dead tag names rule+field");
    }
    {
        ashfall::ReactionTable table = rule("suck", {false, "air"}, "air");
        std::string error;
        expect(!ashfall::validate_reaction_links(table, names, tags, error),
            "primary-air rejected at link");
        expect(error.find("primary reactant cannot be \"air\"") != std::string::npos,
            "primary-air names the fault");
    }
    // The load_reactions path runs the same link stage BEFORE resolve (rules_
    // untouched on reject): dead tag and primary-air structural rejects both
    // fail the load with prior rules intact.
    {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(rule("burnout", {false, "fire"}, "air")), "burnout loads");
        expect(world.reaction_count() == 1, "one rule stored");
        expect(!world.load_reactions(rule("dry", {true, "vapor"}, "air")),
            "dead tag rejected through load_reactions");
        expect(world.reaction_count() == 1, "dead-tag load keeps previous rules");
        ashfall::ReactionTable primary_air = rule("suck", {false, "air"}, "air");
        // NOTE: primary-air is already rejected structurally, so this load
        // fails at structure validation — the link re-check above covers the
        // direct-link caller route for the same fault.
        expect(!world.load_reactions(primary_air), "primary-air rejected through load_reactions");
        expect(world.reaction_count() == 1, "primary-air load keeps previous rules");
    }
}

void test_reaction_p0_warning() {
    // p=0 rules ACCEPT (legitimate priority-suppressors) and populate the
    // warnings channel with the exact contract text.
    ashfall::ReactionTable table;
    table.version = 1;
    ashfall::ReactionSpec idle;
    idle.name = "idle";
    idle.probability = 0;
    idle.reactants = {{false, "fire"}};
    idle.products = {{"air"}};
    table.reactions = {idle};
    std::string error;
    std::vector<std::string> warnings;
    expect(ashfall::validate_reaction_table(table, error, &warnings), "p=0 validates");
    expect(error.empty(), "no error for p=0");
    expect(warnings.size() == 1, "one p=0 warning");
    expect(warnings[0] ==
            "reactions[0] \"idle\": p=0 never fires but suppresses lower rules on full match",
        "p=0 warning text exact");
    const char* kJson =
        "{\"version\": 1, \"reactions\": [{\"name\": \"idle\", \"probability\": 0, "
        "\"reactants\": [{\"material\": \"fire\"}], \"products\": [{\"material\": \"air\"}]}]}";
    ashfall::ReactionTable parsed;
    expect(ashfall::parse_reaction_data(kJson, parsed, error), "p=0 parses");
    ashfall::SimulationWorld world;
    expect(world.load_reactions(parsed), "p=0 loads");
    expect(world.reaction_count() == 1, "p=0 rule stored");
}

// Group 3 (Task T) S3 P1–P4: serialization purity, round-trip + continuation
// (incl. FlatMap-capacity-boundary straddle sizes), canonical save(load(b))
// identity (+ documented non-canonical false case), byte-identity acceptance.
void test_s3_p1_purity_insertion_order_independent() {
    // P1: the v3 writer walks chunks_ (ordered) row-major — emission order is
    // canonical by construction, so insertion order never affects the bytes
    // (this is what kills the sort AND neutralizes layout divergence).
    const std::vector<std::pair<ashfall::CellPosition, ashfall::MaterialId>> cells = {
        {{0, 0}, ashfall::kFire}, {{1, 0}, ashfall::kWater}, {{2, 0}, ashfall::kWood},
        {{0, 1}, ashfall::kStone}, {{1, 1}, ashfall::kSteam}, {{2, 1}, ashfall::kStone},
        {{-70, -70}, ashfall::kSand}, {{130, 5}, ashfall::kOil},
    };
    ashfall::SimulationWorld forward;
    for (const auto& [pos, mat] : cells) forward.set_cell(pos, mat);
    ashfall::SimulationWorld backward;
    for (auto it = cells.rbegin(); it != cells.rend(); ++it) backward.set_cell(it->first, it->second);
    expect(forward.save() == backward.save(), "insertion order never affects save bytes");
    expect(forward.save().size() > 8 && forward.save()[7] == ashfall::SimulationWorld::kSaveVersion3,
        "purity checked on v3 bytes");
    ashfall::SimulationWorld restored;
    expect(restored.load(forward.save()), "order-independent bytes load");
    expect(restored.save() == forward.save(), "order-independent bytes round-trip");
}

void test_s3_p2_capacity_straddle() {
    // P2 + flatmap-review §F2: FlatMap layout differs between the saving
    // session (insertion order) and the loading session (sorted file order),
    // so save→load→continue must agree at every occupancy, especially across
    // capacity-growth thresholds — straddle sizes on both sides.
    const int sizes[] = {1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128,
        129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025};
    for (int n : sizes) {
        ashfall::SimulationWorld world;
        for (int x = 0; x < n; ++x) world.set_cell({x, 0}, ashfall::kStone);
        world.step();
        ashfall::SimulationWorld twin;
        char msg[128];
        std::snprintf(msg, sizeof(msg), "straddle n=%d loads", n);
        expect(twin.load(world.save()), msg);
        std::snprintf(msg, sizeof(msg), "straddle n=%d starts identical", n);
        expect(twin.save() == world.save(), msg);
        for (int tick = 0; tick < 2; ++tick) {
            world.step();
            twin.step();
            std::snprintf(msg, sizeof(msg), "straddle n=%d tracks tick %d", n, tick);
            expect(twin.save() == world.save(), msg);
        }
    }
    // Dynamic twin across a growth threshold: falling sand + conducted heat.
    {
        ashfall::SimulationWorld world;
        world.fill_rect({0, 70}, {130, 70}, ashfall::kStone);
        for (int x = 0; x < 65; ++x) world.set_cell({x, 60}, ashfall::kSand);
        world.set_temperature({3, 60}, 90);
        world.step();
        ashfall::SimulationWorld twin;
        expect(twin.load(world.save()), "dynamic straddle loads");
        for (int tick = 0; tick < 3; ++tick) {
            world.step();
            twin.step();
            expect(twin.save() == world.save(), "dynamic straddle tracks each tick");
            expect(twin.tick() == world.tick(), "dynamic straddle tick tracks");
        }
    }
}

void test_s3_p2_twin_continuation() {
    // P2: build → N ticks → twin via save/load → M ticks, per-tick save
    // compare, with reactions + heat loaded identically on both sides (the v3
    // reaction guard requires the same table or the twin load rejects).
    auto reactions = []() {
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
        return table;
    };
    ashfall::SimulationWorld world;
    expect(world.load_reactions(reactions()), "twin reactions load");
    world.fill_rect({-2, 4}, {2, 4}, ashfall::kStone);
    world.set_cell({0, 0}, ashfall::kFire);
    world.set_cell({1, 0}, ashfall::kWater);
    world.set_cell({-1, 0}, ashfall::kSand);
    world.set_temperature({0, 4}, 60);
    for (int i = 0; i < 3; ++i) world.step();
    ashfall::SimulationWorld twin;
    expect(twin.load_reactions(reactions()), "twin reactions load on the twin");
    expect(twin.load(world.save()), "reaction+heat twin loads");
    expect(twin.save() == world.save(), "twin starts identical");
    for (int i = 0; i < 5; ++i) {
        world.step();
        twin.step();
        expect(twin.save() == world.save(), "twin tracks each post-load tick");
        expect(twin.tick() == world.tick(), "twin tick tracks");
    }
}

void test_s3_p3_canonical_round_trip() {
    // P3: save(load(b)) == b for canonical v3 b (ticked worlds included).
    {
        ashfall::SimulationWorld world;
        world.fill_rect({-3, 5}, {3, 5}, ashfall::kStone);
        world.set_cell({0, 0}, ashfall::kSand);
        world.set_temperature({0, 0}, 0); // air: rejected, nothing stored
        world.set_cell({1, 1}, ashfall::kWater);
        for (int i = 0; i < 7; ++i) world.step();
        const auto canonical = world.save();
        ashfall::SimulationWorld restored;
        expect(restored.load(canonical), "canonical v3 loads");
        expect(restored.save() == canonical, "save(load(b)) == b for canonical b");
    }
    // Documented NON-canonical false case: v2 bytes load fine (reader
    // retention) but resave as v3 — save(load(v2)) != v2 by design (the writer
    // is v3-only). This is the one legal P3 exception.
    {
        const std::vector<std::uint8_t> kOneStoneV2 = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C,
            0x4C, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0,
            0, 0};
        ashfall::SimulationWorld restored;
        expect(restored.load(kOneStoneV2), "non-canonical v2 still loads");
        expect(restored.cell({1, 2}) == ashfall::kStone, "non-canonical state intact");
        expect(restored.save() != kOneStoneV2, "save(load(v2)) != v2 (writer is v3-only)");
        expect(restored.save().size() == 126, "non-canonical resaves as the 126-byte v3 golden");
        ashfall::SimulationWorld reread;
        expect(reread.load(restored.save()), "resaved canonical loads");
        expect(reread.save() == restored.save(), "resaved bytes are canonical");
    }
}

void test_s3_round_trip_matrix() {
    // P4 acceptance set: every shape below round-trips with byte identity.
    auto check_canonical = [](ashfall::SimulationWorld& world, const char* message) {
        const auto saved = world.save();
        ashfall::SimulationWorld restored;
        expect(restored.load(saved), message);
        expect(restored.save() == saved, message);
    };
    { ashfall::SimulationWorld empty; check_canonical(empty, "matrix: empty canonical"); }
    {
        ashfall::SimulationWorld single;
        single.set_cell({1, 2}, ashfall::kStone);
        check_canonical(single, "matrix: single canonical");
        expect(single.save().size() == 126, "matrix: single is the 126-byte golden");
    }
    {
        // Kiln: the full authored play level after a short settle.
        ashfall::SimulationWorld kiln;
        kiln.build_play_level();
        for (int i = 0; i < 5; ++i) kiln.step();
        check_canonical(kiln, "matrix: kiln canonical");
    }
    {
        // Hot-moving: heat riding falling material mid-flight.
        ashfall::SimulationWorld hot;
        hot.fill_rect({-1, 4}, {1, 4}, ashfall::kStone);
        hot.set_cell({0, 0}, ashfall::kSand);
        hot.set_temperature({0, 0}, 100);
        hot.step();
        expect(hot.temperature_count() > 0, "matrix: hot-moving carries heat");
        check_canonical(hot, "matrix: hot-moving canonical");
    }
    {
        // Negative coords + temp edges.
        ashfall::SimulationWorld neg;
        neg.set_cell({-1, 0}, ashfall::kStone);
        neg.set_cell({2, 3}, ashfall::kStone);
        neg.set_temperature({-1, 0}, -1);
        neg.set_temperature({2, 3}, 32767);
        check_canonical(neg, "matrix: negative canonical");
        expect(neg.save().size() == 155, "matrix: negative is the 155-byte golden");
    }
    {
        // Max-id: oil (id 9, highest default id) cells.
        ashfall::SimulationWorld oil;
        oil.fill_rect({10, 20}, {20, 20}, ashfall::kStone);
        oil.fill_rect({10, 10}, {10, 19}, ashfall::kStone);
        oil.fill_rect({20, 10}, {20, 19}, ashfall::kStone);
        oil.fill_rect({11, 15}, {19, 19}, ashfall::kOil);
        check_canonical(oil, "matrix: max-id canonical");
        ashfall::SimulationWorld restored;
        expect(restored.load(oil.save()), "matrix: max-id loads");
        expect(restored.cell({15, 17}) == ashfall::kOil, "matrix: max-id cell preserved");
    }
}

// Group 3 (Task T) v3 migration: roster remap accept, rename/remove reject,
// reaction guard (+ override), future-version + CRC rejects.
void test_v3_skew_remap_accept() {
    // Same names, moved ids: the roster remaps old-id → current-id by stable
    // name, so saves cross-load in both directions with equivalent state.
    ashfall::MaterialTable remapped;
    remapped.version = 1;
    const ashfall::MaterialTable defaults = ashfall::default_material_table();
    const ashfall::MaterialId moved[] = {0, 21, 22, 23, 24, 25, 26, 27, 28, 29};
    for (std::size_t i = 0; i < defaults.materials.size(); ++i) {
        ashfall::MaterialSpec spec = defaults.materials[i];
        spec.id = moved[i];
        remapped.materials.push_back(spec);
    }
    ashfall::SimulationWorld source;
    source.set_cell({1, 2}, ashfall::kStone);
    source.set_cell({3, 4}, ashfall::kSand);
    const auto saved = source.save();
    ashfall::SimulationWorld moved_world(remapped);
    expect(moved_world.load(saved), "remapped-id table accepts the save");
    expect(moved_world.cell({1, 2}) == 21, "stone remapped to moved id");
    expect(moved_world.cell({3, 4}) == 22, "sand remapped to moved id");
    expect(moved_world.tick() == source.tick(), "remap preserves tick");
    // Reaction hashes are NAME-keyed: identical rules under moved ids hash
    // identically, and the remapped save loads back into defaults.
    auto quench_table = []() {
        ashfall::ReactionTable table;
        table.version = 1;
        ashfall::ReactionSpec quench;
        quench.name = "quench";
        quench.probability = 100;
        quench.reactants = {{false, "fire"}, {false, "water"}};
        quench.products = {{"fire"}, {"steam"}};
        table.reactions = {quench};
        return table;
    };
    expect(source.load_reactions(quench_table()), "rules load on defaults");
    expect(moved_world.load_reactions(quench_table()), "rules load on moved ids");
    expect(source.reaction_table_hash() == moved_world.reaction_table_hash(),
        "reaction hash stable across id remap");
    const auto moved_saved = moved_world.save();
    expect(moved_saved != saved, "remapped save bytes differ (roster carries new ids)");
    ashfall::SimulationWorld back;
    expect(back.load_reactions(quench_table()), "rules load on the return world");
    expect(back.load(moved_saved), "remapped save loads back into defaults");
    expect(back.cell({1, 2}) == ashfall::kStone, "stone maps back to default id");
    expect(back.cell({3, 4}) == ashfall::kSand, "sand maps back to default id");
}

void test_v3_skew_roster_rejects() {
    ashfall::SimulationWorld source;
    source.set_cell({1, 2}, ashfall::kStone);
    const auto saved = source.save(); // roster carries the full default table
    {
        // Removed material: table without oil rejects (roster name unresolvable).
        ashfall::MaterialTable pruned;
        pruned.version = 1;
        for (const ashfall::MaterialSpec& spec : ashfall::default_material_table().materials) {
            if (spec.name != "oil") pruned.materials.push_back(spec);
        }
        ashfall::SimulationWorld world(pruned);
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(saved, error), "removed-material table rejects");
        expect(error == ashfall::SimulationWorld::LoadError::RosterUnknownMaterial,
            "removal names RosterUnknownMaterial");
        expect(world.occupied_cell_count() == 0, "rejected skew load stores nothing");
    }
    {
        // Renamed material: "stone" → "granite" rejects the same way.
        ashfall::MaterialTable renamed = ashfall::default_material_table();
        renamed.materials[1].name = "granite";
        ashfall::SimulationWorld world(renamed);
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(saved, error), "renamed-material table rejects");
        expect(error == ashfall::SimulationWorld::LoadError::RosterUnknownMaterial,
            "rename names RosterUnknownMaterial");
    }
}

void test_v3_reaction_guard() {
    auto burnout = [](int probability) {
        ashfall::ReactionTable table;
        table.version = 1;
        ashfall::ReactionSpec rule;
        rule.name = "burnout";
        rule.probability = probability;
        rule.reactants = {{false, "fire"}};
        rule.products = {{"air"}};
        table.reactions = {rule};
        return table;
    };
    ashfall::SimulationWorld bare; // 0 reactions
    bare.set_cell({1, 2}, ashfall::kStone);
    const auto bare_save = bare.save();
    // Same-table load always passes the guard.
    expect(bare.load(bare_save), "matching empty guard accepts");
    ashfall::SimulationWorld ruled;
    expect(ruled.load_reactions(burnout(8)), "burnout loads");
    ruled.set_cell({1, 2}, ashfall::kStone);
    // Count skew (0 vs 1) rejects by default, accepts with the dev override.
    ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
    expect(!ruled.load_with_diagnostic(bare_save, error), "reaction-count skew rejects");
    expect(error == ashfall::SimulationWorld::LoadError::ReactionTableMismatch,
        "count skew names ReactionTableMismatch");
    expect(ruled.cell({1, 2}) == ashfall::kStone, "guard rejection preserves world");
    expect(ruled.load_with_diagnostic(
               bare_save, error, ashfall::LoadOptions{true}),
        "override accepts count skew");
    expect(error == ashfall::SimulationWorld::LoadError::Ok, "override reports Ok");
    expect(ruled.cell({1, 2}) == ashfall::kStone, "override load carries cells");
    expect(ruled.tick() == bare.tick(), "override load carries tick");
    // Content skew (same count, edited probability → different hash) rejects
    // too, and accepts with the override.
    ashfall::SimulationWorld edited;
    expect(edited.load_reactions(burnout(9)), "edited burnout loads");
    edited.set_cell({9, 9}, ashfall::kSand);
    const auto edited_save = edited.save();
    expect(!ruled.load(edited_save), "reaction-content skew rejects");
    expect(ruled.load_with_diagnostic(
               edited_save, error, ashfall::LoadOptions{true}),
        "override accepts content skew");
}

void test_v3_version_and_crc() {
    ashfall::SimulationWorld source;
    source.set_cell({1, 2}, ashfall::kStone);
    const auto valid = source.save();
    ashfall::SimulationWorld world;
    world.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
    world.step();
    const auto snapshot = world.save();
    auto expect_unchanged = [&]() {
        expect(world.save() == snapshot, "rejected v3 load preserves exact state");
    };
    {
        auto future = valid;
        future[7] = 9;
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(future, error), "future version rejects");
        expect(error == ashfall::SimulationWorld::LoadError::BadVersion,
            "future version names BadVersion");
        expect_unchanged();
    }
    {
        auto next_bump = valid;
        next_bump[7] = 4;
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(next_bump, error), "version 4 rejects");
        expect(error == ashfall::SimulationWorld::LoadError::BadVersion,
            "version 4 names BadVersion");
        expect_unchanged();
    }
    {
        // Corrupt the stored CRC itself: recomputed != stored → BadCrc.
        auto corrupt_crc = valid;
        corrupt_crc.back() ^= 0x01;
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(corrupt_crc, error), "CRC-corrupt rejects");
        expect(error == ashfall::SimulationWorld::LoadError::BadCrc, "corrupt CRC names BadCrc");
        expect_unchanged();
    }
    {
        // Corrupt one payload bit without re-sealing: integrity fires before
        // any semantic check → BadCrc (NOT the semantic fault underneath).
        auto corrupt_payload = valid;
        corrupt_payload[20] ^= 0x04;
        ashfall::SimulationWorld::LoadError error = ashfall::SimulationWorld::LoadError::Ok;
        expect(!world.load_with_diagnostic(corrupt_payload, error), "unsealed mutant rejects");
        expect(error == ashfall::SimulationWorld::LoadError::BadCrc,
            "unsealed mutant names BadCrc");
        expect_unchanged();
    }
}

// Group 3 (Task T) S6 sweeps: truncation (every prefix), bit-flips, matrix
// (above), continuation-across-boundary, v3 order policy (S1 tests above).
void test_s6_truncation_sweep() {
    // Every strict prefix of a small v2 AND a small v3 golden rejects, with
    // the sentinel world (cells + heat + tick) byte-identical afterwards.
    const std::vector<std::uint8_t> kOneStoneV2 = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C,
        0x02, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0};
    ashfall::SimulationWorld probe;
    probe.set_cell({1, 2}, ashfall::kStone);
    const auto kOneStoneV3 = probe.save();
    expect(kOneStoneV3.size() == 126, "sweep uses the 126-byte v3 golden");
    const std::vector<std::vector<std::uint8_t>> goldens = {kOneStoneV2, kOneStoneV3};
    for (const auto& golden : goldens) {
        ashfall::SimulationWorld sentinel;
        sentinel.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
        sentinel.set_cell({5, 5}, ashfall::kSand);
        sentinel.set_temperature({5, 5}, 40);
        sentinel.step();
        sentinel.step();
        const auto snapshot = sentinel.save();
        const std::uint64_t tick = sentinel.tick();
        for (std::size_t len = 0; len < golden.size(); ++len) {
            const std::vector<std::uint8_t> prefix(golden.begin(), golden.begin() + len);
            if (sentinel.load(prefix)) {
                std::cerr << "TRUNCATION ACCEPTED len " << len << " of " << golden.size() << '\n';
                expect(false, "every truncation prefix rejects");
            }
            expect(sentinel.save() == snapshot, "truncated load preserves exact state");
            expect(sentinel.tick() == tick, "truncated load preserves tick");
        }
    }
}

void test_s6_bitflip_sweep() {
    // v3 single-stone golden (126 bytes): EVERY one of the 1008 single-bit
    // flips rejects. Framing flips report their own check first (BadMagic for
    // bytes 0..6; byte-7 flips route into the retained v1/v2 readers or
    // BadVersion — the loader orders size → magic → version → CRC → payload);
    // every payload/CRC flip reports BadCrc (CRC32 detects all single-bit
    // detects all single-bit errors, and a flip inside the stored-CRC field
    // corrupts the comparison itself). No accept case exists, so nothing
    // needs documenting beyond this paragraph.
    {
        ashfall::SimulationWorld probe;
        probe.set_cell({1, 2}, ashfall::kStone);
        const auto golden = probe.save();
        ashfall::SimulationWorld sentinel;
        sentinel.fill_rect({0, 10}, {10, 10}, ashfall::kStone);
        sentinel.set_cell({5, 5}, ashfall::kSand);
        sentinel.step();
        const auto snapshot = sentinel.save();
        for (std::size_t i = 0; i < golden.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                auto mutant = golden;
                mutant[i] ^= static_cast<std::uint8_t>(1u << bit);
                ashfall::SimulationWorld::LoadError error =
                    ashfall::SimulationWorld::LoadError::Ok;
                if (sentinel.load_with_diagnostic(mutant, error)) {
                    std::cerr << "V3 FLIP ACCEPTED byte " << i << " bit " << bit << '\n';
                    expect(false, "every v3 single-bit flip rejects");
                }
                // Version-byte flips route into the retained v1/v2 readers
                // (03^bit can read as 01/02) or BadVersion — either way the
                // payload never parses as the intended v3, so rejection (already
                // proven by the load() == false above) is the whole contract.
                const bool framing_ok = (i < 7 &&
                        error == ashfall::SimulationWorld::LoadError::BadMagic) ||
                                        (i == 7 &&
                        error != ashfall::SimulationWorld::LoadError::Ok) ||
                                        (i > 7 && error == ashfall::SimulationWorld::LoadError::BadCrc);
                if (!framing_ok) {
                    std::cerr << "V3 FLIP byte " << i << " bit " << bit << " gave error "
                              << static_cast<int>(error) << '\n';
                    expect(false, "every v3 flip names its stage error");
                }
                expect(sentinel.save() == snapshot, "v3 flip preserves exact state");
            }
        }
    }
    // v2 single-stone golden (33 bytes, no integrity seal): enumerated accept
    // classes below (measured against the landed loader; each accept is
    // verified accept-consistent — its v3 resave loads and is byte-canonical).
    //   - tick bytes 8..15: all 64 flips accept (tick is a free u64);
    //   - x-coord low bytes 20..22 + y-coord low bytes 24..26: all 48 accept
    //     (lone cell, coords stay in range, order check is vacuous);
    //   - id byte 28 bits 1..3: accept (ids 3/5/9 = water/fire/oil — known,
    //     non-air; bit 0 gives air → AirCellStored, bits 4..7 give unknown ids
    //     17/33/65/129 → UnknownMaterial);
    //   - everything else rejects (magic/version/counts/high coord bytes push
    //     coords out of range or break section sizes/temp count).
    // Total: exactly 64 + 48 + 3 = 115 accepts out of 264 flips.
    {
        const std::vector<std::uint8_t> golden = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C,
            0x02, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0,
            0};
        int accepts = 0;
        for (std::size_t i = 0; i < golden.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                auto mutant = golden;
                mutant[i] ^= static_cast<std::uint8_t>(1u << bit);
                ashfall::SimulationWorld world;
                if (world.load(mutant)) {
                    ++accepts;
                    // Accept-consistent + resave-canonical: the v3 resave of
                    // the accepted mutant loads and is byte-identical.
                    const auto resaved = world.save();
                    ashfall::SimulationWorld reread;
                    expect(reread.load(resaved), "flipped accept resaves canonically");
                    expect(reread.save() == resaved, "flipped resave is byte-exact");
                }
            }
        }
        expect(accepts == 115, "v2 flip accept set is exactly the 115 documented cases");
    }
    // Empty v2 golden (24 bytes): exactly the 64 tick-byte flips accept.
    {
        const std::vector<std::uint8_t> golden = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C,
            0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        int accepts = 0;
        for (std::size_t i = 0; i < golden.size(); ++i) {
            for (int bit = 0; bit < 8; ++bit) {
                auto mutant = golden;
                mutant[i] ^= static_cast<std::uint8_t>(1u << bit);
                ashfall::SimulationWorld world;
                if (world.load(mutant)) {
                    ++accepts;
                    const auto resaved = world.save();
                    ashfall::SimulationWorld reread;
                    expect(reread.load(resaved), "empty flip resaves canonically");
                    expect(reread.save() == resaved, "empty resave is byte-exact");
                }
            }
        }
        expect(accepts == 64, "empty v2 flip accept set is exactly the 64 tick cases");
    }
    // Targeted pins for the documented classes (not just counts).
    {
        const std::vector<std::uint8_t> golden = {0x41, 0x53, 0x48, 0x46, 0x41, 0x4C, 0x4C,
            0x02, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0,
            0};
        ashfall::SimulationWorld tick_flip;
        auto tick_mutant = golden;
        tick_mutant[8] ^= 0x01;
        expect(tick_flip.load(tick_mutant), "tick-bit flip accepts");
        expect(tick_flip.tick() == 1 && tick_flip.cell({1, 2}) == ashfall::kStone,
            "tick flip keeps tick=1 and the stone");
        ashfall::SimulationWorld id_flip;
        auto id_mutant = golden;
        id_mutant[28] ^= 0x02; // stone(1) -> water(3)
        expect(id_flip.load(id_mutant), "id bit-1 flip accepts");
        expect(id_flip.cell({1, 2}) == ashfall::kWater, "id flip yields water");
        ashfall::SimulationWorld coord_flip;
        auto coord_mutant = golden;
        coord_mutant[20] ^= 0x80; // x: 1 -> 129
        expect(coord_flip.load(coord_mutant), "coord-bit flip accepts");
        expect(coord_flip.cell({129, 2}) == ashfall::kStone, "coord flip moves the stone");
        expect(coord_flip.cell({1, 2}) == ashfall::kAir, "coord flip vacates the old home");
        auto air_mutant = golden;
        air_mutant[28] ^= 0x01; // stone(1) -> air(0)
        ashfall::SimulationWorld air_world;
        expect(!air_world.load(air_mutant), "id bit-0 flip (air) rejects");
        auto unknown_mutant = golden;
        unknown_mutant[28] ^= 0x10; // stone(1) -> id 17
        ashfall::SimulationWorld unknown_world;
        expect(!unknown_world.load(unknown_mutant), "id bit-4 flip (unknown id) rejects");
    }
}

void test_s6_continuation_across_boundary() {
    // Save while cells + heat straddle the y=63/64 chunk boundary, twin via
    // save/load, then per-tick save compare across the crossing.
    ashfall::SimulationWorld world;
    world.set_cell({5, 62}, ashfall::kSand);
    world.set_cell({5, 63}, ashfall::kSand);
    world.set_temperature({5, 62}, 80);
    world.step(); // both fall; the leader crosses into chunk {0, 1}
    expect(world.cell({5, 64}) == ashfall::kSand, "leader crosses the chunk boundary");
    ashfall::SimulationWorld twin;
    expect(twin.load(world.save()), "boundary-straddling twin loads");
    expect(twin.save() == world.save(), "twin starts identical");
    for (int i = 0; i < 4; ++i) {
        world.step();
        twin.step();
        expect(twin.save() == world.save(), "twin tracks each post-boundary tick");
        expect(twin.tick() == world.tick(), "twin tick tracks");
    }
}

// Group 3 (Task T) B2 engine items (bridge §B2 1–4, engine side): final-cell
// deletion dirties the former chunk; reset dirtiness; load invalidates
// old+new regions; copy_rect full overwrite incl. air.
void test_b2_final_cell_deletion_dirties_chunk() {
    ashfall::SimulationWorld world;
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_cell({200, 200}, ashfall::kStone);
    world.clear_dirty_chunks();
    expect(world.dirty_chunks().empty(), "dirty starts empty");
    world.set_cell({5, 5}, ashfall::kAir); // deletes the last cell of chunk {0, 0}
    const auto dirty = world.dirty_chunks();
    expect(dirty.size() == 1 && dirty[0] == ashfall::CellPosition{0, 0},
        "final-cell deletion dirties exactly the former chunk");
    expect(!world.has_chunk({0, 0}), "emptied chunk is gone");
    expect(world.cell({200, 200}) == ashfall::kStone, "distant chunk untouched");
    // Deleting air from air is a no-op: no dirty mark.
    world.clear_dirty_chunks();
    world.set_cell({9, 9}, ashfall::kAir);
    expect(world.dirty_chunks().empty(), "air-to-air set marks nothing dirty");
}

void test_b2_reset_dirtiness() {
    ashfall::SimulationWorld world;
    world.set_cell({5, 5}, ashfall::kSand);    // chunk {0, 0}
    world.set_cell({128, 0}, ashfall::kStone); // chunk {2, 0}
    world.clear_dirty_chunks();
    world.reset();
    expect(world.tick() == 0 && world.occupied_cell_count() == 0, "reset clears state");
    const auto dirty = world.dirty_chunks();
    expect(std::find(dirty.begin(), dirty.end(), ashfall::CellPosition{0, 0}) != dirty.end(),
        "reset dirties removed chunk {0, 0}");
    expect(std::find(dirty.begin(), dirty.end(), ashfall::CellPosition{2, 0}) != dirty.end(),
        "reset dirties removed chunk {2, 0}");
    expect(dirty.size() == 2, "reset dirties exactly the removed populated chunks");
}

void test_b2_load_invalidates_old_and_new() {
    ashfall::SimulationWorld source;
    source.set_cell({320, 320}, ashfall::kStone); // chunk {5, 5}
    const auto saved = source.save();
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kSand); // chunk {0, 0}, disjoint from source
    world.clear_dirty_chunks();
    expect(world.load(saved), "disjoint load succeeds");
    const auto dirty = world.dirty_chunks();
    expect(std::find(dirty.begin(), dirty.end(), ashfall::CellPosition{0, 0}) != dirty.end(),
        "load dirties the vacated old region");
    expect(std::find(dirty.begin(), dirty.end(), ashfall::CellPosition{5, 5}) != dirty.end(),
        "load dirties the populated new region");
    expect(world.cell({320, 320}) == ashfall::kStone, "load carries the new cell");
    expect(world.cell({0, 0}) == ashfall::kAir, "load clears the old cell");
}

void test_b2_copy_rect_full_overwrite() {
    // Every output element is written, including air: a 0xFF prefill must
    // leave no residue anywhere in the rect.
    ashfall::SimulationWorld world;
    world.set_cell({0, 0}, ashfall::kStone);
    world.set_cell({3, 2}, ashfall::kStone);
    std::vector<ashfall::MaterialId> buffer(12, static_cast<ashfall::MaterialId>(0xFF));
    world.copy_rect({0, 0}, {3, 2}, buffer);
    for (int y = 0; y <= 2; ++y) {
        for (int x = 0; x <= 3; ++x) {
            const bool stone = (x == 0 && y == 0) || (x == 3 && y == 2);
            const ashfall::MaterialId want = stone ? ashfall::kStone : ashfall::kAir;
            const std::size_t index = static_cast<std::size_t>(y * 4 + x);
            if (buffer[index] != want) {
                std::cerr << "COPY_RECT RESIDUE at (" << x << "," << y << ")\n";
                expect(false, "copy_rect fully overwrites including air");
            }
        }
    }
}

// Group 4 (Task O0) identity oracle replay: rebuild each scenario via the
// shared tests/oracle_scenarios.hpp builders, step, and compare the
// per-tick FNV-1a-64 of save() bytes against tests/oracle/<name>.hash.
// Fails before the hash files exist (files ship alongside); passes after.
// Any mismatch after an optimization = behavior change = bug. Hash files
// load via the T6 test-data path pattern (tests/oracle/, then
// ../tests/oracle/ for build-dir ctest runs); `#` header lines are skipped.
std::uint64_t oracle_fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool oracle_load_hashes(const char* name, std::vector<std::uint64_t>& out) {
    out.clear();
    std::string text;
    const std::string candidates[] = {
        std::string("tests/oracle/") + name + ".hash",
        std::string("../tests/oracle/") + name + ".hash",
    };
    for (const std::string& candidate : candidates) {
        std::ifstream file(candidate);
        if (file) {
            std::ostringstream stream;
            stream << file.rdbuf();
            text = stream.str();
            break;
        }
    }
    if (text.empty()) {
        return false;
    }
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        int tick = -1;
        unsigned long long want = 0;
        if (std::sscanf(line.c_str(), "%d %llx", &tick, &want) != 2) {
            return false;
        }
        if (tick != static_cast<int>(out.size())) {
            return false;
        }
        out.push_back(static_cast<std::uint64_t>(want));
    }
    return !out.empty();
}

// (Group 4, Task O4:) independent scan of the non-static count via the
// public cell()/material_info API. The box covers every cell the test below
// places, including a few ticks of movement range.
std::size_t o4_scanned_non_static(ashfall::SimulationWorld& world) {
    std::size_t count = 0;
    for (int y = -8; y <= 8; ++y) {
        for (int x = -8; x <= 8; ++x) {
            const ashfall::MaterialId id = world.cell({x, y});
            if (id == ashfall::kAir) continue;
            ashfall::MaterialSpec spec{};
            expect(world.material_info(id, spec), "o4 scan sees known ids only");
            if (spec.movement != ashfall::Movement::Static) ++count;
        }
    }
    return count;
}

void o4_expect_counter(ashfall::SimulationWorld& world, const char* message) {
    const std::size_t scanned = o4_scanned_non_static(world);
    if (world.non_static_count() != scanned) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s (counter %zu scanned %zu)", message,
            world.non_static_count(), scanned);
        expect(false, msg);
    }
}

void test_o4_non_static_counter() {
    ashfall::SimulationWorld world;
    o4_expect_counter(world, "o4 empty world counts 0");
    // set_cell ops: static set, dynamic sets, class changes, no-ops.
    world.set_cell({0, 0}, ashfall::kStone);
    o4_expect_counter(world, "o4 static set keeps 0");
    world.set_cell({1, 0}, ashfall::kSand);
    world.set_cell({2, 0}, ashfall::kWater);
    world.set_cell({3, 0}, ashfall::kFire);
    o4_expect_counter(world, "o4 powder+liquid+gas sets count 3");
    world.set_cell({1, 0}, ashfall::kStone); // dynamic -> static
    o4_expect_counter(world, "o4 overwrite to static decrements");
    world.set_cell({2, 0}, ashfall::kAir); // dynamic -> air
    o4_expect_counter(world, "o4 erase to air decrements");
    world.set_cell({0, 0}, ashfall::kStone); // same-id no-op
    o4_expect_counter(world, "o4 same-id set is neutral");
    world.set_cell({5, 5}, 42); // unknown id no-op
    o4_expect_counter(world, "o4 unknown id set is neutral");
    // fill_rect ops: bulk set, same-id rewrite, class swaps, air erases.
    world.fill_rect({-2, 2}, {2, 4}, ashfall::kWater);
    o4_expect_counter(world, "o4 liquid fill accumulates");
    world.fill_rect({-2, 2}, {2, 4}, ashfall::kWater);
    o4_expect_counter(world, "o4 same-id fill is neutral");
    world.fill_rect({0, 2}, {1, 4}, ashfall::kStone);
    o4_expect_counter(world, "o4 static fill over liquid decrements");
    world.fill_rect({0, 2}, {1, 4}, ashfall::kAir);
    o4_expect_counter(world, "o4 air fill over static is neutral");
    world.fill_rect({-2, 3}, {2, 3}, ashfall::kAir);
    o4_expect_counter(world, "o4 air fill over liquid decrements");
    // Steps preserve the count on a mixed world (movement only relocates).
    for (int i = 0; i < 3; ++i) {
        world.step();
        o4_expect_counter(world, "o4 stepping a mixed world preserves the count");
    }
    // Reaction op: spread turns static wood into gas fire via set_cell.
    ashfall::SimulationWorld react;
    react.set_cell({0, 0}, ashfall::kFire);
    react.fill_rect({1, 0}, {2, 0}, ashfall::kWood);
    o4_expect_counter(react, "o4 pre-reaction counts the single fire");
    ashfall::ReactionTable spread_table;
    spread_table.version = 1;
    ashfall::ReactionSpec spread;
    spread.name = "spread";
    spread.probability = 100;
    spread.reactants = {{false, "fire"}, {true, "burnable"}};
    spread.products = {{"fire"}, {"fire"}};
    spread_table.reactions = {spread};
    expect(react.load_reactions(spread_table), "o4 spread loads");
    o4_expect_counter(react, "o4 load_reactions touches no cells");
    react.step(); // fire spreads to one wood cell, then both flames rise
    o4_expect_counter(react, "o4 reaction-created gas counted after step");
    expect(react.non_static_count() == 2, "o4 spread yields exactly two flames");
    // Static-only world: the fast-path hook keeps the counter at 0 while the
    // tick still advances and nothing moves.
    ashfall::SimulationWorld statik;
    statik.fill_rect({-3, -3}, {3, 3}, ashfall::kStone);
    o4_expect_counter(statik, "o4 static box counts 0");
    const std::size_t occupied = statik.occupied_cell_count();
    for (int i = 0; i < 5; ++i) statik.step();
    o4_expect_counter(statik, "o4 fast-path steps keep 0");
    expect(statik.occupied_cell_count() == occupied, "o4 fast-path steps move nothing");
    expect(statik.tick() == 5, "o4 fast-path steps still advance the tick");
    // Load op: the recount matches both the scan and the source world.
    const std::vector<std::uint8_t> bytes = world.save();
    ashfall::SimulationWorld loaded;
    expect(loaded.load(bytes), "o4 save loads");
    o4_expect_counter(loaded, "o4 load recount matches scan");
    expect(loaded.non_static_count() == world.non_static_count(), "o4 load recount matches source");
    // Reset op.
    world.reset();
    o4_expect_counter(world, "o4 reset counts 0");
    expect(world.non_static_count() == 0, "o4 reset is exactly 0");
    // Remap op: table load resets to 0, and the counter tracks remapped ids.
    ashfall::MaterialTable table;
    table.version = 1;
    table.materials = {
        {0, "air", ashfall::Movement::Static, false, 0, {0, 0, 0}},
        {4, "stone", ashfall::Movement::Static, true, 99, {0, 0, 0}},
        {5, "sand", ashfall::Movement::Powder, false, 2, {0, 0, 0}},
        {6, "water", ashfall::Movement::Liquid, false, 1, {0, 0, 0}},
    };
    expect(world.load_material_table(table), "o4 remap loads");
    o4_expect_counter(world, "o4 remap resets to 0");
    world.set_cell({0, 0}, 5);
    world.fill_rect({1, 0}, {2, 0}, 6);
    o4_expect_counter(world, "o4 counter tracks remapped ids");
    expect(world.non_static_count() == 3, "o4 remapped sand+water count 3");
    expect(world.load_material_table(ashfall::default_material_table()), "o4 defaults reload");
    o4_expect_counter(world, "o4 reload resets to 0");
}

void test_oracle_replay() {
    struct Case {
        const char* name;
        bool (*build)(ashfall::SimulationWorld&);
        int ticks;
    };
    const Case cases[] = {
        {"A", oracle::build_oracle_A, oracle::kMeasureTicksA},
        {"B", oracle::build_oracle_B, oracle::kMeasureTicksB},
        {"C", oracle::build_oracle_C, oracle::kMeasureTicksC},
        {"D", oracle::build_oracle_D, oracle::kMeasureTicksD},
        {"E", oracle::build_oracle_E, oracle::kMeasureTicksE},
        {"F", oracle::build_oracle_F, oracle::kMeasureTicksF},
    };
    for (const Case& c : cases) {
        std::vector<std::uint64_t> want;
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "oracle %s hash file opens", c.name);
            expect(oracle_load_hashes(c.name, want), msg);
        }
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "oracle %s hash file holds %d ticks", c.name, c.ticks);
            expect(want.size() == static_cast<std::size_t>(c.ticks), msg);
        }
        ashfall::SimulationWorld world;
        {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "oracle %s scenario builds", c.name);
            expect(c.build(world), msg);
        }
        for (int tick = 0; tick < c.ticks; ++tick) {
            world.step();
            const std::uint64_t actual = oracle_fnv1a64(world.save());
            if (actual != want[static_cast<std::size_t>(tick)]) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                    "oracle %s tick %d mismatch: expected %016llx actual %016llx", c.name, tick,
                    static_cast<unsigned long long>(want[static_cast<std::size_t>(tick)]),
                    static_cast<unsigned long long>(actual));
                expect(false, msg);
            }
        }
    }
}

// Group 4 (Task O6) agreement helpers: the chunk mirror is NOT in save()
// bytes, so the oracle cannot pin it — these tests do. o6_expect_agreement
// checks both directions over every live chunk (every cells_ entry matches
// its mirror slot; every non-air mirror slot matches cells_; missing chunks
// read back as air) plus the temps-subset-cells invariant on sampled
// positions.
void o6_expect_agreement(ashfall::SimulationWorld& world, const char* message) {
    constexpr int kSide = ashfall::kChunkSize;
    std::vector<ashfall::MaterialId> buffer(
        static_cast<std::size_t>(kSide * kSide), ashfall::kAir);
    for (const ashfall::CellPosition chunk : world.active_chunks()) {
        expect(world.read_chunk(chunk, buffer), "o6 read_chunk succeeds on active chunk");
        for (int ly = 0; ly < kSide; ++ly) {
            for (int lx = 0; lx < kSide; ++lx) {
                const ashfall::CellPosition pos{
                    chunk.x * kSide + lx, chunk.y * kSide + ly};
                const ashfall::MaterialId mirror =
                    buffer[static_cast<std::size_t>(ly * kSide + lx)];
                const ashfall::MaterialId live = world.cell(pos);
                if (mirror != live) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                        "%s: mirror/cells diverge at (%d,%d): mirror %u live %u", message, pos.x,
                        pos.y, static_cast<unsigned>(mirror), static_cast<unsigned>(live));
                    expect(false, msg);
                }
                if (world.temperature(pos) != 0 && live == ashfall::kAir) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                        "%s: temp stored on air at (%d,%d)", message, pos.x, pos.y);
                    expect(false, msg);
                }
            }
        }
    }
}

void test_o6_mirror_cells_agreement() {
    ashfall::SimulationWorld world;
    o6_expect_agreement(world, "o6 empty world agrees");
    // Deterministic op sequence across chunk boundaries (incl. negative
    // coords): sets, fills, an air-erase, heat, steps (movement exercises
    // the single-pass reconcile), a save/load round trip, and more steps.
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_cell({-3, -70}, ashfall::kStone); // chunk {-1,-2}
    world.set_cell({63, 63}, ashfall::kWater);
    world.set_cell({64, 64}, ashfall::kSand); // adjacent chunk {1,1}
    world.fill_rect({-70, 60}, {-65, 64}, ashfall::kWood);
    o6_expect_agreement(world, "o6 placed cells agree");
    world.fill_rect({10, 12}, {12, 12}, ashfall::kStone);
    world.fill_rect({10, 0}, {12, 11}, ashfall::kSand);
    world.fill_rect({20, 0}, {30, 11}, ashfall::kWater);
    expect(world.set_temperature({11, 5}, 60), "o6 heat stages on sand");
    expect(world.set_temperature({25, 5}, -40), "o6 cold stages on water");
    o6_expect_agreement(world, "o6 pre-step world agrees");
    for (int i = 0; i < 25; ++i) {
        world.step();
        o6_expect_agreement(world, "o6 stepping world agrees");
    }
    world.set_cell({64, 64}, ashfall::kAir); // erase one cross-chunk cell
    world.fill_rect({-70, 60}, {-65, 64}, ashfall::kAir); // erase a wood block
    o6_expect_agreement(world, "o6 erasures agree");
    const std::vector<std::uint8_t> bytes = world.save();
    ashfall::SimulationWorld loaded;
    expect(loaded.load(bytes), "o6 save loads");
    o6_expect_agreement(loaded, "o6 loaded world agrees");
    for (int i = 0; i < 10; ++i) {
        loaded.step();
        o6_expect_agreement(loaded, "o6 stepping loaded world agrees");
    }
    loaded.reset();
    o6_expect_agreement(loaded, "o6 reset world agrees");
    expect(loaded.active_chunks().empty(), "o6 reset drops all chunks");
}

// Group 4 (Task O6) occupancy pin: chunk_occupancy_ has no public getter
// (header scope lock), so this pins its OBSERVABLE invariant — chunk
// existence ⇔ a full read_chunk scan finds non-air — over the live-chunk
// bounding box plus a one-chunk ring (neighbors must be absent AND empty).
// Any counter drift (leaked empty chunk, missing populated chunk) fails.
void test_o6_occupancy_matches_scan() {
    ashfall::SimulationWorld world;
    world.set_cell({5, 5}, ashfall::kSand);
    world.set_cell({-3, -70}, ashfall::kStone);
    world.fill_rect({64, 0}, {130, 65}, ashfall::kWater); // spans chunks {1..2, 0..1}
    world.fill_rect({10, 12}, {12, 12}, ashfall::kStone);
    world.fill_rect({10, 0}, {12, 11}, ashfall::kSand);
    for (int i = 0; i < 15; ++i) world.step();
    world.set_cell({5, 5}, ashfall::kAir);
    world.fill_rect({64, 0}, {70, 65}, ashfall::kAir); // partial erase of the water span
    const std::vector<std::uint8_t> bytes = world.save();
    ashfall::SimulationWorld loaded;
    expect(loaded.load(bytes), "o6 occupancy save loads");
    for (ashfall::SimulationWorld* target : {&world, &loaded}) {
        int min_cx = 0, max_cx = 0, min_cy = 0, max_cy = 0;
        bool have = false;
        for (const ashfall::CellPosition chunk : target->active_chunks()) {
            if (!have) {
                min_cx = max_cx = chunk.x;
                min_cy = max_cy = chunk.y;
                have = true;
            } else {
                if (chunk.x < min_cx) min_cx = chunk.x;
                if (chunk.x > max_cx) max_cx = chunk.x;
                if (chunk.y < min_cy) min_cy = chunk.y;
                if (chunk.y > max_cy) max_cy = chunk.y;
            }
        }
        expect(have, "o6 occupancy world is non-empty");
        constexpr int kSide = ashfall::kChunkSize;
        std::vector<ashfall::MaterialId> buffer(
            static_cast<std::size_t>(kSide * kSide), ashfall::kAir);
        for (int cy = min_cy - 1; cy <= max_cy + 1; ++cy) {
            for (int cx = min_cx - 1; cx <= max_cx + 1; ++cx) {
                const ashfall::CellPosition chunk{cx, cy};
                std::fill(buffer.begin(), buffer.end(), ashfall::kAir);
                expect(target->read_chunk(chunk, buffer), "o6 read_chunk succeeds");
                std::size_t scanned = 0;
                for (ashfall::MaterialId id : buffer) {
                    if (id != ashfall::kAir) ++scanned;
                }
                const bool exists = target->has_chunk(chunk);
                if (exists != (scanned != 0)) {
                    char msg[256];
                    std::snprintf(msg, sizeof(msg),
                        "o6 chunk (%d,%d): exists=%d but scan finds %zu live cells", cx, cy,
                        exists ? 1 : 0, scanned);
                    expect(false, msg);
                }
            }
        }
    }
    // Fully deleting a chunk's last cell must drop the chunk (counter 0 ⇒
    // erase, no scan): wipe the stone cell's whole chunk region cell by cell.
    for (int y = -128; y < 0; ++y) {
        for (int x = -64; x < 0; ++x) world.set_cell({x, y}, ashfall::kAir);
    }
    expect(!world.has_chunk({-1, -2}), "o6 emptied chunk is gone");
}

// Group 4 (Task O6) dirty-sequence pin: a fixed op script must produce the
// EXACT dirty list — this is the vector-backing equivalence proof (push_back
// + sorted-unique read ≡ the old std::set, incl. dedup of repeat marks and
// the no-op paths that mark nothing).
void o6_expect_dirty(
    ashfall::SimulationWorld& world, std::vector<ashfall::CellPosition> want, const char* message) {
    const std::vector<ashfall::CellPosition> actual = world.dirty_chunks();
    if (actual.size() != want.size()) {
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s: dirty size %zu want %zu", message, actual.size(),
            want.size());
        expect(false, msg);
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (!(actual[i] == want[i])) {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%s: dirty[%zu] is (%d,%d) want (%d,%d)", message, i,
                actual[i].x, actual[i].y, want[i].x, want[i].y);
            expect(false, msg);
        }
    }
}

void test_o6_dirty_sequence() {
    ashfall::SimulationWorld world;
    world.set_cell({5, 5}, ashfall::kSand);      // chunk {0, 0}
    world.set_cell({200, 200}, ashfall::kStone); // chunk {3, 3}
    world.clear_dirty_chunks();
    o6_expect_dirty(world, {}, "o6 clear empties the log");
    // Same-id set is a no-op: marks nothing (no push on the early return).
    world.set_cell({5, 5}, ashfall::kSand);
    o6_expect_dirty(world, {}, "o6 same-id set marks nothing");
    // Unknown-id set is rejected: marks nothing.
    world.set_cell({6, 6}, 42);
    o6_expect_dirty(world, {}, "o6 unknown-id set marks nothing");
    // Two marks in one chunk without a clear collapse to one entry.
    world.set_cell({70, 5}, ashfall::kWater); // chunk {1, 0}
    world.set_cell({71, 6}, ashfall::kWater); // chunk {1, 0} again
    o6_expect_dirty(world, {{{1, 0}}}, "o6 repeat chunk marks dedup");
    world.clear_dirty_chunks();
    // Erasing the chunk's last cells still dirties exactly that chunk.
    world.set_cell({70, 5}, ashfall::kAir);
    world.set_cell({71, 6}, ashfall::kAir);
    o6_expect_dirty(world, {{{1, 0}}}, "o6 chunk emptying dirties the former chunk");
    expect(!world.has_chunk({1, 0}), "o6 emptied chunk is gone");
    world.clear_dirty_chunks();
    // One deterministic movement tick: sand falls within chunk {0, 0}.
    world.set_cell({11, 10}, ashfall::kSand);
    world.clear_dirty_chunks();
    world.step();
    expect(world.cell({11, 11}) == ashfall::kSand, "o6 script sand fell one cell");
    o6_expect_dirty(world, {{{0, 0}}}, "o6 same-chunk step dirties one chunk");
    // Air fill over an empty region dirties the covered chunk range
    // (fill_rect marks its range unconditionally — pinned as-is).
    world.clear_dirty_chunks();
    world.fill_rect({300, 300}, {310, 310}, ashfall::kAir); // chunk {4, 4}
    o6_expect_dirty(world, {{{4, 4}}}, "o6 air fill over empty dirties its range");
    // Reset dirties exactly the removed populated chunks, sorted.
    world.clear_dirty_chunks();
    world.reset();
    o6_expect_dirty(world, {{{0, 0}, {3, 3}}}, "o6 reset dirties removed chunks");
    // Load dirties the vacated old region plus the populated new region.
    ashfall::SimulationWorld source;
    source.set_cell({320, 320}, ashfall::kStone); // chunk {5, 5}
    const std::vector<std::uint8_t> bytes = source.save();
    world.set_cell({0, 0}, ashfall::kSand); // chunk {0, 0}, disjoint
    world.clear_dirty_chunks();
    expect(world.load(bytes), "o6 disjoint load succeeds");
    o6_expect_dirty(world, {{{0, 0}, {5, 5}}}, "o6 load dirties old and new regions");
}

// Group 5 (Task M3, §7): occupied_cell_count() equals a globally scanned
// count — the exact-counter pin for total_occupied_ (the chunk-authoritative
// replacement for cells_.size()). Placement, erasure, ticks, load, reset.
void test_m3_occupied_counter_matches_scan() {
    auto scanned_occupied = [](ashfall::SimulationWorld& world, int x0, int y0, int x1, int y1) {
        std::size_t count = 0;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (world.cell({x, y}) != ashfall::kAir) ++count;
            }
        }
        return count;
    };
    ashfall::SimulationWorld world;
    // Spread across chunk boundaries, incl. negative coords.
    world.fill_rect({-70, -70}, {-65, -60}, ashfall::kStone);
    world.fill_rect({60, 60}, {70, 75}, ashfall::kSand);
    world.set_cell({0, 0}, ashfall::kWater);
    world.set_cell({-1, -1}, ashfall::kWater);
    expect(world.occupied_cell_count() == scanned_occupied(world, -80, -80, 80, 80),
        "m3 counter matches scan after placement");
    // Erasures, incl. a whole-chunk wipe.
    world.set_cell({0, 0}, ashfall::kAir);
    world.fill_rect({-70, -70}, {-65, -60}, ashfall::kAir);
    expect(world.occupied_cell_count() == scanned_occupied(world, -80, -80, 80, 80),
        "m3 counter matches scan after erasure");
    // Ticks: movement relocates but preserves the multiset (the scan box
    // covers the fall range).
    for (int i = 0; i < 10; ++i) {
        world.step();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "m3 counter matches scan after tick %d", i);
        expect(world.occupied_cell_count() == scanned_occupied(world, -80, -80, 90, 100), msg);
    }
    // Load twin recounts identically; reset zeroes.
    ashfall::SimulationWorld twin;
    expect(twin.load(world.save()), "m3 counter twin loads");
    expect(twin.occupied_cell_count() == world.occupied_cell_count(),
        "m3 load recount matches source");
    expect(twin.occupied_cell_count() == scanned_occupied(twin, -80, -80, 90, 100),
        "m3 counter matches scan after load");
    world.reset();
    expect(world.occupied_cell_count() == 0, "m3 reset zeroes the counter");
    expect(scanned_occupied(world, -80, -80, 90, 100) == 0, "m3 reset empties the scan");
}

// Group 5 (Task M3, §7 + insertion-history pin): grow-then-delete and
// permuted builds of the same logical world yield identical save bytes —
// the F2-flavored proof for the chunk-walk invariant (canonical by
// construction; history must not leak into bytes). Extends S3 P1 (which
// covers forward/backward insertion only) with bulk-grow-then-wipe history.
void test_m3_grow_delete_reinsert_history_independent() {
    const std::vector<std::pair<ashfall::CellPosition, ashfall::MaterialId>> cells = {
        {{0, 0}, ashfall::kFire}, {{1, 0}, ashfall::kWater}, {{2, 0}, ashfall::kWood},
        {{0, 1}, ashfall::kStone}, {{1, 1}, ashfall::kSteam}, {{2, 1}, ashfall::kStone},
        {{-70, -70}, ashfall::kSand}, {{130, 5}, ashfall::kOil},
    };
    ashfall::SimulationWorld direct;
    for (const auto& [pos, mat] : cells) direct.set_cell(pos, mat);
    // Grow-then-delete: bulk fill across chunks, then wipe the region back
    // to air (dropping whole chunks) and rebuild the same logical set.
    ashfall::SimulationWorld regrown;
    regrown.fill_rect({-80, -80}, {140, 20}, ashfall::kSand);
    regrown.fill_rect({-80, -80}, {140, 20}, ashfall::kAir);
    for (const auto& [pos, mat] : cells) regrown.set_cell(pos, mat);
    expect(regrown.save() == direct.save(), "m3 grow-then-delete matches direct build bytes");
    // Strided permutation (3 coprime to 8 — deterministic, no RNG).
    ashfall::SimulationWorld permuted;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const auto& [pos, mat] = cells[(i * 3) % cells.size()];
        permuted.set_cell(pos, mat);
    }
    expect(permuted.save() == direct.save(), "m3 strided-permuted build matches direct bytes");
    // Histories stay converged tick by tick (movement + heat-free settling).
    for (int i = 0; i < 5; ++i) {
        direct.step();
        regrown.step();
        permuted.step();
        char msg[128];
        std::snprintf(msg, sizeof(msg), "m3 regrown tracks tick %d", i);
        expect(regrown.save() == direct.save(), msg);
        std::snprintf(msg, sizeof(msg), "m3 permuted tracks tick %d", i);
        expect(permuted.save() == direct.save(), msg);
    }
}

// Group 5 (Task M3, §7): hot/cold powder-liquid swaps across a chunk
// boundary (the pair straddles y=63/64) — the payload rides the material,
// not the coordinate, and copy_rect/cell agree across the seam.
void test_m3_hot_cold_swap_across_chunk_boundary() {
    auto build_crossing = [](ashfall::MaterialId upper, ashfall::MaterialId lower, int hot_y) {
        ashfall::SimulationWorld world;
        world.fill_rect({-1, 65}, {1, 65}, ashfall::kStone); // floor in chunk {0, 1}
        world.set_cell({0, 64}, lower); // chunk {0, 1}
        world.set_cell({0, 63}, upper); // chunk {0, 0}: straddles y=63/64
        expect(world.set_temperature({0, hot_y}, 100), "m3 crossing heat stages");
        return world;
    };
    auto expect_seam_agreement = [](ashfall::SimulationWorld& world) {
        std::vector<ashfall::MaterialId> rect(5 * 5, static_cast<ashfall::MaterialId>(0xFF));
        world.copy_rect({-2, 62}, {2, 66}, rect);
        for (int y = 62; y <= 66; ++y) {
            for (int x = -2; x <= 2; ++x) {
                const std::size_t index = static_cast<std::size_t>((y - 62) * 5 + (x + 2));
                if (rect[index] != world.cell({x, y})) {
                    char msg[128];
                    std::snprintf(msg, sizeof(msg),
                        "m3 copy_rect/cell agree at (%d,%d) across the seam", x, y);
                    expect(false, msg);
                }
            }
        }
    };
    // Hot sand above cold settled water: sand sinks across the seam.
    {
        ashfall::SimulationWorld world = build_crossing(ashfall::kSand, ashfall::kWater, 63);
        world.step();
        expect(world.cell({0, 64}) == ashfall::kSand, "m3 hot sand sinks across the chunk seam");
        expect(world.cell({0, 63}) == ashfall::kWater, "m3 cold water rises across the seam");
        expect(world.temperature({0, 64}) == 20, "m3 sunk sand keeps its payload");
        expect(world.temperature({0, 63}) == 20, "m3 risen water warms by conduction only");
        expect(world.temperature({0, 65}) == 20, "m3 floor warms under the hot sand");
        expect(world.temperature_count() == 3, "m3 crossing carries exactly three entries");
        expect_seam_agreement(world);
    }
    // Cold sand above hot settled water: the payload rides the water up.
    {
        ashfall::SimulationWorld world = build_crossing(ashfall::kSand, ashfall::kWater, 64);
        world.step();
        expect(world.cell({0, 64}) == ashfall::kSand, "m3 cold sand sinks across the seam");
        expect(world.cell({0, 63}) == ashfall::kWater, "m3 hot water rises across the seam");
        expect(world.temperature({0, 63}) == 20, "m3 risen water keeps its payload");
        expect(world.temperature({0, 64}) == 20, "m3 sunk sand warms by conduction only");
        expect(world.temperature({0, 65}) == 0, "m3 floor stays ambient under cold sand");
        expect(world.temperature_count() == 2, "m3 reverse crossing carries exactly two entries");
        expect_seam_agreement(world);
    }
}

// Group 5 (Task M3, §7): exact temperature-gate boundaries + p=0/p=100
// edges, on static materials (no movement between reaction and assert).
void test_m3_reaction_gate_boundary_and_p_edges() {
    auto gate_table = [](bool has_min, int min_value, bool has_max, int max_value, int probability) {
        ashfall::ReactionTable table;
        table.version = 1;
        ashfall::ReactionSpec ignite;
        ignite.name = "ignite";
        ignite.probability = probability;
        ignite.reactants = {{true, "burnable"}};
        ignite.products = {{"fire"}};
        ignite.gate.has_min = has_min;
        ignite.gate.min_value = min_value;
        ignite.gate.has_max = has_max;
        ignite.gate.max_value = max_value;
        table.reactions = {ignite};
        return table;
    };
    auto count_in_box = [](ashfall::SimulationWorld& world, ashfall::MaterialId id) {
        std::size_t count = 0;
        for (int y = -3; y <= 1; ++y) {
            for (int x = -2; x <= 12; ++x) {
                if (world.cell({x, y}) == id) ++count;
            }
        }
        return count;
    };
    // Exact min edge at p=100: temp == min fires, min-1 stays wood.
    {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(gate_table(true, 25, false, 0, 100)), "m3 min-gate loads");
        world.set_cell({0, 0}, ashfall::kWood);
        world.set_cell({10, 0}, ashfall::kWood);
        expect(world.set_temperature({0, 0}, 25), "m3 edge heat stages");
        expect(world.set_temperature({10, 0}, 24), "m3 below-edge heat stages");
        world.step(); // wood is static; the fresh flame rises one cell the same tick
        expect(world.cell({10, 0}) == ashfall::kWood, "m3 min-1 wood never ignites");
        expect(count_in_box(world, ashfall::kFire) == 1, "m3 exact-min wood ignites once");
        expect(count_in_box(world, ashfall::kWood) == 1, "m3 only the edge cell transforms");
    }
    // Exact max edge at p=100: temp == max fires, max+1 stays wood.
    {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(gate_table(false, 0, true, 25, 100)), "m3 max-gate loads");
        world.set_cell({0, 0}, ashfall::kWood);
        world.set_cell({10, 0}, ashfall::kWood);
        expect(world.set_temperature({0, 0}, 25), "m3 edge heat stages");
        expect(world.set_temperature({10, 0}, 26), "m3 above-edge heat stages");
        world.step();
        expect(world.cell({10, 0}) == ashfall::kWood, "m3 max+1 wood never ignites");
        expect(count_in_box(world, ashfall::kFire) == 1, "m3 exact-max wood ignites once");
        expect(count_in_box(world, ashfall::kWood) == 1, "m3 only the edge cell transforms");
    }
    // p=0 never fires on a full match; p=100 always fires (same gateless fixture).
    {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(gate_table(false, 0, false, 0, 0)), "m3 p=0 loads");
        world.set_cell({0, 0}, ashfall::kWood);
        for (int i = 0; i < 5; ++i) world.step();
        expect(world.cell({0, 0}) == ashfall::kWood, "m3 p=0 never fires on a full match");
        expect(world.occupied_cell_count() == 1, "m3 p=0 preserves the cell");
    }
    {
        ashfall::SimulationWorld world;
        expect(world.load_reactions(gate_table(false, 0, false, 0, 100)), "m3 p=100 loads");
        world.set_cell({0, 0}, ashfall::kWood);
        world.step();
        expect(count_in_box(world, ashfall::kFire) == 1, "m3 p=100 fires on the first tick");
        expect(world.cell({0, 0}) != ashfall::kWood, "m3 p=100 consumes the wood");
    }
}

// Group 5 (Task M3, migration pin): 1024² smoke — build + tick + save/load
// round-trip on a big SPARSE world. Bounds the format (chunk directory over
// a 16×16-chunk span), not a timing test: few ticks, sparse fills, fast.
void test_m3_1024_sparse_smoke() {
    ashfall::SimulationWorld world;
    world.fill_rect({0, 1023}, {1023, 1023}, ashfall::kStone); // floor row
    for (int i = 0; i < 16; ++i) {
        const int c = i * 64;
        world.set_cell({c, c}, ashfall::kSand);
        world.set_cell({c + 63, c}, ashfall::kWater);
    }
    world.set_cell({-1, -1}, ashfall::kStone);
    world.set_cell({1023, 0}, ashfall::kWood);
    const std::size_t built = world.occupied_cell_count();
    expect(built == 1024 + 32 + 2, "m3 smoke builds its sparse span");
    for (int i = 0; i < 3; ++i) world.step();
    expect(world.occupied_cell_count() == built, "m3 smoke conserves mass across ticks");
    const auto saved = world.save();
    ashfall::SimulationWorld twin;
    expect(twin.load(saved), "m3 smoke save loads");
    expect(twin.save() == saved, "m3 smoke round-trips byte-exact");
    expect(twin.tick() == world.tick(), "m3 smoke tick preserved");
    twin.step();
    world.step();
    expect(twin.save() == world.save(), "m3 smoke twin tracks post-load");
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
    test_partial_surface_remains_level();
    test_uneven_water_mound_flattens_in_basin();
    test_water_mound_levels_quickly();
    test_chunk_boundaries_crossing();
    test_play_level_layout();
    test_water_trickles_down_sand_slope();
    test_command_replay();
    test_step_marks_dirty_chunks();
    test_save_load_rejects_malformed();
    test_save_load_varied_topology();
    test_save_golden_empty_v3();
    test_save_v2_reader_empty();
    test_save_golden_single_stone_v3();
    test_save_v2_reader_single_stone();
    test_save_golden_negative_coords_and_temps_v3();
    test_save_v2_reader_negative_coords_and_temps();
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
    test_oil_floats_on_water();
    test_oil_flash_reacts_and_heats();
    test_oil_flash_first_tick_golden();
    test_oil_flash_production_row();
    test_reaction_shared_reactant_double_transform();
    test_reaction_half_applied_instance();
    test_heat_hot_sand_falls_to_air();
    test_heat_cold_follows_hot_no_duplication();
    test_heat_hot_powder_swaps_cold_liquid();
    test_heat_cold_powder_swaps_hot_liquid();
    test_heat_hot_liquid_sinks_through_cold_liquid();
    test_heat_infiltration_displaces_heated_arrival();
    test_heat_hot_swap_save_load_continuation();
    test_heat_infiltration_save_load_continuation();
    test_save_rejects_duplicate_cell();
    test_save_rejects_unsorted_cells();
    test_save_rejects_duplicate_temp();
    test_save_rejects_unsorted_temps();
    test_save_rejects_ambient_temp();
    test_save_rejects_oversize_count();
    test_save_rejects_out_of_range_coord();
    test_save_rejects_chunk_bomb();
    test_loader_rejects_dual_key_reactant();
    test_loader_rejects_forged_movement_enum();
    test_loader_rejects_out_of_range_density();
    test_loader_rejects_oversize_material_table();
    test_loader_failed_reload_preserves_world();
    test_loader_parse_output_is_transactional();
    test_temperature_set_and_query();
    test_temperature_single_cell_cools_exact();
    test_temperature_two_cells_equalize_exact();
    test_temperature_ambient_world_stays_sparse();
    test_temperature_save_round_trip_v3();
    test_temperature_v1_save_still_loads();
    test_temperature_cleared_by_edits_and_reset();
    test_temperature_follows_falling_sand();
    test_json_syntax_corpus();
    test_json_parser_limits();
    test_json_material_corpus();
    test_json_reaction_corpus();
    test_json_identifier_policy();
    test_reaction_link_stage();
    test_reaction_p0_warning();
    test_s3_p1_purity_insertion_order_independent();
    test_s3_p2_capacity_straddle();
    test_s3_p2_twin_continuation();
    test_s3_p3_canonical_round_trip();
    test_s3_round_trip_matrix();
    test_v3_skew_remap_accept();
    test_v3_skew_roster_rejects();
    test_v3_reaction_guard();
    test_v3_version_and_crc();
    test_s6_truncation_sweep();
    test_s6_bitflip_sweep();
    test_s6_continuation_across_boundary();
    test_b2_final_cell_deletion_dirties_chunk();
    test_b2_reset_dirtiness();
    test_b2_load_invalidates_old_and_new();
    test_b2_copy_rect_full_overwrite();
    test_o4_non_static_counter();
    test_o6_mirror_cells_agreement();
    test_o6_occupancy_matches_scan();
    test_o6_dirty_sequence();
    test_m3_occupied_counter_matches_scan();
    test_m3_grow_delete_reinsert_history_independent();
    test_m3_hot_cold_swap_across_chunk_boundary();
    test_m3_reaction_gate_boundary_and_p_edges();
    test_m3_1024_sparse_smoke();
    test_oracle_replay();
    std::cout << "All simulation tests passed.\n";
}

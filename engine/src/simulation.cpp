#include "ashfall/simulation.hpp"

#include <algorithm>
#include <array>
#include <unordered_set>

namespace ashfall {
namespace {

constexpr std::array<std::uint8_t, 8> kSaveMagic = {'A', 'S', 'H', 'F', 'A', 'L', 'L', 1};

int floor_divide(int value, int divisor) {
    const int quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

int positive_modulo(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) out.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

bool read_u32(std::span<const std::uint8_t> input, std::size_t& offset, std::uint32_t& value) {
    if (offset + 4 > input.size()) return false;
    value = 0;
    for (int byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(input[offset++]) << (byte * 8);
    return true;
}

bool read_u64(std::span<const std::uint8_t> input, std::size_t& offset, std::uint64_t& value) {
    if (offset + 8 > input.size()) return false;
    value = 0;
    for (int byte = 0; byte < 8; ++byte) value |= static_cast<std::uint64_t>(input[offset++]) << (byte * 8);
    return true;
}

CellPosition chunk_pos_of(CellPosition pos) {
    return {floor_divide(pos.x, kChunkSize), floor_divide(pos.y, kChunkSize)};
}

} // namespace

SimulationWorld::SimulationWorld() {
    materials_.emplace(kAir, MaterialDefinition{Movement::Static, false, 0});
    materials_.emplace(kStone, MaterialDefinition{Movement::Static, true, 99});
    materials_.emplace(kSand, MaterialDefinition{Movement::Powder, false, 2});
    materials_.emplace(kWater, MaterialDefinition{Movement::Liquid, false, 1});
}

MaterialId SimulationWorld::cell(CellPosition position) const {
    const CellPosition chunk_position = chunk_pos_of(position);
    const auto chunk = chunks_.find(chunk_position);
    if (chunk == chunks_.end()) return kAir;
    const int x = positive_modulo(position.x, kChunkSize);
    const int y = positive_modulo(position.y, kChunkSize);
    return chunk->second[static_cast<std::size_t>(y * kChunkSize + x)];
}

void SimulationWorld::reset() {
    for (const auto& [chunk_position, _] : chunks_) {
        mark_chunk_dirty(chunk_position);
    }
    chunks_.clear();
    tick_ = 0;
    occupied_count_ = 0;
}

void SimulationWorld::set_cell(CellPosition position, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    const CellPosition chunk_position = chunk_pos_of(position);
    const int local_x = positive_modulo(position.x, kChunkSize);
    const int local_y = positive_modulo(position.y, kChunkSize);
    const std::size_t index = chunk_index(local_x, local_y);

    auto chunk_it = chunks_.find(chunk_position);
    MaterialId previous = kAir;
    if (chunk_it != chunks_.end()) previous = chunk_it->second[index];
    if (previous == material_id) return;

    if (material_id == kAir) {
        if (chunk_it == chunks_.end()) return;
        chunk_it->second[index] = kAir;
        --occupied_count_;
        if (chunk_storage_empty(chunk_it->second)) chunks_.erase(chunk_it);
    } else {
        if (chunk_it == chunks_.end()) {
            auto [inserted_it, _] = chunks_.try_emplace(
                chunk_position, std::vector<MaterialId>(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir));
            chunk_it = inserted_it;
        }
        chunk_it->second[index] = material_id;
        if (previous == kAir) ++occupied_count_;
    }
    mark_chunk_dirty(chunk_position);
}

void SimulationWorld::fill_rect(CellPosition first, CellPosition last, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    const int min_x = std::min(first.x, last.x);
    const int max_x = std::max(first.x, last.x);
    const int min_y = std::min(first.y, last.y);
    const int max_y = std::max(first.y, last.y);
    const CellPosition min_chunk = chunk_pos_of({min_x, min_y});
    const CellPosition max_chunk = chunk_pos_of({max_x, max_y});
    for (int cy = min_chunk.y; cy <= max_chunk.y; ++cy) {
        for (int cx = min_chunk.x; cx <= max_chunk.x; ++cx) {
            mark_chunk_dirty({cx, cy});
        }
    }
    // Incremental per-chunk writes: O(rect area), not O(world size).
    for (int chunk_y = min_chunk.y; chunk_y <= max_chunk.y; ++chunk_y) {
        for (int chunk_x = min_chunk.x; chunk_x <= max_chunk.x; ++chunk_x) {
            const int cell_min_x = std::max(min_x, chunk_x * kChunkSize);
            const int cell_max_x = std::min(max_x, chunk_x * kChunkSize + kChunkSize - 1);
            const int cell_min_y = std::max(min_y, chunk_y * kChunkSize);
            const int cell_max_y = std::min(max_y, chunk_y * kChunkSize + kChunkSize - 1);
            const CellPosition chunk_position{chunk_x, chunk_y};
            auto chunk_it = chunks_.find(chunk_position);
            if (material_id == kAir) {
                if (chunk_it == chunks_.end()) continue;
                for (int y = cell_min_y; y <= cell_max_y; ++y) {
                    for (int x = cell_min_x; x <= cell_max_x; ++x) {
                        const std::size_t index = chunk_index(x - chunk_x * kChunkSize, y - chunk_y * kChunkSize);
                        if (chunk_it->second[index] != kAir) {
                            chunk_it->second[index] = kAir;
                            --occupied_count_;
                        }
                    }
                }
                if (chunk_storage_empty(chunk_it->second)) chunks_.erase(chunk_it);
            } else {
                if (chunk_it == chunks_.end()) {
                    auto [inserted_it, _] = chunks_.try_emplace(
                        chunk_position,
                        std::vector<MaterialId>(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir));
                    chunk_it = inserted_it;
                }
                for (int y = cell_min_y; y <= cell_max_y; ++y) {
                    for (int x = cell_min_x; x <= cell_max_x; ++x) {
                        const std::size_t index = chunk_index(x - chunk_x * kChunkSize, y - chunk_y * kChunkSize);
                        if (chunk_it->second[index] == kAir) ++occupied_count_;
                        chunk_it->second[index] = material_id;
                    }
                }
            }
        }
    }
}


void SimulationWorld::step() {
    CellMap current = collect_cells();
    CellMap next;
    next.reserve(current.size() + 16);
    std::vector<std::pair<CellPosition, MaterialId>> dynamic_cells;
    dynamic_cells.reserve(current.size());
    // Homes of cells that stayed put this tick. Sand may only displace water
    // at a settled home (owner relocates to the swap source, which is safe);
    // overwriting an in-flight arrival would destroy a cell and break mass
    // conservation. Down/diag targets live a row below, so in bottom-up order
    // their owners are always decided already.
    std::unordered_set<CellPosition, CellPositionHash> stayed;
    stayed.reserve(current.size());

    for (const auto& [position, material_id] : current) {
        if (material(material_id).movement == Movement::Static) {
            next[position] = material_id;
        } else {
            dynamic_cells.push_back({position, material_id});
        }
    }

    std::sort(dynamic_cells.begin(), dynamic_cells.end(), [](const auto& left, const auto& right) {
        if (left.first.y != right.first.y) return left.first.y > right.first.y;
        return left.first.x < right.first.x;
    });

    for (const auto& [source, material_id] : dynamic_cells) {
        const auto& mat_def = material(material_id);
        const bool left_first = prefers_left(source, tick_);

        if (mat_def.movement == Movement::Powder) {
            const CellPosition down{source.x, source.y + 1};
            const CellPosition diag_l{source.x - 1, source.y + 1};
            const CellPosition diag_r{source.x + 1, source.y + 1};

            const std::array<CellPosition, 2> diags = left_first
                ? std::array<CellPosition, 2>{diag_l, diag_r}
                : std::array<CellPosition, 2>{diag_r, diag_l};

            auto get_density_in_next = [&](CellPosition p) -> int {
                auto it = next.find(p);
                if (it == next.end()) return 0;
                return material(it->second).density;
            };

            bool moved = false;
            // Claim a powder target mass-safely: empty cells are taken;
            // settled occupants are swapped (owner relocates to source);
            // in-flight arrivals block (stealing them destroys mass).
            auto try_powder_target = [&](CellPosition dest) -> bool {
                if (get_density_in_next(dest) >= mat_def.density) return false;
                auto it = next.find(dest);
                if (it == next.end()) {
                    next[dest] = material_id;
                } else if (it->second != kAir && stayed.contains(dest)) {
                    MaterialId displaced = it->second;
                    next[dest] = material_id;
                    next[source] = displaced;
                } else {
                    return false;
                }
                return true;
            };
            if (try_powder_target(down)) {
                moved = true;
            } else {
                for (CellPosition diag : diags) {
                    const int dx = diag.x - source.x;
                    const CellPosition horiz{source.x + dx, source.y};
                    if (is_solid(horiz) && is_solid(down)) {
                        continue;
                    }
                    if (try_powder_target(diag)) {
                        moved = true;
                        break;
                    }
                }
            }
            if (!moved) {
                next[source] = material_id;
                stayed.insert(source);
            }
        } else if (mat_def.movement == Movement::Liquid) {
            const CellPosition down{source.x, source.y + 1};
            const CellPosition diag_l{source.x - 1, source.y + 1};
            const CellPosition diag_r{source.x + 1, source.y + 1};
            const CellPosition side_l{source.x - 1, source.y};
            const CellPosition side_r{source.x + 1, source.y};

            const std::array<CellPosition, 2> diags = left_first
                ? std::array<CellPosition, 2>{diag_l, diag_r}
                : std::array<CellPosition, 2>{diag_r, diag_l};

            const std::array<CellPosition, 2> sides = left_first
                ? std::array<CellPosition, 2>{side_l, side_r}
                : std::array<CellPosition, 2>{side_r, side_l};

            auto is_empty_down = [&](CellPosition p) -> bool {
                return !next.contains(p);
            };

            bool moved = false;
            if (is_empty_down(down)) {
                next[down] = material_id;
                moved = true;
            } else {
                for (CellPosition diag : diags) {
                    const int dx = diag.x - source.x;
                    const CellPosition horiz{source.x + dx, source.y};
                    if (is_solid(horiz) && is_solid(down)) {
                        continue;
                    }
                    if (is_empty_down(diag)) {
                        next[diag] = material_id;
                        moved = true;
                        break;
                    }
                }
                if (!moved) {
                    // Lateral flow with pressure gating: only water resting on
                    // water spreads. Thin films and lone droplets on solid
                    // rest instead of vibrating, and dispersion (up to 4
                    // cells, preferred direction first) levels pools quickly.
                    // Targets owned by a cell this tick (present in current)
                    // are skipped, never stolen: that race destroys mass.
                    // Stone walls stop the scan.
                    constexpr int kDispersion = 4;
                    const auto below_it = next.find(down);
                    const bool deep = below_it != next.end() && below_it->second == kWater;
                    // Wetness in the settled/next state (decided cells only).
                    auto wet_at = [&](CellPosition p) -> bool {
                        const auto it = next.find(p);
                        return it != next.end() && it->second == kWater;
                    };
                    if (deep) {
                        // A cell embedded in a complete surface row (wet on
                        // both sides) rests, so flat surfaces stay flat. A
                        // cell with a dry side flows toward the gap, so bumps
                        // drain and sparse rows level instead of freezing into
                        // permanent single-pixel dots.
                        if (!(wet_at(side_l) && wet_at(side_r))) {
                            for (CellPosition side : sides) {
                                const int step_x = side.x - source.x;
                                for (int dist = 1; dist <= kDispersion; ++dist) {
                                    const CellPosition target{source.x + step_x * dist, source.y};
                                    if (is_solid(target)) break;
                                    if (current.contains(target)) continue;
                                    if (!next.contains(target)) {
                                        next[target] = material_id;
                                        moved = true;
                                        break;
                                    }
                                }
                                if (moved) break;
                            }
                        }
                    } else {
                        // Infiltration: a lone surface droplet (open sky, no
                        // wet sides) sitting on settled sand seeps down,
                        // swapping with it. Micro-puddles drain into dunes
                        // instead of dotting slopes; packed water, buried
                        // water, and water over stone are unaffected.
                        const CellPosition above{source.x, source.y - 1};
                        const bool open_above = !current.contains(above);
                        const auto settled_it = next.find(down);
                        // Any sand here: settled or in flight. Swapping
                        // relocates (never overwrites), so percolation is
                        // mass-safe even through a slumping dune; the source
                        // home is free in next (writers only move down or
                        // skip current-occupied laterals).
                        const bool sand_below =
                            settled_it != next.end() && settled_it->second == kSand;
                        // Escape hatch: only seep when the displaced sand can
                        // roll somewhere (a diag of the source holding air or
                        // water in current). Otherwise the pair swap back and
                        // forth every tick: visible flicker, zero drainage.
                        // A dot in a fully-cupped pit stays put (a puddle).
                        auto escapable = [&](CellPosition p) -> bool {
                            const auto it = current.find(p);
                            return it == current.end() || it->second == kWater;
                        };
                        const CellPosition seep_l{source.x - 1, source.y + 1};
                        const CellPosition seep_r{source.x + 1, source.y + 1};
                        if (open_above && !wet_at(side_l) && !wet_at(side_r) && sand_below &&
                            (escapable(seep_l) || escapable(seep_r))) {
                            next[down] = material_id;
                            next[source] = kSand;
                            moved = true;
                        }
                    }
                }
            }
            if (!moved) {
                next[source] = material_id;
                stayed.insert(source);
            }
        }
    }

    // Mark dirty chunks based on changes between current and next
    for (const auto& [pos, mat] : current) {
        auto it = next.find(pos);
        if (it == next.end() || it->second != mat) {
            mark_chunk_dirty(chunk_pos_of(pos));
        }
    }
    for (const auto& [pos, mat] : next) {
        auto it = current.find(pos);
        if (it == current.end() || it->second != mat) {
            mark_chunk_dirty(chunk_pos_of(pos));
        }
    }

    rebuild_chunks(next);
    ++tick_;
}


std::size_t SimulationWorld::occupied_cell_count() const { return occupied_count_; }

bool SimulationWorld::has_chunk(CellPosition chunk_position) const {
    return chunks_.contains(chunk_position);
}

std::vector<CellPosition> SimulationWorld::active_chunks() const {
    std::vector<CellPosition> positions;
    positions.reserve(chunks_.size());
    for (const auto& [pos, _] : chunks_) {
        positions.push_back(pos);
    }
    return positions;
}

bool SimulationWorld::read_chunk(CellPosition chunk_position, std::span<MaterialId> out_buffer) const {
    constexpr std::size_t kCellsPerChunk = static_cast<std::size_t>(kChunkSize * kChunkSize);
    if (out_buffer.size() < kCellsPerChunk) return false;
    const auto it = chunks_.find(chunk_position);
    if (it != chunks_.end()) {
        std::copy_n(it->second.data(), kCellsPerChunk, out_buffer.data());
    } else {
        std::fill_n(out_buffer.data(), kCellsPerChunk, kAir);
    }
    return true;
}

void SimulationWorld::copy_rect(CellPosition first, CellPosition last, std::span<MaterialId> out_buffer) const {
    const int min_x = std::min(first.x, last.x);
    const int max_x = std::max(first.x, last.x);
    const int min_y = std::min(first.y, last.y);
    const int max_y = std::max(first.y, last.y);
    const int width = max_x - min_x + 1;
    const int height = max_y - min_y + 1;
    const std::size_t total_cells = static_cast<std::size_t>(width * height);
    if (out_buffer.size() < total_cells) return;

    std::size_t index = 0;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            out_buffer[index++] = cell({x, y});
        }
    }
}

std::vector<std::uint8_t> SimulationWorld::save() const {
    const CellMap cells = collect_cells();
    // Sort for deterministic bytes (unordered storage is O(1) but unordered).
    std::vector<std::pair<CellPosition, MaterialId>> ordered(cells.begin(), cells.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.first.x != right.first.x) return left.first.x < right.first.x;
        if (left.first.y != right.first.y) return left.first.y < right.first.y;
        return left.second < right.second;
    });
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSaveMagic.size() + 12 + ordered.size() * 9);
    bytes.insert(bytes.end(), kSaveMagic.begin(), kSaveMagic.end());
    append_u64(bytes, tick_);
    append_u32(bytes, static_cast<std::uint32_t>(ordered.size()));
    for (const auto& [position, material_id] : ordered) {
        append_u32(bytes, static_cast<std::uint32_t>(position.x));
        append_u32(bytes, static_cast<std::uint32_t>(position.y));
        bytes.push_back(material_id);
    }
    return bytes;
}

bool SimulationWorld::load(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kSaveMagic.size() + 12 || !std::equal(kSaveMagic.begin(), kSaveMagic.end(), bytes.begin())) return false;
    std::size_t offset = kSaveMagic.size();
    std::uint64_t saved_tick = 0;
    std::uint32_t count = 0;
    if (!read_u64(bytes, offset, saved_tick) || !read_u32(bytes, offset, count) || bytes.size() - offset != static_cast<std::size_t>(count) * 9) return false;
    CellMap loaded;
    loaded.reserve(static_cast<std::size_t>(count) + 16);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t raw_x = 0;
        std::uint32_t raw_y = 0;
        if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) || offset >= bytes.size()) return false;
        const MaterialId material_id = bytes[offset++];
        if (material_id == kAir || !materials_.contains(material_id)) return false;
        loaded[{static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)}] = material_id;
    }
    std::set<CellPosition> changed_chunks;
    for (const auto& [chunk_position, _] : chunks_) {
        changed_chunks.insert(chunk_position);
    }
    rebuild_chunks(loaded);
    for (const auto& [chunk_pos, _] : chunks_) {
        changed_chunks.insert(chunk_pos);
    }
    for (const CellPosition chunk_position : changed_chunks) {
        mark_chunk_dirty(chunk_position);
    }
    tick_ = saved_tick;
    return true;
}

bool SimulationWorld::is_solid(CellPosition position) const {
    const auto it = materials_.find(cell(position));
    if (it == materials_.end()) return false;
    return it->second.solid;
}

void SimulationWorld::build_play_level() {
    reset();
    // Floor and side walls.
    fill_rect({0, 240}, {255, 255}, kStone);
    fill_rect({0, 40}, {8, 239}, kStone);
    fill_rect({247, 40}, {255, 239}, kStone);
    // Staggered stone platforms (left) and a central mesa.
    fill_rect({20, 200}, {70, 205}, kStone);
    fill_rect({40, 160}, {90, 165}, kStone);
    fill_rect({100, 180}, {155, 186}, kStone);
    // Sand dunes resting on the floor.
    fill_rect({15, 220}, {55, 239}, kSand);
    fill_rect({150, 225}, {185, 239}, kSand);
    // Water reservoir (right): stone divider wall + pooled water.
    fill_rect({190, 200}, {196, 239}, kStone);
    fill_rect({197, 215}, {246, 239}, kWater);
    // Overhead rain pocket with a diggable sand plug on its left half.
    // Stone cheeks contain the slump so the pocket only releases when dug.
    fill_rect({145, 126}, {210, 131}, kStone);
    fill_rect({145, 100}, {149, 131}, kStone);
    fill_rect({206, 100}, {210, 131}, kStone);
    fill_rect({150, 106}, {172, 125}, kSand);
    fill_rect({173, 106}, {205, 125}, kWater);
}

bool SimulationWorld::can_move_to(CellPosition min_pos, CellPosition max_pos) const {
    const int min_x = std::min(min_pos.x, max_pos.x);
    const int max_x = std::max(min_pos.x, max_pos.x);
    const int min_y = std::min(min_pos.y, max_pos.y);
    const int max_y = std::max(min_pos.y, max_pos.y);
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            if (is_solid({x, y})) return false;
        }
    }
    return true;
}

std::vector<CellPosition> SimulationWorld::dirty_chunks() const {
    return std::vector<CellPosition>(dirty_chunks_.begin(), dirty_chunks_.end());
}

void SimulationWorld::clear_dirty_chunks() {
    dirty_chunks_.clear();
}

void SimulationWorld::mark_chunk_dirty(CellPosition chunk_position) {
    dirty_chunks_.insert(chunk_position);
}

const MaterialDefinition& SimulationWorld::material(MaterialId id) const { return materials_.at(id); }

bool SimulationWorld::chunk_storage_empty(const std::vector<MaterialId>& chunk) noexcept {
    for (MaterialId material_id : chunk) {
        if (material_id != kAir) return false;
    }
    return true;
}

bool SimulationWorld::prefers_left(CellPosition position, std::uint64_t tick) {
    std::uint64_t hash = static_cast<std::uint32_t>(position.x) * 0x9E3779B1u;
    hash ^= static_cast<std::uint32_t>(position.y) * 0x85EBCA77u;
    hash ^= tick * 0xC2B2AE3Du;
    hash ^= hash >> 16;
    return (hash & 1u) == 0;
}


SimulationWorld::CellMap SimulationWorld::collect_cells() const {
    CellMap cells;
    cells.reserve(occupied_count_ + 16);
    for (const auto& [chunk_position, chunk] : chunks_) {
        for (int y = 0; y < kChunkSize; ++y) for (int x = 0; x < kChunkSize; ++x) {
            const MaterialId material_id = chunk[static_cast<std::size_t>(y * kChunkSize + x)];
            if (material_id != kAir) cells[{chunk_position.x * kChunkSize + x, chunk_position.y * kChunkSize + y}] = material_id;
        }
    }
    return cells;
}

void SimulationWorld::rebuild_chunks(const CellMap& cells) {
    chunks_.clear();
    occupied_count_ = cells.size();
    if (cells.empty()) return;
    for (const auto& [position, material_id] : cells) {
        const CellPosition chunk_position{floor_divide(position.x, kChunkSize), floor_divide(position.y, kChunkSize)};
        auto [chunk, inserted] = chunks_.try_emplace(chunk_position);
        if (inserted) chunk->second.assign(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir);
        const int x = positive_modulo(position.x, kChunkSize);
        const int y = positive_modulo(position.y, kChunkSize);
        chunk->second[static_cast<std::size_t>(y * kChunkSize + x)] = material_id;
    }
}

} // namespace ashfall

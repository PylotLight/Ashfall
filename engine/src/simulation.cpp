#include "ashfall/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>

namespace ashfall {
namespace {

[[maybe_unused]] constexpr std::array<std::uint8_t, 8> kSaveMagic = {'A', 'S', 'H', 'F', 'A', 'L', 'L', 1};
constexpr std::array<std::uint8_t, 8> kSaveMagicV2 = {'A', 'S', 'H', 'F', 'A', 'L', 'L', 2};

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

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
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

bool read_u16(std::span<const std::uint8_t> input, std::size_t& offset, std::uint16_t& value) {
    if (offset + 2 > input.size()) return false;
    value = static_cast<std::uint16_t>(input[offset] | (static_cast<std::uint16_t>(input[offset + 1]) << 8));
    offset += 2;
    return true;
}

CellPosition chunk_pos_of(CellPosition pos) {
    return {floor_divide(pos.x, kChunkSize), floor_divide(pos.y, kChunkSize)};
}

// Bottom-up (y desc), x asc within a row: the exact order the old full
// std::sort produced. Counting sort by y (O(n + rows)), then sorts each
// short row by x. Falls back to std::sort on degenerate y-spread.
void order_dynamic_cells(std::vector<std::pair<CellPosition, MaterialId>>& cells,
                         std::vector<std::pair<CellPosition, MaterialId>>& scratch) {
    if (cells.size() < 2) return;
    int min_y = cells[0].first.y;
    int max_y = cells[0].first.y;
    for (const auto& entry : cells) {
        if (entry.first.y < min_y) min_y = entry.first.y;
        if (entry.first.y > max_y) max_y = entry.first.y;
    }
    const auto full_sort = [&]() {
        std::sort(cells.begin(), cells.end(), [](const auto& left, const auto& right) {
            if (left.first.y != right.first.y) return left.first.y > right.first.y;
            return left.first.x < right.first.x;
        });
    };
    const std::int64_t span = static_cast<std::int64_t>(max_y) - static_cast<std::int64_t>(min_y) + 1;
    if (span > 8192 || span > static_cast<std::int64_t>(cells.size()) * 8) {
        full_sort();
        return;
    }
    const std::size_t rows = static_cast<std::size_t>(span);
    std::vector<std::size_t> row_size(rows, 0);
    for (const auto& entry : cells) {
        ++row_size[static_cast<std::size_t>(static_cast<std::int64_t>(max_y) - entry.first.y)];
    }
    std::vector<std::size_t> row_start(rows + 1, 0);
    for (std::size_t row = 0; row < rows; ++row) row_start[row + 1] = row_start[row] + row_size[row];
    scratch.clear();
    scratch.resize(cells.size());
    std::vector<std::size_t> cursor(row_start.begin(), row_start.end() - 1);
    for (auto& entry : cells) {
        const std::size_t row = static_cast<std::size_t>(static_cast<std::int64_t>(max_y) - entry.first.y);
        scratch[cursor[row]++] = std::move(entry);
    }
    for (std::size_t row = 0; row < rows; ++row) {
        std::sort(scratch.begin() + static_cast<std::ptrdiff_t>(row_start[row]),
                  scratch.begin() + static_cast<std::ptrdiff_t>(row_start[row + 1]),
                  [](const auto& left, const auto& right) { return left.first.x < right.first.x; });
    }
    cells = std::move(scratch);
}

} // namespace

SimulationWorld::SimulationWorld() {
    load_material_table(default_material_table());
}

SimulationWorld::SimulationWorld(const MaterialTable& table) {
    load_material_table(table);
}

bool SimulationWorld::load_material_table(const MaterialTable& table) {
    std::string error;
    if (!validate_material_table(table, error)) return false;
    materials_.clear();
    material_order_.clear();
    for (const MaterialSpec& spec : table.materials) {
        materials_.emplace(spec.id, spec);
        material_order_.push_back(spec.id);
    }
    reset();
    return true;
}

MaterialId SimulationWorld::cell(CellPosition position) const {
    const MaterialId* found = cells_.find(position);
    return found == nullptr ? kAir : *found;
}

void SimulationWorld::reset() {
    for (const auto& [chunk_position, _] : chunks_) {
        mark_chunk_dirty(chunk_position);
    }
    chunks_.clear();
    cells_.clear();
    temps_.clear();
    temp_scratch_.clear();
    temp_prune_scratch_.clear();
    tick_ = 0;
}

void SimulationWorld::write_chunk_cell(CellPosition position, MaterialId material_id) {
    write_chunk_cell_deferred(position, material_id, nullptr);
}

void SimulationWorld::write_chunk_cell_deferred(
    CellPosition position, MaterialId material_id, std::set<CellPosition>* maybe_emptied) {
    const CellPosition chunk_position = chunk_pos_of(position);
    const int local_x = positive_modulo(position.x, kChunkSize);
    const int local_y = positive_modulo(position.y, kChunkSize);
    const std::size_t index = chunk_index(local_x, local_y);
    if (material_id == kAir) {
        const auto chunk_it = chunks_.find(chunk_position);
        if (chunk_it == chunks_.end()) return;
        chunk_it->second[index] = kAir;
        // O(chunk) emptiness scans are deferred in bulk paths: the caller
        // collects touched chunks and sweeps once (see step()).
        if (maybe_emptied != nullptr) {
            maybe_emptied->insert(chunk_position);
        } else if (chunk_storage_empty(chunk_it->second)) {
            chunks_.erase(chunk_it);
        }
        return;
    }
    auto chunk_it = chunks_.find(chunk_position);
    if (chunk_it == chunks_.end()) {
        auto [inserted_it, _] = chunks_.try_emplace(
            chunk_position, std::vector<MaterialId>(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir));
        chunk_it = inserted_it;
    }
    chunk_it->second[index] = material_id;
}

void SimulationWorld::set_cell(CellPosition position, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    const MaterialId* found = cells_.find(position);
    const MaterialId previous = found == nullptr ? kAir : *found;
    if (previous == material_id) return;
    if (material_id == kAir) {
        cells_.erase(position);
        temps_.erase(position);
    } else {
        cells_.insert_or_assign(position, material_id);
    }
    write_chunk_cell(position, material_id);
    mark_chunk_dirty(chunk_pos_of(position));
}

void SimulationWorld::fill_rect(CellPosition first, CellPosition last, MaterialId material_id) {
    if (!materials_.contains(material_id)) return;
    const int min_x = std::min(first.x, last.x);
    const int max_x = std::max(first.x, last.x);
    const int min_y = std::min(first.y, last.y);
    const int max_y = std::max(first.y, last.y);
    if (material_id == kAir) {
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const CellPosition position{x, y};
                temps_.erase(position);
                if (!cells_.contains(position)) continue;
                cells_.erase(position);
                write_chunk_cell(position, kAir);
            }
        }
    } else {
        for (int y = min_y; y <= max_y; ++y) {
            for (int x = min_x; x <= max_x; ++x) {
                const CellPosition position{x, y};
                cells_.insert_or_assign(position, material_id);
                write_chunk_cell(position, material_id);
            }
        }
    }
    const CellPosition min_chunk = chunk_pos_of({min_x, min_y});
    const CellPosition max_chunk = chunk_pos_of({max_x, max_y});
    for (int cy = min_chunk.y; cy <= max_chunk.y; ++cy) {
        for (int cx = min_chunk.x; cx <= max_chunk.x; ++cx) {
            mark_chunk_dirty({cx, cy});
        }
    }
}


void SimulationWorld::step() {
    // cells_ IS the tick-start state: no collect copy. next replaces it.
    const FlatCells& current = cells_;
    FlatCells& next = next_scratch_;
    next.clear();
    next.reserve_for(cells_.size() + 16);
    DynCells& dynamic_cells = dyn_scratch_;
    dynamic_cells.clear();
    dynamic_cells.reserve(cells_.size());
    // Homes of cells that stayed put this tick. Sand may only displace water
    // at a settled home (owner relocates to the swap source, which is safe);
    // overwriting an in-flight arrival would destroy a cell and break mass
    // conservation. Down/diag targets live a row below, so in bottom-up order
    // their owners are always decided already.
    FlatMarks& stayed = stayed_scratch_;
    stayed.clear();
    stayed.reserve_for(cells_.size());

    current.for_each([&](const CellPosition& position, const MaterialId& material_id) {
        if (material(material_id).movement == Movement::Static) {
            next.insert_or_assign(position, material_id);
        } else {
            dynamic_cells.push_back({position, material_id});
        }
    });

    order_dynamic_cells(dynamic_cells, order_scratch_);
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
                const MaterialId* found = next.find(p);
                if (found == nullptr) return 0;
                return material(*found).density;
            };

            bool moved = false;
            // Claim a powder target mass-safely: empty cells are taken;
            // settled occupants are swapped (owner relocates to source);
            // in-flight arrivals block (stealing them destroys mass).
            auto try_powder_target = [&](CellPosition dest) -> bool {
                if (get_density_in_next(dest) >= mat_def.density) return false;
                MaterialId* occupant = next.find(dest);
                if (occupant == nullptr) {
                    next.insert_or_assign(dest, material_id);
                } else if (*occupant != kAir && stayed.contains(dest)) {
                    MaterialId displaced = *occupant;
                    next.insert_or_assign(dest, material_id);
                    next.insert_or_assign(source, displaced);
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
                next.insert_or_assign(source, material_id);
                stayed.insert_or_assign(source, 1);
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
                next.insert_or_assign(down, material_id);
                moved = true;
            } else {
                for (CellPosition diag : diags) {
                    const int dx = diag.x - source.x;
                    const CellPosition horiz{source.x + dx, source.y};
                    if (is_solid(horiz) && is_solid(down)) {
                        continue;
                    }
                    if (is_empty_down(diag)) {
                        next.insert_or_assign(diag, material_id);
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
                    const MaterialId* below_mat = next.find(down);
                    const bool deep = below_mat != nullptr &&
                                      material(*below_mat).movement == Movement::Liquid;
                    // Wetness in the settled/next state (decided cells only).
                    auto wet_at = [&](CellPosition p) -> bool {
                        const MaterialId* found = next.find(p);
                        return found != nullptr && material(*found).movement == Movement::Liquid;
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
                                        next.insert_or_assign(target, material_id);
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
                        const MaterialId* below_now = next.find(down);
                        // Any powder here: settled or in flight. Swapping
                        // relocates (never overwrites), so percolation is
                        // mass-safe even through a slumping dune; the source
                        // home is free in next (writers only move down or
                        // skip current-occupied laterals).
                        const MaterialId below_id = below_now != nullptr ? *below_now : kAir;
                        const bool powder_below =
                            below_now != nullptr && material(below_id).movement == Movement::Powder;
                        // Escape hatch: only seep when the displaced sand can
                        // roll somewhere (a diag of the source holding air or
                        // water in current). Otherwise the pair swap back and
                        // forth every tick: visible flicker, zero drainage.
                        // A dot in a fully-cupped pit stays put (a puddle).
                        auto escapable = [&](CellPosition p) -> bool {
                            const MaterialId* found = current.find(p);
                            return found == nullptr || material(*found).movement == Movement::Liquid;
                        };
                        const CellPosition seep_l{source.x - 1, source.y + 1};
                        const CellPosition seep_r{source.x + 1, source.y + 1};
                        if (open_above && !wet_at(side_l) && !wet_at(side_r) && powder_below &&
                            (escapable(seep_l) || escapable(seep_r))) {
                            next.insert_or_assign(down, material_id);
                            next.insert_or_assign(source, below_id);
                            moved = true;
                        }
                    }
                }
            }
            if (!moved) {
                next.insert_or_assign(source, material_id);
                stayed.insert_or_assign(source, 1);
            }
        }
    }

    // Patch the chunk mirror + dirty set from the tick diff (minimal writes:
    // removals/changes in the first loop, pure additions in the second).
    // Emptiness sweeps are deferred: one O(chunk) scan per touched chunk.
    std::set<CellPosition> maybe_emptied;
    cells_.for_each([&](const CellPosition& pos, const MaterialId& mat) {
        const MaterialId* updated = next.find(pos);
        if (updated == nullptr) {
            write_chunk_cell_deferred(pos, kAir, &maybe_emptied);
            mark_chunk_dirty(chunk_pos_of(pos));
        } else if (*updated != mat) {
            write_chunk_cell(pos, *updated);
            mark_chunk_dirty(chunk_pos_of(pos));
        }
    });
    next.for_each([&](const CellPosition& pos, const MaterialId& mat) {
        if (!cells_.contains(pos)) {
            write_chunk_cell(pos, mat);
            mark_chunk_dirty(chunk_pos_of(pos));
        }
    });
    for (const CellPosition chunk_position : maybe_emptied) {
        const auto chunk_it = chunks_.find(chunk_position);
        if (chunk_it != chunks_.end() && chunk_storage_empty(chunk_it->second)) {
            chunks_.erase(chunk_it);
        }
    }

    std::swap(cells_, next);
    ++tick_;
    conduct_temperature();
}


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

    // Dense chunk-vector row copies; missing chunks are air.
    std::fill_n(out_buffer.data(), total_cells, kAir);
    const CellPosition min_chunk = chunk_pos_of({min_x, min_y});
    const CellPosition max_chunk = chunk_pos_of({max_x, max_y});
    for (int cy = min_chunk.y; cy <= max_chunk.y; ++cy) {
        for (int cx = min_chunk.x; cx <= max_chunk.x; ++cx) {
            const auto it = chunks_.find({cx, cy});
            if (it == chunks_.end()) continue;
            const int copy_min_x = std::max(min_x, cx * kChunkSize);
            const int copy_max_x = std::min(max_x, cx * kChunkSize + kChunkSize - 1);
            const int copy_min_y = std::max(min_y, cy * kChunkSize);
            const int copy_max_y = std::min(max_y, cy * kChunkSize + kChunkSize - 1);
            for (int y = copy_min_y; y <= copy_max_y; ++y) {
                const std::size_t src = static_cast<std::size_t>((y - cy * kChunkSize) * kChunkSize +
                                                                 (copy_min_x - cx * kChunkSize));
                const std::size_t dst = static_cast<std::size_t>((y - min_y) * width + (copy_min_x - min_x));
                const std::size_t run = static_cast<std::size_t>(copy_max_x - copy_min_x + 1);
                std::memcpy(out_buffer.data() + dst, it->second.data() + src, run * sizeof(MaterialId));
            }
        }
    }
}

std::vector<std::uint8_t> SimulationWorld::save() const {
    // Sort for deterministic bytes (flat storage is O(1) but unordered).
    std::vector<std::pair<CellPosition, MaterialId>> ordered;
    ordered.reserve(cells_.size());
    cells_.for_each([&](const CellPosition& position, const MaterialId& material_id) {
        ordered.emplace_back(position, material_id);
    });
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        if (left.first.x != right.first.x) return left.first.x < right.first.x;
        if (left.first.y != right.first.y) return left.first.y < right.first.y;
        return left.second < right.second;
    });
    // v2 temps section: sorted by (x, y) for deterministic bytes. Ambient
    // worlds store a zero count (4 bytes) — the only size cost when cold.
    std::vector<std::pair<CellPosition, std::int16_t>> ordered_temps;
    ordered_temps.reserve(temps_.size());
    temps_.for_each([&](const CellPosition& position, const std::int16_t& temp) {
        ordered_temps.emplace_back(position, temp);
    });
    std::sort(ordered_temps.begin(), ordered_temps.end(), [](const auto& left, const auto& right) {
        if (left.first.x != right.first.x) return left.first.x < right.first.x;
        return left.first.y < right.first.y;
    });
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kSaveMagicV2.size() + 12 + ordered.size() * 9 + 4 + ordered_temps.size() * 10);
    bytes.insert(bytes.end(), kSaveMagicV2.begin(), kSaveMagicV2.end());
    append_u64(bytes, tick_);
    append_u32(bytes, static_cast<std::uint32_t>(ordered.size()));
    for (const auto& [position, material_id] : ordered) {
        append_u32(bytes, static_cast<std::uint32_t>(position.x));
        append_u32(bytes, static_cast<std::uint32_t>(position.y));
        bytes.push_back(material_id);
    }
    append_u32(bytes, static_cast<std::uint32_t>(ordered_temps.size()));
    for (const auto& [position, temp] : ordered_temps) {
        append_u32(bytes, static_cast<std::uint32_t>(position.x));
        append_u32(bytes, static_cast<std::uint32_t>(position.y));
        append_u16(bytes, static_cast<std::uint16_t>(temp));
    }
    return bytes;
}

bool SimulationWorld::load(std::span<const std::uint8_t> bytes) {
    // Migration decision (per docs/AGENT_GUIDE.md): v1 loads with all cells
    // ambient (no heat section); v2 adds the sparse temps tail. Anything
    // else is rejected; failed loads leave the world (cells, heat, tick)
    // untouched. The host wrapper is unchanged — old user saves load.
    if (bytes.size() < kSaveMagicV2.size() + 12) return false;
    if (!std::equal(kSaveMagicV2.begin(), kSaveMagicV2.end() - 1, bytes.begin())) return false;
    const std::uint8_t version = bytes[kSaveMagicV2.size() - 1];
    if (version != 1 && version != 2) return false;
    std::size_t offset = kSaveMagicV2.size();
    std::uint64_t saved_tick = 0;
    std::uint32_t count = 0;
    if (!read_u64(bytes, offset, saved_tick) || !read_u32(bytes, offset, count)) return false;
    if (version == 1 && bytes.size() - offset != static_cast<std::size_t>(count) * 9) return false;
    if (version == 2 && bytes.size() - offset < static_cast<std::size_t>(count) * 9 + 4) return false;
    FlatCells loaded;
    loaded.reserve_for(static_cast<std::size_t>(count) + 16);
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t raw_x = 0;
        std::uint32_t raw_y = 0;
        if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) || offset >= bytes.size()) return false;
        const MaterialId material_id = bytes[offset++];
        if (material_id == kAir || !materials_.contains(material_id)) return false;
        loaded.insert_or_assign({static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)}, material_id);
    }
    FlatTemps loaded_temps;
    if (version == 2) {
        std::uint32_t temp_count = 0;
        if (!read_u32(bytes, offset, temp_count)) return false;
        if (bytes.size() - offset != static_cast<std::size_t>(temp_count) * 10) return false;
        for (std::uint32_t index = 0; index < temp_count; ++index) {
            std::uint32_t raw_x = 0;
            std::uint32_t raw_y = 0;
            std::uint16_t raw_temp = 0;
            if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) ||
                !read_u16(bytes, offset, raw_temp))
                return false;
            const auto temp = static_cast<std::int16_t>(raw_temp);
            if (temp == 0) return false; // canonical saves never store ambient
            loaded_temps.insert_or_assign(
                {static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)}, temp);
        }
    }
    std::set<CellPosition> changed_chunks;
    for (const auto& [chunk_position, _] : chunks_) {
        changed_chunks.insert(chunk_position);
    }
    rebuild_chunks(loaded);
    cells_ = std::move(loaded);
    temps_ = std::move(loaded_temps);
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
    // 512x512 play level: floor, walls, platforms, dunes, reservoir,
    // and a contained rain pocket with a diggable sand plug.
    fill_rect({0, 480}, {511, 511}, kStone);
    fill_rect({0, 80}, {16, 479}, kStone);
    fill_rect({494, 80}, {511, 479}, kStone);
    // Staggered stone platforms (left) and a central mesa.
    fill_rect({40, 400}, {140, 410}, kStone);
    fill_rect({80, 320}, {180, 330}, kStone);
    fill_rect({200, 360}, {310, 372}, kStone);
    // Sand dunes resting on the floor.
    fill_rect({30, 440}, {110, 479}, kSand);
    fill_rect({320, 450}, {379, 479}, kSand);
    // Water reservoir (right): stone divider wall + pooled water.
    fill_rect({380, 400}, {392, 479}, kStone);
    fill_rect({394, 430}, {492, 479}, kWater);
    // Overhead rain pocket with a diggable sand plug on its left half.
    // Stone cheeks contain the slump so the pocket only releases when dug.
    fill_rect({290, 252}, {420, 262}, kStone);
    fill_rect({290, 200}, {298, 262}, kStone);
    fill_rect({412, 200}, {420, 262}, kStone);
    fill_rect({300, 212}, {344, 250}, kSand);
    fill_rect({346, 212}, {410, 250}, kWater);
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

const MaterialSpec& SimulationWorld::material(MaterialId id) const { return materials_.at(id); }

bool SimulationWorld::material_info(MaterialId id, MaterialSpec& out) const {
    const auto found = materials_.find(id);
    if (found == materials_.end()) return false;
    out = found->second;
    return true;
}

void SimulationWorld::set_temperature(CellPosition position, int temperature) {
    const int clamped = std::clamp(temperature, -32767, 32767);
    if (clamped == 0) {
        temps_.erase(position);
    } else {
        temps_.insert_or_assign(position, static_cast<std::int16_t>(clamped));
    }
}

int SimulationWorld::temperature_at(CellPosition position) const {
    const std::int16_t* found = temps_.find(position);
    return found == nullptr ? 0 : static_cast<int>(*found);
}

int SimulationWorld::temperature(CellPosition position) const { return temperature_at(position); }

void SimulationWorld::conduct_temperature() {
    // Ambient fast path: no stored heat anywhere, so ambient worlds pay one
    // branch per tick and the hot loop is untouched (perf note: conduction
    // is O(occupied) integer ops only while heat exists).
    if (temps_.empty()) return;
    // No advection in P2.4: drop heat whose cell is gone BEFORE conducting,
    // or falling hot material would heat its own wake through the stale
    // snapshot entry. P2.5 carries heat with moving material instead.
    temp_prune_scratch_.clear();
    temps_.for_each([&](const CellPosition& position, const std::int16_t&) {
        if (!cells_.contains(position)) temp_prune_scratch_.push_back(position);
    });
    for (const CellPosition position : temp_prune_scratch_) temps_.erase(position);
    if (temps_.empty()) return;
    // Snapshot reads (temps_ untouched during the pass) → per-position
    // results are iteration-order independent and deterministic. Integer
    // mean over self + 4 neighbors (missing = ambient 0); truncates toward
    // zero so small temps vanish instead of lingering at ±1.
    FlatTemps& conducted = temp_scratch_;
    conducted.clear();
    conducted.reserve_for(cells_.size() + 16);
    cells_.for_each([&](const CellPosition& position, const MaterialId&) {
        int sum = temperature_at(position);
        sum += temperature_at({position.x - 1, position.y});
        sum += temperature_at({position.x + 1, position.y});
        sum += temperature_at({position.x, position.y - 1});
        sum += temperature_at({position.x, position.y + 1});
        const int next = sum / 5;
        if (next != 0) conducted.insert_or_assign(position, static_cast<std::int16_t>(next));
    });
    std::swap(temps_, temp_scratch_);
}

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


void SimulationWorld::rebuild_chunks(const FlatCells& cells) {
    chunks_.clear();
    cells.for_each([&](const CellPosition& position, const MaterialId& material_id) {
        const CellPosition chunk_position{floor_divide(position.x, kChunkSize), floor_divide(position.y, kChunkSize)};
        auto [chunk, inserted] = chunks_.try_emplace(chunk_position);
        if (inserted) chunk->second.assign(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir);
        const int x = positive_modulo(position.x, kChunkSize);
        const int y = positive_modulo(position.y, kChunkSize);
        chunk->second[static_cast<std::size_t>(y * kChunkSize + x)] = material_id;
    });
}

} // namespace ashfall

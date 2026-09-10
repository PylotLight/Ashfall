#include "ashfall/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>
#ifdef ASHFALL_STEP_INSTRUMENT
#include <chrono>
#endif

namespace ashfall {
namespace {

// (Group 1, Task C2:) the old anonymous-namespace kSaveMagic/kSaveMagicV2
// 8-byte constants are removed; the canonical spellings are now
// SimulationWorld::kSaveMagicPrefix + kSaveVersion1/kSaveVersion2 (header).
// Nothing outside this file referenced the old names (verified by grep).

#ifdef ASHFALL_STEP_INSTRUMENT
inline double step_ms_between(
    const std::chrono::steady_clock::time_point& start,
    const std::chrono::steady_clock::time_point& end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
#endif

int floor_divide(int value, int divisor) {
    const int quotient = value / divisor;
    return value % divisor < 0 ? quotient - 1 : quotient;
}

int positive_modulo(int value, int divisor) {
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

// C++20 two's-complement makes this bit pattern exact for negative coords:
// callers pass static_cast<std::uint32_t>(position.x), and load round-trips
// via static_cast<std::int32_t>(raw_x). Do not "simplify" the casts away.
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

// (Group 2, Task S1 chunk-bomb guard:) sortable chunk identity for a cell:
// ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cy with floor-divided chunk
// coords (negative cells map to negative chunks, matching rebuild_chunks).
// load() collects one key per cell into a vector, sorts + uniques it, and
// rejects over kMaxSaveChunks. Transient is bounded by the cell cap
// (<= (1<<22) keys x 8 B = 32 MiB worst case).
std::uint64_t load_chunk_key(CellPosition pos) {
    const auto chunk_x = static_cast<std::uint32_t>(floor_divide(pos.x, kChunkSize));
    const auto chunk_y = static_cast<std::uint32_t>(floor_divide(pos.y, kChunkSize));
    return (static_cast<std::uint64_t>(chunk_x) << 32) | chunk_y;
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
    reactions_.clear();
    return true;
}

bool SimulationWorld::material_id_by_name(const std::string& name, MaterialId& out) const {
    for (const auto& [id, spec] : materials_) {
        if (spec.name == name) {
            out = id;
            return true;
        }
    }
    return false;
}

bool SimulationWorld::matches_reactant(MaterialId id, const ReactionSlot& slot) const {
    if (slot.is_tag) {
        for (const std::string& tag : material(id).tags) {
            if (tag == slot.tag) return true;
        }
        return false;
    }
    return id == slot.id;
}

std::uint32_t SimulationWorld::reaction_roll(
    CellPosition position, std::uint64_t tick, std::uint32_t rule) {
    // Position-keyed (never iteration-keyed): identical worlds built in
    // different insertion orders roll identically, and save/load round-trips
    // preserve behavior. Same mix family as prefers_left.
    std::uint64_t hash = static_cast<std::uint32_t>(position.x) * 0x9E3779B1u;
    hash ^= static_cast<std::uint32_t>(position.y) * 0x85EBCA77u;
    hash ^= tick * 0xC2B2AE3Du;
    hash ^= static_cast<std::uint64_t>(rule + 1) * 0x27D4EB2Du;
    hash ^= hash >> 16;
    return static_cast<std::uint32_t>(hash % 100u);
}

bool SimulationWorld::load_reactions(const ReactionTable& table) {
    std::string error;
    if (!validate_reaction_table(table, error)) return false;
    std::vector<ResolvedReaction> resolved;
    resolved.reserve(table.reactions.size());
    for (const ReactionSpec& spec : table.reactions) {
        ResolvedReaction rule;
        rule.name = spec.name;
        rule.probability = spec.probability;
        rule.reactant_count = static_cast<int>(spec.reactants.size());
        for (int i = 0; i < rule.reactant_count; ++i) {
            if (spec.reactants[i].is_tag) {
                rule.reactants[i].is_tag = true;
                rule.reactants[i].tag = spec.reactants[i].name;
            } else if (!material_id_by_name(spec.reactants[i].name, rule.reactants[i].id)) {
                return false;
            }
        }
        rule.product_count = static_cast<int>(spec.products.size());
        for (int i = 0; i < rule.product_count; ++i) {
            if (!material_id_by_name(spec.products[i].material, rule.products[i].id)) return false;
        }
        rule.has_min = spec.gate.has_min;
        rule.min_temp = spec.gate.min_value;
        rule.has_max = spec.gate.has_max;
        rule.max_temp = spec.gate.max_value;
        rule.has_product_temp = spec.has_product_temperature;
        rule.product_temp = spec.product_temperature;
        resolved.push_back(std::move(rule));
    }
    reactions_ = std::move(resolved);
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
    heat_carry_scratch_.clear();
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


void SimulationWorld::run_reactions() {
    // Reaction pre-pass: evaluates every rule against the tick-start
    // snapshot, stages transforms, then applies once. Staging (never
    // in-place edits) plus position-keyed rolls make outcomes independent of
    // FlatMap scan order: the same logical world reacts identically however
    // its cells were inserted. Cost is O(cells x rules) only while rules
    // are loaded; reactionless worlds pay one branch.
    // Option A semantics (adopted 2026-09-10): rules are competing
    // transformations, not atomic consumption. (a) Two primaries sharing
    // one neighbor reactant BOTH transform while one neighbor claim wins
    // (first hit per cell). (b) A primary winning its own cell while its
    // neighbor product loses to the neighbor's own reaction applies HALF
    // the instance. Revisit transactional iff consumption rules are scoped.
#ifdef ASHFALL_STEP_INSTRUMENT
    // reaction_cells/reaction_hits are stored directly here (run_reactions
    // runs inside step()'s reactions phase); step() fills every other
    // counter field at the end and must not overwrite these two.
    if (reactions_.empty()) {
        last_counters_.reaction_cells = 0;
        last_counters_.reaction_hits = 0;
        return;
    }
#else
    if (reactions_.empty()) return;
#endif
    struct Hit {
        CellPosition pos;
        MaterialId product;
        std::uint32_t rule;
        bool own; // primary-cell transform beats neighbor-imposed claims
        CellPosition primary;
    };
    std::vector<Hit> hits;
    hits.reserve(cells_.size());
#ifdef ASHFALL_STEP_INSTRUMENT
    std::size_t visited_cells = 0;
#endif
    cells_.for_each([&](const CellPosition& position, const MaterialId& material_id) {
#ifdef ASHFALL_STEP_INSTRUMENT
        ++visited_cells;
#endif
        for (std::uint32_t rule_idx = 0; rule_idx < reactions_.size(); ++rule_idx) {
            const ResolvedReaction& rule = reactions_[rule_idx];
            if (!matches_reactant(material_id, rule.reactants[0])) continue;
            const int heat = temperature_at(position);
            if (rule.has_min && heat < rule.min_temp) continue;
            if (rule.has_max && heat > rule.max_temp) continue;
            const auto roll = reaction_roll(position, tick_, rule_idx);
            if (rule.reactant_count == 1) {
                if (roll < static_cast<std::uint32_t>(rule.probability)) {
                    hits.push_back({position, rule.products[0].id, rule_idx, true, position});
                }
                break;
            }
            const bool left_first = prefers_left(position, tick_);
            const std::array<CellPosition, 4> neighbors = {
                CellPosition{position.x, position.y - 1},
                CellPosition{position.x, position.y + 1},
                left_first ? CellPosition{position.x - 1, position.y}
                           : CellPosition{position.x + 1, position.y},
                left_first ? CellPosition{position.x + 1, position.y}
                           : CellPosition{position.x - 1, position.y}};
            bool owned = false;
            for (const CellPosition neighbor : neighbors) {
                const MaterialId* found = cells_.find(neighbor);
                const bool empty = found == nullptr;
                const ReactionSlot& want = rule.reactants[1];
                const bool match = (want.id == kAir && !want.is_tag)
                                       ? empty
                                       : (!empty && matches_reactant(*found, want));
                if (!match) continue;
                // Full match (R1 + R2 + gate): this rule owns the primary
                // this tick, whether or not the roll lands. R1-only matches
                // must NOT consume the primary, or an inapplicable rule
                // would shadow every lower-priority rule forever (e.g. wet
                // quench shadowing burnout for a dry fire).
                if (roll < static_cast<std::uint32_t>(rule.probability)) {
                    hits.push_back({position, rule.products[0].id, rule_idx, true, position});
                    if (rule.product_count == 2) {
                        hits.push_back({neighbor, rule.products[1].id, rule_idx, false, position});
                    }
                }
                owned = true;
                break;
            }
            if (owned) break; // a fully-matching rule owns the primary this tick
        }
    });
    // Deterministic conflict resolution: total sort order (position, own
    // first, rule priority, primary position) — first hit per cell wins.
#ifdef ASHFALL_STEP_INSTRUMENT
    last_counters_.reaction_cells = visited_cells;
    last_counters_.reaction_hits = hits.size();
#endif
    std::sort(hits.begin(), hits.end(), [](const Hit& left, const Hit& right) {
        if (left.pos.x != right.pos.x) return left.pos.x < right.pos.x;
        if (left.pos.y != right.pos.y) return left.pos.y < right.pos.y;
        if (left.own != right.own) return left.own > right.own;
        if (left.rule != right.rule) return left.rule < right.rule;
        if (left.primary.x != right.primary.x) return left.primary.x < right.primary.x;
        return left.primary.y < right.primary.y;
    });
    bool have_applied = false;
    CellPosition applied{0, 0};
    for (const Hit& hit : hits) {
        if (have_applied && hit.pos == applied) continue;
        set_cell(hit.pos, hit.product);
        const ResolvedReaction& rule = reactions_[hit.rule];
        if (rule.has_product_temp) set_temperature(hit.pos, rule.product_temp);
        applied = hit.pos;
        have_applied = true;
    }
}

void SimulationWorld::step() {
#ifdef ASHFALL_STEP_INSTRUMENT
    const auto step_t0 = std::chrono::steady_clock::now();
    StepTimings timings;
    std::size_t powder_moves = 0, liquid_moves = 0, gas_moves = 0, swaps = 0;
#endif
    // Reactions transform the tick-start state before movement sees it.
#ifdef ASHFALL_STEP_INSTRUMENT
    {
        const auto t0 = std::chrono::steady_clock::now();
        run_reactions();
        timings.reactions_ms = step_ms_between(t0, std::chrono::steady_clock::now());
    }
#else
    run_reactions();
#endif
    // cells_ IS the tick-start state: no collect copy. next replaces it.
    const FlatCells& current = cells_;
    FlatCells& next = next_scratch_;
#ifdef ASHFALL_STEP_INSTRUMENT
    const auto clear_t0 = std::chrono::steady_clock::now();
#endif
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
#ifdef ASHFALL_STEP_INSTRUMENT
    // clear_reserve covers next.clear/reserve + scratch clears/reserves at
    // the top of step().
    timings.clear_reserve_ms = step_ms_between(clear_t0, std::chrono::steady_clock::now());
    const auto classify_t0 = std::chrono::steady_clock::now();
#endif

    current.for_each([&](const CellPosition& position, const MaterialId& material_id) {
        if (material(material_id).movement == Movement::Static) {
            next.insert_or_assign(position, material_id);
        } else {
            dynamic_cells.push_back({position, material_id});
        }
    });

#ifdef ASHFALL_STEP_INSTRUMENT
    timings.classify_ms = step_ms_between(classify_t0, std::chrono::steady_clock::now());
    // Classified exactly once each: static = tick-start size minus dynamic.
    const std::size_t step_dynamic_count = dynamic_cells.size();
    const std::size_t step_static_count = current.size() - step_dynamic_count;
    const auto ordering_t0 = std::chrono::steady_clock::now();
#endif
    order_dynamic_cells(dynamic_cells, order_scratch_);
#ifdef ASHFALL_STEP_INSTRUMENT
    timings.ordering_ms = step_ms_between(ordering_t0, std::chrono::steady_clock::now());
#endif
    // Per-arm timing choice (documented per contract): two clock reads per
    // dynamic cell (entry + exit) beat per-iteration reads inside every arm
    // (which would distort the hot loop); the delta of the whole per-cell
    // iteration, including dispatch (material()/prefers_left), is attributed
    // to that cell's movement class. Residual: one clock read (~tens of ns)
    // per dynamic cell lands outside every phase, so phase-sum can undershoot
    // total by ~25ns x dynamic_cells on heavy worlds — expected, not a bug.
    for (const auto& [source, material_id] : dynamic_cells) {
#ifdef ASHFALL_STEP_INSTRUMENT
        const auto arm_t0 = std::chrono::steady_clock::now();
#endif
        const auto& mat_def = material(material_id);
        const bool left_first = prefers_left(source, tick_);
#ifdef ASHFALL_STEP_INSTRUMENT
        const Movement arm_class = mat_def.movement;
#endif

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
                    stage_heat_carry(source, dest);
                } else if (*occupant != kAir && stayed.contains(dest)) {
                    MaterialId displaced = *occupant;
                    next.insert_or_assign(dest, material_id);
                    next.insert_or_assign(source, displaced);
                    stage_heat_carry(source, dest);
                    stage_heat_carry(dest, source);
#ifdef ASHFALL_STEP_INSTRUMENT
                    ++swaps;
#endif
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
#ifdef ASHFALL_STEP_INSTRUMENT
            } else {
                ++powder_moves;
#endif
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
            {
                // Density-ordered displacement (docs/NOITA_MATERIALS.md §2):
                // a heavier liquid/gas sinks through a settled lighter one
                // (lava through water, water through steam); lighter stays
                // floating (oil on water — P2.6). Only settled occupants are
                // swapped; in-flight arrivals block, as with powder.
                MaterialId* occupant = next.find(down);
                if (occupant == nullptr) {
                    next.insert_or_assign(down, material_id);
                    stage_heat_carry(source, down);
                    moved = true;
                } else if (*occupant != kAir && stayed.contains(down)) {
                    const auto& occupant_def = material(*occupant);
                    if ((occupant_def.movement == Movement::Liquid ||
                         occupant_def.movement == Movement::Gas) &&
                        occupant_def.density < mat_def.density) {
                        const MaterialId displaced = *occupant;
                        next.insert_or_assign(down, material_id);
                        next.insert_or_assign(source, displaced);
                        stage_heat_carry(source, down);
                        stage_heat_carry(down, source);
                        moved = true;
#ifdef ASHFALL_STEP_INSTRUMENT
                        ++swaps;
#endif
                    }
                }
            }
            if (!moved) {
                for (CellPosition diag : diags) {
                    const int dx = diag.x - source.x;
                    const CellPosition horiz{source.x + dx, source.y};
                    if (is_solid(horiz) && is_solid(down)) {
                        continue;
                    }
                    if (is_empty_down(diag)) {
                        next.insert_or_assign(diag, material_id);
                        stage_heat_carry(source, diag);
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
                                        stage_heat_carry(source, target);
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
                            stage_heat_carry(source, down);
                            stage_heat_carry(down, source);
                            moved = true;
#ifdef ASHFALL_STEP_INSTRUMENT
                            ++swaps;
#endif
                        }
                    }
                }
            }
            if (!moved) {
                next.insert_or_assign(source, material_id);
                stayed.insert_or_assign(source, 1);
#ifdef ASHFALL_STEP_INSTRUMENT
            } else {
                ++liquid_moves;
#endif
            }
            // Gas never swaps (bubbles emerge from the liquid side via
            // density down-swaps; gas itself only takes free targets), so the
            // swaps counter has no gas contribution by construction.
        } else if (mat_def.movement == Movement::Gas) {
            // Gases rise, pool under ceilings, and disperse sideways — all
            // table-driven, no id branches. An upward target is only free
            // when NOBODY owns it: cells above are still unprocessed in
            // bottom-up order (present in current, absent from next), so
            // taking their home now would overwrite their later decision
            // and destroy mass. Blocked gas waits a tick; columns still
            // rise coherently at one cell per tick, top first. Bubbles
            // through liquid emerge from the liquid side (density
            // down-swaps); gas never swaps upward itself. Dissipation is a
            // data reaction (vapour + adjacent air), not movement.
            const CellPosition up{source.x, source.y - 1};
            const CellPosition diag_l{source.x - 1, source.y - 1};
            const CellPosition diag_r{source.x + 1, source.y - 1};
            const CellPosition side_l{source.x - 1, source.y};
            const CellPosition side_r{source.x + 1, source.y};

            const std::array<CellPosition, 2> diags = left_first
                ? std::array<CellPosition, 2>{diag_l, diag_r}
                : std::array<CellPosition, 2>{diag_r, diag_l};
            const std::array<CellPosition, 2> sides = left_first
                ? std::array<CellPosition, 2>{side_l, side_r}
                : std::array<CellPosition, 2>{side_r, side_l};

            auto gas_free = [&](CellPosition p) -> bool {
                return !next.contains(p) && !current.contains(p);
            };

            bool moved = false;
            auto try_gas_target = [&](CellPosition dest) -> bool {
                if (!gas_free(dest)) return false;
                next.insert_or_assign(dest, material_id);
                stage_heat_carry(source, dest);
                return true;
            };
            if (try_gas_target(up)) {
                moved = true;
            } else {
                for (CellPosition diag : diags) {
                    const int dx = diag.x - source.x;
                    const CellPosition side{source.x + dx, source.y};
                    if (is_solid(side) && is_solid(up)) {
                        continue;
                    }
                    if (try_gas_target(diag)) {
                        moved = true;
                        break;
                    }
                }
                if (!moved) {
                    constexpr int kDispersion = 4;
                    for (CellPosition side : sides) {
                        const int step_x = side.x - source.x;
                        for (int dist = 1; dist <= kDispersion; ++dist) {
                            const CellPosition target{source.x + step_x * dist, source.y};
                            if (is_solid(target)) break;
                            if (current.contains(target)) continue;
                            if (!next.contains(target)) {
                                next.insert_or_assign(target, material_id);
                                stage_heat_carry(source, target);
                                moved = true;
                                break;
                            }
                        }
                        if (moved) break;
                    }
                }
            }
            if (!moved) {
                next.insert_or_assign(source, material_id);
                stayed.insert_or_assign(source, 1);
#ifdef ASHFALL_STEP_INSTRUMENT
            } else {
                ++gas_moves;
#endif
            }
        }
#ifdef ASHFALL_STEP_INSTRUMENT
        {
            const double arm_ms = step_ms_between(arm_t0, std::chrono::steady_clock::now());
            if (arm_class == Movement::Powder) {
                timings.powder_ms += arm_ms;
            } else if (arm_class == Movement::Liquid) {
                timings.liquid_ms += arm_ms;
            } else {
                timings.gas_ms += arm_ms;
            }
        }
#endif
    }

    // Patch the chunk mirror + dirty set from the tick diff (minimal writes:
    // removals/changes in the first loop, pure additions in the second).
    // Emptiness sweeps are deferred: one O(chunk) scan per touched chunk.
#ifdef ASHFALL_STEP_INSTRUMENT
    const auto reconcile_t0 = std::chrono::steady_clock::now();
    // changed_positions and dirty_chunk_marks count ONLY the three reconcile
    // call sites below (not global mark_chunk_dirty traffic); both loops
    // diff the tick, so removals + changes + additions is the position delta.
    std::size_t changed_positions = 0, dirty_chunk_marks = 0;
#endif
    std::set<CellPosition> maybe_emptied;
    cells_.for_each([&](const CellPosition& pos, const MaterialId& mat) {
        const MaterialId* updated = next.find(pos);
        if (updated == nullptr) {
            write_chunk_cell_deferred(pos, kAir, &maybe_emptied);
            mark_chunk_dirty(chunk_pos_of(pos));
#ifdef ASHFALL_STEP_INSTRUMENT
            ++changed_positions;
            ++dirty_chunk_marks;
#endif
        } else if (*updated != mat) {
            write_chunk_cell(pos, *updated);
            mark_chunk_dirty(chunk_pos_of(pos));
#ifdef ASHFALL_STEP_INSTRUMENT
            ++changed_positions;
            ++dirty_chunk_marks;
#endif
        }
    });
    next.for_each([&](const CellPosition& pos, const MaterialId& mat) {
        if (!cells_.contains(pos)) {
            write_chunk_cell(pos, mat);
            mark_chunk_dirty(chunk_pos_of(pos));
#ifdef ASHFALL_STEP_INSTRUMENT
            ++changed_positions;
            ++dirty_chunk_marks;
#endif
        }
    });
    for (const CellPosition chunk_position : maybe_emptied) {
        const auto chunk_it = chunks_.find(chunk_position);
        if (chunk_it != chunks_.end() && chunk_storage_empty(chunk_it->second)) {
            chunks_.erase(chunk_it);
        }
    }

    std::swap(cells_, next);
#ifdef ASHFALL_STEP_INSTRUMENT
    // reconcile covers both diff loops + the emptiness sweep + the swap.
    timings.reconcile_ms = step_ms_between(reconcile_t0, std::chrono::steady_clock::now());
#endif
    ++tick_;
#ifdef ASHFALL_STEP_INSTRUMENT
    const std::size_t carried_heat = heat_carry_scratch_.size();
    const auto heat_carry_t0 = std::chrono::steady_clock::now();
#endif
    for (const auto& [dest, temp] : heat_carry_scratch_) temps_.insert_or_assign(dest, temp);
    heat_carry_scratch_.clear();
#ifdef ASHFALL_STEP_INSTRUMENT
    timings.heat_carry_ms = step_ms_between(heat_carry_t0, std::chrono::steady_clock::now());
    const auto conduct_t0 = std::chrono::steady_clock::now();
#endif
    conduct_temperature();
#ifdef ASHFALL_STEP_INSTRUMENT
    // conduct covers the pre-conduction prune + conduct_temperature().
    timings.conduct_ms = step_ms_between(conduct_t0, std::chrono::steady_clock::now());
    timings.total_ms = step_ms_between(step_t0, std::chrono::steady_clock::now());
    last_timings_ = timings;
    // reaction_cells/reaction_hits were stored by run_reactions() above;
    // every other field is filled here (never whole-struct-assigned, so the
    // reaction fields survive).
    last_counters_.occupied = cells_.size();
    last_counters_.static_cells = step_static_count;
    last_counters_.dynamic_cells = step_dynamic_count;
    last_counters_.powder_moves = powder_moves;
    last_counters_.liquid_moves = liquid_moves;
    last_counters_.gas_moves = gas_moves;
    last_counters_.swaps = swaps;
    last_counters_.nonzero_temps = temps_.size();
    last_counters_.carried_heat = carried_heat;
    last_counters_.changed_positions = changed_positions;
    last_counters_.dirty_chunk_marks = dirty_chunk_marks;
#endif
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
    constexpr std::size_t kHeaderBytes = 7 + 1 + 12; // prefix + version + tick + ncell
    bytes.reserve(kHeaderBytes + ordered.size() * 9 + 4 + ordered_temps.size() * 10);
    bytes.insert(bytes.end(), kSaveMagicPrefix.begin(), kSaveMagicPrefix.end());
    bytes.push_back(kSaveVersion2);
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
    LoadError ignored = LoadError::Ok;
    return load_with_diagnostic(bytes, ignored);
}

bool SimulationWorld::load_with_diagnostic(std::span<const std::uint8_t> bytes, LoadError& error) {
    // Migration decision (per docs/AGENT_GUIDE.md): v1 loads with all cells
    // ambient (no heat section); v2 adds the sparse temps tail. Anything
    // else is rejected; failed loads leave the world (cells, heat, tick)
    // untouched. The host wrapper is unchanged — old user saves load.
    // Transient memory note: pre-commit the live tables, `loaded` /
    // `loaded_temps`, and the rebuilt chunk map coexist (~2.2x steady-state
    // tables); a streaming loader is later work. The chunk-key vector below
    // is bounded by the cell cap ((1<<22) x 8 B <= 32 MiB), so the note
    // stays accurate.
    // v2-strict (Group 2, Task S1; version byte stays 2): load() newly
    // REJECTS only malformed/foreign inputs — canonical saves (what save()
    // emits: ascending, unique, within caps) still load, so honest saves are
    // unaffected. Newly-rejected classes: oversize files/counts (Oversize),
    // out-of-range coords (CoordOutOfRange), duplicate cells/temps
    // (DuplicateCell/DuplicateTemp), non-ascending sections
    // (UnsortedCells/UnsortedTemps — save() always emits ascending), and
    // chunk-count bombs (TooManyChunks). Temps-on-air stay ACCEPTED (the S4
    // set-on-air rejection waits for v3).
    //
    // (Owner caps 2026-09-10: kMaxSaveBytes = 64 MiB,
    // kMaxSaveCells = kMaxSaveTemps = 1<<22, kMaxSaveChunks = 4096,
    // kMaxSaveCoord = 1<<24 (|x|,|y| bound; play level is 0..511).)
    error = LoadError::Ok;
    constexpr std::size_t kHeaderBytes = 7 + 1 + 12; // prefix + version + tick + ncell
    constexpr std::size_t kMaxSaveBytes = 64u * 1024u * 1024u;
    constexpr std::uint32_t kMaxSaveCells = 1u << 22;
    constexpr std::uint32_t kMaxSaveTemps = 1u << 22;
    constexpr std::size_t kMaxSaveChunks = 4096;
    constexpr std::int32_t kMaxSaveCoord = 1 << 24;
    if (bytes.size() > kMaxSaveBytes) {
        error = LoadError::Oversize;
        return false;
    }
    if (bytes.size() < kHeaderBytes) {
        error = LoadError::TooShort;
        return false;
    }
    if (!std::equal(kSaveMagicPrefix.begin(), kSaveMagicPrefix.end(), bytes.begin())) {
        error = LoadError::BadMagic;
        return false;
    }
    const std::uint8_t version = bytes[kSaveMagicPrefix.size()];
    if (version != kSaveVersion1 && version != kSaveVersion2) {
        error = LoadError::BadVersion;
        return false;
    }
    std::size_t offset = kSaveMagicPrefix.size() + 1;
    std::uint64_t saved_tick = 0;
    std::uint32_t count = 0;
    if (!read_u64(bytes, offset, saved_tick) || !read_u32(bytes, offset, count)) {
        error = LoadError::BadCellSection;
        return false;
    }
    if (count > kMaxSaveCells) {
        // Count cap BEFORE reserve_for AND before the section-size extent
        // math below: an oversized count in a tiny file rejects here
        // (Oversize) with no large allocation and no throw.
        error = LoadError::Oversize;
        return false;
    }
    // Load-bearing: the cast to std::size_t happens BEFORE the multiply, so
    // the extent is overflow-safe on 64-bit. Do not "simplify" this into a
    // 32-bit multiply.
    if (version == kSaveVersion1 && bytes.size() - offset != static_cast<std::size_t>(count) * 9) {
        error = LoadError::BadCellSection;
        return false;
    }
    if (version == kSaveVersion2 && bytes.size() - offset < static_cast<std::size_t>(count) * 9 + 4) {
        error = LoadError::BadCellSection;
        return false;
    }
    FlatCells loaded;
    loaded.reserve_for(static_cast<std::size_t>(count) + 16);
    // v2-strict ordering: save() always emits ascending (x, y), so only
    // foreign/hand-corrupted inputs newly reject here. Two agreeing
    // mechanisms: the prev-compare names the failure kind (equality ->
    // Duplicate*, descent -> Unsorted*) while find_or_insert on the local
    // table belt-and-braces rejects any duplicate the compare could miss.
    // They always agree: strictly-ascending pairs are pairwise distinct, so
    // !inserted is unreachable when the compare passes.
    bool have_prev_cell = false;
    CellPosition prev_cell{0, 0};
    for (std::uint32_t index = 0; index < count; ++index) {
        std::uint32_t raw_x = 0;
        std::uint32_t raw_y = 0;
        if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) || offset >= bytes.size()) {
            error = LoadError::BadCellSection;
            return false;
        }
        const MaterialId material_id = bytes[offset++];
        if (material_id == kAir) {
            error = LoadError::AirCellStored;
            return false;
        }
        if (!materials_.contains(material_id)) {
            error = LoadError::UnknownMaterial;
            return false;
        }
        // C++20 two's-complement: the u32 payload round-trips exactly via
        // static_cast<std::int32_t> (same convention as save()).
        const CellPosition position{static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)};
        if (position.x > kMaxSaveCoord || position.x < -kMaxSaveCoord || position.y > kMaxSaveCoord ||
            position.y < -kMaxSaveCoord) {
            error = LoadError::CoordOutOfRange;
            return false;
        }
        if (have_prev_cell) {
            if (position == prev_cell) {
                error = LoadError::DuplicateCell;
                return false;
            }
            if (position < prev_cell) {
                error = LoadError::UnsortedCells;
                return false;
            }
        }
        // Task F contract: FindOrInsertResult find_or_insert(const Key& key,
        // Value value) with .ptr/.inserted; claim-or-observe WITHOUT
        // overwrite. !inserted -> DuplicateCell (same outcome as the
        // equality check above).
        const auto cell_claim = loaded.find_or_insert(position, material_id);
        if (!cell_claim.inserted) {
            error = LoadError::DuplicateCell;
            return false;
        }
        prev_cell = position;
        have_prev_cell = true;
    }
    FlatTemps loaded_temps;
    if (version == kSaveVersion2) {
        std::uint32_t temp_count = 0;
        if (!read_u32(bytes, offset, temp_count)) {
            error = LoadError::BadTempSection;
            return false;
        }
        if (temp_count > kMaxSaveTemps) {
            error = LoadError::Oversize;
            return false;
        }
        if (bytes.size() - offset != static_cast<std::size_t>(temp_count) * 10) {
            error = LoadError::BadTempSection;
            return false;
        }
        // Same v2-strict ordering pair as the cell section above
        // (prev-compare names it, find_or_insert belt-and-braces).
        bool have_prev_temp = false;
        CellPosition prev_temp{0, 0};
        for (std::uint32_t index = 0; index < temp_count; ++index) {
            std::uint32_t raw_x = 0;
            std::uint32_t raw_y = 0;
            std::uint16_t raw_temp = 0;
            if (!read_u32(bytes, offset, raw_x) || !read_u32(bytes, offset, raw_y) ||
                !read_u16(bytes, offset, raw_temp)) {
                error = LoadError::BadTempSection;
                return false;
            }
            const auto temp = static_cast<std::int16_t>(raw_temp);
            if (temp == 0) {
                error = LoadError::AmbientTempStored; // canonical saves never store ambient
                return false;
            }
            const CellPosition position{static_cast<std::int32_t>(raw_x), static_cast<std::int32_t>(raw_y)};
            if (position.x > kMaxSaveCoord || position.x < -kMaxSaveCoord || position.y > kMaxSaveCoord ||
                position.y < -kMaxSaveCoord) {
                error = LoadError::CoordOutOfRange;
                return false;
            }
            if (have_prev_temp) {
                if (position == prev_temp) {
                    error = LoadError::DuplicateTemp;
                    return false;
                }
                if (position < prev_temp) {
                    error = LoadError::UnsortedTemps;
                    return false;
                }
            }
            // Temps-on-air STILL ACCEPTED: the S4 set-on-air rejection
            // (temps subset of cells) waits for v3; prune stays the backstop.
            const auto temp_claim = loaded_temps.find_or_insert(position, temp);
            if (!temp_claim.inserted) {
                error = LoadError::DuplicateTemp;
                return false;
            }
            prev_temp = position;
            have_prev_temp = true;
        }
    }
    // Chunk-bomb guard: N far-apart cells x 4 KiB chunk vectors is the hole
    // the "no thin-file blowup" proof missed. Count distinct chunks BEFORE
    // building the mirror; reject over kMaxSaveChunks (TooManyChunks).
    {
        std::vector<std::uint64_t> chunk_keys;
        chunk_keys.reserve(loaded.size());
        loaded.for_each([&](const CellPosition& position, const MaterialId&) {
            chunk_keys.push_back(load_chunk_key(position));
        });
        std::sort(chunk_keys.begin(), chunk_keys.end());
        chunk_keys.erase(std::unique(chunk_keys.begin(), chunk_keys.end()), chunk_keys.end());
        if (chunk_keys.size() > kMaxSaveChunks) {
            error = LoadError::TooManyChunks;
            return false;
        }
    }
    // (Group 2, Task S5 commit-phase safety:) the chunk mirror is built into
    // a LOCAL map first, so a bad_alloc during rebuild leaves the live world
    // untouched ("failed load => untouched" holds even under bad_alloc).
    // Commit uses only non-throwing moves, tick_ LAST. The dirty marks run
    // post-commit and may throw: sim state already holds, the presentational
    // dirty set is best-effort (accepted limitation).
    std::map<CellPosition, std::vector<MaterialId>> fresh_chunks;
    loaded.for_each([&](const CellPosition& position, const MaterialId& material_id) {
        const CellPosition chunk_position{
            floor_divide(position.x, kChunkSize), floor_divide(position.y, kChunkSize)};
        auto [chunk, inserted] = fresh_chunks.try_emplace(chunk_position);
        if (inserted) chunk->second.assign(static_cast<std::size_t>(kChunkSize * kChunkSize), kAir);
        const int local_x = positive_modulo(position.x, kChunkSize);
        const int local_y = positive_modulo(position.y, kChunkSize);
        chunk->second[static_cast<std::size_t>(local_y * kChunkSize + local_x)] = material_id;
    });
    std::set<CellPosition> changed_chunks;
    for (const auto& [chunk_position, _] : chunks_) {
        changed_chunks.insert(chunk_position);
    }
    for (const auto& [chunk_position, _] : fresh_chunks) {
        changed_chunks.insert(chunk_position);
    }
    chunks_ = std::move(fresh_chunks);
    cells_ = std::move(loaded);
    temps_ = std::move(loaded_temps);
    tick_ = saved_tick;
    for (const CellPosition chunk_position : changed_chunks) {
        mark_chunk_dirty(chunk_position);
    }
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
    // Fire pit demo (P2.5): sealed stone kiln with a tall wood pile, starter
    // flames embedded in its heart, and a water cup. Spread and conducted
    // heat burn the pile, quench turns edge flames to steam, condense rains
    // lid water back down, and burnout clears the last strays during the
    // 200-tick setup settle; fuel is finite so extinction is guaranteed and
    // the stone shell keeps it deterministic. (An open tub fizzles: starter
    // flames rise away from the pile before the p=40 spread roll lands.)
    fill_rect({440, 320}, {470, 320}, kStone); // kiln floor (31)
    fill_rect({440, 310}, {440, 319}, kStone); // west wall (10)
    fill_rect({470, 310}, {470, 319}, kStone); // east wall (10)
    fill_rect({441, 310}, {469, 310}, kStone); // lid (29)
    fill_rect({446, 317}, {452, 319}, kWood);  // wood pile (21)
    fill_rect({449, 318}, {450, 318}, kFire);  // starter, embedded (2; wood left 19)
    fill_rect({458, 318}, {462, 319}, kWater); // quench cup (10)
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

void SimulationWorld::stage_heat_carry(CellPosition from, CellPosition to) {
    const std::int16_t* found = temps_.find(from);
    if (found != nullptr) heat_carry_scratch_.emplace_back(to, *found);
}

int SimulationWorld::temperature(CellPosition position) const { return temperature_at(position); }

void SimulationWorld::conduct_temperature() {
    // Ambient fast path: no stored heat anywhere, so ambient worlds pay one
    // branch per tick and the hot loop is untouched (perf note: conduction
    // is O(occupied) integer ops only while heat exists).
    if (temps_.empty()) return;
    // P2.5 carries heat with moving material via `stage_heat_carry`
    // (destination-keyed, applied post-swap); temps whose cell is gone are
    // pruned pre-conduction, or falling hot material would heat its own wake
    // through the stale snapshot entry.
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

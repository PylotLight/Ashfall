#pragma once

#include <array>
#include <bitset>
#include <cassert>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ashfall/cell_types.hpp"
#include "ashfall/flat_cell_map.hpp"
#include "ashfall/material_data.hpp"
#include "ashfall/reaction_data.hpp"

namespace ashfall {

#ifdef ASHFALL_STEP_INSTRUMENT
// Write-only step instrumentation (Group 1, Task C1). Populated at the end
// of step(), AFTER conduct_temperature(). Sim code must NEVER read these:
// timers/counters are write-only during step() and must not feed sim
// decisions (determinism rule). Storage + accessors exist ONLY under this
// macro; callers (e.g. bench/bench_step.cpp) guard reads with the same
// macro, so every TU compiles either way.
struct StepTimings {
    double reactions_ms = 0.0, clear_reserve_ms = 0.0, classify_ms = 0.0;
    double ordering_ms = 0.0, powder_ms = 0.0, liquid_ms = 0.0, gas_ms = 0.0;
    double reconcile_ms = 0.0, heat_carry_ms = 0.0, conduct_ms = 0.0, total_ms = 0.0;
};
struct StepCounters {
    std::size_t occupied = 0, static_cells = 0, dynamic_cells = 0;
    std::size_t powder_moves = 0, liquid_moves = 0, gas_moves = 0, swaps = 0;
    std::size_t reaction_cells = 0, reaction_hits = 0;
    std::size_t nonzero_temps = 0, carried_heat = 0;
    std::size_t changed_positions = 0, dirty_chunk_marks = 0;
};
#endif

/// Dense runtime mirror of the material table for hot paths (Group 4, Task
/// O1): indexed directly by MaterialId, so per-cell dispatch avoids the
/// std::map lookup. Populated in load_material_table (all 256 slots;
/// unlisted ids have valid=false). materials_ stays for names/metadata and
/// all public/cold queries. Engine invariant: stored cells always hold
/// validated ids (set/fill/load/reaction paths all gate), so the
/// material_fast() debug assert documents rather than defends.
struct MaterialRuntime {
    Movement movement = Movement::Static;
    int density = 0;
    bool solid = false;
    bool valid = false;
};
/// Portable, deterministic cellular world. Godot and rendering code must not own this state.
struct LoadOptions {
    bool allow_reaction_table_mismatch = false;
};
class SimulationWorld {
public:
    SimulationWorld();
    // Build from a data table (validated; id 0 must be air). Loading a table
    // resets the world, since live cells reference the previous ids.
    // Loading a material table also clears reactions, since rules
    // reference its ids.
    explicit SimulationWorld(const MaterialTable& table);
    bool load_material_table(const MaterialTable& table);
    // Data-defined reactions (see data/reactions.json). Names resolve
    // against the current material table; unknown names reject the load
    // with rules_ untouched. Loading a material table clears reactions,
    // since rules reference its ids.
    bool load_reactions(const ReactionTable& table);
    [[nodiscard]] std::size_t reaction_count() const noexcept { return reactions_.size(); }
    // Sparse per-cell temperature (ambient 0; only non-zero is stored).
    // Deterministic integer conduction runs each step(); queries read the
    // settled post-step state. Setting 0 erases the entry. Out-of-range
    // values clamp to int16. Heat is cell-owned (v3 gate): setting heat on
    // air is rejected (returns false, nothing stored); stored/erased
    // otherwise (returns true). P2.5 carries heat with moving material via
    // staged next-state temps (heat_next_scratch_, committed post-swap);
    // temps whose cell is gone are pruned pre-conduction (backstop — see
    // conduct_temperature).
    bool set_temperature(CellPosition position, int temperature);
    [[nodiscard]] int temperature(CellPosition position) const;
    [[nodiscard]] std::size_t temperature_count() const noexcept { return temps_.size(); }
    // Table-order material ids (insertion order from the loaded table) for
    // presentation layers (palette, shortcuts). Engine dispatch never uses
    // order, only the movement class.
    [[nodiscard]] std::vector<MaterialId> material_ids() const { return material_order_; }
    // Look up one table row by id; returns false for unknown ids.
    [[nodiscard]] bool material_info(MaterialId id, MaterialSpec& out) const;

    [[nodiscard]] MaterialId cell(CellPosition position) const;
    void reset();
    void set_cell(CellPosition position, MaterialId material);
    void fill_rect(CellPosition first, CellPosition last, MaterialId material);
    void step();

    [[nodiscard]] std::size_t occupied_cell_count() const noexcept { return total_occupied_; }
    // (Group 4, Task O4:) exact count of non-static cells (movement !=
    // Static), maintained on every mutation path; the step() fast-path hook
    // reads it. Exposed for the counter test only.
    [[nodiscard]] std::size_t non_static_count() const noexcept { return non_static_count_; }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] bool has_chunk(CellPosition chunk_position) const;
    [[nodiscard]] std::vector<CellPosition> active_chunks() const;
    bool read_chunk(CellPosition chunk_position, std::span<MaterialId> out_buffer) const;
    void copy_rect(CellPosition first, CellPosition last, std::span<MaterialId> out_buffer) const;
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] bool load(std::span<const std::uint8_t> bytes);
    // Save-format constants + load diagnostics (Group 1, Task C2, backlog
    // S2; Group 3, Task S: v3). v1 = cells only (all ambient), v2 = cells +
    // sparse temps tail, v3 = roster + reaction guard + extents + chunk-major
    // cells + filtered temps + CRC32. Readers v1..vN are retained; the writer
    // emits v3 ONLY. load() delegates to load_with_diagnostic and ignores
    // the diagnostic, so tick outcomes are unchanged by this refactor.
    static constexpr std::array<std::uint8_t, 7> kSaveMagicPrefix = {'A', 'S', 'H', 'F', 'A', 'L', 'L'};
    static constexpr std::uint8_t kSaveVersion1 = 1;
    static constexpr std::uint8_t kSaveVersion2 = 2;
    static constexpr std::uint8_t kSaveVersion3 = 3;
    enum class LoadError : std::uint8_t {
        Ok = 0,
        TooShort,
        BadMagic,
        BadVersion,
        BadCellSection,
        BadTempSection,
        UnknownMaterial,
        AirCellStored,
        AmbientTempStored,
        Oversize,
        DuplicateCell,
        DuplicateTemp,
        UnsortedCells,
        UnsortedTemps,
        TooManyChunks,
        CoordOutOfRange,
        BadCrc,
        BadRoster,
        RosterUnknownMaterial,
        ReactionTableMismatch,
        TempsOnAir,
        BadChunkOrder,
        BadExtents
    };
    [[nodiscard]] bool load_with_diagnostic(
        std::span<const std::uint8_t> bytes, LoadError& error, LoadOptions options = {});
    // CRC32 over the canonical per-rule encoding (reaction names, decimal
    // probabilities, NAME-keyed material refs, gates, product temps, joined
    // by '\0'); stable across id remaps, so the v3 reaction guard compares
    // content identity, not ids. Also useful for future dev UI.
    [[nodiscard]] std::uint32_t reaction_table_hash() const;
#ifdef ASHFALL_STEP_INSTRUMENT
    [[nodiscard]] const StepTimings& last_step_timings() const noexcept { return last_timings_; }
    // Counting rules: `reaction_cells` = cells visited in the run_reactions
    // pre-pass; `reaction_hits` = hits.size() before conflict resolution.
    // `swaps` = powder settled-occupant swaps + liquid density down-swaps +
    // infiltration swaps (gas never swaps). `changed_positions` = removals +
    // changes + additions counted in the two reconcile diff loops.
    // `dirty_chunk_marks` = mark_chunk_dirty calls at the three reconcile
    // call sites (not global). `carried_heat` = heat_next_scratch_.size() at commit time. `nonzero_temps` = temps_.size() post-step.
    [[nodiscard]] const StepCounters& last_step_counters() const noexcept { return last_counters_; }
#endif

    [[nodiscard]] bool is_solid(CellPosition position) const;
    [[nodiscard]] bool can_move_to(CellPosition min_pos, CellPosition max_pos) const;
    [[nodiscard]] std::vector<CellPosition> dirty_chunks() const;
    void clear_dirty_chunks();
    void mark_chunk_dirty(CellPosition chunk_position);

    /// Hand-authored P1 play level: floor, walls, platforms, sand dunes and a
    /// water reservoir. Resets the world first; deterministic content.
    void build_play_level();
    /// Player spawn for the play level, in cell units (feet position).
    [[nodiscard]] static CellPosition play_level_spawn() noexcept { return {256, 480}; }

private:
    // Flat open-addressing storage for the hot path: step() performs ~100k
    // inserts/lookups per tick at 512x512, where unordered_map node traffic
    // dominates. (Group 5, Task M:) the persistent authoritative cell store
    // is the chunk directory (chunks_ + chunk_occupancy_ + total_occupied_);
    // the scratch tables below are cleared and reused every tick (never
    // reallocated in steady state).
    using FlatCells = FlatMap<CellPosition, MaterialId, CellPositionHash>;
    using FlatMarks = FlatMap<CellPosition, std::uint8_t, CellPositionHash>;
    using DynCells = std::vector<std::pair<CellPosition, MaterialId>>;

    [[nodiscard]] const MaterialSpec& material(MaterialId id) const;
    [[nodiscard]] const MaterialRuntime& material_fast(MaterialId id) const {
#ifndef NDEBUG
        assert(material_fast_[id].valid);
#endif
        return material_fast_[id];
    }
    [[nodiscard]] static bool prefers_left(CellPosition position, std::uint64_t tick);
    void write_chunk_cell(CellPosition position, MaterialId material_id);
    void write_chunk_cell_deferred(
        CellPosition position, MaterialId material_id, std::set<CellPosition>* maybe_emptied);
    // (Group 5, Task M, Stage 1:) chunk-backed read/query helpers. Same
    // math as write_chunk_cell (chunk_pos_of + positive_modulo +
    // chunk_index); missing chunk or air slot reads as kAir. The chunk
    // directory is authoritative — these were differential-proven first.
    // (Stage 5:) header-inlining was tried for these (movement/reconcile
    // probe volume) and reverted: measured worse on the 1024² micro
    // (263 vs 252 ms/step) and B (2.40 vs 2.25) — loop bloat beats call
    // overhead here. Outlined definitions live in the .cpp.
    [[nodiscard]] MaterialId cell_from_chunks(CellPosition position) const;
    [[nodiscard]] bool contains_in_chunks(CellPosition position) const;
    // (Group 5, Task M, Stage 3:) chunk-walk iteration: std::map key order
    // + row-major within each dense vector. Only the VISITED SET feeds
    // consumers — classify is totally resorted by order_dynamic_cells
    // (y-desc, x-asc), reactions are totally conflict-sorted by (pos, own,
    // rule, primary), and extents/conduction accumulate order-independently
    // — so walk order never feeds a simulation decision.
    template <typename Fn>
    void for_each_chunk_cell(Fn&& fn) const {
        for (const auto& [chunk_position, storage] : chunks_) {
            for (std::size_t index = 0; index < storage.size(); ++index) {
                const MaterialId material_id = storage[index];
                if (material_id == kAir) continue;
                fn(CellPosition{chunk_position.x * kChunkSize +
                                    static_cast<int>(index % static_cast<std::size_t>(kChunkSize)),
                                chunk_position.y * kChunkSize +
                                    static_cast<int>(index / static_cast<std::size_t>(kChunkSize))},
                   material_id);
            }
        }
    }
    [[nodiscard]] static std::size_t chunk_index(int local_x, int local_y) noexcept {
        return static_cast<std::size_t>(local_y * kChunkSize + local_x);
    }
    // (Group 4, Task O6:) touch helper for changed-position
    // reconciliation (perf §9.2): successful movement registers its
    // endpoints (vacated source + written destination; both partners of a
    // swap) into touch_scratch_. Duplicates allowed; reconcile dedups via
    // sort+unique (deterministic). Place-backs (static or dynamic) rewrite
    // the identical id, so before==after by construction and they are
    // deliberately NOT touched — dynamics can never overwrite a
    // static-occupied next slot (density / stayed / emptiness gates), and
    // stays rewrite their own id. The NDEBUG full-reconstruction comparator
    // in step() proves this coverage every debug tick.
    void touch_next(CellPosition position) { touch_scratch_.push_back(position); }

    // (Group 5, Task M:) chunks_ is the authoritative occupied set,
    // persistent across ticks: step() classifies from the chunk walk and
    // reconciles movement diffs back into it, so there is no per-tick
    // collect/rebuild pass and no global material map. The same dense
    // vectors serve chunk-granular presentation and read_chunk queries.
    FlatCells next_scratch_;
    FlatMarks stayed_scratch_;
    DynCells dyn_scratch_;
    DynCells order_scratch_;
    // (Group 4, Task O2:) retained counting-sort rows for
    // order_dynamic_cells (cleared + grown-only per call via assign() —
    // steady-state zero alloc). Passed by ref; the function stays a free
    // function in the anonymous namespace.
    std::vector<std::size_t> order_row_size_;
    std::vector<std::size_t> order_row_start_;
    std::vector<std::size_t> order_cursor_;
    std::map<MaterialId, MaterialSpec> materials_;
    std::array<MaterialRuntime, 256> material_fast_;
    // (Group 4, Task O4:) exact static fast-path counter: cells whose
    // material_fast_ movement != Static. Maintained on EVERY mutation path
    // (set_cell delta, fill_rect accumulate, load recount, reset → 0;
    // reaction applies route via set_cell). Step movement never changes the
    // material multiset (relocations + swaps only), so the hook reads it
    // without rescanning.
    std::size_t non_static_count_ = 0;
    std::vector<MaterialId> material_order_;
    [[nodiscard]] int temperature_at(CellPosition position) const;
    void conduct_temperature();
    // Heat advection, cell-owned (Group 2, Task H, 2026-09-10).
    // OLD behavior: stage_heat_carry read temps_[from] (tick-start, coordinate
    // key) into a vector of (dest, temp) pairs applied post-swap, and old
    // source entries were never cleared. A cold cell moving into the old
    // location of a hot cell inherited the stale coordinate temp (duplication:
    // heat stuck to the old coordinate AND followed the material), and an
    // infiltration-displaced in-flight arrival staged the old coordinate's
    // temp instead of the arrival's. Pinned by BEHAVIOR(v2-heat) tests (Task T).
    // NEW behavior: movement stages every cell it writes into heat_next_scratch_
    // (next-state temps shadowing temps_); a written cell with no staged entry
    // implicitly reads ambient 0, which is CORRECT by construction (nothing
    // stale survives — the bug class is eliminated structurally, not by
    // clearing). heat_of reads staged-next FIRST (the §3.2 infiltration fix:
    // an in-flight arrival's staged temp wins over the old coordinate's temp).
    // stage_heat_carry moves one payload; stage_heat_swap moves two atomically
    // (both heats are read before either is written — sequential stages would
    // read back the just-staged partner). step() commits heat_next_scratch_
    // into temps_ post-swap; conduct_temperature() is unchanged.
    [[nodiscard]] int heat_of(CellPosition position) const;
    void stage_heat_carry(CellPosition from, CellPosition to);
    void stage_heat_swap(CellPosition first, CellPosition second);

    using FlatTemps = FlatMap<CellPosition, std::int16_t, CellPositionHash>;
    FlatTemps temps_;
    FlatTemps temp_scratch_;
    // Next-state temperatures staged by movement, committed into temps_ after
    // the old global-map swap. Cleared + reserve_for(temps_.size() + 16) at step top
    // (F3 spirit: sized from the working set). Sparse invariant: only nonzero
    // is stored, mirroring temps_.
    FlatTemps heat_next_scratch_;
    std::vector<CellPosition> temp_prune_scratch_;
    // (Group 4, Task O5:) retained candidate list for the sparse conduction
    // pass (nonzero-temp positions + 4-neighbors, sorted + deduped per tick;
    // cleared + grown-only — steady-state zero alloc).
    std::vector<CellPosition> cond_cand_scratch_;
    struct ReactionSlot {
        bool is_tag = false;
        std::string tag;
        MaterialId id = kAir;
    };
    struct ResolvedReaction {
        std::string name;
        int probability = 0;
        ReactionSlot reactants[2];
        int reactant_count = 0;
        struct Product {
            MaterialId id = kAir;
        };
        Product products[2];
        int product_count = 0; // <= reactant_count; missing tail = identity
        bool has_min = false;
        int min_temp = 0;
        bool has_max = false;
        int max_temp = 0;
        bool has_product_temp = false;
        int product_temp = 0;
    };
    std::vector<ResolvedReaction> reactions_;
    // (Group 4, Task O3:) compiled reaction matching. Per-rule slot bitsets
    // (tag slots: all matching ids resolved at load; id slots: single bit;
    // an air second reactant is special-cased via second_is_air, NOT in the
    // bitset — the empty-slot check stays as today) plus per-primary
    // candidate lists holding rule indices in ASCENDING original-table order
    // (ownership semantics preserved structurally). Rebuilt in
    // load_reactions; cleared with reactions_. hits_ is the run_reactions
    // scratch (capacity retained across ticks); last_hits_ sizes its reserve
    // (max(64, last_hits_ * 2) per tick — measured candidates, not cells).
    struct CompiledRule {
        std::array<std::bitset<256>, 2> reactant_bits;
        bool second_is_air = false;
    };
    struct Hit {
        CellPosition pos;
        MaterialId product;
        std::uint32_t rule;
        bool own; // primary-cell transform beats neighbor-imposed claims
        CellPosition primary;
    };
    std::vector<CompiledRule> compiled_rules_;
    std::array<std::vector<std::uint32_t>, 256> rules_by_primary_;
    std::vector<Hit> hits_;
    std::size_t last_hits_ = 0;

    [[nodiscard]] bool material_id_by_name(const std::string& name, MaterialId& out) const;
    [[nodiscard]] bool matches_reactant(MaterialId id, const ReactionSlot& slot) const;
    [[nodiscard]] static std::uint32_t reaction_roll(
        CellPosition position, std::uint64_t tick, std::uint32_t rule);
    void run_reactions();
    // (Group 5, Task M, Stage 5 verdict:) the directory STAYS std::map
    // (owner decision). The swap conditional triggered on the 1024² micro
    // (+19.6% vs pre-migration), so a sorted-vector directory was
    // implemented, measured, and reverted: map ≡ vector on the micro and
    // A/C/D, vector −4-6% on B. Profiles show the delta is per-read
    // coordinate math + probe structure (not tree traversal), so no
    // directory type recovers it — a movement chunk-locality redesign
    // would, and that is future work, not this task.
    std::map<CellPosition, std::vector<MaterialId>> chunks_;
    // (Group 4, Task O6:) per-chunk occupancy (non-air slot count).
    // Invariant: an entry exists iff the chunk exists, and the count equals
    // the chunk's non-air slots. Maintained on EVERY chunk-mirror write path
    // (write_chunk_cell_deferred both variants, load-commit rebuild,
    // reset-clear); emptiness queries read it (O(1)) instead of scanning the
    // dense vector. fill_rect(air) inherits this (no rescans).
    std::map<CellPosition, std::size_t> chunk_occupancy_;
    // (Group 5, Task M, Stage 2:) exact global occupied count, maintained
    // alongside chunk_occupancy_ on every chunk-mirror write path (plus
    // load-commit rebuilds and reset-clear). It backs occupied_cell_count();
    // O(1), exact. Agreement with the old global map was proven by the
    // /tmp Stage-2 scratch after every step.
    std::size_t total_occupied_ = 0;
    // (Group 4, Task O6:) vector-backed dirty set with IDENTICAL observable
    // semantics to the old std::set: mark=push_back (duplicates allowed),
    // read=sorted-unique copy, clear=empty. dirty_chunks() still returns a
    // sorted, unique vector.
    std::vector<CellPosition> dirty_chunks_;
    // (Group 4, Task O6:) changed-position scratch: cleared + grown-only per
    // step (steady-state zero alloc), deduped in reconcile.
    std::vector<CellPosition> touch_scratch_;
    std::uint64_t tick_ = 0;
#ifdef ASHFALL_STEP_INSTRUMENT
    // Write-only instrumentation state (see determinism rule above): step()
    // stores here at the end; sim code must never read these members.
    StepTimings last_timings_;
    StepCounters last_counters_;
#endif
};

} // namespace ashfall

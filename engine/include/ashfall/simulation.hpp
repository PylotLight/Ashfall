#pragma once

#include <array>
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

/// Portable, deterministic cellular world. Godot and rendering code must not own this state.
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
    // values clamp to int16. Heat may be set on any position (even air);
    // P2.5 carries heat with moving material via `stage_heat_carry`
    // (destination-keyed, applied post-swap); temps whose cell is gone are
    // pruned pre-conduction.
    void set_temperature(CellPosition position, int temperature);
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

    [[nodiscard]] std::size_t occupied_cell_count() const noexcept { return cells_.size(); }
    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }
    [[nodiscard]] bool has_chunk(CellPosition chunk_position) const;
    [[nodiscard]] std::vector<CellPosition> active_chunks() const;
    bool read_chunk(CellPosition chunk_position, std::span<MaterialId> out_buffer) const;
    void copy_rect(CellPosition first, CellPosition last, std::span<MaterialId> out_buffer) const;
    [[nodiscard]] std::vector<std::uint8_t> save() const;
    [[nodiscard]] bool load(std::span<const std::uint8_t> bytes);
    // Save-format constants + load diagnostics (Group 1, Task C2, backlog
    // S2). v1 = cells only (all ambient), v2 = cells + sparse temps tail.
    // load() delegates to load_with_diagnostic and ignores the diagnostic,
    // so tick outcomes and save bytes are unchanged by this refactor.
    static constexpr std::array<std::uint8_t, 7> kSaveMagicPrefix = {'A', 'S', 'H', 'F', 'A', 'L', 'L'};
    static constexpr std::uint8_t kSaveVersion1 = 1;
    static constexpr std::uint8_t kSaveVersion2 = 2;
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
        CoordOutOfRange
    };
    [[nodiscard]] bool load_with_diagnostic(std::span<const std::uint8_t> bytes, LoadError& error);
#ifdef ASHFALL_STEP_INSTRUMENT
    [[nodiscard]] const StepTimings& last_step_timings() const noexcept { return last_timings_; }
    // Counting rules: `reaction_cells` = cells visited in the run_reactions
    // pre-pass; `reaction_hits` = hits.size() before conflict resolution.
    // `swaps` = powder settled-occupant swaps + liquid density down-swaps +
    // infiltration swaps (gas never swaps). `changed_positions` = removals +
    // changes + additions counted in the two reconcile diff loops.
    // `dirty_chunk_marks` = mark_chunk_dirty calls at the three reconcile
    // call sites (not global). `carried_heat` = heat_carry_scratch_.size()
    // at apply time. `nonzero_temps` = temps_.size() post-step.
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
    // dominates. cells_ is authoritative and persistent; the scratch tables
    // are cleared and reused every tick (never reallocated in steady state).
    using FlatCells = FlatMap<CellPosition, MaterialId, CellPositionHash>;
    using FlatMarks = FlatMap<CellPosition, std::uint8_t, CellPositionHash>;
    using DynCells = std::vector<std::pair<CellPosition, MaterialId>>;

    [[nodiscard]] const MaterialSpec& material(MaterialId id) const;
    [[nodiscard]] static bool prefers_left(CellPosition position, std::uint64_t tick);
    void rebuild_chunks(const FlatCells& cells);
    void write_chunk_cell(CellPosition position, MaterialId material_id);
    void write_chunk_cell_deferred(
        CellPosition position, MaterialId material_id, std::set<CellPosition>* maybe_emptied);
    [[nodiscard]] static std::size_t chunk_index(int local_x, int local_y) noexcept {
        return static_cast<std::size_t>(local_y * kChunkSize + local_x);
    }
    [[nodiscard]] static bool chunk_storage_empty(const std::vector<MaterialId>& chunk) noexcept;

    // cells_ is the authoritative occupied set, persistent across ticks:
    // step() reads it in place and swaps in the next state, so there is no
    // per-tick collect/rebuild pass. chunks_ mirrors it for chunk-granular
    // presentation and read_chunk queries.
    FlatCells cells_;
    FlatCells next_scratch_;
    FlatMarks stayed_scratch_;
    DynCells dyn_scratch_;
    DynCells order_scratch_;
    std::map<MaterialId, MaterialSpec> materials_;
    std::vector<MaterialId> material_order_;
    [[nodiscard]] int temperature_at(CellPosition position) const;
    void conduct_temperature();
    // Heat advection: movement stages each mover's tick-start temperature
    // keyed by destination; step() applies the stages post-swap (before
    // conduction), so heat rides the material instead of pruning at the
    // vacated cell. Destinations are unique per tick (mass conservation),
    // so last-write conflicts cannot occur.
    void stage_heat_carry(CellPosition from, CellPosition to);

    std::vector<std::pair<CellPosition, std::int16_t>> heat_carry_scratch_;

    using FlatTemps = FlatMap<CellPosition, std::int16_t, CellPositionHash>;
    FlatTemps temps_;
    FlatTemps temp_scratch_;
    std::vector<CellPosition> temp_prune_scratch_;
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

    [[nodiscard]] bool material_id_by_name(const std::string& name, MaterialId& out) const;
    [[nodiscard]] bool matches_reactant(MaterialId id, const ReactionSlot& slot) const;
    [[nodiscard]] static std::uint32_t reaction_roll(
        CellPosition position, std::uint64_t tick, std::uint32_t rule);
    void run_reactions();
    std::map<CellPosition, std::vector<MaterialId>> chunks_;
    std::set<CellPosition> dirty_chunks_;
    std::uint64_t tick_ = 0;
#ifdef ASHFALL_STEP_INSTRUMENT
    // Write-only instrumentation state (see determinism rule above): step()
    // stores here at the end; sim code must never read these members.
    StepTimings last_timings_;
    StepCounters last_counters_;
#endif
};

} // namespace ashfall

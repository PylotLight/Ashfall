# Perf & Quality Review Packet — sim hot paths

Attach these three files alongside this brief: `engine/src/simulation.cpp` (1047 lines),
`engine/include/ashfall/simulation.hpp` (161 lines), `bridge/src/ashfall_world.cpp` (523 lines).
Build: CMake Release `-O3`, Apple Silicon, C++20. Benchmarks below from a throwaway
`/tmp/bench_step.cpp` (not in repo) timing 200 `step()` calls, carousel: mean ms/step.

## Measured step() costs (Release, 512×512 world)

| Scenario | ms/step | occupied | notes |
|---|---|---|---|
| A. Play level, settled, reactionless | 4.04 | 51,857 | mostly static + pooled gas; kiln unburned (no rules loaded) |
| B. 12.5k water resolving in a basin | 1.55 | 12,580 | liquid-heavy, cold |
| C. 300×10 wood+fire with 2 rules | 0.30 | 5,610 | reactions + gas + heat |
| D. Static 300×300 stone box (control) | 3.37 | 90,601 | NOTHING moves, yet costs ~37 ns/occupied-cell/tick |

Takeaway: cost scales with occupied cells, not active cells. There is no sleeping; every
tick reprocesses the world. Production HUD shows ~6.8 ms/step with rules + presentation.

## Segments to review (hardest first)

### 1. `step()` movement dispatch — simulation.cpp:405–688
Single pass over all non-static cells in bottom-up order (`order_dynamic_cells`, :65–105:
counting sort by row + per-row `std::sort`; three local `std::vector<std::size_t>`
allocated every tick). Powder/liquid/gas branches each do 3–8 `FlatMap` find/inserts per
cell plus `material(id)` lookups (see §3). Liquid lateral dispersion scans up to 4 cells
with an `is_solid()` call per step (:556–571); gas rises max 1 cell/tick and waits when
blocked (:615–687). Quality questions live here too: pool leveling, U-tube equalization,
scan-direction bias, gas streaking.

### 2. Chunk-mirror diff — simulation.cpp:690–715
Two full `for_each` passes (`cells_` + `next`) plus `std::set`/`std::map` ops per touched
chunk. `chunks_` is `std::map<CellPosition, std::vector<MaterialId>>` of 4096-entry
vectors; `dirty_chunks_` is a `std::set` (tree insert per mark). Is the mirror pulling
its weight vs. deriving presentation ranges from the dirty set?

### 3. `material(id)` → `materials_.at(id)` — simulation.cpp:956
A `std::map` tree lookup, called ~3–6× per dynamic cell per tick (:406, :415, :427–431,
:505–508, :543, :545–548, :587–588, :594–596) and inside `is_solid()` (:885–889, itself
called in every corner-cut guard). ids are `uint8` (≤256, currently 10). Dense array?
Same question for `matches_reactant` tag scans (:141) and `temperature_at` FlatMap finds
in the reaction loop (:319–320, per cell × per rule).

### 4. `run_reactions()` — simulation.cpp:298–383
O(cells × rules): temp lookup + `reaction_roll` hash per cell-rule, `prefers_left` hash
per dynamic cell, 4-neighbor probing with map finds, then `std::sort` over all hits and
per-hit `set_cell()` — which does chunk-mirror writes + dirty-set inserts each. Note
`reaction_roll` mixes the rule's table *index* into the hash (:159), so inserting a rule
reshuffles unrelated rules' outcomes (deterministic per-dataset, fragile across edits).

### 5. `conduct_temperature()` — simulation.cpp:986–1017
When ANY heat exists anywhere, iterates ALL occupied cells × 5 `temperature_at` lookups
each, and `reserve_for(cells_.size()+16)` oversizes the scratch. Alternative: iterate the
hot set + its neighborhood instead. (Feedback suggests auditing whether sparse-map
lookup cost beats a dense 512 KiB `int16` array + active mask.)

### 6. Bridge repaint + upload — ashfall_world.cpp:348–523
Dirty ≤8 chunks → partial repaint; otherwise FULL 512² `copy_rect` + per-pixel paint
(liquids scan depth below, up to 5) + full `ImageTexture.update()` upload (~2 MiB/frame
for both layers ≈ 120 MiB/s at 60 Hz). Any moderately active scene takes the full path.
Questions: real upload granularity of `ImageTexture.update()`, per-chunk textures vs.
atlas, and (per feedback) never baking per-frame flicker into the CPU snapshot.

## Constraints any change must preserve

- Bitwise determinism: same data + inputs → identical state (tests assert exact counts,
  byte-identical saves, twin-world equality). Ordering, hashes, and conflict resolution
  are load-bearing; parallelization needs a deterministic schedule.
- Data-driven content: new instances of existing behavior stay data-only (verified by
  empty-diff gate on `simulation.cpp`/`bridge/` at each content commit).
- No Godot types in `engine/`; bridge translates only.

## Questions for the reviewer

1. Rank §1–§6 by expected ms saved in scenarios A/B/D, with the smallest safe first step
   for each. What would you profile first to confirm?
2. Which sleeping/active-region design fits this codebase (owned-cell swap semantics,
   settled-vs-in-flight distinction) without breaking mass conservation or determinism?
3. Is `std::map` for materials/chunks/dirty-sets actually measurable here, or noise next
   to the FlatMap traffic — and what replaces each with minimal churn?
4. Any quality red flags in the liquid/gas movement rules (§1) that would read as wrong
   physics before we invest in prettier rendering of them?

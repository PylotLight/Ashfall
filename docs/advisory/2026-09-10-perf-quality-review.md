# External Perf & Quality Review — raw feedback (2026-09-10)

Source: advisory model review of `docs/PERF_REVIEW_PACKET.md` + `engine/src/simulation.cpp`
+ partial `engine/include/ashfall/simulation.hpp` + partial `bridge/src/ashfall_world.cpp`.
Reproduced verbatim for the local record. Actionable items extracted into
`docs/advisory/PERF_TASK_BACKLOG.md`.

---

Yes—and this is enough code to make the review much more concrete.

**My main conclusion: your largest immediate problem is not expensive physics. It is rebuilding and reconciling sparse storage every tick, including for terrain that cannot move.** There are also a few correctness issues I would address before optimizing the movement algorithm.

One limitation: the pasted `simulation.hpp` stops early, and `ashfall_world.cpp` contains only the beginning of `_bind_methods()`. I can review the simulation implementation directly, but the bridge’s repaint/upload behavior is currently supported only by your written description. I also need `flat_cell_map.hpp` to assess the dominant container costs properly.

# 1. Executive findings

## What I would do first

1. **Add regression tests for temperature transport and reaction conflicts.**
2. **Instrument the phases inside `step()` and commit the benchmark harness.**
3. **Replace hot material-tree lookups with a 256-entry runtime table.**
4. **Fix scratch-buffer reuse and compile reaction matching at load time.**
5. **Add an exact static-world movement fast path.**
6. **Restrict conduction to the exact thermal candidate region.**
7. **Replace whole-world reconciliation with changed-position reconciliation.**
8. **Then design sleeping and, likely, authoritative dense-per-chunk storage.**

I would not start with multithreading, GPU simulation, or new movement rules.

## Most important architectural observation

Your earlier brief described a 512×512 grid. The implementation is actually:

```text
Sparse position → material map             authoritative
Sparse position → temperature map          authoritative
Sparse next-state map                      rebuilt each step
Sparse position → stayed marker map        rebuilt each step
Dense chunk material arrays                maintained as a mirror
```

The 512×512 area appears to be a scenario/rendering extent, **not a bounded dense simulation grid**.

That distinction matters. You are paying sparse per-cell lookup costs while also maintaining much of the dense chunk representation you could eventually use as authoritative storage.

**I would keep the portable C++ architecture, but seriously consider changing the authoritative storage within it.** That is a focused internal refactor, not an engine restart.

---

# 2. What the benchmark establishes—and what it does not

Scenario D is the strongest diagnostic:

> Approximately 90,600 static occupied cells cost 3.37 ms despite no movement.

From the code, the movement portion of that scenario still does approximately:

```text
Clear next storage
Clear stayed storage
Visit every occupied cell
    Look up material definition
    Insert static cell into next

Visit every old cell
    Look up its equivalent in next

Visit every next cell
    Look up its equivalent in current

Swap maps
Increment tick
```

That is substantial work with no physical outcome.

However, I would slightly qualify the packet’s takeaway:

> “Cost scales with occupied cells, not active cells.”

The code unquestionably contains occupancy-proportional work. But the four totals do not establish one universal scaling law: scenario A has fewer occupied cells than D and costs more, because dynamics and other work also matter.

The more precise conclusion is:

> **Every tick has a substantial occupancy-dependent baseline, with additional costs for dynamics, reactions, and heat.**

Also:

- Scenario A is intentionally reactionless. Its unburned kiln is not an unexplained discrepancy.
- Scenario C’s mean can hide a reaction workload that changes substantially over 200 ticks.
- The 6.8 ms production figure is not directly comparable until we see the timer boundaries in the bridge.
- We cannot assign honest millisecond savings to individual segments from scenario totals alone.

I can rank hypotheses. I cannot honestly claim “this map replacement saves 1.2 ms” without an A/B measurement.

---

# 3. Correctness issue first: heat transport can duplicate heat

This is the strongest concrete bug in the supplied implementation.

## 3.1 The problem

Your movement code records:

```cpp
void SimulationWorld::stage_heat_carry(CellPosition from, CellPosition to) {
    const std::int16_t* found = temps_.find(from);
    if (found != nullptr) heat_carry_scratch_.emplace_back(to, *found);
}
```

Then after movement:

```cpp
for (const auto& [dest, temp] : heat_carry_scratch_)
    temps_.insert_or_assign(dest, temp);
```

Old source entries are not cleared explicitly. Conduction later removes a temperature only when its position is empty.

That fails when **a cold cell moves into the old location of a hot cell**.

### Minimal example

Start with:

```text
y=0: cold sand
y=1: hot sand
y=2: air
```

With bottom-up movement:

1. Hot sand moves from `y=1` to `y=2`; heat is staged for `y=2`.
2. Cold sand moves from `y=0` to `y=1`; nothing is staged because its temperature is zero.
3. The old temperature at `y=1` remains.
4. Pruning does not remove it because `y=1` is occupied again.

You now have hot entries at both `y=1` and `y=2` before conduction.

**Temperature has stuck to an old coordinate while also following the moving material.**

The same problem occurs in swaps when one participant is ambient. Absence from a sparse map is a meaningful zero value, but your carry mechanism does not transport that zero.

## 3.2 In-flight displacement makes it harder

The infiltration branch explicitly allows swapping with an in-flight powder arrival:

```cpp
const MaterialId below_id = below_now != nullptr ? *below_now : kAir;
...
stage_heat_carry(down, source);
```

But `temperature_at(down)` describes the old coordinate’s temperature, not necessarily the temperature of the particle that just arrived there.

So even adding “clear old source temperatures” is not a complete solution.

## 3.3 Recommended correction

**Material ownership and auxiliary-state ownership need to move together.**

A useful logical abstraction is:

```cpp
struct CellPayload {
    MaterialId material;
    std::int16_t temperature;
};
```

That does not require an array-of-structs storage layout. It means the movement operations must behave as though they move this payload:

```text
Move source payload into destination.
Swap source payload with the actual destination payload.
Carry ambient zero just as deliberately as nonzero heat.
```

For your current architecture, I would introduce a next-state temperature representation alongside `next`, with helper operations for:

- Placing an original cell.
- Moving an original cell.
- Swapping with a settled next-state occupant.
- Relocating an in-flight next-state occupant.

The last case must obtain temperature from **the next-state occupant**, not from the original coordinate.

### Required tests

- Hot cell moves into air.
- Cold cell follows a hot cell.
- Hot powder swaps with cold liquid.
- Cold powder swaps with hot liquid.
- A heated in-flight powder is displaced by infiltration.
- The same cases continue identically across save/load.

These tests need to account for the subsequent conduction step, or expose a test-only movement-stage fixture.

**I would fix this before changing thermal storage or adding melting. Otherwise you will optimize and expand an incorrect transport model.**

---

# 4. Reaction conflict resolution is deterministic, but not transactional

Your reaction pre-pass has a good property: candidates are evaluated against one snapshot and applied in a deterministic order.

However, you stage **individual output-cell claims**, not atomic reaction instances.

```cpp
hits.push_back({position, product0, ...});
hits.push_back({neighbor, product1, ...});
```

Then the first winning hit at each coordinate is applied independently.

## 4.1 Consequence

Suppose two primaries both react with the same neighboring reactant:

```text
A + B → C + D
```

Both A cells can become C, while only one output claim wins at B.

Another case:

- Reaction X wins its primary-cell product.
- Its neighbor product loses to that neighbor’s own reaction.
- Only half of X is applied.

This is deterministic. It is not necessarily the intended chemistry.

For spreading fire, independent per-cell transformations might be acceptable. For future rules involving consumption, neutralization, resource production, or conservation, they can be wrong.

## 4.2 Decide which semantics you want

### Option A: transformations with conflict-resolved claims

Keep the implementation, but document it accurately.

Rules describe competing transformations, not atomic consumption events.

### Option B: transactional reactions

Stage an entire reaction instance:

```cpp
struct ReactionCandidate {
    // Primary position, rule index, participating cells,
    // read/consume/write claims, resulting products.
};
```

Then:

1. Sort candidates deterministically.
2. Check whether the required claims can be accepted together.
3. Accept all outputs or reject the reaction instance.
4. Apply accepted results.

You also need to distinguish:

- Consumed reactants.
- Catalysts that may be shared.
- Exclusive output destinations.

This is a behavior change, not a performance optimization. Keep it in a separate commit and version your expectations appropriately.

## 4.3 Rule ownership on failed probability is load-bearing

Your comments correctly explain:

> A fully matching higher-priority rule owns the primary even if its probability roll fails.

That means a 1% rule can suppress a lower-priority 100% rule whenever the first rule fully matches.

That may be intentional. Preserve it during optimization.

Specifically, compiled candidate lists must retain original table priority, and a failed roll cannot automatically mean “try the next rule.”

---

# 5. Ranking the hot paths

Here is my provisional ranking, based on the actual code rather than hypothetical timings.

| Scenario | Highest-priority hypotheses |
|---|---|
| **D: static stone** | Full next-state reconstruction; full diff lookups; material lookup; `FlatMap` clearing/capacity behavior |
| **A: reactionless play level** | Those same baseline costs, plus dynamic ordering and movement lookup traffic |
| **B: resolving water** | Movement lookup traffic; next-state construction; diff; material lookup; ordering |
| **Production with rules** | Above, plus reaction scanning and conduction whenever heat remains |
| **Rendering** | Separate benchmark; cannot explain a pure `SimulationWorld::step()` cost |

## Ranking by best engineering return

### 1. Dense material lookup

Small change, broad use, low semantic risk.

### 2. Scratch reuse and reaction compilation

Small-to-medium changes, preserve behavior, reduce avoidable overhead.

### 3. Static-world fast path

Very large opportunity for D, narrower applicability elsewhere.

### 4. Exact sparse conduction candidates

Potentially large when a small heated region exists in a large world.

### 5. Changed-position diff

Eliminates reconciliation work proportional to unchanged occupancy.

### 6. Persistent storage plus activity scheduling

Largest structural opportunity, but also the highest proof burden.

The order is not a claim that material lookups dominate. It is a **risk-adjusted implementation order**.

---

# 6. Material `std::map`: replace it in hot paths

With 8-bit material IDs, tree lookup is unnecessary for simulation dispatch.

A map with ten entries is not catastrophically expensive in isolation. Repeating it across millions of operations is still avoidable.

## 6.1 Minimal-churn solution

Keep the existing map for names, metadata, and public information queries. Add a compiled runtime array:

```cpp
struct MaterialRuntime {
    Movement movement;
    int density;
    bool solid;
    bool valid;
};

std::array<MaterialRuntime, 256> runtime_materials_;
```

Populate it when loading the material table.

Then hot code becomes:

```cpp
const auto& def = runtime_materials_[material_id];
```

And:

```cpp
bool SimulationWorld::is_solid(CellPosition p) const {
    return runtime_materials_[cell(p)].solid;
}
```

Ensure air has the correct runtime properties.

This avoids copying strings and tag vectors into the hot representation and does not require changing your public material API.

## 6.2 Compile tag matching

For each reaction slot, compile a membership table over material IDs:

```cpp
std::bitset<256> matches;
```

Or use another compact fixed lookup representation.

Then:

```cpp
matches_reactant(id, slot)
```

becomes a bit test rather than:

- Tree lookup.
- Tag-vector iteration.
- String comparison.

## 6.3 Compile candidate rules per primary material

Build:

```cpp
std::array<std::vector<std::uint32_t>, 256> primary_rules_;
```

For each material, store only the rules whose first reactant matches.

**Keep the original rule indices, in their original order.**

That preserves:

- Rule priority.
- Existing `reaction_roll()` inputs.
- Ownership behavior.
- Conflict sort keys.

For common inert stone, this can reduce reaction processing to an empty candidate-list check.

## 6.4 Skip unnecessary thermal and hash work

Currently:

```cpp
const int heat = temperature_at(position);
```

runs for every matching first reactant even when the rule has no thermal gates.

Only query temperature when necessary.

Similarly, the roll can be evaluated after finding a full pair match. It is a pure function, so delaying it does not consume RNG state or change subsequent decisions.

Preserve the rule’s ownership decision even when the roll fails.

---

# 7. There is a real scratch-buffer reuse problem

At the end of `order_dynamic_cells()`:

```cpp
cells = std::move(scratch);
```

Here `cells` is `dyn_scratch_`, and `scratch` is `order_scratch_`.

With the usual allocator behavior:

- `dyn_scratch_` takes `order_scratch_`’s allocation.
- Its previous allocation is released.
- `order_scratch_` loses the allocation you wanted to reuse.

That undermines the purpose of two persistent scratch vectors.

## Change it to

```cpp
cells.swap(scratch);
```

Now:

- `cells` contains the newly ordered data.
- `scratch` holds the old buffer.
- Both allocations can be reused on later ticks.

This preserves ordering.

Also move the three row-work arrays into reusable storage:

```text
row_size
row_start
cursor
```

Use `resize()`/`assign()` while retaining capacity.

The local reaction `hits` vector is another allocation candidate:

```cpp
std::vector<Hit> hits;
hits.reserve(cells_.size());
```

A member scratch vector can retain capacity. But do not automatically reserve for the entire occupied world forever; measure candidate counts and memory use.

**These are good early commits because they are easy to isolate and validate.**

---

# 8. Static-world fast path: the cleanest way to attack scenario D

You do not need a full sleeping implementation to stop reconstructing a world containing no movable material.

Maintain an exact count of non-static cells:

```cpp
std::size_t dynamic_cell_count_;
```

Update it through every authoritative material mutation:

- `set_cell()`.
- `fill_rect()`.
- Reactions.
- Load/reset.
- Any future internal mutation path.

After `run_reactions()`:

```cpp
if (dynamic_cell_count_ == 0) {
    ++tick_;
    conduct_temperature();
    return;
}
```

This is conceptual pseudocode; account for your scratch invariants and the heat-transport correction.

Important details:

- Reactions still run first because they may create dynamic material.
- Temperature still evolves.
- Tick still advances.
- Dirty chunks from external edits or reactions remain intact.
- Do not incorrectly clear pending presentation changes.

For the reactionless, cold, all-static control, this should remove nearly all of the current per-cell tick work.

**That is an algorithmic improvement, not a speculative micro-optimization.**

However, it will not solve scenario A if even one dynamic cell remains. It is a useful exact special case, not the final activity system.

---

# 9. Chunk mirror diff: distinguish full-map costs from tree costs

Your packet groups several costs together here. They deserve separation.

## 9.1 In the static control, chunk trees are not the main diff cost

For D, if nothing changes:

```cpp
if (updated == nullptr) { ... }
else if (*updated != mat) { ... }
```

never writes a chunk.

The second loop also finds every position in `cells_`.

So the expensive work is primarily:

- Two full map traversals.
- A lookup in the other map for every occupied cell.

**Dirty-set inserts and chunk-map writes are essentially absent in this case.**

Replacing `dirty_chunks_` with a bitset will not solve D.

## 9.2 First structural improvement: collect changed-position candidates

During movement, collect the positions that might differ:

```text
Source of a successful move
Destination of a successful move
Both positions of a swap
```

Deduplicate them, then compare:

```text
old material at p
new material at p
```

Update the mirror only when those differ.

This retains an important property of your current diff:

> Two identical materials exchanging ownership do not necessarily create a visual material change.

Do not simply mark every movement destination dirty forever. Compare final material identity at candidate positions.

This can change reconciliation cost from:

```text
O(all occupied cells)
```

to something closer to:

```text
O(successful movement endpoints)
```

It does **not** remove the cost of constructing `next`. That comes later.

### Proof requirement

All movement write paths must go through helpers that register their affected positions.

Otherwise one missed branch produces a stale chunk mirror.

In debug builds, compare the optimized mirror against a full reconstruction periodically or every tick in tests.

## 9.3 Replace emptiness scans with occupancy counts

Each dense chunk can maintain:

```cpp
std::uint32_t occupied_count;
```

Update it only on transitions:

```text
Air → occupied: increment
Occupied → air: decrement
Occupied → occupied: unchanged
```

Then empty-chunk detection is O(1), rather than scanning 4096 entries.

This also improves `fill_rect(..., kAir)`, which currently can call the nondeferred emptiness scan repeatedly.

## 9.4 Dirty-set replacement

For an unbounded chunk world, use:

- A dirty flag in each chunk record.
- A vector of dirty chunk coordinates.
- A retained tombstone/coordinate entry for deleted chunks needing repaint.

Sort the exported list if callers depend on canonical order.

Do not replace the current structure with a fixed 8×8 bitset unless you deliberately make the simulation bounded. The code currently supports negative and out-of-view coordinates.

---

# 10. Conduction can use an exact sparse candidate set

For the current conduction formula:

```cpp
next_temperature[p] =
    (self + left + right + up + down) / 5;
```

an occupied cell can only receive a nonzero result if it is:

- Currently nonzero, or
- Adjacent to a currently nonzero temperature.

Therefore, after your existing pruning step, the complete candidate set is:

```text
Nonzero temperature positions
+
Their four cardinal neighbors
```

restricted to occupied cells.

## Implementation sketch

1. Prune thermal entries on air, matching current semantics.
2. Collect each remaining thermal position and its four neighbors.
3. Deduplicate.
4. Skip unoccupied candidates.
5. Evaluate the exact existing sum and `/ 5`.
6. Store nonzero results.

**Deduplicate before computing, but preserve the exact integer formula.** Do not replace it with separately rounded per-edge transfers and expect identical results.

This can be a behavior-preserving optimization.

## Sparse versus dense

Use a benchmark-dependent crossover.

For very sparse heat, candidate gathering is attractive.

For widespread heat, a dense or chunk-local thermal pass may win because sparse lookup and deduplication become overhead.

A good long-term arrangement is:

```text
Sparse world directory
    ↓
Dense material chunk
Optional dense thermal chunk
Thermal activity mask / bounds
```

## Physics caveat

Your current operation is a five-point average with air treated as a zero-temperature sink. It is not material-dependent thermal conduction.

Consequences:

- All materials conduct identically.
- Exposed hot cells cool quickly.
- Integer truncation dissipates small values.
- Heat capacity is absent.
- A material does not remain hot merely because it is lava; something must keep injecting heat.

Keep optimizing the existing formula separate from improving that model.

---

# 11. Sleeping must preserve ownership semantics—not merely skip a loop

Your concern about owned-cell swaps is exactly right.

This is unsafe:

```cpp
if (chunk.sleeping)
    continue;
```

Your algorithm constructs a replacement map. Skipped cells disappear unless something else preserves them. Even if copied, their settled/in-flight status can differ from the original schedule.

## 11.1 “Stayed this tick” is not “safe to sleep”

`stayed` means:

> This destination contains an occupant eligible under the current movement pass’s ownership rules.

Sleep means:

> Skipping future evaluations cannot change the result until some dependency changes.

Those are different concepts.

A cell might stay because:

- Both directions are permanently blocked.
- Its preferred direction happened to lose a conflict.
- A neighbor has not been processed yet.
- A probabilistic rule failed.
- Temperature has not crossed a threshold yet.

Only some of those justify sleep.

## 11.2 Keep activity domains separate

At minimum:

- Movement activity.
- Reaction activity.
- Thermal activity.
- Presentation dirtiness.

Static wood can sleep for movement while remaining reactive.

Settled water can remain thermally active.

A visually unchanged lava region may require continuous presentation animation.

## 11.3 Wake the actual dependency neighborhood

The current rules inspect more than immediate cardinal neighbors.

Relevant dependencies include:

- Diagonal targets.
- Corner-cut guards.
- Up to four horizontal cells.
- Above/below occupancy.
- Reaction neighbors.
- Thermal neighbors.

Wake all potentially affected evaluators when one of their dependencies changes.

For initial chunk sleeping, a conservative neighboring-chunk halo is easier to prove than minimal cell-level wake bounds.

## 11.4 Preserve global processing order

Your movement order is:

```text
y descending globally
x ascending within each row
```

Processing entire chunks in chunk-coordinate order does not necessarily preserve that order.

For example, doing all rows of the left chunk before all rows of the right chunk changes same-row and cross-boundary competition.

For behavior-preserving optimization, retain the effective global order of active dynamic cells.

## 11.5 Recommended staged design

### Stage A: exact all-static fast path

Small and safe.

### Stage B: persistent authoritative cells

Stop reconstructing unchanged static occupancy every tick.

### Stage C: active dynamic positions with conservative wake-up

Initially allow false positives: extra active cells cost time but preserve correctness.

### Stage D: prove sleeping inert cells equivalent to the reference

Run optimized and reference implementations side by side in randomized small worlds.

Compare every tick:

- Materials.
- Temperatures.
- Tick.
- Canonical saves.
- Subsequent continuation after loading.

**Do not use “unchanged for N ticks” as an exact sleeping proof.** Tick-dependent preferences and reactions can make a later tick differ.

If you choose heuristic sleeping, that is a new simulation policy and should be treated as such.

---

# 12. Movement-quality red flags visible in the code

These are not all bugs. Some are stylized rules that may be acceptable. But several will be visible before better rendering helps.

## 12.1 Same-row decisions are asymmetric

You process `x` ascending.

But water checks neighboring wetness in `next`:

```cpp
if (!(wet_at(side_l) && wet_at(side_r))) {
    ...
}
```

For a normal contiguous liquid row:

- The left neighbor has usually already been processed.
- The right neighbor usually has not.

So the comment:

> “A cell embedded in a complete surface row … rests”

is not reliably implemented by this test. `next` is not a completed snapshot of that row.

This is an important source of directional bias and unintended surface activity.

**Test a flat pool and its mirrored version.** Because position hashes also affect direction, design fixtures that distinguish hash preference from schedule bias, rather than expecting arbitrary mirrored seeds to agree automatically.

## 12.2 Lateral scans jump past occupied nonsolid cells

Both liquid and gas contain:

```cpp
if (is_solid(target)) break;
if (current.contains(target)) continue;
```

The scan can pass an occupied nonsolid position and choose an empty cell farther away.

This behaves like a bounded transport shortcut, not literal collision-resolved traversal.

That may be useful for fluid leveling, but ask:

- Should water jump past oil?
- Should gas pass across water horizontally?
- Should intermediate occupancy block transport?
- Should only same-fluid chains support that shortcut?

Document the intended rule instead of letting `solid` alone define fluid permeability.

## 12.3 Thin liquid films do not spread

Your lateral gate requires liquid below:

```cpp
const bool deep = below_mat != nullptr &&
                  material(*below_mat).movement == Movement::Liquid;
```

A droplet on flat stone therefore does not spread laterally.

That avoids jitter, but it also produces beads or stacks where a player may expect puddles.

This is an art/gameplay trade-off. It should eventually become a material behavior parameter, not a universal liquid property.

## 12.4 There is no general U-tube pressure equalization

Liquid primarily moves:

- Down.
- Down-diagonal.
- Horizontally.
- Into powder under special infiltration conditions.

There is no general mechanism for pressure to drive a liquid upward into the other arm of a connected vessel.

You may get local leveling while still failing hydrostatic equalization.

Do not describe the current lateral rule as a general pressure system. It is a local flow heuristic.

## 12.5 Infiltration is universal and can fight gravity

Any liquid can infiltrate any powder under the current conditions.

It has no:

- Permeability.
- Wetness capacity.
- Absorption state.
- Material-specific affinity.
- Rate parameter.

The escape check is also based on the original snapshot and does not prove the displaced powder will actually escape.

Potential symptoms:

- Water/sand swapping loops.
- Strange behavior with lava or oil.
- Sand lifting in ways that look like displacement rather than absorption.

I would make infiltration an explicit material-pair capability when you next revise behavior.

## 12.6 Gas is strongly constrained by the bottom-up schedule

The guard:

```cpp
!next.contains(p) && !current.contains(p)
```

prevents gas from entering cells occupied at the beginning of the pass, even if those occupants will leave.

That is ownership-safe, but it limits coherent upward motion in packed gas.

Expected symptoms:

- Vacancy-driven movement.
- Streaks.
- Slow release from crowded pockets.
- Artificial sideways spreading under ceilings.

This is a good candidate for a later gas-specific update design, but changing it now would alter outcomes.

## 12.7 `stayed` marks survive displacement

When a settled occupant is swapped out, you leave the destination in `stayed`.

The incoming cell now occupies a coordinate still labeled as a settled home.

That conflicts with the comment that in-flight arrivals must not qualify.

I am **not** claiming this alone proves mass loss; your source ownership constraints may still preserve cell counts. But it means the marker is not faithful to the stated invariant.

Add a test with:

- Multiple material densities.
- Competing diagonal/down swaps.
- A destination being displaced more than once in a tick.

Then decide whether `stayed` describes the coordinate, the original owner, or the current occupant. Right now those concepts can diverge.

---

# 13. Long-term storage recommendation

For your intended game, I would favor:

```text
Sparse chunk directory
    ↓
Dense material arrays within chunks
Optional dense auxiliary arrays
Chunk occupancy and activity metadata
Reusable local scratch
```

rather than a global hash entry for every occupied cell.

Why:

- Terrain is locally dense.
- Neighbor access dominates.
- Material IDs are tiny.
- You already maintain dense chunk arrays.
- Rendering naturally consumes chunks.
- Thermal activity can be localized by chunk.
- Unchanged terrain can remain in place.

An authoritative chunk can contain approximately:

```cpp
struct Chunk {
    std::array<MaterialId, 64 * 64> materials;
    // Optional temperature storage.
    // Occupied and dynamic counts.
    // Movement/reaction/thermal activity metadata.
    // Presentation revision or dirty flag.
};
```

The exact layout needs benchmarking. In particular, 64×64 rendering chunks need not imply a 64×64 minimum activity unit.

## Migration strategy

1. Keep the current implementation as a test oracle.
2. Introduce chunk-backed read/query helpers.
3. Prove they return identical values.
4. Move authoritative material storage.
5. Preserve movement ordering initially.
6. Remove the redundant global material map only after equivalence tests pass.

This is the largest promising architectural improvement, but it should follow the smaller measured changes.

---

# 14. Bridge review: what I can say without the missing body

From the packet, not the pasted bridge code:

- CPU repaint threshold: eight chunks.
- Full repaint above that threshold.
- Whole-image updates for both RGBA layers.

## First changes to evaluate

1. **Measure repaint separately from texture submission and GPU work.**
2. **Return early when no relevant material data changed.**
3. **Choose partial repaint based on affected area and measured overhead, not only chunk count.**
4. **Keep full upload and full repaint as separate decisions.**
5. **Keep animation out of the CPU color bake.**

A full texture update does not inherently require a full CPU repaint.

For Godot’s high-level `ImageTexture.update(image)`, there is no caller-specified dirty rectangle in that API. Do not assume your CPU dirty tracking creates a matching GPU subregion upload.

But the approximately 120 MiB/s raw payload is not, by itself, evidence that uploads dominate. You need timings for:

- CPU painting.
- Image/packed-array copying.
- Allocation or copy-on-write.
- Submission stalls.
- Actual GPU transfer/render behavior.

## One future correctness issue already visible in the engine

`set_temperature()` does not mark presentation data dirty.

That is fine while visuals ignore temperature. It becomes a problem if temperature affects:

- Emission.
- Material tint.
- Heat distortion.
- Thermal debug views.

Introduce distinct field revisions or dirty channels rather than making every temperature change trigger a full material repaint.

---

# 15. Benchmark and profiling plan

Please commit the harness. A `/tmp` benchmark is useful for discovery but weak as a long-term performance contract.

## Record these phases

```text
Reactions
Clear/reserve scratch
Classify/copy static cells
Order dynamic cells
Powder movement
Liquid movement
Gas movement
Active-region maintenance.
Snapshot extraction.
Image preparation.
Texture upload submission.
Gameplay entities.
```

Also count:

- Occupied cells.
- Dynamic cells by movement type.
- Successful moves and swaps.
- Reaction candidates and accepted hits.
- Nonzero temperatures.
- Thermal candidate count.
- Changed positions.
- Dirty chunks.
- Container capacities, if available.

## Use two timing modes

### Trajectory benchmark

Run a fixed initial scene for many ticks.

Useful for total gameplay cost, but report how the workload changes.

### Repeatable-state benchmark

Restore the same settled or active snapshot outside the timed region, then measure a fixed interval.

Useful for comparing implementations against the same starting workload.

Run multiple trials, include warm-up, and report at least median and p95.

Use Instruments’ sampling profiler on the release executable with symbols to determine whether time is dominated by:

- Hash probing.
- `clear()`.
- Allocation.
- Tree traversal.
- Sorting.
- Memory bandwidth.

**`flat_cell_map.hpp` is the most important missing source for that next review.** Its clear policy, load factor, tombstones, hash, and iteration scheme could materially change the diagnosis.

---

# 16. Suggested commit sequence

Keep correctness changes separate from equivalent optimizations.

| Commit | Scope | Acceptance |
|---|---|---|
| 1 | Commit harness, add phase counters | Reproducible baseline |
|---|---|---|
| 2 | Add adversarial heat/movement/reaction tests | Expose undocumented behavior |
|---|---|---|
| 3 | Correct auxiliary-state transport | Explicit new expected thermal results |
|---|---|---|
| 4 | Runtime material array | Old/new state identical each tick |
|---|---|---|
| 5 | Scratch swaps and retained buffers | Same outputs, fewer allocations |
|---|---|---|
| 6 | Compiled reaction matching/candidates | Same original rule indices and outcomes |
|---|---|---|
| 7 | Exact no-dynamic movement fast path | Static scenario avoids reconstruction |
|---|---|---|
| 8 | Exact sparse thermal candidates | Identical conduction results |
|---|---|---|
| 9 | Changed-position mirror reconciliation | Mirror equals full reconstruction |
|---|---|---|
| 10 | Occupancy counts and dirty bookkeeping | Same observable chunk contents |
|---|---|---|
| 11 | Chunk-authoritative storage/activity prototype | Differential replay testing |
| Separate track | Revised liquid/gas/reaction semantics | Deliberately versioned behavior changes |

# Bottom line

**Your smell-list is directionally good, but I would sharpen it in three ways:**

1. **The static control is primarily indicting whole-state reconstruction and full-map reconciliation—not dirty-set insertion or complicated movement.**
2. **Material maps are worth eliminating from hot paths, but they are a low-risk improvement, not a substitute for removing occupancy-proportional work.**
3. **The heat-transport bug and nontransactional reaction semantics deserve attention before new physical interactions.**

The highest-value next packet would contain:

- Full `simulation.hpp`.
- `flat_cell_map.hpp`.
- Full bridge implementation.
- Material/reaction struct definitions and validation.
- The committed benchmark harness.

With those, the next pass can move from architectural recommendations to concrete, reviewable patches—starting with the runtime material table, scratch-buffer fix, and a properly tested static fast path.

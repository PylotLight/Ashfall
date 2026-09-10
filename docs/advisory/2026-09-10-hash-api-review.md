# External Hash/API Review — raw feedback (2026-09-10)

Source: advisory model review of `engine/include/ashfall/cell_types.hpp` +
`engine/include/ashfall/simulation.hpp` (follow-up closing the F4 hash question
and the migration-readiness question). Reproduced verbatim for the local record.
Follow-ups extracted into `docs/advisory/PERF_TASK_BACKLOG.md` (hash/API section).

---

## Advisory verdict

- **(a) Hash:** no correctness defect apparent in the hash itself, but **F4 cannot be closed with a p95 ≤ 3 claim from source inspection**. There is also a provable weakness for chunk-aligned sparse patterns that deserves a benchmark.
- **(b) Reservations/API:** reserve by **peak distinct entries**, not operation count. The headers support conditional bounds, but exact per-buffer counts and growth thresholds require `simulation.cpp` and `flat_cell_map.hpp`. A small `find_or_insert` API is worthwhile.
- **(c) Migration:** **no public API fundamentally blocks chunk-authoritative storage or later entity ownership**. The main contract gaps are dirty-state consumption, empty/thermal-only chunks, coordinate/buffer semantics, and the boundary between terrain queries and entity collision.

This is a source-level review of the two supplied headers, not a measured probe-length result.

---

## A. `CellPositionHash`

### What is sound

Unsigned arithmetic makes the intended 32-bit wrapping well-defined on the current target platforms. Negative coordinates convert to `uint32_t` deterministically.

For a power-of-two bucket count \(B = 2^k\), assuming the table uses:

```cpp
bucket = hash(position) & (B - 1);
```

there is a useful property: **at fixed `x`, the initial bucket is a permutation of `y` modulo \(B\)**.

The multiplier `0x85EBCA77` is odd, so multiplication by it is invertible modulo \(2^k\). Adding the `x`-dependent constant and XORing with the fixed `x` contribution preserve that property.

Consequently, a contiguous vertical column of at most \(B\) cells has no repeated initial buckets. That is reassuring, but it does **not** establish good clustering across multiple columns.

### A provable spatial weakness

For positions:

```cpp
{64 * i, 64 * j}
```

the hash always satisfies:

```cpp
(hash(position) & 15u) == 9u
```

Reason:

- The `x` multiplication has six low zero bits.
- Its right shift by two still has four low zero bits.
- The `y` multiplication has six low zero bits.
- The additive constant ends in hexadecimal `9`.

More generally, positions sharing the same within-chunk offset have identical low four hash bits.

**Implication:** a repeated feature at one fixed location in every 64×64 chunk initially targets only one in sixteen buckets in a masked power-of-two table. Linear probing then has to spread those keys into neighboring buckets.

This does **not** mean a dense rectangle has the same problem: a dense rectangle contains every local offset. It does mean “works well on dense grids” would not establish “works well for all of this game’s spatial data.”

The current `chunks_` uses `std::map`, so this is not presently a chunk-map hashing bug. It can affect sparse cell/temperature patterns, or a future hashed chunk index using this hash.

### About the p95 target

For ordinary linear probing under the usual random-hashing model, at 50% occupancy:

- Expected successful search length: approximately **1.5 probes**.
- Expected unsuccessful search/insertion search length: approximately **2.5 probes**.

Those are **means**, not p95 bounds. A p95 target must distinguish:

1. Successful lookups.
2. Unsuccessful lookups.
3. New-key insertions.
4. Existing-key insertions/updates.

Do not combine those into one percentile and call it a hash verdict.

### Smallest useful F4 experiment

Instrument the actual `FlatMap`, counting every examined slot, including the terminating empty slot for a miss.

| Workload | Why |
|---|---|
| Dense 64², 256², 512² regions | Current intended use |
| Translated regions, including negative coordinates | Avoid origin-specific reassurance |
| Long horizontal and vertical strips | Floors, walls, reservoirs |
| `{64*i + ox, 64*j + oy}` patterns | Exercise the proven low-bit correlation |
| Actual hot-path keys and lookup sequences | Weight the result by real usage |

Use the actual insertion/traversal order, not only shuffled keys. Report actual occupancy, p50/p95/p99/max, and total probes per tick.

If strict growth at the threshold prevents exactly 50% occupancy, test just below it and report that explicitly.

**Recommendation:** keep F4 open until those measurements exist. If the current hash disappoints, compare it with a version having a strong final avalanche. A 32-bit finalizer can address the demonstrated low-bit structure, although it cannot eliminate collisions already present in the original 32-bit hash. A mixed packed 64-bit coordinate is another candidate—not an automatic upgrade without measuring total runtime.

Any hash change must preserve the deterministic outcome tests. Hash-table iteration must not decide simulation arbitration or canonical save order.

---

## B. Reserve sizes and `find_or_insert`

### Reserve element counts, not bucket counts

Assuming `reserve(E)` means “support at least `E` live entries without growth,” call it with **`E`**, not `2*E`.

For a power-of-two table allowing occupancy up to and including 50%, the required bucket count is:

\[
B = \operatorname{bit\_ceil}(2E)
\]

If insertion instead grows when the resulting occupancy is **equal to** 50%, the requirement becomes:

\[
B = \operatorname{bit\_ceil}(2E + 1)
\]

That threshold distinction matters at exact powers of two.

For example, **262,144 entries require 524,288 buckets only if exactly 50% is allowed**. An exclusive threshold would require the next power of two.

The actual implementation is needed to settle this precisely, including minimum capacity and overflow handling.

### Per-buffer reservation bounds

Define:

- `N`: occupied cells at tick start.
- `Nnext`: maximum distinct entries accumulated in the next-state map.
- `D`: movement candidates collected for this tick.
- `Hpeak`: maximum simultaneously resident nonzero temperature entries during the relevant phase.

Then:

| Storage | Reservation target | Qualification |
|---|---|---|
| `cells_` | Peak occupied count | Include edits, loading, and world generation—not just stepping |
| `next_scratch_` | `Nnext` | `N` suffices if this phase cannot increase occupied count |
| `stayed_scratch_` | Maximum distinct marked positions | `N` if marks only identify tick-start occupied cells |
| `dyn_scratch_` | `D` | `N` is a conservative bound if each occupied cell is appended at most once |
| `order_scratch_` | Maximum entries actually sorted | Cannot determine whether this is `D`, `N`, or another set from the declaration |
| `heat_carry_scratch_` | Number of staged moves | At most `D` if each mover stages once |
| `temps_` | `Hpeak` | **Not necessarily bounded by `N`**, because public setters allow heat on air |
| `temp_scratch_` | Distinct positions inserted during conduction | Requires the conduction loop |
| `temp_prune_scratch_` | Maximum appended prune candidates | Bounded by the scanned set only if each position is appended once |

For illustration, one-hop four-neighbor conduction from `H` sources has a candidate union of at most `5H` positions **if it considers each source and its four neighbors**. That is a conditional bound, not a finding about your implementation.

Two important details:

1. **Both sides of a swapped map pair need adequate capacity.** Reserving only one can leave alternate ticks growing.
2. Reserve against distinct entries. Ten probes or updates to one position still consume one entry.

Keep capacity through normal ticks, using the already-planned explicit shrink policy outside the hot path. Do not reserve for an entire theoretical world extent when only a much smaller active set is needed.

**To provide exact call-site sizes rather than these bounds, I need the insertion/append loops and `FlatMap::reserve`/growth logic.**

### API worth adding

For these scalar mapped values, a minimal API is enough:

```cpp
// Returns {stored value, whether a new entry was inserted}.
// Existing values remain unchanged.
std::pair<Value*, bool> find_or_insert(
    const Key& key,
    Value initial_value);
```

Useful call-site transformations:

```cpp
// Before:
if (!marks.contains(p)) {
    marks.insert(p, 1);
    // ...
}

// After:
auto [value, inserted] = marks.find_or_insert(p, 1);
if (inserted) {
    // ...
}
```

And:

```cpp
auto [value, inserted] = next.find_or_insert(destination, material);
if (inserted) {
    // Destination successfully claimed.
}
```

Required contracts:

- Existing-key calls do not overwrite.
- At adequate capacity, search and insertion use one probe walk.
- Growth may require reprobing; do not promise universally one walk.
- An existing-key hit should not cause an unnecessary growth.
- Document pointer invalidation. Rehash invalidates pointers; deletion may also relocate entries depending on the erase strategy.
- Keep keys immutable through the returned interface.

Keep `insert_or_assign` separate. “Claim if absent” and “overwrite” are different simulation operations.

For temperatures, do not accidentally turn read-only queries into insertion of stored ambient-zero entries. Preserve the zero-is-absent invariant at the defined phase boundary.

---

## C. Public API and chunk-authoritative migration

### The boundary is already good

These operations are storage-independent:

```cpp
cell()
set_cell()
fill_rect()
temperature()
set_temperature()
read_chunk()
copy_rect()
save()
load()
```

No public iterator, pointer, or reference exposes a `FlatMap` entry or chunk vector. `occupied_cell_count()` and `temperature_count()` can become maintained counters.

Therefore, making chunks authoritative can be an internal migration rather than a bridge-wide API rewrite.

### 1. Define chunk existence independently of scheduling

These names need explicit semantics:

```cpp
has_chunk()
active_chunks()
```

Does “active” mean:

- Has non-air material?
- Has any authoritative state, including heat?
- Needs simulation work?
- Is allocated?

Those sets may coincide today but will diverge with chunk sleeping and thermal state.

**Recommendation:** preserve a stable world-facing meaning and introduce a separate internal work set for scheduling. Do not silently redefine `active_chunks()` during migration.

Because heat can be set on air, “no occupied cells” cannot by itself prove a chunk has no state worth retaining.

### 2. Dirty-state consumption is the largest API limitation

```cpp
dirty_chunks()
clear_dirty_chunks()
```

is a **single shared dirty stream**. That is fine for one bridge consumer. Multiple consumers can lose updates if one clears it before another reads it.

Before a second consumer needs independent incremental updates, use a generation/cursor contract or per-chunk revisions. No need to build that machinery prematurely.

Specify these cases now:

- Removing the final cell still reports the affected chunk as dirty.
- Reset/load invalidates previously displayed chunks.
- A missing chunk has a documented read result so the renderer can clear old pixels.
- What becomes dirty when temperature changes?
- Does `mark_chunk_dirty()` mean presentation invalidation, or simulation scheduling?

Those last two responsibilities should not become accidentally coupled.

### 3. Document coordinate and buffer contracts

Especially for:

```cpp
fill_rect()
copy_rect()
read_chunk()
can_move_to()
```

Specify:

- Inclusive versus exclusive rectangle endpoints.
- Reversed-endpoint behavior.
- Supported coordinate range.
- Negative-coordinate chunk mapping using floor division.
- Row-major layout and required output length.
- Failure behavior and whether buffers remain untouched.
- Overflow-safe extent calculations.
- Invalid material-ID behavior in `set_cell()`.

For `read_chunk()`, one complete material plane is exactly **4,096 entries**. State whether larger spans are accepted and what absence does to the output buffer.

These are migration guardrails, not requests to change settled save semantics.

### 4. Entity ownership is not blocked

Nothing here requires Godot to own authoritative entities.

The possible future naming problem is:

```cpp
can_move_to(min_pos, max_pos)
```

Today it appears to be a terrain occupancy query. Later, “can move” might include entity overlap, collision layers, one-way surfaces, or swept collision.

Document it as a terrain test if that is its current role. Later entity movement can compose that test without changing its meaning.

Keep entity ownership in a portable engine-level world/session layer when needed. There is no reason to force entity storage into cellular chunk storage merely because chunks become authoritative.

### 5. Two small cleanup findings

- The public temperature comment says “no advection in P2.4,” while the private API explicitly describes heat advection. **Update the public contract.** This is documentation drift, separate from the already-filed heat-duplication issue.
- These headers require **C++20**, notably through `std::span` and defaulted `<=>`. If C++17 compatibility is still advertised, narrow that claim or provide compatibility implementations.

---

## Suggested disposition

1. **F4:** remain open; add probe instrumentation and the chunk-aligned sparse case.
2. **Reservations:** inspect the actual phase loops and growth threshold before filing numeric prescriptions.
3. **Container API:** add `find_or_insert` if current callers perform repeated searches; test hit, insertion, threshold growth, collision chains, and invalidation behavior.
4. **Migration preparation:** document chunk/dirty/read semantics and repair the temperature comment. No public API redesign is required now.
5. **Next bridge round:** proceed as planned. Focus on whether dirty chunks lead to partial uploads or full-texture work, deletion/reset clearing, and whether Godot duplicates authoritative state.

**Bottom line:** the API boundary is migration-friendly. The hash is not yet a measured performance pass, and it has a concrete regular-grid correlation that the closing F4 benchmark should not omit.

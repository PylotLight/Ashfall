# Migration-Prep Contracts (Group 1, Task E — docs only, no redesign)

Source of truth: `engine/include/ashfall/simulation.hpp`,
`engine/src/simulation.cpp`, `engine/include/ashfall/cell_types.hpp`.
Every statement below was verified against those files; signatures are quoted verbatim.
No behavior change is proposed or described here — contracts only.

Coordinate basis (all sections): `CellPosition { int x; int y; }`
(`cell_types.hpp`), `kChunkSize = 64`, `MaterialId = std::uint8_t`, `kAir = 0`.
Chunk mapping uses floor division, so negatives are correct:
`chunk_pos_of` → `floor_divide(pos, kChunkSize)` plus `positive_modulo`
for the in-chunk offset (`simulation.cpp:14-22,58-60`); chunk-vector index is
`chunk_index(local_x, local_y) = local_y * kChunkSize + local_x`, i.e. row-major,
x fastest (`simulation.hpp:96-98`).

---

## 1. `has_chunk` vs `active_chunks` (storage presence, NOT activity)

```cpp
[[nodiscard]] bool has_chunk(CellPosition chunk_position) const;          // simulation.hpp:62
[[nodiscard]] std::vector<CellPosition> active_chunks() const;            // simulation.hpp:63
```

- `has_chunk(p)` returns `chunks_.contains(p)` (`simulation.cpp:725-727`):
  chunk storage for `p` is present.
- `active_chunks()` returns one entry per key currently in `chunks_`
  (`simulation.cpp:729-736`): the stored-chunk list, in `std::map` key order.
- NEITHER implies simulated activity. There is no sleep/dormancy system:
  `step()` visits every occupied cell each tick via `current.for_each`
  (`simulation.cpp:405-411`) with no chunk-activity check anywhere in the
  tick path, and `chunks_` is documented in-header as a mirror "for
  chunk-granular presentation and read_chunk queries" (`simulation.hpp:101-104`).
- Naming warning for migration: "active" means "stored", not "awake". Any
  future internal work set (e.g. a dirty/sleep set) must be a SEPARATE
  structure; do not reinterpret `active_chunks()` as a scheduling input.
- "No occupied cells ≠ no state": heat may exist on air. `set_temperature`
  performs no occupancy check (`simulation.cpp:965-972`), and surviving
  entries are pruned only when a step runs while heat exists: each tick with
  non-empty `temps_`, `conduct_temperature()` drops entries whose cell is
  gone BEFORE conducting (`simulation.cpp:994-998`). A world with zero cells
  can therefore still hold `temps_` state between ticks.

## 2. Dirty-stream spec (presentation invalidation, NOT scheduling)

```cpp
[[nodiscard]] std::vector<CellPosition> dirty_chunks() const;             // simulation.hpp:71
void clear_dirty_chunks();                                                // simulation.hpp:72
void mark_chunk_dirty(CellPosition chunk_position);                       // simulation.hpp:73
```

`mark_chunk_dirty` inserts into `dirty_chunks_`
(`simulation.cpp:952-954`). It is presentation invalidation only: nothing in
the simulation reads `dirty_chunks_` to decide work (no sleep system reads
it — see §1).

Producers (every `mark_chunk_dirty` call site in `simulation.cpp`):

- `set_cell` → marks `chunk_pos_of(position)`, but ONLY on an actual change.
  Unknown-material and same-material calls return before marking
  (`simulation.cpp:248-252,260`).
- `fill_rect` → marks every chunk overlapped by the rect (`simulation.cpp:288-294`).
  Unlike `set_cell` there is no no-change early-out: re-filling identical
  material still marks. Unknown material returns unmarked (`simulation.cpp:264`).
- Reactions → via `set_cell` (and `set_temperature`, which itself never
  marks) inside `run_reactions` (`simulation.cpp:375-379`).
- `step()` reconcile → marks removals and in-place changes in the first diff
  loop, pure additions in the second (`simulation.cpp:694-709`).
- `load()` → marks the union of pre-load and post-load chunk regions
  (`simulation.cpp:868-880`).
- `reset()` → marks all pre-reset chunk regions (`simulation.cpp:203-207`).

Consumer rule: the bridge presenter is the SOLE consumer. It snapshots
`dirty_chunks()`, repaints, then calls `clear_dirty_chunks()`
(`bridge/src/ashfall_world.cpp:353,501-502`). The GDScript bindings
`get_dirty_chunks` / `clear_dirty_chunks` (`bridge/src/ashfall_world.cpp:27-28,167-176`)
MUST NOT preempt that cycle: GDScript may read, but clearing outside the
present-frame consume point drops repaint work. (Policy contract; the engine
cannot enforce it — the binding exists and forwards directly.)

Engine-side clearing cases — none of these auto-clear, each verified by the
absence of any `clear_dirty_chunks()` call on the path:

- Final-cell deletion: `write_chunk_cell` erases emptied chunk storage
  (`simulation.cpp:232-236`; deferred bulk variant in `step()`,
  `simulation.cpp:710-715`), but the dirty mark written for the removal
  stays set. Storage deletion ≠ dirty clearance.
- `reset()`: marks old regions dirty and leaves the set populated for the
  presenter (`simulation.cpp:203-214`; no clear call).
- `load()`: marks old+new regions dirty and leaves the set populated
  (`simulation.cpp:868-883`; no clear call). Failed loads leave cells, heat,
  tick — and the dirty set — untouched (all `return false` paths precede the
  commit at `simulation.cpp:872-882`).
- `copy_rect` air overwrite / missing-chunk reads: `copy_rect` prefills the
  caller buffer with `kAir` and skips missing chunks (`simulation.cpp:761-767`).
  It is `const` and never touches the dirty set. `read_chunk` likewise fills
  `kAir` on missing chunks and returns `true` (`simulation.cpp:742-747`).

Temperature and dirtiness: temperature changes NEVER mark chunks directly.
`set_temperature` has no mark call (`simulation.cpp:965-972`); conduction and
heat-carry application touch only `temps_` (`simulation.cpp:719-721,1004-1016`).
Heat becomes visible to the presenter only when a cell change on the same
chunk marks it.

## 3. Coordinate / buffer contracts per function

```cpp
void set_cell(CellPosition position, MaterialId material);                // simulation.hpp:56
void fill_rect(CellPosition first, CellPosition last, MaterialId material); // simulation.hpp:57
bool read_chunk(CellPosition chunk_position, std::span<MaterialId> out_buffer) const; // simulation.hpp:64
void copy_rect(CellPosition first, CellPosition last, std::span<MaterialId> out_buffer) const; // simulation.hpp:65
```

- `set_cell(pos, id)`: single cell, no range. Unknown `id` → silent no-op
  (returns before any write or mark). Same-as-current → no-op. `kAir` erases
  the cell AND its temp entry synchronously (`simulation.cpp:248-261`).
- `fill_rect(first, last, id)`: BOTH endpoints inclusive; reversed ranges are
  normalized via `std::min`/`std::max` on each axis (`simulation.cpp:265-268`),
  so `fill_rect(b, a, …) == fill_rect(a, b, …)`. `kAir` path erases cells and
  their temp entries; non-air path writes cells and leaves temps untouched
  (`simulation.cpp:269-287`). Iterates in 32-bit `int` steps with no extent
  multiplication, so there is no width×height overflow site here — but full-
  `int32`-span rects mean a near-infinite loop; callers keep rects modest.
- `read_chunk(chunk_position, out)`: `chunk_position` is a CHUNK coordinate
  (already divided), not a cell coordinate. Requires
  `out.size() >= 64*64 (4096)`; on short buffers returns `false` WITHOUT
  touching the buffer (`simulation.cpp:738-741`). Missing chunk → fills all
  4096 entries with `kAir`, returns `true`. Layout is the chunk vector copied
  verbatim: row-major, `out[y*64+x]` (`simulation.cpp:742-747` with
  `chunk_index`).
- `copy_rect(first, last, out)`: both endpoints inclusive, reversed ranges
  normalized (`simulation.cpp:751-754`). Row-major, `dst[(y-min_y)*width +
  (x-min_x)]` with `width = max_x-min_x+1` (`simulation.cpp:772-777`). On
  short buffers (`out.size() < width*height`) returns WITHOUT touching the
  buffer (`simulation.cpp:757-758`); otherwise prefills the WHOLE buffer with
  `kAir` first, then overwrites populated regions chunk-row by chunk-row —
  missing chunks read as air (`simulation.cpp:761-780`). Extent note:
  `width`, `height`, and `width * height` are computed in 32-bit `int`
  arithmetic with no widening (`simulation.cpp:755-757`); `int32`-spanning
  rects can overflow before the size check. Callers keep rects modest;
  widening/chunking the iteration is future work, not this contract.
- `can_move_to(min_pos, max_pos)` (`simulation.hpp:70`): AABB form — both
  endpoints inclusive, reversed endpoints normalized
  (`simulation.cpp:932-935`); loops in `int` steps (same modest-extent
  caller duty as `fill_rect`).
- Coord range: full `int32` cell space. Negative coordinates map correctly via
  `floor_divide` chunking and `positive_modulo` local offsets
  (`simulation.cpp:14-22,58-60`); save/load round-trips negatives through
  two's-complement `u32` bit patterns (`simulation.cpp:811-813,848`).

## 4. `can_move_to` is a terrain-only test

```cpp
[[nodiscard]] bool is_solid(CellPosition position) const;                 // simulation.hpp:69
[[nodiscard]] bool can_move_to(CellPosition min_pos, CellPosition max_pos) const; // simulation.hpp:70
```

`can_move_to` returns false iff any cell in the (normalized, inclusive) box
`is_solid` (`simulation.cpp:931-942`); `is_solid` resolves `cell(position)`
— `kAir` when empty (`simulation.cpp:198-201`) — through the loaded material
table's `solid` flag, reading unknown ids as non-solid
(`simulation.cpp:885-889`). It knows nothing of entities, actors, or
gameplay blocking: those compose LATER in an engine session layer, NOT in
chunk storage and NOT in this function. Migration must not add entity checks
here; the signature stays a pure terrain predicate.

## 5. Temperature comment-drift note (P2.5 carries heat)

The header comment on `set_temperature` (`simulation.hpp:38-43`) and the prune
comment in `conduct_temperature` (`simulation.cpp:991-993`) still carry
"No advection in P2.4 … P2.5 carries heat" phrasing. Verified current
behavior is P2.5 heat-carry:

```cpp
void stage_heat_carry(CellPosition from, CellPosition to);                // simulation.hpp:119
```

Movement stages each mover's tick-start temperature keyed by DESTINATION
(`simulation.cpp:979-982`); `step()` applies the stages post-swap, before
conduction (`simulation.cpp:719-721`), so heat rides the material instead of
pruning at the vacated cell; destinations are unique per tick (mass
conservation), so last-write conflicts cannot occur (`simulation.hpp:114-119`).
The stale "no advection" tails are being removed under Task C; this section
records the drift so migration readers trust the code (`stage_heat_carry` +
apply order), not the old phrasing.

## 6. C++17-vs-20 verdict: C++20 required — record, don't change

- `<span>` is in the public API: `simulation.hpp:9` includes it and
  `read_chunk` / `copy_rect` / `load` all take `std::span` parameters
  (`simulation.hpp:64-67`).
- `<=>` is used: `CellPosition::operator<=>` is defaulted
  (`cell_types.hpp:28`; `<compare>` included in both headers).
- `contains` (C++20 map/set/FlatMap API) is used throughout `simulation.cpp`
  (e.g. `materials_.contains`, `cells_.contains`, `chunks_.contains`,
  `stayed.contains`).
- `CMakeLists.txt:9` already pins `cxx_std_20` on `ashfall_sim`
  (`target_compile_features(ashfall_sim PUBLIC cxx_std_20)`).
- Verdict: the C++20 requirement is live and load-bearing — no narrowing to
  C++17 is possible without rewriting public signatures. Recorded; no change.

## 7. Clamp-range decision record: symmetric `[-32767, 32767]` retained

```cpp
void set_temperature(CellPosition position, int temperature);             // simulation.hpp:44
[[nodiscard]] int temperature(CellPosition position) const;               // simulation.hpp:45
```

`set_temperature` clamps with `std::clamp(temperature, -32767, 32767)` and
erases the entry on exactly `0` (`simulation.cpp:965-972`); storage is
`std::int16_t` entries serialized LE via `append_u16`/`read_u16`
(`simulation.cpp:32-35,51-56,817-821`). Full-`int16` (`-32768`) is rejected
to keep negation symmetry (`-min == max`, no `-0`): the conducted stencil
(`sum/5` over self + 4 neighbors, truncating toward zero,
`simulation.cpp:1007-1014`) stays symmetric about ambient. Task D fixes the
test message to `"cold clamps to symmetric min -32767 (not int16 -32768)"`;
the bound itself is unchanged.

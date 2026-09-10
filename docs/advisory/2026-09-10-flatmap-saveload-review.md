# External FlatMap + Save/Load Review — raw feedback (2026-09-10)

Source: advisory model review of `engine/src/simulation.cpp` (save/load regions),
`engine/include/ashfall/flat_cell_map.hpp`, and partial bridge code, following the
perf packet round. Reproduced verbatim for the local record. Follow-ups extracted
into `docs/advisory/PERF_TASK_BACKLOG.md` (FlatMap + save/load section).

Chosen as primary record over the competing response (filed separately only as
addenda): deeper verification (traced probe/tombstone/growth invariants),
iteration-order-after-load catch, cache-residency scaling argument, P1–P4 canonical
contract, name-remap v3 design. From the other response we keep: sparse-chunk bomb
limits, golden-bytes vectors, fuzz skeletons + seeded-hash warning.

---

Both files reviewed. Two verdicts up front:

**FlatMap: the core algorithm is correct as you use it — I traced the probe/tombstone/growth invariants and found no correctness bug.** The findings are a semantic wart in `reserve_for`, a stale comment that hides real tombstone traffic, an undocumented pointer-invalidation contract, and a determinism surface you haven't written down. It also does **not** overturn the perf diagnosis — with one important scaling caveat that actually *strengthens* the dense-chunk recommendation.

**Save/load: mechanically safer than I expected — every read path is guarded, and I could not construct a thin-file allocation blowup. But it has one P0 shipping defect: the save does not identify the material table, so a content update silently reinterprets every saved world.** Plus two canonical-format holes (duplicate positions accepted, temps-on-air policy undefined) and an unresolved contract question you asked me to settle: byte-identical vs. canonical payload. I'll settle it.

---

# Part 1 — FlatMap

## 1.1 What I verified (so you don't re-derive it)

**Probe termination.** `find_slot` loops until an empty slot. Termination requires the invariant *occupied-or-tombstoned slots < capacity*, i.e., at least one empty slot exists. Your growth trigger `(size_ + tombstones_) * 2 >= capacity_` before every insert guarantees `size_ + tombstones_ <= capacity_/2` after any insert. `erase()` decreases `size_` and increases `tombstones_` by the same amount, so the sum never grows via erase. Therefore the invariant holds after every operation, and every probe terminates. ✔

**Tombstone correctness.** Insert probes *past* tombstones to find either the live key (update in place — the tombstone is not consumed) or an empty slot (insert at first tombstone seen). That is the standard correct scheme; the common bug — reusing a tombstone before confirming the key isn't further along — is not present. ✔

**Growth math.** `rehash(2C)` fires when `(s+t)*2 >= C`; live `n <= s+t`, so `n*2 <= (s+t)*2` — wait, at the trigger point `(s+t)*2 >= C`, so worst case `n*2 ≈ C < 2C`: the fresh table never re-triggers mid-population. ✔

**Rehash drops tombstones**, and `reserve_for` targets `cap >= 2n+16`, so post-reserve load is strictly <50% — appropriate for linear probing. ✔

**One caveat on termination:** it depends entirely on that growth invariant. One future edit that inserts without the check (say, a bulk-load fast path) turns every `find_slot` into a potential infinite loop. Add a debug-only probe bound:

```cpp
std::size_t probes = 0;
for (;;) {
    // ...
    if (++probes > capacity_) { assert(false && "FlatMap invariant violated"); return -1; }
}
```

Cheap insurance that converts a hang into a loud failure.

## 1.2 Findings, ranked

| # | Finding | Severity |
|---|---|---|
| 1 | `reserve_for` **shrinks** the table | Perf/correctness-of-intent; causes tick-to-tick churn |
| 2 | Header comment "step() never erases" is false — `conduct_temperature()`'s prune erases from `temps_` inside `step()` | Documentation; matters because it hides real tombstone traffic |
| 3 | Pointer/reference invalidation on insert is undocumented; several `step()` sites hold `next.find()` pointers across nearby inserts | Latent foot-gun (currently safe — audited below) |
| 4 | Iteration order is layout-dependent, and layout depends on insert history — an undeclared determinism surface | Fragility (currently safe — see §1.5) |
| 5 | `rehash()` builds a fresh `FlatMap` with a **default-constructed `Hash`** — a stateful hasher would be silently reset | Minor trap |
| 6 | `operator[]` does two full probes (find, insert, find again) | Micro |
| 7 | No probe statistics/debug counters | Missing tooling for exactly the investigation you're doing |
| 8 | `CellPosition`'s hash lives in `cell_types.hpp`, which I still haven't seen | Unverifiable clustering behavior |

## 1.3 `reserve_for` shrinks — fix this

```cpp
if (cap == capacity_) return;
rehash(cap);   // runs even when cap < capacity_
```

Consequences in the hot path:

- `step()` calls `next.reserve_for(cells_.size() + 16)` every tick. When the live cell count oscillates across a power-of-two boundary (evaporation, settling, a draining pool), capacity flips between e.g. 65536 and 131072, and **every flip is three allocations plus re-zeroing** — repeatedly, at 60 Hz.
- `reserve_for(0)` forces capacity down to 64 (the `while` loop never runs). Load a big world, `reset()`, step → shrink to 64 → immediately grow back.

For a hot-path container whose whole reason to exist is allocation stability, `reserve` should never shrink. Semantics: *ensure capacity for n, keep what you have*; shrinking is an explicit `shrink_to_fit()` for load/reset paths only:

```cpp
void reserve_for(std::size_t expected) {
    std::size_t cap = 64;
    while (cap < expected * 2 + 16) cap *= 2;
    if (cap <= capacity_) return;   // never shrink implicitly
    rehash(cap);
}
```

If you genuinely want memory back after loading a small world, call `shrink_to_fit()` explicitly there. Optionally add hysteresis (grow at 50%, shrink only below 25% on explicit request) — but never-shrink alone fixes the churn.

## 1.4 The erase comment is wrong, and it matters

Header: *"Erase uses tombstones (edit paths only; step() never erases)."*

`step()` ends with `conduct_temperature()`, whose prune loop does `temps_.erase(position)` for every heat entry whose cell vanished. That *is* the step path. So:

- Tombstones **do** occur during hot ticks, bounded by stale-heat count.
- The next `insert_or_assign` into `temps_` (heat-carry apply, next tick) may trigger a rehash purely to purge them — correct behavior, but someone reading the comment would rule it out when debugging.

Either update the comment ("movement never erases; the thermal prune does, transiently — tombstones are purged by the `temps_`/`temp_scratch_` swap") or restructure the prune to build the survivor set into scratch and swap, which also removes the erase traffic entirely. The restructure is a few lines and I'd take it.

## 1.5 Pointer invalidation and iteration order: safe today, but write the contract down

**Invalidation.** Any `insert_or_assign` may `rehash()`, invalidating every pointer/reference into the map. In `step()` you hold `next.find()` results across adjacent logic in `try_powder_target`, the liquid density-displacement block, and the seep branch. I audited each: every dereference happens *before* the next insert on that path (`displaced` is copied out first, etc.). Safe — but nothing enforces this. Add one line to the header:

> Pointers/references returned by `find`/`operator[]` are invalidated by any subsequent `insert_or_assign` (rehash). Do not hold them across writes.

**Iteration order.** `for_each` visits slot order, which depends on the hash, the capacity history, and the insert/erase history. After `load()`, `cells_` is populated in sorted file order, so its layout — and therefore its iteration order — differs from the session that saved, **even though the logical content is identical**. Today every consumer is order-independent: `save()` sorts, the diff writes idempotently into the mirror plus a `std::set`, reactions stage and sort. That's load-bearing and invisible. Write it into the header as an invariant, and add a test that inserts the same key set in shuffled orders and asserts identical *sorted* projections. The day someone emits events or picks "the first cell" from a `for_each`, that test is what saves you from a save/load-only divergence — the worst kind, because your continuation tests at round count N may not touch the order-dependent branch.

## 1.6 The hash — the missing piece I still can't review

Everything above is hash-agnostic. Whether linear probing behaves here depends entirely on `cell_types.hpp`'s `CellPosition` hash: grid data is the *worst* classical case for multiplicative/xor hashes (structured coordinates → secondary clustering). Send that file. Meanwhile, a ten-line debug test gives you the answer empirically: build a representative world, histogram probe lengths on `find` misses. If p95 probes ≤ 3 at 50% load, you're fine.

## 1.7 What FlatMap means for the perf diagnosis

Direct answer to your packet's framing ("the single file most likely to change the perf diagnosis"): **it doesn't change it — it confirms it, with a caveat.**

The container is close to optimal for what it is: inline storage, ≤50% load, one probe-chain per op, `clear()` = memset. Scenario D's ~37 ns/cell is not FlatMap being slow; it's the algorithm paying for full reconstruction plus a double full-map diff, ~5–7 map operations per cell.

The caveat — and this is the important one:

**Your FlatMap numbers are flattered by cache residency.** At 90k occupied cells, capacity is 2¹⁸ slots. Per map: keys ≈ 2 MB, values ≈ 0.25 MB, occupancy ≈ 0.25 MB. `cells_` + `next` + `stayed` + temps ≈ **7–8 MB — sitting in last-level cache on Apple Silicon** (8–32 MB depending on chip). That's why a random-access probe costs ~20–25 ns instead of ~80–100 ns DRAM latency, and why 37 ns/cell looks almost acceptable.

Scale the world 4× and the working set leaves LLC. Per-op costs roughly triple, and suddenly the same algorithm is 15–20 ms/tick with **no code change**. So the honest prediction is: FlatMap performs well exactly up to the world size where you need dense chunks anyway. Don't micro-tune the container; fix the algorithm (round 2, §8–§13) before the cliff, not after.

Minor items while you're in there: `operator[]`'s double probe (return the slot from the insert path instead); a combined `find_or_insert`-style API would collapse the `find` → `insert_or_assign` double-probing pattern that `try_powder_target` and friends repeat several times per moved cell — worth it only if profiling shows probe traffic mattering after the algorithmic fixes; and debug counters (rehash count, total probes) compiled out in release, which would have answered half of your round-2 questions mechanically.

---

# Part 2 — save()/load()

## 2.1 Read-path audit (your explicit question: "every read_u32 path")

I walked every byte-consuming region. **All reads are guarded; I found no out-of-bounds read path.**

| Region | Guard | Verdict |
|---|---|---|
| Header (magic 8 + tick 8 + count 4) | `bytes.size() < 8 + 12` rejects up front | ✔ |
| Magic + version | 7-byte `std::equal`, then `bytes[7] ∈ {1,2}` | ✔ |
| Per-cell `x`, `y` | `read_u32` self-checks `offset + 4 > size` | ✔ |
| Per-cell material byte | explicit `offset >= bytes.size()` before `bytes[offset++]` | ✔ (slightly awkward; fine) |
| v1 tail | exact equality `size - offset == count*9` before the loop | ✔ |
| v2 pre-temps | `size - offset >= count*9 + 4` | ✔ (lower bound only — see §2.2) |
| `temp_count` u32 | guaranteed ≥4 bytes remain by the v2 precheck | ✔ |
| Per-temp records | exact equality `size - offset == temp_count*10` before the loop; `temp == 0` rejected | ✔ |

Also correct: cells are rejected if `kAir` or not in the loaded table; failed loads return before any member mutation; `changed_chunks` covers both old and new chunk sets so the bridge repaints cleared regions; `tick_` restores.

## 2.2 The DoS question — an honest negative, with two nuances

I specifically hunted for the unbounded-allocation hole, because `loaded.reserve_for(count + 16)` runs before per-record validation. It doesn't bite, because `count` is bounded by input size in both versions:

- v1: `count*9 == size - offset` exactly → `count ≤ size/9`.
- v2: `count*9 + 4 ≤ size - offset` → `count ≤ (size - 24)/9`.

So a small file cannot claim a huge count; claiming 4×10⁹ cells requires a ~38 GB file. **No thin-file blowup exists.** Don't file one.

Two nuances worth recording:

1. **Memory multiplier.** `reserve_for` allocates ~2 slots/entry × ~10 bytes ≈ **2.2× the file size** transiently, before a single record is validated. A crafted 100 MB save allocates ~220 MB and *then* may fail on record 3. Streaming the load (grow naturally, or validate-then-reserve) would make rejection cheap. Acceptable now; worth a comment.
2. **`count*9` arithmetic** is done in `size_t` — correct. If anyone ever "simplifies" it to `uint32_t`, it overflows at count ≥ 477M. The current cast is load-bearing; comment it.

## 2.3 P0 — the save does not identify the material table

This is the defect that loses user worlds. The save stores **raw material ids**. `load()` validates them against *whatever table is currently loaded*. If `materials.json` changes between save and load — a new material inserted mid-roster, two materials swapped, a renumbering during a refactor — every id is silently reinterpreted:

> Save made with id 7 = lava. Content update makes id 7 = wood. Load succeeds, the world renders, the kiln contains burning wood, the lava ocean is a wood ocean. No error, ever.

This is the classic version-skew corruption, and it will happen the first time you reorder your roster, which will be the first time you add materials in bulk. You are pre-release with a handful of test saves — **this is the cheapest it will ever be to fix.**

**Recommended v3 format:** embed the material roster in the save and resolve by name:

```text
u32 material_count
per material: u8 id, u8 name_len, name bytes   (≤256 entries; names are your stable identity)
... existing v2 payload, ids as-is ...
```

On load:

1. Build saved-id → name map.
2. Resolve each name against the live table → live id.
3. Unknown name → reject with "save references material 'lava' not in current table" (a *loud* error, not silent reinterpretation).
4. Remap ids during the cell/temp read. Costs one u8 + len per material — tens of bytes total.

Alternative (weaker but cheaper): store a hash of the canonical table and reject on mismatch. That protects users but turns every legitimate roster addition into "old saves rejected" — the name-mapping version accepts roster *additions* and only rejects genuine removals/renames, which is the behavior you want during content expansion.

While you're in the header: store the **reaction-table hash** too (or at least its count). Reactions are not part of the world state, but they are part of the *continuation contract* — the same world under different rules diverges. Decide the policy explicitly: I'd reject on mismatch by default with a dev override, because "my save behaves differently after a rules tweak" is a bug report you want to be loud.

## 2.4 P1 — duplicate positions are silently accepted

Neither loop rejects duplicate keys; `insert_or_assign` makes last-writer-wins:

- A file with `(x, y) → sand` twice, second occurrence `water`, loads as water. No error.
- Same for duplicate temp positions.

Worse, `save()`'s sort tiebreak on material *masks* this: duplicates sort adjacently, the loader takes the larger id, and a subsequent re-save emits one clean entry. The format silently canonicalizes malformed input instead of rejecting it — the exact anti-pattern you just fixed in the JSON layer (duplicate keys → loud error). Same policy here:

```cpp
if (!loaded.insert(position, material_id)) return false;  // need an inserting-emplace that reports duplicates
```

(That may want a small `FlatMap::try_emplace`-style return, or check `contains` first on the load path, where perf is irrelevant.) Apply to both cells and temps. Add the four malformed fixtures to the test corpus: duplicate cell, duplicate temp, temp-on-air (below), unsorted-but-otherwise-valid.

That last one is a policy decision: `load()` currently accepts cells in any order. Strict-canonical loading (reject unsorted) buys tamper-evidence and makes "accepted ⇒ canonical" a clean invariant; it costs hand-editability. I lean strict, but either is defensible — *decide it and test it*, because right now the acceptance set is undefined.

## 2.5 P1 — temps-on-air is undefined policy

The header says heat may be set on any position "even air." The step prune deletes such entries before conduction. `save()` will happily serialize them; `load()` will accept them; the next step deletes them. So the same logical world saves differently depending on whether you saved before or after a tick, and "accepted payload" and "canonical payload" quietly diverge.

Now that movement carries heat (round 2's fix), heat-on-air has no remaining legitimate producer — products get their temperature *after* `set_cell`, and `stage_heat_carry` only fires on moves. I'd make the contract:

> **Temperature is cell-owned.** `set_temperature` on an unoccupied position is rejected (or ignored — rejected, so hosts learn immediately). The prune loop stays as a backstop. Canonical invariant: `temps ⊆ cells`.

That simplifies the save format's validation (reject temp entries whose position is absent from the cell set), closes the save-timing ambiguity, and is consistent with the round-2 payload-ownership fix. It *is* a behavior change to the public API — gate it behind the same v3 bump and note it in the migration doc.

## 2.6 P2 — the commit phase is not exception-safe, but nearly is

"Failed loads leave the world untouched" holds for every *validation* failure. But the commit sequence mutates members progressively: `rebuild_chunks(loaded)` rewrites `chunks_` *before* `cells_` is swapped. A `bad_alloc` mid-`rebuild_chunks` leaves a mirror that disagrees with `cells_` — not corruption of authoritative state, but a violated invariant and a stale renderer.

The fix is small and makes the claim literally true: build the new chunk map into a local, then commit with non-throwing moves:

```cpp
std::map<CellPosition, std::vector<MaterialId>> fresh_chunks;
// populate from `loaded`...
changed_chunks = union(keys(chunks_), keys(fresh_chunks));
chunks_ = std::move(fresh_chunks);   // vector moves: noexcept
cells_ = std::move(loaded);          // noexcept
temps_ = std::move(loaded_temps);    // noexcept
for (auto c : changed_chunks) mark_chunk_dirty(c);
tick_ = saved_tick;                  // last
```

(If/when `chunks_` becomes a dense directory per round 2, this gets simpler still.)

## 2.7 Encoding notes (all fine; two to document)

- **Endianness: solved.** Explicit little-endian byte assembly in both directions — no platform assumptions. Good.
- **Negative coordinates** round-trip via `uint32_t` wrap. Pre-C++20 that cast is implementation-defined; your build is C++20, where it's well-defined two's complement. One comment line, or zigzag-encode in v3 if you want to be pedantic.
- **`int16` temps** round-trip through `u16` including negatives, same C++20 note.
- **Clamp asymmetry**: `set_temperature` clamps to `[-32767, 32767]`, excluding −32768. Harmless, but it's an undocumented format property — one day someone "fixes" the clamp and changes nothing observable *except* that a boundary value becomes representable. Document or symmetrize now.
- **Version-in-magic-byte** caps you at 256 format versions inside an 8-byte magic. Fine forever, realistically — but when v3 adds a roster, consider moving version to a proper `u16` header field and keeping the magic purely as magic. Do it in the same bump; version-field churn is cheapest when you're already migrating.

## 2.8 Settling "byte-identical" vs. canonical-payload identity

Round 1 raised it; here is the resolution I'd commit to:

**The contract is canonical payload determinism. Define it as four testable properties:**

1. **P1 — serialization is a pure function:** `save(w)` depends only on authoritative state (cells, temps, tick, and — after v3 — table identity). No environment input, no iteration-order dependence. (Today's sort enforces this; §2.9 removes its cost.)
2. **P2 — round-trip on states:** `load(save(w))` restores an equal state (cells, temps, tick), and continuation equality holds: `stepⁿ(load(save(w))) == stepⁿ(w)`, byte-compared via save. You have this; keep it, and make one test cross a FlatMap capacity boundary so the layout-order concern in §1.5 is actually exercised.
3. **P3 — round-trip on payloads:** for every payload `b` that `load` accepts, `save(load(b)) == b`. **Note this is false today** for crafted non-canonical inputs (duplicates collapse; unsorted input re-sorts). After §2.4's strictness it becomes true by construction — which is exactly why duplicates should be rejected rather than tolerated.
4. **P4 — acceptance is defined:** the set of accepted payloads per version is exactly specified (canonical vN, plus legacy v1/v2), not "whatever doesn't trip a check."

Then: **the raw bytes save() emits are the canonical payload, and CI keeps asserting byte-identity of that payload.** That's cheap now and catches accidental nondeterminism (e.g., someone replaces the sort with an unsorted iteration).

When you eventually wrap the payload — checksums, compression — the *file* is a versioned container around the payload. File-level byte-identity becomes "deterministic given pinned library + level," which you may or may not assert; **the replay/determinism tests target the payload, always.** Concretely: pin compression parameters and forbid dictionary training (data-trained dictionaries change when the training corpus changes), or accept file-level variance and hash the payload into your replay logs. Decide that when it exists; don't let it contaminate the current contract.

One more thing the current format *doesn't* save and shouldn't: `dirty_chunks_` (correctly regenerated on load) and any RNG state (you have none — position-hashed rolls are the reason; that's a genuine structural win, guard it).

## 2.9 Format direction: kill the sort, align with the chunk future

`save()` is O(n log n) per call — sort of ~90k pairs, a few ms, fine for manual saves. But it's the wrong shape twice over:

1. Autosave-at-frequency plans (survival game!) will feel it as hitches.
2. The sort exists *only* because FlatMap iteration is unordered. Dense chunks (round 2 §13) are ordered by construction.

So design **v3 cell ordering as chunk-major, row-major within chunk** `(chunk_y, chunk_x, local_index)`. Deterministic with no sort, O(n), trivially produced by memcpy per chunk once storage migrates, and the loader's insertion order then *matches* live iteration order — which also happens to neutralize the §1.5 layout-divergence concern for free. While you're there: add extents (`u32 width, height`) with a defined policy for out-of-extents data (reject — you have no legitimate producer of far-flung cells), and a **CRC32 of the payload** so the autosave ring can distinguish "corrupt sector" from "old version" without a parse attempt.

This is a design sketch, not a mandate to build now — but it's the reason I'd *not* invest in micro-optimizing the current sort.

## 2.10 Migration policy (write it down once)

You've done v1→v2 correctly: readers for both, writer emits latest, v1 = ambient temps, failed loads are no-ops, magic distinguishes versions. Formalize it:

- Readers: v1…vN retained; writer: always latest.
- Each version bump gets: a fixture file in the repo, a load test, and a one-paragraph migration note.
- Unknown version → reject with the version number in the error.
- v3 (roster + ordering + extents + checksum) is the first bump that *rejects previously-accepted* malformed inputs — call that out in the notes so a craft-dependent test doesn't surprise you.

## 2.11 Serializer test & fuzz plan

Cheap, high-yield, and most of it is property-based rather than hand-written:

| Test | Property |
|---|---|
| **Truncation sweep** | For a canonical save, every prefix length: `load` returns false, world bitwise unchanged |
| **Bit-flip sweep** | For every byte × bit of a canonical save: either reject, or accept + world internally consistent (mirror == rebuild) + `save(load(b)) == b` (post-strictness) |
| Round-trip matrix | Empty world; single cell; full kiln scenario; hot-and-moving world (temps on in-flight cells — pairs with the round-2 heat tests); negative coords if legal; max-id material |
| Continuation across boundary | Save/load at a tick where `cells_` sits just above/below a FlatMap growth threshold |
| Skew tests (v3) | Load under modified roster → rejected or name-remapped; load under modified reactions → policy behavior |
| Policy fixtures | Duplicate cell, duplicate temp, temp-on-air, unsorted cells — one per accepted/rejected decision |

Fuzzing: the bit-flip sweep *is* a structured fuzzer for a 200-byte save; generalize with a byte-level mutator over a seed corpus of canonical saves, with the standing invariants (no crash; reject ⇒ unchanged; accept ⇒ resave canonical). That's ~30 lines on top of the harness you already have from the JSON fuzz targets — same build, same sanitizers.

---

# Part 3 — patch sequence

| # | Patch | Type | Exit criterion |
|---|---|---|---|
| 1 | FlatMap: never-shrink `reserve_for` + `shrink_to_fit`; probe-bound assert; fix the erase comment | Perf/hygiene | Capacity-thrash scenario stable; no behavioral change |
| 2 | FlatMap: document invalidation + iteration-order contract; add shuffled-insert determinism test | Contract | Test green on shuffled orders |
| 3 | Send/review `cell_types.hpp` hash; probe-length histogram | Verification | p95 probes ≤ ~3 at 50% load on representative world |
| 4 | Save/load: reject duplicate cell + temp positions | Strictness | Fixtures rejected; canonical acceptance unchanged |
| 5 | Temps-on-air policy (cell-owned heat) — pair with round-2 heat-transport fix | Behavior | Save-timing ambiguity gone; temps ⊆ cells invariant tested |
| 6 | v3 format: embedded material roster (name remap) + reaction hash + extents + version field | **P0** | Skew test rejects/remaps loudly; v1/v2 fixtures still load |
| 7 | Commit-phase exception safety (build-then-move) | Correctness | Contract "failed load ⇒ untouched" holds under bad_alloc injection |
| 8 | Truncation + bit-flip sweeps in CI | Testing | Green under ASan/UBSan |
| 9 | *(Later, with chunk storage)* chunk-ordered payload, CRC32, compressed wrapper | Format | P1–P4 properties re-asserted on v4 |

**Bottom line.** FlatMap: keep it, fix the shrink, document the two contracts, and stop suspecting it — the diagnosis remains algorithmic, and its cache-residency grace period is a reason to do the dense-chunk migration *sooner*. Save/load: the byte-level mechanics are sound, but a save format that can't recognize its own content table isn't a trust boundary yet — the v3 roster is the one change on this list I'd call mandatory before any save file you care about accumulates.

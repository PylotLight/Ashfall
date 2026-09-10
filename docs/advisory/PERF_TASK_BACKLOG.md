# Perf & Quality Task Backlog (from 2026-09-10 advisory review)

Source: `docs/advisory/2026-09-10-perf-quality-review.md` (§ references below).
Status for ALL items: **backlog, unscheduled** — do not start until scoped into a
phase plan (candidate: P4A Simulation quality and scaling). Order below follows the
reviewer's risk-adjusted sequence (§5–§16), NOT a claim about millisecond savings.

Global rules for this track: correctness commits separate from equivalent
optimizations; every optimization proves behavior identity (exact state, saves,
twin-world continuation); keep the data-only content gate (empty diff on
`simulation.cpp`/`bridge/` for content commits).

## Next advisory round prerequisites (needed before concrete patches)

- [ ] Assemble next review packet: full `engine/include/ashfall/simulation.hpp`,
      `engine/include/ashfall/flat_cell_map.hpp`, full `bridge/src/ashfall_world.cpp`,
      material/reaction struct definitions + validation, committed benchmark harness.
      (Bottom line — without `flat_cell_map.hpp` (clear policy, load factor,
      tombstones, hash, iteration) the container diagnosis can't be finalized.)

## Correctness first (before any optimization or new physics)

- [ ] **Heat-transport duplication bug (§3).** `stage_heat_carry` stages nonzero heat
      for destinations but never transports ambient zero, so a cold cell moving into
      a hot cell's old location duplicates heat (minimal repro: cold sand y=0, hot
      sand y=1, air y=2 — ends hot at y=1 AND y=2). In-flight infiltration swaps read
      temperature from the old coordinate, not the arrived particle — "clear sources"
      alone is an incomplete fix. Fix: next-state temperature representation moving
      with material ownership (place / move / swap-settled / relocate-in-flight).
      Tests required: hot→air, cold-follows-hot, hot-powder↔cold-liquid both ways,
      heated in-flight powder displaced by infiltration, all continued across
      save/load. Acceptance (§16.3): explicit new expected thermal results.
- [ ] **Reaction semantics decision (§4).** Current staging applies individual
      output-cell claims, not atomic reaction instances (two primaries sharing one
      neighbor reactant; half-applied X when the neighbor product loses). Decide:
      Option A (document as competing transformations) or Option B (transactional
      candidates with consume/catalyst/output claims). This is a BEHAVIOR change —
      separate commit, versioned expectations. Hard constraint either way: preserve
      failed-roll ownership (§4.3 — a fully-matching 1% rule suppresses lower rules;
      compiled candidate lists must keep original indices/order).

## Instrument (first)

- [ ] **Commit the benchmark harness + phase counters (§15, §16.1).** Port
      `/tmp/bench_step.cpp` scenarios (settled level, water basin, combustion,
      static box) into the repo with trajectory + repeatable-state modes, warm-up,
      median/p95. Record per-phase timings (reactions, clear/reserve, classify,
      ordering, powder/liquid/gas movement, both diffs, chunk cleanup, heat
      transport, pruning, conduction) plus counters (occupied/dynamic-by-type,
      moves/swaps, reaction candidates/hits, nonzero temps, changed positions,
      dirty chunks). Acceptance: reproducible baseline.
- [ ] **Bridge-side timings (§14).** Separately measure CPU paint vs.
      Image/packed-array copies vs. submission stalls vs. GPU transfer; confirm
      whether `ImageTexture.update()` uploads whole-image regardless of CPU dirty
      tracking. Fix the 6.8 ms HUD number's timer boundaries first — it's not
      comparable to step() timings today.

## Optimizations (risk-adjusted order)

- [ ] **Runtime material table (§6.1, §16.4).** `std::array<MaterialRuntime,256>`
      (movement/density/solid/valid) populated at table load; hot code indexes by id.
      Keep `std::map` for names/metadata/public queries. Acceptance: old/new state
      identical each tick.
- [ ] **Scratch reuse (§7, §16.5).** `cells.swap(scratch)` instead of move-assign in
      `order_dynamic_cells`; hoist `row_size`/`row_start`/`cursor` to retained
      storage; member scratch for reaction `hits` (sized by measured candidates, not
      blind `cells_.size()` reserve). Acceptance: same outputs, fewer allocations.
- [ ] **Compiled reaction matching (§6.2–6.4, §16.6).** Per-slot `bitset<256>`
      membership; per-material candidate rule lists keeping ORIGINAL indices/order;
      skip temp queries for gateless rules; evaluate rolls after full pair match
      (pure function — ownership semantics preserved). Acceptance: identical outcomes.
- [ ] **Exact static fast path (§8, §16.7).** Maintain exact non-static count through
      all mutation paths (`set_cell`, `fill_rect`, reactions, load/reset); after
      `run_reactions()`, if zero: tick++, conduct, return (reactions still run first;
      dirty chunks intact). Acceptance: static scenario avoids reconstruction. Note:
      solves scenario-D class only, not the general case.
- [ ] **Sparse conduction candidates (§10, §16.8).** Candidate set = nonzero thermal
      positions + 4-neighbors, restricted to occupied; keep the exact integer
      formula (dedupe before computing, no per-edge rounding changes). Plan the
      sparse/dense crossover (dense/chunk-local pass wins for widespread heat).
      Acceptance: identical conduction results.
- [ ] **Changed-position reconciliation (§9, §16.9).** Movement write paths register
      affected positions (sources/destinations/swap pairs) via helpers; diff only
      candidates, comparing final material identity (identical-material swaps stay
      clean). Debug-build full-reconstruction comparison. Plus: per-chunk occupancy
      counts (O(1) emptiness, fixes `fill_rect(air)` rescans) and dirty-set rework
      (per-chunk flag + dirty vector + tombstones; keep unbounded-negative-coord
      support — no fixed bitset). Acceptance: mirror equals full reconstruction;
      same observable chunk contents (§16.10).
- [ ] **Chunk-authoritative storage prototype (§11, §13, §16.11).** Sparse directory
      → dense per-chunk material arrays (+ optional thermal, occupancy/dynamic
      counts, activity metadata). Migration: oracle → chunk-backed query helpers →
      identical values → move authority → keep global order → remove global map.
      Activity domains stay separate (movement/reaction/thermal/presentation);
      conservative halo wake-ups; order-preserving active sets; differential replay
      proof (materials, temps, tick, saves, post-load continuation). Heuristic sleep
      = new policy, versioned as such — never "unchanged N ticks" as exact proof.

## Behavior revisions (separate track, versioned — §12, §16)

Not bugs-to-fix-silently; each is a deliberate semantics change with its own commit:

- [ ] Same-row liquid asymmetry (`wet_at` reads incomplete `next` row) — mirror-pool test distinguishing hash preference from schedule bias.
- [ ] Lateral-scan permeability (jumps past occupied nonsolids) — document intended rule per fluid pair.
- [ ] Thin-film spreading gate — consider per-material parameter vs. universal rule.
- [ ] U-tube equalization — no general pressure mechanism today; don't call lateral flow "pressure".
- [ ] Infiltration as explicit material-pair capability (permeability/rate/affinity) instead of universal.
- [ ] Gas schedule constraints (vacancy-only entry) — later gas-specific update design.
- [ ] `stayed`-mark meaning after displacement (coordinate vs. owner vs. occupant) — multi-density competing-swap test.

## Deferred / explicitly not now (reviewer's list, §16 + bottom line)

Multithreading, GPU sim, new movement rules, full pressure/fluid solvers, per-cell
Godot nodes/lights, arbitrary rigid-body fragmentation — all after the above.

## Test-hardening (from 2026-09-10 test-suite review — LAST open item, now closed)

Source: `docs/advisory/2026-09-10-test-suite-review.md`. Headline: keep justified
exact counts; fix weak oracles (T1–T6 first, no sim changes); label content/golden
contracts separately; add continuation + adversarial-history fixtures before
chunk migration. Standing decisions: parser-overflow stays rejected (not reopened);
heat-swap fixtures belong to the existing heat-correctness task (no duplicate bug).

- [ ] **T1–T6 oracle fixes (test-only patch).** T1 slope drainage → escape-route +
      enclosed-pocket fixtures (never bare `== 0`); T2 shadowing → assert
      `occupied == 1` + wood survives (HIGHEST-VALUE tiny fix — current form passes
      with the immortal-fire bug via rising); T3 quiescence → semantic-state
      compare over a sustained window + filled-bottom-row assert, drop sliced-save
      coupling (v3-unsafe); T4 flattening → genuinely uneven initial fixture,
      bottom-contiguous columns, empty columns included, remove unused save;
      T5 reset dirtiness → clear-first then `{{2,0}}` membership; T6 oil flash →
      split p=100+explicit-temp behavior vs. named 90% first-tick golden vs.
      production-row integration (no casual multi-tick loop — eligibility moves).
- [ ] **Contract labeling + thermal decision.** Mark content/golden pins (spawn,
      pit 19+2, 90%-first-tick, 200-tick extinction, kiln geometry) as scenario
      locks distinct from engine correctness; add material histograms where walls
      inflate counts; decide clamp range symmetric [-32767,32767] (fix message) vs.
      full int16 (fix expectation).
- [ ] **Strengthen strong areas.** Per-tick conservation + wall preservation +
      global count; `expect_rejected` extended to dirty tracking + heated worlds;
      remapped-ID paired normalized-state comparison; missing-chunk `all_of` +
      `copy_rect` over mixed regions with non-air-prefilled buffer.
- [ ] **Adversarial determinism.** Growth/collision-scale key sets; reversed +
      permuted insertion; grow-then-delete histories; snapshot equality pre-step +
      per-tick compare; multi-primary competing reactions; translation ⇒
      invariants only (hash decisions may legitimately differ).
- [ ] **Save/load continuation.** Build → N ticks → twin via save/load → M ticks,
      compare each tick (reactions, temps, swaps, negative chunk boundaries; twin
      preloaded with identical material/reaction config). Best migration test per
      line of code.
- [ ] **Loader honesty.** Invalid table load on POPULATED world (cells/heat/
      reactions/tick compared after); rejection proves old quench still behaves
      (not just count == 1); parser empty-selector cases; "canonical" literals
      renamed to frozen fixtures (reaction literal is 8 rows vs. 9 production —
      missing oil-flash) + real-file integration test via test-data path.
- [ ] **Save-test isolation pre-v3.** Version-specific group (v1 hand-build, empty
      v2 size, `size()-5` mutation); v3 framing-vs-semantic rejection split;
      independent hand-built fixtures (never only `save()`-generated).
- [ ] **Migration additions table (§7).** cell/copy_rect/read_chunk agreement;
      counter-vs-scan equality; last-cell deletion; reset/load dirtiness cleared;
      continuation; history independence; hot/cold cross-chunk swaps; competing
      reactions; gate boundary + p=0/p=100; span-size + negative-coord cases.

## Bridge + host follow-ups (from 2026-09-10 bridge/host review)

Source: `docs/advisory/2026-09-10-bridge-host-review.md`. Headlines: no partial GPU
uploads (dirty tracking = CPU paint only); fix shading-halo + shader coverage/color
mismatch before lighting; present once per frame; controller owns gameplay rules
today (temporary playground layer — engine session layer before survival play).
Test-file audit still open (send `tests/simulation_tests.cpp`).

- [ ] **Correctness first.** Shading-invalidation halo (dirty chunk ± vertical
      neighbors, or pixel-rect +5 rows; renderer expands dirtiness, sim stays
      unaware) + byte-compare-vs-full-repaint regression test (insert/delete/
      liquid↔solid, across partial/full threshold); shader: remove dilation,
      establish correct filtered color/alpha, then reassess smoothing (if dilation
      stays: propagate color w/ coverage, coherent alpha, liquid-liquid meetings,
      no painting over solids); split texture recovery from world reset (recovery
      must not mutate sim; rebind controller refs or signal).
- [ ] **Cadence + instrumentation.** Present at most once per rendered frame
      (accumulate dirty across ticks/edits; sync helper stays for tests); split
      `last_step_ms_` into sim/dirty-collect/material-read/paint/set_data/submit
      + updates-per-frame + dirty pixels + logical bytes (GPU cost separately);
      early-return before `ptrw()` (CoW trap); visible-set filtering (offscreen
      dirt must not trigger repaint/upload); contiguous-scratch-tile+halo vs.
      per-cell-query benchmark for the partial path.
- [ ] **Contracts.** Documented sole dirty consumer (bridge presenter; GDScript
      `get/clear_dirty_chunks` must not preempt it); engine clearing tests
      (final-cell deletion, reset, load old+new regions, `copy_rect` air
      overwrite); bridge validation (reject out-of-range/unknown ids BEFORE u8
      narrow; checked dims arithmetic + limits before alloc; finite sim-rate
      range; fix 30 Hz comment); host save envelope (14-byte header reality;
      `sim_len` vs. remaining + cap before `get_buffer()`; trailing-byte policy;
      finite in-domain coords; metadata validated before engine commit; material
      by stable NAME; atomic temp-file write).
- [ ] **Input/UI.** Resolve `S` move-down vs. single-step conflict; reject
      outside-image clicks before brush clamp; focus-loss/release-outside drawing
      reset; throttle `_update_marker`; transient save/load status separate from
      HUD refresh.
- [ ] **Lighting prototype (after correctness).** Keep two-layer renderer; solid
      masking; data-driven visual profiles per round-1 sketch (NOT id branches);
      one light + emissive treatment first; decide canvas-lighting vs. custom
      field (never recover material properties from grain-modified RGB).
      Scene: world-space Node2D + UI layer eventually; HUD/editor separated.
- [ ] **Player authority migration (P4B track).** Engine/session layer owns
      position, swept (not endpoint — tunneling at 90 cells/s) collision,
      dig/place eligibility, persistence; GDScript sends intent at sim boundaries.
      Whole-game determinism stays open until then.
- [ ] **Presentation tests (§F 1–7).** Partial==full repaint; deletion/reset/load
      clearing both layers; no-work ⇒ no mutation/submission; multi-tick ⇒ one
      flush; texture recreation preserves world; shader fixtures (drop, channel,
      border, two liquids); host-save truncation/lengths/floats/remap.
- [ ] **Test-file audit round (still open).** Send `tests/simulation_tests.cpp`;
      apply the §F taxonomy (golden locks vs. conservation proofs vs.
      schedule locks); keep small labeled golden layer + invariant/scenario tests.

## FlatMap + save/load follow-ups (from 2026-09-10 review)

Source: `docs/advisory/2026-09-10-flatmap-saveload-review.md` (primary; the competing
response contributed only the addenda marked [R2] below). Headline verdicts: FlatMap
algorithm correct (traced proofs), keep it; save/load byte-mechanics sound but the
format can't recognize its own content table (v3 roster = mandatory before saves you
care about accumulate). Strategic note: FlatMap numbers are flattered by LLC
residency (~7–8 MB working set); 4× world → ~3× per-op cost with no code change —
do the dense-chunk migration BEFORE that cliff, not after.

- [ ] **F1. FlatMap hygiene.** Never-shrink `reserve_for` + explicit
      `shrink_to_fit()` (load/reset paths only); debug-only probe-bound assert
      (hang → loud failure); fix "step() never erases" comment (thermal prune DOES
      erase — or restructure prune into build-survivors-and-swap); preserve hasher
      across `rehash`; power-of-two asserts. Exit: capacity-thrash stable, no
      behavior change.
- [ ] **F2. FlatMap contracts.** Document pointer-invalidation (any insert may
      rehash) + iteration-order-is-layout rule (load() repopulates in sorted file
      order → different layout than the saving session; all consumers currently
      order-independent — write that as an invariant); shuffled-insert determinism
      test (same keys, shuffled orders → identical sorted projections). Exit: green.
- [ ] **F3. Scratch sizing from working set.** `stayed` from dynamic count, thermal
      scratch from `temps_.size()*5`, reaction `hits` from candidate estimate —
      not `cells_.size()`. Exit: scenarios D/A show reduced clear/reserve cost.
- [ ] **F4. Hash verdict (needs `cell_types.hpp`).** Probe-length histogram on
      representative world (want p95 ≤ ~3 at 50% load on dense block); grid data is
      the worst case for weak multiplicative hashes. [R2] No seeded/keyed hashes
      (would break determinism assumptions); splitmix64-class fixed mix if the
      current hash clusters. Exit: measured, not theorized.
- [x] **S1. Save/load limits + duplicates.** Format caps (file bytes, cell/temp
      counts, coord bounds — validate count BEFORE `reserve_for`), duplicate cell +
      temp rejection (needs `try_emplace`-style insert or pre-check; load path so
      perf is irrelevant), unsorted-acceptance policy DECIDED + tested. [R2]
      Sparse-chunk bomb: cap distinct chunks on load (count first, reject over —
      N far-apart cells × 4 KiB vectors is the hole the "no thin-file blowup"
      proof missed) + dirty-set growth bound. Exit: oversized/duplicate fuzz inputs
      reject without large allocs.
- [ ] **S2. Magic cleanup + diagnostics + golden bytes.** Prefix/version constants,
      `load_with_diagnostic()` error enum for tools/fuzz triage. [R2] Golden-bytes
      test: hand-computed LE vectors (`x=-1 → FF FF FF FF`, `tick=0x0102… →
      08 07…`, temp −1/32767) locking the format. Exit: v1/v2 still load, failures
      named.
- [x] **S3. Canonical-payload contract (SETTLED — adopt P1–P4).** P1 serialization
      purity (sort enforces today); P2 state round-trip + continuation equality
      (add a FlatMap-capacity-boundary crossing test for the §F2 layout concern);
      P3 payload round-trip (`save(load(b)) == b`, false today for non-canonical —
      becomes true via S1 strictness); P4 defined acceptance set. CI asserts
      byte-identity of the canonical payload. Compression later = versioned
      container around the payload (pinned params, no trained dictionaries) —
      replay tests target the payload, always. Exit: properties tested.
- [x] **S4. v3 format design (DO FIRST as design, implement after S1–S3).**
      Embedded roster (id + name → REMAP by name on load; chosen over hash-only
      because roster additions keep working, only removals/renames reject) +
      reaction-table hash/count (continuation contract — reject-on-mismatch default
      w/ dev override) + extents + proper version field + chunk-major/row-major
      ordering (kills the sort AND neutralizes layout divergence) + CRC32.
      Temps-on-air → cell-owned heat (`temps ⊆ cells`, reject set-on-air; pair with
      the heat-transport fix; prune stays as backstop) gated behind this bump.
      Migration policy: readers v1…vN retained, writer latest, fixture + test +
      paragraph per bump; v3 is the first bump that REJECTS previously-accepted
      inputs — call it out. Exit: skew tests loud, v1/v2 fixtures load.
- [x] **S5. Commit-phase exception safety.** Build chunk map into a local, commit
      with non-throwing moves, tick last — "failed load ⇒ untouched" holds even
      under `bad_alloc`. Exit: holds under injection.
- [x] **S6. Serializer sweeps in CI.** Truncation sweep (every prefix → false +
      unchanged) + bit-flip sweep (reject, or accept-consistent + resave-canonical)
      + round-trip matrix (empty/single/kiln/hot-moving/negative/max-id) +
      continuation-across-boundary + v3 skew + policy fixtures. [R2] Loader fuzz
      targets S1 (crash/DoS) + S2 (round-trip differential on generated worlds) —
      same build/sanitizers as JSON fuzzing. Exit: green under ASan/UBSan.
- [ ] **Comment-level fixes (batch with above).** `count*9` size_t cast is
      load-bearing (comment it); C++20 two's-complement coordinate/temp wrap
      (comment it); clamp asymmetry −32767..32767 (document or symmetrize);
      2.2× transient memory multiplier on load (comment; streaming later).

## Hash verdict + migration prep (from 2026-09-10 hash/API review)

Source: `docs/advisory/2026-09-10-hash-api-review.md`. Headlines: hash has no
correctness defect but F4 stays open (no p95 claim from inspection); reserve by
peak distinct entries; public API does NOT block chunk-authoritative migration or
entity ownership — only contract gaps to close.

- [ ] **F4 probe instrumentation (keep open).** Count every examined slot incl.
      terminating empty on miss; split hit/miss/insert/update (means, not one
      blended p95 — theory says ~1.5 hit / ~2.5 miss at 50% load). Workloads:
      dense 64²/256²/512², translated + negative-coord regions, H/V strips,
      `{64i+ox, 64j+oy}` patterns (PROVEN low-4-bit correlation: same
      within-chunk offset ⇒ identical low 4 hash bits ⇒ 1-in-16 buckets for
      chunk-aligned sparse features), actual hot-path key sequences in real
      traversal order. Report occupancy + p50/p95/p99/max + probes/tick. If hash
      disappoints: 32-bit finalizer or packed-64 mix candidates — measured, and
      any change must preserve deterministic outcome tests; iteration must never
      arbitrate simulation or save order.
- [ ] **Exact reserve-size pass.** Needs only already-sent files
      (`simulation.cpp` phase loops + `flat_cell_map.hpp` growth threshold incl.
      the ==50% edge: `bit_ceil(2E)` vs `bit_ceil(2E+1)`); no new packet needed.
      Targets per the N/Nnext/D/Hpeak table in the review; both sides of swapped
      pairs; distinct entries not probes; never reserve whole-world extent.
- [ ] **FlatMap `find_or_insert`.** `{ptr, inserted}`, no overwrite on hit, one
      probe walk at adequate capacity, no gratuitous growth on hit, documented
      invalidation, immutable keys; keep `insert_or_assign` separate (claim vs.
      overwrite are different ops); temps must not materialize ambient zeros on
      read. Tests: hit, insert, threshold growth, collision chains, invalidation.
- [ ] **Migration-prep contracts (docs only, no redesign).** `has_chunk` vs.
      `active_chunks` (stable world-facing meaning + separate internal work set;
      heat-on-air means "no occupied cells" ≠ "no state"); dirty-stream spec
      (final-cell removal, reset/load invalidation, missing-chunk reads, what
      temperature changes dirty, `mark_chunk_dirty` = presentation vs. scheduling
      — decouple); coordinate/buffer contracts for `fill_rect`/`copy_rect`/
      `read_chunk`/`can_move_to`/`set_cell` (endpoints, reversed ranges, coord
      range, floor-division negatives, row-major lengths, failure/untouched
      guarantees, overflow-safe extents); `can_move_to` documented as terrain-only
      (entities compose later in engine-level session layer, NOT in chunk
      storage). Small cleanups in the same pass: public temperature comment drift
      ("no advection in P2.4" vs. actual carried heat — separate from the
      duplication bug); verify C++17-vs-20 claim (span, `<=>`) and narrow if stale.

## Design clarification to carry forward (from §2 of this review + round-1 §2)

"Engine owns simulation" must eventually include gameplay state beyond cells (or state
explicitly what stays non-authoritative); "data-only material" standard refined to:
new INSTANCES of existing behavior = data-only; new BEHAVIOR CATEGORIES may extend
the engine with contracts + tests (don't grow the reaction schema into a programming
language to protect an empty diff).

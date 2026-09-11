# Advisory Review Handoff — current state + next item

Paste this (plus the files listed under "Next packet") into a fresh chat to continue
the advisory-review thread without the full history.

## Project state (2026-09-10, tree clean at `260b4ff`)

Godot 4.7 + portable C++17/20 cellular sim (falling-sand survival RPG, Noita-inspired).
P0 (bridge), P1 (deterministic world), P2 (data-driven materials/reactions) complete
and verified — all 7 suites green (sim tests, P0/P1/P2 headless verifies, visual sim,
60-frame boot). 10 materials, 9 reaction rows, sparse int16 temps, save v2. Sim step
~4–7 ms for 512² in Release on Apple Silicon. Strict contracts: bitwise determinism
(hash rolls, no RNG), engine owns sim (no Godot types), data-only content additions
(empty-diff gate on `simulation.cpp`/`bridge/` per content commit).

## Advisory history (all filed under `docs/advisory/`)

1. **Visual direction** → `2026-09-10-visual-direction-review.md` (round 1,
   filed). Lighting path A→C, animation policy, `material_visuals.json` sketch,
   P3A/P3B proposal, burning-cave benchmark. Direction source for P3 scoping.
   (Our brief that prompted it: `docs/ARCH_ADVISORY_BRIEF.md`.)
2. **Perf round** → `PERF_REVIEW_PACKET.md` (our benchmarks) →
   `2026-09-10-perf-quality-review.md` (verdict: problem is sparse-storage
   rebuild/reconcile per tick, not physics; + heat-duplication bug §3, keep
   separate). Tasks → `PERF_TASK_BACKLOG.md`.
3. **JSON round** → `2026-09-10-json-parser-review.md` (verdict: careful but
   unhardened; resource limits + unknown-key strictness + reactant-exclusivity fix
   first; NO overflow bug — do not file one). Tasks → `JSON_PARSER_BACKLOG.md`.
4. **FlatMap + save/load round** → `2026-09-10-flatmap-saveload-review.md`
   (Response 1 chosen over Response 2; R2 kept only as addenda). Verdicts: FlatMap
   correct (traced proofs), fix shrink + contracts; save/load byte-safe but needs
   v3 roster (P0) + limits + canonical contract (P1–P4 adopted). Tasks → appended to
   `PERF_TASK_BACKLOG.md`. Strategic warning banked: LLC residency flatters current
   numbers (~7–8 MB); 4× world → ~3× per-op cost — dense-chunk migration before the
   cliff. Open question both reviewers asked for: `cell_types.hpp` hash.
5. **Hash/API round** → `2026-09-10-hash-api-review.md`. Verdicts: hash sound but
   F4 stays open (proven low-4-bit correlation on chunk-aligned sparse patterns —
   benchmark must include `{64i+ox}` workloads); reserve by peak distinct entries;
   public API migration-friendly, contract gaps only (dirty consumption, chunk
   existence, coordinate/buffer semantics, `can_move_to` as terrain test). Tasks →
   `PERF_TASK_BACKLOG.md` hash/API section.
6. **Bridge/host round** → `2026-09-10-bridge-host-review.md`. Verdicts: no
   partial GPU uploads (CPU-paint only); fix shading halo + shader coverage/color
   before lighting; present once per frame; controller owns gameplay rules today
   (temporary — engine session layer before survival play). Test-file audit still
   open. Tasks → `PERF_TASK_BACKLOG.md` bridge + host section.
7. **Test-suite round** → `2026-09-10-test-suite-review.md` (FINAL review item —
   all layers covered). Verdict: good regression base, weak oracles (T1–T6 fix
   list, no sim changes); keep justified exact counts; label golden/content pins;
   add continuation + adversarial-history fixtures pre-migration. Tasks →
   `PERF_TASK_BACKLOG.md` test-hardening section.

## Key decisions already settled (don't relitigate)

- Canonical-payload identity P1–P4 adopted for saves; v3 = name-remap roster (not
  hash-only), + reaction hash, extents, CRC, chunk-major ordering.
- "Data-only" refined: new INSTANCES of existing behavior = data-only; new BEHAVIOR
  CATEGORIES may extend the engine with contracts + tests.
- No parser replacement until hardened; no multithreading/GPU-sim/new movement
  rules until the backlog's correctness + instrument steps land. No multiplayer,
  permanently off the table.

## Next: implement (advisory phase complete)

All layers reviewed. Execution order from the backlogs:

1. **Group 1 — instrument + contracts:** DONE (`da5e170` harness/counters/F1/S2/bridge
   timers; `656e938` T1–T6 + golden bytes + contracts doc).
2. **Group 2 — correctness, versioned:** DONE (`ad48a29` R pins → `107a760` find_or_insert
   → `1cbcee7` v2-strict + commit safety → `8bcd5fd` loader validators → `8d64884`
   cell-owned heat + `405baf7` ambient fast path → `1fc2458` pins; P1 still 51840).
3. **Group 3 — hardening + formats:** DONE (`e8cf03a` JSON P1/P2/P4/P7/P8 + hyphen
   identifiers → `2dc9c79` save v3 → `41762cb` presentation/cadence/host →
   `efa3200` batteries + v3 goldens).
4. **Group 4 — measured optimizations:** DONE (`e6052f5` oracle → `2758afb` runtime
   table + scratch/reserves → `96bc8b8` tile repaint 8.7× → `b891e65` compiled
   matching → `0a3136e` static fast path → `ae0cc73` sparse conduction + reconcile).
   Closing bench: A 3.46 / B 1.68 / C 0.24 / D 0.00 ms/step.
5. **Group 5 — storage migration:** DONE (`29e3673` vsync probe V; `607eefd`
   chunk-authoritative cells + shift/mask math (1024² dynamics −7% vs FlatMap);
   `ab8325e` §7 gaps + F2 + bench E).
6. **Group 6 — fuzzing + sanitizers (CI skipped, local-only):** DONE (`cfff9e5`
   FUZZ=ON wiring + probe + README → `1ca7cee` LLVM `<algorithm>` fix →
   `1d32a2b` 3 JSON targets + dict + 62 seeds → `fbc9603` 2 structured targets →
   `c8e5d3e` 2 save targets + 20 seeds → `a0672c3` offline differential +
   verdict shim). Zero findings: 60 s smokes clean (~2.3M struct fills, ~1.3M
   JSON runs), sanitizer suite 2/2 on LLVM + Apple Clang, differential 357/357.
   Notes: LLVM clang 23 via brew required (Apple Clang can't link libFuzzer);
   run fuzzers against /tmp corpus copies (never the repo dir); CI job
   (.github/workflows) deferred — no push planned.

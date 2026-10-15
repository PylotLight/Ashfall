# E1 split timers + benchmark worlds — baseline record (Slice E, task E1)

Machine: Apple Silicon (arm64), native release (`CMAKE_BUILD_TYPE=Release`),
Godot 4.7.2, GDExtension rebuilt in-tree. Date: 2026-09-12.
§17 budgets are for validation only (copied at the bottom).

## Stepping model (so HUD figures are interpretable)

- **Fixed-60 stepping, NOT per-frame.** Sim advances only in
  `AshfallWorld._physics_process` via an accumulator at `sim_rate_hz = 60`
  (default), at most `max_steps_per_physics_frame = 2` steps per physics
  callback; excess backlog is dropped (never spiral). Render rate never
  changes sim speed (`KEY_V` vsync toggle only caps display).
- **Present is decoupled:** exactly one `flush_present()` per rendered
  `_process` frame; sim steps and edits only accumulate dirtiness.
- **HUD `sim` ms = last `world_.step()` span ONLY** (sim, no present) —
  directly comparable to `bench_step` / `bench_e1` ms/step. Present cost is
  separate (`dirty / read / paint / setdata / submit`, see tooltip).
- **HUD `frame` ms** = render-frame delta EMA (includes vsync cap).
- Manual stepping (`N` key / Step button) = one synchronous `step()`.

## CPU split mapping (§10 → measured phase)

| §10 | Phase(s) | Notes |
|---|---|---|
| reactions | `reactions_ms` | run_reactions pre-pass |
| powder / liquid / gas movement | `powder_ms` / `liquid_ms` / `gas_ms` | per-arm dispatch |
| temperature | `heat_carry_ms` + `conduct_ms` | advection staging + conduction |
| active-region maintenance | `clear_reserve_ms` + `classify_ms` + `ordering_ms` + `reconcile_ms` | scratch / dynamic-set / chunk-reconcile; the engine has NO separate active-region system |
| snapshot extraction | bridge `material_read_ms` (`copy_rect`) | |
| image prep | bridge `paint_ms` + `set_data_ms` | |
| upload submission | bridge `submit_ms` (`ImageTexture::update` call span — CPU-side, NOT GPU execution) | |
| entities | N/A (0) | engine owns no entities; avatar is host-side GDScript |

GPU splits (base world / liquid pass / lighting / bloom / particles /
composite) are **not measurable from any engine/bridge/GDScript surface**
in this build (no per-pass GPU timestamps; headless runs the dummy driver).
Passes present in the cave: base TextureRect (nearest), water overlay
(linear + shader), 6× PointLight2D rig + glow decal, WorldEnvironment
glow-bloom, letterbox composite; **no particle pass exists in P3A**.
Per-pass GPU cost needs an on-screen Xcode GPU frame capture.

## Benchmark worlds (`bench/bench_e1.cpp`, 512², per-tick samples)

`./ashfall_bench_e1 --ticks 300 --repeats 3` → 900 pooled tick samples per world.

| world | median | p95 | worst | mean | R | M (p/l/g) | T | A | occupied | temps |
|---|---|---|---|---|---|---|---|---|---|---|
| empty | 0.000 | 0.000 | 0.003 | 0.000 | 0 | 0 | 0 | 0 | 0 | 0 |
| static terrain | 0.000 | 0.000 | 0.002 | 0.000 | 0 | 0 | 0 | 0 | 56424 | 0 |
| falling sand (40k) | 6.421 | 6.891 | 10.107 | 6.105 | 0.000 | 1.591/0/0 | 0 | 3.377 | 56384 | 0 |
| moving liquid (~66k) | 22.065 | 23.191 | 35.783 | 21.790 | 0.000 | 0/19.199/0 | 0 | 0.949 | 98734 | 0 |
| broad combustion | 0.472 | 2.649 | 3.018 | 0.828 | 0.107 | 0/0.182/0.041 | 0.076 | 0.372 | 28107 | 0 |
| broad temperature | 15.538 | 16.907 | 19.629 | 15.532 | 0 | 0 | 15.532 | 0 | 90601 | 48148 |
| repeated explosions | 2.069 | 3.271 | 3.745 | 2.235 | 0.116 | 0.651/0.175/0.040 | 0.117 | 0.784 | 57818 | 15 |
| chunk-boundary traversal | 9.988 | 10.924 | 11.970 | 9.844 | 0.000 | 0.535/5.353/0 | 0 | 2.644 | 68424 | 0 |

Phase columns are means over the 900 samples; median/p95/worst are wall
ms/step. Findings: liquid movement dominates the moving-liquid world
(~19 ms alone — over the 60 Hz frame on its own); conduction over 48k
hot cells costs ~15.5 ms; reconcile (changed-position sort+unique) is the
largest term in the sand world (3.1 ms); the combustion front consumes its
fuel within ~100 ticks (median reflects the burned-down tail, p95 the
active front). Explosion drivers inject between (never inside) timed steps.

## Cave live splits (headless, real scene, 120 ticks)

`godot --headless --path . -s /tmp/e1_cave_splits.gd` (throwaway, uncommitted):
sim **median 2.743 / p95 3.005 / worst 3.125 ms**;
CPU groups R 0.193 / M 1.065 (liquid 1.046 — the reservoir) / T 0.000 /
A 1.397 (`classify` 1.370 — any dynamic cell forces the full 163k-cell
chunk-walk; cf. the all-static bench world at 0.000);
present means dirty 0.000 / read 0.001 / paint 0.071 / setdata 0.001 /
submit 0.001 ms, worst-sum 0.399; 57/121 flushes painted (fire shimmer is
shader-side, so settled frames submit nothing).
HUD line renders: `2.97 ms (R 0.20 M 1.08 T 0.00 A 1.61)`.

## Validation vs §17 provisional budgets

| Subsystem | Budget | Worst observed (this baseline) |
|---|---|---|
| cellular sim, representative active | 4–7 ms CPU | cave 3.1 ✓; heavy worlds exceed (liquid 35.8, temp 19.6 — stress worlds, not representative scenes) |
| gameplay/entities | 1–2 ms | 0 — none in engine ✓ |
| snapshot prep/submission | < 1–2 ms CPU | 0.4 worst-sum ✓ |
| world/material rendering | 1–2 ms GPU | uninstrumented (headless) — needs on-screen capture |
| lighting + bloom | 1–3 ms GPU | uninstrumented — needs on-screen capture |
| particles/composite | bounded | no particle pass; composite uninstrumented |

## Files (uncommitted, per contract)

- `CMakeLists.txt`: `ASHFALL_STEP_INSTRUMENT` PUBLIC on `ashfall_sim`
  (write-only timers; zero sim-rule change — P1 still 51840) + `ashfall_bench_e1` target.
- `bridge/include/ashfall_world.hpp`, `bridge/src/ashfall_world.cpp`:
  `PresentPhases` gains the engine `StepTimings`/`StepCounters` snapshot;
  new `get_sim_phases()` / `get_sim_counters()` bindings.
- `bench/bench_e1.cpp`: the 8 worlds above (new file; `bench_step.cpp` untouched).
- `scripts/main_controller.gd`, `scripts/cave_benchmark_controller.gd`:
  sim-label split text + stepping-model tooltip only.

## Suites (all post-change)

`ctest` 2/2 · `godot_p1_verify` (51840) · `godot_p2_verify` ·
`godot_headless_verify` · `godot_presentation_verify` ·
`godot_presentation_toggles_verify` — all green.

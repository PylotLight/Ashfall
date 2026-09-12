# P3A exit gate — milestone record (Slice E, task E3)

Date: 2026-09-12. Machine: Apple Silicon (arm64), native release, Godot 4.7.2
(same baseline machine as Slice E task E1).
Gate suites: `tests/godot_p3a_fixtures_verify.gd` (E2, 132 PASS) +
`tests/godot_p3a_exit_verify.gd` (E3, 105 PASS), both green headless 2026-09-12.
Full battery (all Godot verifies + `ctest` 2/2) green — see §Battery below.

Headless limit, stated honestly: headless drivers cannot execute canvas
shaders, so C1–C3 are proven at the deterministic-INPUT level (scene content,
cluster/light state, LUT bytes, toggle states, sim-bytes identity) — the
contract the E2 suite pins. Rendered pixels and the clip are human captures
(§Human capture list). No pixel assertion below would pass headless AND mean
anything.

## Per-criterion evidence (§16 P3A)

| # | §16 criterion | Headless evidence (this gate) | Human remainder |
|---|---|---|---|
| C1 | Scene reads as a place at normal play scale | Cave boots headless: 512×512 sim, tick=60 after deterministic 60-step settle, occupied=**163348** (was 163432 before the R7 sawtooth fix replaced the
row hash — same amplitude/range, decorrelated deltas; content count shifted,
engine sim untouched, P1=51840 holds); content samples (1-in-4 cells) fire 39 / water 703 / wood 935 / oil 99 / stone 3955 / rock 35175 / sand 54; player placeholder at CAVE_SPAWN (240,330); resolution ladder metadata sim/world_render/lighting/display/ui on scene root; `run/main_scene` = cave (boot default), demo one key away; view textures bound (E3/C1, E2/A) | Silhouette/character/viewport ratios + before/after play-scale read — capture 1 |
| C2 | Fire visibly changes nearby surfaces | Live cave yields **2 emissive clusters, weight 375.0**; cluster anchor snapped to open space (never lit-black stone); 5× `_update_light` retarget frames drive the 6-rig Stage-A lights to energy > 0; emission inputs fire=2.5 / lava=1.5 / water=0.0 exactly (hot water cannot glow by construction); solids receive light while blocking transmission (E3/C2, E2/A) | Fire-lit vs unlit surface A/B at same paused tick — capture 2 |
| C3 | Materials distinguishable with bloom disabled | Bloom node present, off-state takes, on round-trips (KEY_B runtime path target), boot default ON; bloom-OFF LUT proof: stone/rock, sand/wood, water/oil palette rows pairwise distinct (q8 distances > 10/10/30); stone vs rock CPU-mirror evals differ — all bloom-independent inputs (E3/C3, E2/C §F extension) | Bloom on/off A/B pair at same paused tick, boundaries unwashed — capture 3 |
| C4 | Presentation toggles leave sim hashes identical | Compact gauntlet on the LIVE cave: bloom off/on, follow off/on, zoom in/out ×3, 10 light-retarget frames, clock run/pause/forced — save-bytes + tick + occupied identical at every step; negative control (set_cell sand) goes red. Authoritative proof: `godot_presentation_toggles_verify.gd` (D2) green — every toggle incl. camera/light/clock/flush paths, P1=51840 holds, same negative control (E3/C4 + D2 battery) | None (fully headless) |
| C5 | Pause/replay animation behavior documented | Pause freezes world clock exactly; `set_forced(tick,phase)` pins replay values, `tick_world` no-op while forced, `reset()` zeroes on rebuild; contract documented in `scripts/presentation_clocks.gd` header (world ≠ UI clock, no blanket TIME, forced-capture protocol) and exercised by the D2 suite (E3/C5 + D2 battery) | None (fully headless); replay uses forced-tick captures — capture 4 protocol |
| C6 | Performance measured on the target Mac | `docs/perf/e1-baseline.md` on this machine: fixed-60 stepping model stated; cave live sim **median 2.743 / p95 3.005 / worst 3.125 ms** (R 0.193 / M 1.065 / T 0.000 / A 1.397); present worst-sum 0.399 ms, 57/121 flushes paint; stress worlds pinned (liquid worst 35.783, temp worst 19.629 — not representative scenes); §17 validation table; GPU splits honestly uninstrumented — need on-screen capture (E3/C6 presence + number pins) | On-screen GPU frame capture (world/lighting/bloom/composite splits) — capture 5 |

Plan-level gate ("clip reads as a game in a burning cave"): human only — captures 1–4.

## Battery (post-gate, 2026-09-12)

- `godot_p3a_exit_verify.gd` (E3, NEW): 105 PASS, 0 FAIL.
- `godot_p3a_fixtures_verify.gd` (E2): 132 PASS, 0 FAIL.
- `godot_presentation_toggles_verify.gd` (D2): green.
- `godot_headless_verify`, `godot_p1_verify` (51840), `godot_p2_verify`, `godot_presentation_verify`: green.
- `ctest` (build/): 2/2 green.

## Human capture list (the clip — cannot be captured headless)

Setup for every A/B pair: boot the cave in-editor, pause (auto_step off),
force the capture tick via `set_animation_override(N, 0.0, 1.0)` + clocks
`set_forced(N, phase)` so both frames are the identical deterministic frame;
camera fixed (follow off) between the pair. Suggested N = 100.

1. **Place at scale** (C1): one wide shot of the full cavern (rock wall, stone
   lining, stalactites, beam structure, reservoir, fire bed, placeholder) +
   one close follow-cam shot at the fire bed showing character-vs-viewport
   ratio. Reads-as-a-place verdict lives here.
2. **Lights on/off** (C2): same paused tick N, rig energies live vs all six
   `PointLight2D` energies forced 0 — nearby stone/wood surfaces visibly change.
3. **Bloom on/off** (C3): same paused tick N, `BloomToggle` on vs off (KEY_B) —
   materials stay distinguishable with bloom off; flame/lava halo tight, no
   washed boundaries with bloom on.
4. **Camera sweep + pause freeze** (C5/clip): unpaused 5–10 s slow pan across
   fire bed → reservoir → beam (animation alive: flame flicker regional, water
   quiet), then pause mid-flicker — frame freezes exactly; resume continues
   cleanly. This is the "game in a burning cave" read.
5. **GPU numbers** (C6): on-screen Xcode GPU frame capture of the cave —
   per-pass splits (world / lighting / bloom / composite; no particle pass in
   P3A) against the §17 budgets (world 1–2 ms, lighting+bloom 1–3 ms GPU).

Region-lock note: E2+E3 added only `tests/godot_p3a_fixtures_verify.gd`,
`tests/godot_p3a_exit_verify.gd`, and this record. Zero code/shader/scene/data
edits. Uncommitted per task instructions.

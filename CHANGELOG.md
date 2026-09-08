# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added
- P2.3 renderer palette from data: base colors/names from the loaded table (liquid depth shading by movement class; water pixels byte-identical); sidebar palette generated from the table with table-order shortcuts 1-9.
- P2.2 table-driven engine: `SimulationWorld` builds its material table from data (defaults byte-identical); step dispatch keys off movement class so remapped ids behave identically; host loads `data/materials.json` through `AshfallWorld::load_materials` every boot.
- P2.1 material data: dependency-free versioned JSON-subset loader (`ashfall/material_data`) with strict schema validation (unique ids/names, air-0 anchor, solid-implies-static) plus canonical `data/materials.json`; no simulation behavior change.
- Open-water sinking regression test: a sand block dropped into a wide basin sinks coherently, conserves mass, leaves nothing suspended, and rests on the floor (transient mid-water clumps are normal; only partial-row surface shimmer persists by design).
- P1.4 play level: `SimulationWorld::build_play_level` (floor, walls, platforms, mesa, sand dunes, water reservoir, contained rain pocket with diggable sand plug) plus `play_level_spawn`; deterministic content covered by `test_play_level_layout`.
- Player avatar in the Godot host: top-down kinematic movement (WASD/arrows) colliding against native stone via new `AshfallWorld` queries `is_solid_cell`/`can_move_to_box`; red marker overlay, live position HUD, letterbox-correct world/display mapping (also fixes brush painting on non-square panels).
- Avatar world interaction: dig sand/water (`E`) and place held material (`F`) in a 3x3 around the avatar; stone is undiggable, placement only fills air.
- P1.5 persistence: `AshfallWorld::save_to_bytes`/`load_from_bytes` plus controller save file (`user://ashfall_save.bin`: version, avatar position/material, sim bytes) with Save/Load buttons and `F5`/`F9`; headless `tests/godot_p1_verify.gd` covers level, collision, and save/load identity.
- Phased task documentation in `docs/tasks/README.md` and `docs/tasks/P0_FOUNDATION.md`.
- Public chunk snapshot and query methods on `ashfall::SimulationWorld` (`read_chunk`, `has_chunk`, `active_chunks`, `copy_rect`).
- Deterministic simulation tests for chunk and rect buffer extraction in `tests/simulation_tests.cpp`.
- Integrated `godot-cpp` bindings for Godot 4.7 via CMake.
- Implemented `AshfallWorld` GDExtension bridge node owning native `SimulationWorld` and generating palette-mapped RGBA8 texture snapshots.
- Created `ashfall.gdextension` manifest configuration for cross-platform dynamic libraries.
- Implemented Godot presentation host scene (`main.tscn`) and presentation/input-forwarding controller (`scripts/main_controller.gd`) supporting interactive cell modification, stepping, and reference world observation without any GDScript simulation logic.
- Headless automated verification test harness (`tests/godot_headless_verify.gd`) confirming Godot integration, native sand simulation, and texture snapshot generation.
- Formally validated P0 exit criteria and authored Milestone P1 task specification in `docs/tasks/P1_CELLULAR_WORLD.md`.
- Deterministic headless visual simulation check (`tests/godot_visual_simulation.gd`) rendering the native bridge to PNG.
- P1.1–P1.3 backend coverage: true cross-boundary movement, mass conservation, sand/water settling, command replay, malformed-payload rejection, varied-topology and step-dirty tracking tests.
- Water infiltration: lone surface droplets on sand seep down (mass-safe swap, escape-gated so no swap-cycle flicker); micro-puddles drain into dunes instead of dotting slopes. Covered by `test_water_trickles_down_sand_slope`.
- Floating-water shading regression check in `tests/godot_visual_simulation.gd` (column top never darker than middle).

### Fixed
- Sidebar ran outside the screen on short windows: its content min-height (~979 px) forced the whole layout taller than the 720 px canvas. Sidebar now lives in a `ScrollContainer` (no horizontal scroll) so it fits any height; layout fit is asserted in P1 scene verification.
- Water rendered offset from its cells (painting "missed" visible water): `WaterOverlay` carried a stray 128.5 px vertical offset from an editor nudge, shifting the whole water layer down while the brush edited correct cells. Overlay offsets removed; layer alignment is now asserted in P1 scene verification.
- Boot showed raw authored rectangles (floating water/sand blocks). `setup_play_level` now settles 200 ticks deterministically (~0.8 s), so first paint is dunes-at-repose with flat pools; dune2 shifted right to keep the settled spawn box clear. Pocket containment is now a permanent P1 assert.
- Water depth shading inverted on floating blobs and falling streams (dark-on-top). Depth is now min(water-below, water-above): only genuinely enclosed deep water goes dark teal.
- Window letterbox glare: explicit `canvas_items`/`keep` stretch, dark clear color matching the world background, dark viewport panel stylebox.

### Improved
- World scale 256 → 512 at full 60 Hz (no rate compromise — the flat-map
  optimization brought 512 ticks to ~5 ms on the play level, ~31% of a frame).
  Play level, reference world, avatar speed/brushes, and all coordinate-based
  tests re-authored ×2; P0 headless verify still exercises an explicit 256 world.
- Simulation performance: hash-based cell storage with deterministic sorted saves, incremental `set_cell`/`fill_rect` (O(area) not O(world)), O(1) occupied-cell count; bridge fixed-rate pacing (30 Hz, backlog dropped) and throttled HUD; Release (`-O3`) build. Bridge tick cost on the reference world dropped from ~38 ms to ~2.3 ms.
- Water behavior: pressure-gated lateral dispersion (up to 4 cells over supporting water) levels pools quickly; mass-safe sand/water swaps via settled-home ownership tracking; surface tension — cells embedded in a complete surface row rest while bumps drain, so no permanent single-pixel dots and settled surfaces level within one cell.
- Water rendering: dual-layer presentation — opaque base (background, stone, sand, nearest-crisp) plus a water-only layer (depth ramp, surface highlight, coverage alpha) sampled linearly under `shaders/water_fx.gdshader` (coverage closing + smoothstep, foam shoreline, animated shimmer, hash dither). No hard squares; isolated droplets read as soft beads. Dual paint keeps bridge tick at ~2.7 ms on the reference world.
- Perf HUD: sidebar now shows live FPS, frame time (EMA), and last sim-step time (ms, measured in `AshfallWorld::step`).


# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

### Added
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

### Improved
- Simulation performance: hash-based cell storage with deterministic sorted saves, incremental `set_cell`/`fill_rect` (O(area) not O(world)), O(1) occupied-cell count; bridge fixed-rate pacing (30 Hz, backlog dropped) and throttled HUD; Release (`-O3`) build. Bridge tick cost on the reference world dropped from ~38 ms to ~2.3 ms.
- Water behavior: pressure-gated lateral dispersion (up to 4 cells over supporting water) levels pools quickly; mass-safe sand/water swaps via settled-home ownership tracking; surface tension — cells embedded in a complete surface row rest while bumps drain, so no permanent single-pixel dots and settled surfaces level within one cell.
- Water rendering: dual-layer presentation — opaque base (background, stone, sand, nearest-crisp) plus a water-only layer (depth ramp, surface highlight, coverage alpha) sampled linearly under `shaders/water_fx.gdshader` (coverage closing + smoothstep, foam shoreline, animated shimmer, hash dither). No hard squares; isolated droplets read as soft beads. Dual paint keeps bridge tick at ~2.7 ms on the reference world.
- Perf HUD: sidebar now shows live FPS, frame time (EMA), and last sim-step time (ms, measured in `AshfallWorld::step`).


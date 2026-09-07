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


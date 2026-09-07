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


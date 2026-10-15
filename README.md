# Ashfall

Ashfall is a **Noita-inspired, systemic falling-sand action prototype**: a Godot presentation layer wrapped around a deterministic C++ material world. The goal is an original game built around readable pixel-art caves and meaningful interactions between terrain, liquids, heat, tools, and hazards—not a reproduction of another game's assets or implementation.

## Current state (2026-10-15)

- **P0–P2 foundations are implemented:** portable C++20 simulation, Godot GDExtension bridge, data-driven materials/reactions, heat, fire, save/load, deterministic tests, and benchmarks.
- **P3 presentation is implemented but not fully signed off:** the default `cave_benchmark.tscn` has procedural cave generation, material profiles, local fire lighting, camera framing, animation, and a debug panel. Rendered visual approval and target-Mac GPU capture remain separate gates.
- **P4A first playable prototype is implemented; the full loop remains open:** the default cave now has an original surface entrance, asymmetric rooms, grounded side-view movement/jump, and a Spring Wand water/fire/wood interaction. An exit/finish state, tick-indexed replay ownership, transient entity save state, and rendered sign-off remain to do.
- `main.tscn` remains the small material-demo/reference scene; the cave is the default boot scene.

See [the roadmap](docs/ROADMAP.md), [architecture](ARCHITECTURE.md), and [the scoped playable-slice task](docs/tasks/P4_NOITA_LIKE_SLICE.md) for current priorities and exit criteria.

## Build and test

Use Godot 4.7.x, CMake 3.20+, a C++20 compiler, and the pinned `godot-cpp` submodule:

```sh
git clone --recurse-submodules https://github.com/PylotLight/Ashfall.git
cd Ashfall
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
godot --headless --path . --script tests/godot_headless_verify.gd
```

Run the project with `godot --path .`. **A/D** or arrows move, **Space/W/Up** jump, mouse aims, and left-click casts the Spring Wand. **F1** opens the developer panel (material painting/telemetry); **F2** opens the material demo from that panel. **P** pauses/resumes world simulation; **R** rebuilds the cave.

## Project boundaries

- `engine/` — portable authoritative simulation and serialization.
- `bridge/` — Godot GDExtension adapter; no simulation rules belong here.
- `scripts/`, `shaders/`, `data/` — presentation, host-side prototype input, and visual/material definitions.
- `tests/`, `bench/` — deterministic regression suites, fixtures, and measurements.
- `docs/` — current design, task scope, evidence, and review notes.

Keep new cell behavior in the C++ core with deterministic tests. Treat host-side gameplay code as a prototype boundary until input, entity state, and replay ownership are explicitly designed.

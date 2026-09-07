# Ashfall

Ashfall is a cross-platform, falling-sand simulation RPG foundation.

The repository currently contains only the pieces that own or verify the
simulation:

- `engine/` — portable C++20 simulation library, with no Godot dependency.
- `tests/` — deterministic simulation regression tests.
- `project.godot` and `main.tscn` — a minimal Godot host, ready for the
  GDExtension bridge.

Godot will provide rendering, input, tools, and UI. The C++ core owns world
state, simulation ticks, and save data. See [ARCHITECTURE.md](ARCHITECTURE.md)
for the boundary and build sequence.

## Build and test

With CMake and a C++20 compiler installed:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Development rules

- Add simulation behaviour in `engine/` with a deterministic regression test.
- Keep Godot types out of `engine/include`.
- Do not add a GPU implementation until it can be checked against the CPU
  reference for the same seed and command stream.
- Keep this README and `ARCHITECTURE.md` current; avoid phase handoff docs and
  duplicate roadmaps.

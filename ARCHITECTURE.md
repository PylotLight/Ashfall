# Ashfall Architecture

## Rule of ownership

The simulation is a portable C++ library. It owns material cells, chunks,
ticks, and serialized world state. Godot is an application shell: it renders
snapshots, hosts UI and tools, and translates player actions into simulation
commands.

```text
Godot UI / input / rendering
            |
            v
     GDExtension bridge (next)
            |
            v
  engine/ SimulationWorld  <--- standalone tests and benchmarks
```

No Godot type appears in `engine/include`. That keeps simulation behaviour
portable to macOS, Windows, and Linux, and lets a future GLSL compute backend
be validated against the CPU implementation.

## Current vertical slice

`SimulationWorld` stores sparse 64 by 64 chunks. Unallocated cells are air.
It supports static stone, falling sand, spreading water, deterministic ticks,
and versioned binary save/load. The rules are intentionally small until the
test suite expands for each new material interaction.

## Next milestones

1. Build a GDExtension bridge that exposes `SimulationWorld` to Godot without
   moving world ownership back into GDScript.
2. Add a renderer that uploads only changed chunks.
3. Add material definitions and reactions to the C++ core, with unit tests.
4. Profile the CPU reference, then implement a GPU backend that produces the
   same state for identical seeds and command streams.

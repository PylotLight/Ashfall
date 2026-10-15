# Ashfall Design

## Purpose

Ashfall is a 2D physics-sandbox RPG inspired by the systemic material play of
Noita. Its differentiator is a large, reactive cellular world: materials,
entities, quests, and NPC behaviour all observe the same world state.

The project is an engine-first vertical slice, not a content-complete game.
The first playable goal is a world where a player can alter sand and water,
observe deterministic results, and save/reload exactly. Fire, AI, procedural
generation, spells, and art follow only after that core is demonstrably sound.

## Technical direction

| Concern | Decision |
| --- | --- |
| Host, editor, UI, rendering | Godot 4.x |
| Authoritative simulation | Portable C++20 |
| Godot/native boundary | `godot-cpp` GDExtension |
| Initial simulation backend | Deterministic CPU reference |
| Later acceleration | SIMD, worker threads, then GLSL compute |
| Platforms | macOS, Windows, Linux |
| Build | CMake; Ninja preferred |
| Tests | Headless deterministic C++ tests |
| Profiling | Platform profiler plus Godot profiler |

Godot is deliberately not the simulation engine. It must not contain a node
per cell, be responsible for material rules, or be the only place where world
state can exist. Its job is to host, render, inspect, and control the native
world.

```text
Godot input / UI / renderer / editor
                 |
                 v
        GDExtension world bridge
                 |
                 v
        SimulationWorld public API
             /              \
 CPU reference backend    GPU backend (later)
             \              /
       tests, benchmark, save system
```

## Simulation model

### World and chunks

The world is a sparse set of fixed-size chunks, never one enormous global
array. An unallocated cell is air. Chunks gain lifecycle states as the game
needs them: active, sleeping, dirty, resident, and evictable. Any update that
crosses a chunk edge must have a deterministic test.

The current core uses 64 by 64 chunks. This is a starting value, not a public
file-format promise. Chunk dimensions and cell layout must be isolated from
gameplay code before tuning them.

### Cells and materials

Each cell eventually carries a material identifier and compact runtime state
(for example temperature, lifetime, and flags). Material definitions live in
data rather than subclasses such as `Sand.cpp` or `Water.cpp`.

The planned material definition contains only properties needed by implemented
rules: movement model, density, solid/anchor behaviour, flammability,
conductivity, viscosity, temperature behaviour, hardness, and reaction keys.
Do not add speculative fields merely because a later material might use them.

Initial supported materials are:

| ID | Material | Behaviour |
| --- | --- | --- |
| 0 | air | empty |
| 1 | stone | static/solid |
| 2 | sand | powder; falls and piles |
| 3 | water | liquid; falls and spreads |

### Time and determinism

Simulation time advances in explicit fixed ticks. A tick is a pure result of
the prior world state, the seed, and an ordered command stream. Random-looking
movement uses a stable integer hash, never wall clock time or platform RNG.

Every feature that changes cells must be reproducible by replaying commands.
Save/load stores the full authoritative state required to resume the same
future simulation; it is not a display snapshot.

### CPU before GPU

The CPU implementation is the correctness oracle. It must have deterministic
tests and a benchmark before any GPU simulation is introduced. The GPU backend
will accept the same state and commands, then be checked against expected CPU
results for supported scenarios. A GPU implementation may differ internally,
but may not quietly become a second set of rules.

## Public boundaries

`engine/include` is the stable, Godot-free simulation interface. It may expose
world creation, fixed stepping, material data, cell/chunk snapshots, edits,
save/load, and instrumentation. It may not expose Godot classes, rendering
resource IDs, UI concepts, or scene paths.

The GDExtension bridge owns conversion only:

- translates Godot input into simulation commands;
- steps the native world at a fixed cadence;
- requests dirty chunk snapshots for rendering;
- exposes safe debug data to Godot tools.

Rendering consumes snapshots and dirty regions. It never directly mutates
authoritative cell memory. Gameplay entities likewise submit commands and
query world state through the API.

## Future systems

Material simulation expands in this order: powder/liquid correctness, gases,
temperature, fire, then data-driven reactions. Entities follow: player,
projectiles, NPCs, and interactive objects. World generation provides caves,
biomes, deposits, and structures. Gameplay then adds spells, combat,
inventory, AI, progression, and world-state-driven quests.

Rigid-body extraction is a later feature: disconnected solid components can
be detected, represented as physics bodies, and resettled into the grid. It
is not a prerequisite for the first material or player milestones.

## Non-goals for the foundation

- A custom windowing, editor, audio, or asset pipeline.
- Godot nodes for every simulated cell.
- GPU simulation before the CPU oracle is correct and benchmarked.
- Procedural generation, enemies, spells, or art before the material slice is
  visible and testable.
- A broad material catalogue before reaction and state persistence are ready.

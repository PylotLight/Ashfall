# Ashfall Roadmap

Each phase has an exit criterion. Do not begin a dependent phase because a
document says it is next; begin it only after its criterion is verified.

## P0 — Native bridge and visible reference world

Build a `godot-cpp` GDExtension that owns one `SimulationWorld`, steps it at a
fixed rate, and presents a chunk snapshot to Godot.

Exit criterion: launch Godot, instantiate a native 256 by 256 test world,
modify cells through the bridge, step it, and visibly observe the native
result. No simulation rule is implemented in GDScript.

## P1 — Deterministic cellular world

Complete air, stone, sand, and water in the C++ core. Add chunk-boundary,
mass-conservation, settling, command replay, and save/load tests. Replace the
test snapshot with dirty-chunk presentation.

Exit criterion: a player can alter sand and water in a hand-authored scene;
stone blocks movement; quitting and loading restores the exact authoritative
world and player state.

## P2 — Data-driven materials and reactions

Load material definitions and reaction definitions from versioned data. Add
gases, compact per-cell state, temperature, fire, and reaction execution.
Introduce activation/sleeping for chunks only when profiling demonstrates a
need.

Exit criterion: a data-only material/reaction addition works through the
native core, renderer, save format, and deterministic tests without adding a
special material branch.

## P3 — Rendering and inspection

Render dirty chunk textures efficiently, add a camera, material readability,
lighting as appropriate, and a native-world debug overlay. Keep rendering
separate from simulation state.

Exit criterion: visual updates are limited to dirty chunks and profiling data
shows simulation, upload, and render costs separately.

## P4 — Entities and terrain interaction

Add player collision/query APIs, digging and placement commands, projectiles,
and a minimal entity system. Investigate rigid-body extraction only after
cell/world interfaces are stable.

Exit criterion: a player can move through, dig, and alter the same world the
simulation owns, with collision based on authoritative native state.

## P5 — World structure

Add seeded generation: caves, material deposits, biomes, and structures.
Define the persistence split between an exact-state hub and seed-regenerated
procedural areas.

Exit criterion: travel between a persistent hand-authored hub and a seeded
cave works, with the hub retaining changes and the cave recreating from seed.

## P6 — Gameplay vertical slice

Add one environmental puzzle, a small spell/tool set, one NPC that responds
to hazards, and one world-state-gated interaction or quest.

Exit criterion: a player solves a problem through material simulation and an
NPC or dialogue response reflects the physical change.

## P7 — Scale and acceleration

Measure before changing architecture. Improve chunk scheduling, storage,
SIMD, worker execution, and memory use. Implement a GPU backend only for
behaviour already covered by CPU comparison scenarios.

Exit criterion: target-frame-time benchmarks and CPU/GPU equivalence tests
are automated for representative active worlds.

## P8 — Tooling and content

Create a material editor, world inspector, replay viewer, benchmarks, and
authoring tools. Scale content only after the core loop remains measurable
and deterministic.

Exit criterion: a contributor can create a material, reaction, test scenario,
and benchmark without editing unrelated engine code.

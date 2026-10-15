# Ashfall Architecture

## Ownership contract

```text
Godot host: input, camera, UI, shaders, effects, scene authoring
                         |
                         v
GDExtension bridge: lifecycle, queries, snapshots, data loading
                         |
                         v
Portable C++ core: material cells, fixed ticks, reactions, heat, saves
```

The C++ simulation owns authoritative cell state and serialization. The bridge adapts that API for Godot; it must not become a second implementation of material rules. Rendering consumes presentation data and may never silently change the simulation. The CPU core remains the correctness reference for future acceleration.

## Implemented

- Sparse 64×64 chunks, deterministic powder/liquid/gas motion, temperature, table-driven materials and reactions, and versioned save/load.
- Godot 4.7 GDExtension (`AshfallWorld`) with cell queries, edits, stepping, presentation textures, instrumentation, and material/reaction loading.
- Profile-driven material rendering, a separate water layer, fire/lava emission, clustered 2D lights, constrained bloom, tick-driven animation, and camera-cropped texture views.
- Default cave scene with deterministic authored geometry, landmark rooms, reservoirs, fire/fuel, an avatar overlay, and a debug panel.

## Current prototype boundary

Avatar position and movement are currently host-side. The host queries native collision and submits cell edits for dig/place; those actions are not yet a replayable authoritative entity-command stream. The current movement model is not yet the intended side-view platformer loop. Keep this explicit: the cave is a presentation/gameplay prototype, not evidence that Noita-scale gameplay or determinism is complete.

## Next architectural decisions

1. Prove the first authored interaction room with side-view traversal, one wand/tool, and one water/fire/terrain puzzle.
2. Before adding persistent runs or more entities, define tick-indexed input commands and decide which player/projectile state moves into C++.
3. Profile representative active caves and separate simulation, upload, and GPU costs on target hardware.
4. Add seeded region generation and persistence only after a short playable loop is readable and reliable.

Do not add GPU-authoritative simulation, unbounded lighting, or a large material roster until the CPU reference and the interaction contract can verify the behavior.

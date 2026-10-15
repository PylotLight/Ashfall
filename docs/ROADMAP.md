# Ashfall Roadmap

Status reviewed 2026-10-15. A phase is complete only when its exit evidence exists; a design note or headless assertion is not a rendered visual sign-off.

## P0 — Native bridge and visible reference world — Complete

A Godot GDExtension owns the native world and exposes stepping, edits, queries, and presentation. Deterministic bridge and scene tests cover the reference path.

## P1 — Deterministic cellular world — Complete

The portable C++ core owns material cells, fixed ticks, edits, and versioned world saves; regression tests cover movement, conservation, boundaries, and replay.

## P2 — Data-driven materials and reactions — Complete

Materials, heat, gases, and reaction definitions load through validated data tables. Material behavior and save compatibility are covered in the native tests.

## P3 — Rendering and inspection — Implemented; rendered sign-off remains open

The project now has the default cave scene, a cropped follow camera, material-profile shader, structured material palettes, fire/lava emission, a bounded clustered-light rig, restrained bloom, tick-driven presentation animation, performance instrumentation, and deterministic presentation tests.

**Current rendered evidence:** [2026-10-15 opening-room capture](perf/captures/2026-10-15-cave-review.png) at 1280×720 shows the cropped spawn room, arch, compact bridge, enlarged wizard, wick, seep, and in-game sidebar. This is a positive C1 iteration check, not complete P3 sign-off. **Remaining:** fire-lit/unlit and bloom on/off A/B captures, a pause/resume camera sweep, and a target-Mac GPU frame capture. Headless tests prove state and input contracts, not pixel quality.

## P4 — Playable interaction slice — Prototype exists; next gameplay milestone

The current host prototype supports avatar movement, native-cell collision queries, digging/placement, material selection, and save/load. It is not yet a side-view platformer or a complete interaction loop.

### P4A target: one playable cave room

- Side-view horizontal movement, gravity, grounded jump, and readable player/aim direction.
- One wand with one reliable projectile/tool; clear collision, cooldown, and material feedback.
- One authored route with a recoverable environmental chain: reveal/redirect water, quench burning wood, and open a blocked passage.
- A small health/hazard rule only if it is needed to make the route meaningful; no sprawling combat system.
- Input and world changes are testable and repeatable. Decide the authoritative ownership of player/projectile state before persistence expands.

**Exit:** a new player can traverse and finish the room without debug controls; the chain visibly changes the same native world; no soft-lock; save/reload preserves the result; the main view reads clearly at 1280×720.

Detailed acceptance and non-goals: [P4 Noita-like playable slice](tasks/P4_NOITA_LIKE_SLICE.md).

## P5 — Persistent world structure — Not started

Add seeded regions, explicit active/sleeping policy, and persistent edits. Start with one hub and one generated cave; defer large-scale streaming until profiling justifies it.

## P6 — Repeatable systemic run — Not started

Add a small selection of tools/material combinations, an encounter or hazard, and a legible run goal. Expand content only when the interaction chain remains robust.

## P7 — Scale and acceleration — Not started

Profile representative active worlds first. Improve chunk scheduling, storage, SIMD, and worker execution against the CPU reference; any GPU backend must match defined CPU scenarios.

## P8 — Authoring and replay tools — Not started

Build inspectors, scenario/replay tooling, and content authoring only to remove friction from verified game systems.

## Near-term order

1. Review the rendered opening-room capture; the first-pass silhouette, avatar scale, camera crop, and in-game UI are now visible. Continue the broader level-design/lighting pass based on visual review.
2. Close the P3 visual evidence that can be captured here; keep the target-Mac GPU measurement explicitly open.
3. Implement P4A traversal and the single-room water/fire/tool interaction.
4. Define replay/state ownership before persistent gameplay or more complex entities.

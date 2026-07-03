# Game Design Document — Physics-Sandbox RPG
*Working title: "Ashfall" (placeholder)*

A standalone reference doc — goals, tech stack, and MVP scope. Pairs with
`roadmap.md` (phase-by-phase build order) but should make sense on its own.

---

## 1. Vision

A falling-sand physics sandbox in the spirit of Noita, where fire, liquid,
gas, and material simulation are the core verb — combined with NPCs and
quests that perceive and react to that same simulated world, rather than
sitting beside it as a separate scripted layer.

## 2. Design Pillars

1. **Every pixel matters** — materials and their reactions are gameplay, not
   set dressing.
2. **NPCs live in the simulation** — they perceive hazards, react to
   player-caused changes, and reference world state in dialogue.
3. **Systemic over scripted** — quests resolve against world-state checks
   (did the room actually flood?), solvable multiple physical ways.
4. **Permadeath meets persistence** — procedural, run-based dungeon areas;
   a persistent hub layer that remembers what happened there.

## 3. Tech Stack

| Layer | Choice | Notes |
|---|---|---|
| Engine | Godot 4.7 | Forward+ renderer required (Compatibility mode has no compute shader support) |
| Simulation core | GLSL compute shaders via Godot's `RenderingDevice` API | Pull-based, race-free update — proven in the MVP benchmark (flat 120fps to 4M cells) |
| Game logic / AI / UI | GDScript | No GDExtension/C++ needed at current scale — benchmark headroom means this can stay in scope for the MVP; revisit only if profiling shows a GDScript-side bottleneck, not preemptively |
| Dialogue | Yarn Spinner (Godot plugin) or Ink | Don't build a custom dialogue engine — solved problem, not a differentiator |
| Pathfinding | Custom flow-field/A* over a coarse hazard grid | Godot's built-in navmesh isn't a good fit for a constantly-mutating destructible world |
| Rigid bodies | Godot's built-in 2D physics | Used only for extracted debris (Phase 3), not the sim itself |
| Save system | Custom serialization (RLE per chunk/row) | Full pixel-state saves are too large uncompressed |
| Version control | Git | — |
| Primary dev platform | macOS (Apple Silicon) | Compute path runs Vulkan-over-Metal via translation layer — works, but is one more layer than Windows/Linux if something behaves oddly |

## 4. MVP Feature Set

The MVP is a vertical slice, not a content-complete game: one hub area, one
procedural area, a small material palette, one or two NPCs. The goal is to
prove every pillar works *together* in one playable loop, not to have a lot
of content.

### A. Physics — Materials & Interactions
- Data-driven material palette (~6-8 materials): stone, sand, water, fire,
  smoke/gas, flammable wood, ash, steam.
- Data-driven reaction table (water + fire → steam, wood + fire → ash +
  spreading fire, etc.).
- Per-cell temperature driving reactions systemically rather than via
  hardcoded pairwise cases.
- Rigid-body extraction: disconnected solid structures fall and become
  physics objects, then resettle into the grid.

### B. Physics — Expandability & Maintainability
- Material properties and reactions live in data (resource/JSON), not
  hardcoded shader branches — adding a material is a data change.
- Clear boundary between the sim core (compute shader + minimal GDScript
  bridge) and the game layer (AI, dialogue, UI) so either can change
  independently.
- The grid-size/FPS benchmark harness stays in the project as a standing
  regression check whenever materials or reactions are added.

### C. Character Interaction & Movement
- Player movement (run, jump) with collision against solid sim cells.
- Digging/material manipulation as the core interaction verb (already
  proven in the MVP as mouse-paint; becomes a real game action).
- Camera follow with zoom.

### D. World Design & Generation
- One hand-authored persistent hub area (exact-state save/load).
- One procedurally generated cave/dungeon area (CA-based generation, reusing
  the sim's own primitives), seed-based, not persisted as full state.
- Basic save/load covering both.

### E. Basic AI / NPCs + Interaction
- One to two NPCs with reactive movement: pathfind against the coarse hazard
  grid, flee active fire/liquid hazards.
- One dialogue tree per NPC, with at least one branch gated on world state
  (something the player did to the physical world, not a flag set by a
  cutscene).

### F. Graphics / UI / UX
- Minimal HUD: health, currently-selected material/tool.
- Dialogue UI: text box with branching choices.
- Pause/save menu.

## 5. Explicit Non-Goals for MVP

Multiplayer, full inventory/itemization, combat depth, quest system beyond
one proof-of-concept quest, large content volume, mobile/console export,
localization. All of these are legitimate later scope — deliberately absent
here so the MVP stays small enough to actually finish and evaluate.

## 6. MVP Success Criteria

A player can: walk from the hub into a procedural cave, burn or flood an
obstacle to solve a simple physical puzzle, watch a structure collapse into
a physics object as a result, encounter an NPC who flees the hazard they
created, and have a dialogue with that NPC that references what just
happened. If that loop works end-to-end, the pillars have been validated
together — not just individually.

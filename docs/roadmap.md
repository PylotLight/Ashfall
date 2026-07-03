# Project Roadmap: Physics-Sandbox RPG
*From falling-sand foundation to full NPC/RPG/UI systems*

Status as of last benchmark: chunked brute-force compute sim holds flat 120fps
through 4096x4096 cells, ~100fps at 10,000x10,000. That result changes the
plan below — dirty-rect chunking moves from "foundational requirement" to
"later optimization," and we can afford to spend the early phases on
*correctness and content* rather than performance engineering.

Each phase has a goal, concrete tasks, and an **exit criterion** — a
specific, testable thing that has to be true before moving to the next
phase. Don't start a phase whose exit criterion depends on something an
earlier phase hasn't actually delivered yet.

---

## Phase 0 — Fix & Harden the MVP
*Goal: the existing demo is trustworthy to build on.*

- Fix window/viewport size so the full grid (including floor) is always
  visible (`project.godot` display settings, or scale sim-to-window
  dynamically).
- Fix boundary-overwrite bug (mouse painting can currently erase the stone
  walls at the grid edge) — clamp brush to interior cells.
- Add a debug overlay: active-cell count (not just FPS) — you'll want this
  once dirty-tracking exists, to verify it's actually reducing work, not
  just to watch FPS.
- Commit to source control now if you haven't (git), before complexity grows.

**Exit criterion:** demo runs correctly at default settings with no visual
bugs, and you have a git history to bisect against as things get added.

---

## Phase 1 — Solidify the Simulation Core
*Goal: turn the 3-material proof-of-concept into the real foundation
everything else builds on. This is the highest-leverage phase — mistakes
here get expensive to unwind later.*

- **Data-driven materials**: move material properties (density, whether it
  falls/spreads, color) out of hardcoded shader branches into a structured
  format (e.g. a small uniform buffer array indexed by material ID) so
  adding a new material is a data change, not a shader rewrite. Do this
  *before* adding more materials — retrofitting it later means rewriting the
  shader twice.
- **Player entity**: a simple physics-driven capsule/rect that collides with
  solid cells (stone, and eventually settled sand). This is a separate
  collision system from the cellular automaton — likely a coarse
  "is this AABB overlapping solid cells" check against the sim grid, not a
  full rigid body yet.
- **Digging/interaction**: player can remove terrain (already have the brush
  mechanism from the MVP — wire it to a game action instead of raw mouse
  paint).
- **Camera**: follow the player, with room to zoom (you'll want this for
  both the physics sandbox feel and later readability with NPCs on screen).
- **Basic save/load**: serialize the grid (simple RLE-per-row is enough at
  this stage) plus player position. Validates the save architecture early,
  before world size/complexity make it painful to retrofit.

**Exit criterion:** you can walk a character around a hand-placed test level,
dig through sand, get blocked by stone, save, quit, and reload into the same
state.

---

## Phase 2 — Expand the Material Palette
*Goal: this is where the "fun" of the physics sandbox actually comes from —
reactions and combinations, not just more materials.*

- Add: fire (spreads to flammable materials, has a lifetime, converts fuel
  to smoke/ash), a gas (rises, diffuses, occupies space), and one
  reactive pair (e.g. water + fire = steam, or water + lava = stone).
- Temperature as a per-cell value (you already have a spare byte or two in
  the packed cell format) — this is what makes fire/freezing/melting
  systemic rather than a hardcoded pairwise table.
- A simple reaction table: data-driven (material A + material B [+ temp
  threshold] → result), read by the shader, so new reactions are data
  entries, not new shader logic.

**Exit criterion:** you can light something on fire, watch it spread and
burn out, and produce a byproduct (smoke/ash) — without having touched
`sand_sim.glsl`'s core update loop, only the material/reaction data.

---

## Phase 3 — Rigid Body Extraction
*Goal: the single most recognizable "Noita feel" mechanic — structures
collapse into physics objects when disconnected from stable ground.*

- Flood-fill / connected-component detection on solid cells (can run as a
  periodic CPU pass, doesn't need to be every frame).
- When a solid region becomes disconnected from anything anchored (map
  edge, or a "foundation" flag), extract it into a `RigidBody2D` with a
  generated collision polygon (marching squares or similar on the region's
  boundary), remove those cells from the sim grid.
- Re-settle: when the rigid body comes to rest, write it back into the cell
  grid at its final position/rotation (rounding rotation is the hard part —
  Noita handles this with real angle snapping; a simplified version that
  only allows 0°/90°/180°/270° resettling is a reasonable first pass).

**Exit criterion:** you can dig out the support from under a stone platform
and watch it fall and land as a physics object, then walk on it as if it
were normal terrain again.

---

## Phase 4 — World Structure
*Goal: move from one hand-placed test level to a real explorable space.*

- Decide the persistence split now (this was flagged as a design tension
  early on): **procedural, seed-based dungeon areas** (permadeath-style,
  don't need full-state saving) vs. a **persistent hub area** (needs exact
  chunk-level save/load, since player-caused changes must stick).
- Procedural cave generation: reuse the cellular-automaton primitives you
  already have (a CA smoothing pass is a standard cave-gen technique) rather
  than building a separate generator.
- Chunk streaming: only now, if the earlier benchmark numbers don't hold up
  at "real level" scale, revisit chunk-based dirty tracking. Test this
  explicitly before assuming you need it.

**Exit criterion:** player can walk from a persistent hub area into a
procedurally generated area and back, with hub changes surviving a
save/reload and dungeon areas regenerating from seed.

---

## Phase 5 — Character & NPC Foundation
*Goal: get a second physics-aware agent moving around before adding
dialogue/AI complexity on top of it.*

- NPC entity sharing the player's collision/movement code where possible.
- **Hazard grid**: a coarse (e.g. 8x8-downsampled) danger map derived from
  the sim grid — fire/liquid/gas presence per coarse cell. This is what lets
  NPC pathfinding stay cheap and decoupled from raw sim resolution, per the
  original design doc.
- Basic pathfinding (A* or flow-field) against the hazard grid, with a
  fallback "flee hazard" behavior as the simplest possible reactive AI.

**Exit criterion:** an NPC placed near a fire you started will path away
from it convincingly, without querying individual sim cells directly.

---

## Phase 6 — Dialogue & NPC AI Depth
*Goal: the actual novel fusion point — NPCs that react to and reference
world state.*

- Dialogue system (Yarn Spinner or Ink, don't build your own) with
  condition/effect hooks into world-state queries (has this room burned,
  is this NPC standing in water, etc.).
- Utility-AI or behavior-tree layer on top of Phase 5's reactive pathing —
  NPCs weigh "talk to player" vs. "flee hazard" vs. "do my job" as scored
  options instead of a rigid state machine.
- Event-driven sim notifications (region ignited, structure collapsed) so
  NPCs react to state *changes*, not just continuous polling.

**Exit criterion:** one NPC has a multi-branch dialogue tree where at least
one branch is only reachable because of something the player did to the
physical world (burned their shop down, flooded their basement, etc.).

---

## Phase 7 — RPG Systems
*Goal: quests and progression that use the physics sandbox as their
resolution mechanism, not scripted triggers.*

- Quest system as state predicates over the sim (e.g. "liquid volume in
  tagged region > threshold" rather than a trigger volume) — this is the
  payoff of everything built so far.
- Inventory/item system.
- Persistent reputation/relationship values tied to the hub-layer NPCs from
  Phase 4's persistence split.

**Exit criterion:** at least one quest is solvable multiple physically
different ways (flood it, burn it, freeze it) and the game recognizes
completion via world-state check in all of them.

---

## Phase 8 — UI/UX
*Goal: intentionally late — UI churns less painfully once the underlying
systems it displays are stable.*

- HUD (health, inventory, active tool/material).
- Dialogue UI (portrait, text, branch choices).
- Menus (save/load, settings, death/respawn flow appropriate to the
  procedural-dungeon permadeath loop from Phase 4).

**Exit criterion:** a first-time player can understand and play the game
without you narrating what's happening.

---

## Phase 9 — Content & Polish
*Goal: everything after this point is scaling up what already works —
more materials, more NPCs, more biomes, more quests — not new systems.*

---

## Sequencing Notes

- **Phases 0-3 are single-player-sandbox-only** — no NPCs, no dialogue, no
  RPG systems. Resist the urge to start Phase 5+ work early; a shaky
  physics foundation is far more expensive to fix once NPCs and quests
  depend on it.
- **Phase 1's data-driven material system is the phase most worth getting
  right the first time** — nearly everything downstream (reactions, hazard
  grid, quest predicates) reads from that same material/state data.
- Re-run the grid-size FPS benchmark from the MVP after Phase 2 (more
  materials/reactions = more shader branching = possible perf change) and
  again after Phase 4 (real level sizes, not synthetic empty grids) — don't
  assume the Phase 0 numbers hold forever.

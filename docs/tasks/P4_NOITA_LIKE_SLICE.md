# P4A — Noita-like Playable Cave Slice

**Status:** playable prototype, traversable route-cleared exit, fixed-tick replay, transient save/load, and local bloom-off 1280×720 rendered sign-off implemented. Target-Mac GPU profiling remains open separately.

## First-prototype implementation (2026-10-15)

- Replaced the benchmark arena with an original, fixed-seed surface ridge, sky-connected cave mouth, asymmetric first chamber, optional high pocket, and a lower eastern room.
- Added a 3×5-cell-feet side-view controller with horizontal collision against the native world, gravity, grounded jump, a two-cell step-up allowance, and a character-scale camera crop.
- Added the Spring Wand: mouse-facing, bounded-rate water bolts with screen-space travel/impact feedback. Impacts can cut soft sand/timber, write water into the same authoritative cells, release the authored spring, and rely on native reactions for fire/steam.
- Moved the material palette, telemetry, and material-demo button into the F1 debug panel; normal play shows the entry note and tool controls.
- Added `tests/godot_p4a_playable_verify.gd` for the surface-to-grille walk, jump/fall, wall collision, spring release, steam reaction, grille breach, and finish trigger.
- Added `tests/godot_p4a_replay_verify.gd` for contiguous command ticks and byte-identical playback from the authored seed.

**Exit phase (2026-10-15):** added a visible eastern portal, an exit trigger, a route-cleared overlay, and a restart prompt. The exit is presentation-only and does not alter the native cell world.

**Replay phase (2026-10-15):** gameplay runs on a fixed 60 Hz controller tick with one contiguous, one-based command record per tick. Commands include horizontal input, jump, aim/fire, pause toggles, and single-step events. Recording starts on boot/rebuild; F6 stops recording and F7 rebuilds the authored seed then replays the in-memory stream. The playback test compares serialized native-world bytes and player/transient state exactly. Debug brush edits are intentionally outside the player replay contract; recordings are capped at ten minutes.

**Persistence phase (2026-10-15):** save-envelope v3 stores player position/velocity/grounding, aim, wand cooldown, awake/pause/finish flags, gameplay tick, pending tick inputs, active projectiles, impact FX, selected material, and native simulation bytes. Loads validate lengths, bounds, counts, and finite values before applying state; v2 position/material/world saves remain supported with safe defaults for new fields. Loading intentionally clears the in-memory seed replay because its original command history is not part of the save format. `tests/godot_p4a_save_verify.gd` covers exact native-world restoration, transient round-trip, v2 migration, and truncated-file rejection.

**Rendered sign-off phase (2026-10-15):** a GUI-mode 1280×720 bloom-off run verified the water release/native steam reaction, timber-grille breach, side-view movement/jumps through the east connector, and route-cleared portal state. The run exposed and fixed a soft-lock at the eastern chamber shoulder by carving a deterministic avatar-height connector and moving the decorative ledge out of its clearance; the completion overlay was also softened to preserve portal context. Capture runner: `xvfb-run -a godot --audio-driver Dummy --path . --resolution 1280x720 --windowed --script tests/godot_p4a_render_signoff.gd`. Evidence: [water/fire interaction](../perf/captures/2026-10-15-p4a-bloom-off-interaction.png) and [route-cleared finish](../perf/captures/2026-10-15-p4a-bloom-off-route-cleared.png). Captured on Mesa llvmpipe software rendering; this confirms viewport/layout/content, not target-Mac GPU performance.

**Remaining:** profile the rendered scene on the target Mac GPU and gather player feedback. This remains a playable first-room slice, not a claim of complete Noita-like feature parity.

**Purpose:** prove that Ashfall is a game about readable movement and causal material interaction, not only a simulation benchmark.

## Player promise

In one short cave run, the player can explore, aim a simple tool, alter the material world, and use the resulting physical change to reach an exit. The presentation and mechanics are original to Ashfall; this scope does not use Noita code, assets, characters, or level layouts.

## Slice contents

1. **One authored cave:** a clear entrance, central landmark room, one optional side pocket, one hazard/tool interaction, and one exit. Routes must be legible without a debug overlay.
2. **Side-view traversal:** horizontal movement, gravity, grounded jump, stable collision against native terrain, and a readable avatar/facing direction. Keep movement tunable in one place.
3. **One wand/tool:** mouse aim, one projectile or short-range tool, bounded rate of fire, obvious impact feedback, and predictable interaction with the native world. No configurable wand deck yet.
4. **One environmental chain:** expose or redirect water, quench burning wood/fire, then clear or pass the resulting barrier to reach the exit. Reactions must use the existing native simulation rules where applicable.
5. **A compact play HUD:** selected tool, minimal health or hazard cue if required, and controls. Simulation telemetry stays in the debug panel.
6. **Repeatability:** a fixed seed/layout and scripted input test for the chain. Player movement, jump, aim/fire, pause toggles, and simulation single-steps are captured as one-based 60 Hz commands and reproduced from the same seed. Developer brush edits and camera-only controls are outside this gameplay replay contract.

## Acceptance checks

- A first-time player can move, jump, aim, use the tool, and finish the room without opening F1.
- The tool affects the same authoritative cells shown by the renderer; the water/fire chain has a visible and testable result.
- No spawn, geometry, or reaction state can trap the player without a recovery path.
- The cave and character remain readable at 1280×720 with bloom disabled.
- A repeat run with the same seed and input sequence produces the same world result under the chosen determinism contract.
- Save/reload is added only after the ownership of player/tool state is explicit; then it restores the interaction result exactly.

Evidence for the rendered check is captured at 1280×720 with bloom off in the GUI-mode sign-off runner above. Visual composition/content were checked on Mesa llvmpipe; target-Mac GPU performance remains a separate gate.

## Explicit non-goals

- Full Noita feature parity, copied assets/layouts, dozens of spells, shops, or a complete roguelite progression loop.
- Infinite world streaming, multiplayer, broad enemy AI, rigid-body fragments, or a second simulation backend.
- Hundreds of materials or a general-purpose spell scripting language.

## Suggested order

1. Profile the completed route on the target Mac GPU; local visual captures do not substitute for device performance.
2. Gather hands-on playtest feedback and address concrete traversal/readability issues.
3. Keep broader content and progression outside this first-room slice until those checks are complete.

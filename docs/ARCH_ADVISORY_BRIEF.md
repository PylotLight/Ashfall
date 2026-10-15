# Architecture Advisory Brief — falling-sand survival RPG (Godot 4.7 + C++)

**Goal:** a Noita-like: pixel cellular simulation (sand/water/fire/oil/lava) the player digs, pours, and ignites. **Current state:** P0–P2 complete and verified (all suites green). The sim is real and data-driven; the screenshot looks flat and nothing like Noita. We want to know what to change, in what order.

## 1. Architecture (3 layers, strict ownership)

- **`engine/` (portable C++17, zero Godot types): owns all sim state and rules.** 512×512 grid of `uint8` material ids; per-tick `step()` runs: reactions pre-pass → movement dispatch (keyed off a `Movement` enum: static/powder/liquid/gas, never material ids) → integer temperature conduction over a sparse `int16` map. No RNG, no wall clock, no floats in sim logic.
- **Data (`data/materials.json`, `data/reactions.json`, versioned): owns content.** 10 materials (air/stone/sand/water/steam/fire/wood/lava/rock/oil) with id/name/movement/solid/density/color/tags/hardness; 9 reaction rows (probability, tag-or-id reactants/products with positional mapping, temperature gates, product temps). Adding oil + its burn rule required zero engine-logic changes (verified by empty-diff gate) — this is our proven exit criterion.
- **Determinism contract (non-negotiable):** all randomness is integer hashes of `(position, tick, rule-index)`; same data + same inputs → bitwise-identical worlds, including across save/load. Tests assert exact counts and byte-identical saves.
- **`bridge/` (GDExtension `AshfallWorld` node): translates, never simulates.** Owns the `SimulationWorld`, snapshots dirty 64×64 chunks into two CPU layers: opaque base RGBA8 (one flat color per material ± tiny static grain) + liquids-only overlay (depth-shaded RGB + coverage alpha). 256-entry caches, no per-pixel map lookups.
- **Godot host (`scripts/main_controller.gd`): presentation + input only.** Paints brush strokes, steps at 60 Hz, draws palette UI. One canvas shader (`water_fx.gdshader`) smooths liquid edges with linear filtering + smoothstep, foam-lit rims, and a ±2% static shimmer. No other shaders, no lights, no glow, no background — the world floats on a flat void.

## 2. Measured facts

- Sim step: ~6–7 ms for 512² at 60 Hz (screenshot HUD) — most of the frame budget is already spent before rendering anything fancy.
- Cells render 1:1 as hard squares; static grain is ±6/255 (invisible at normal zoom); liquids get a 5-deep depth ramp + surface highlight; fire is just orange gas pixels that rise and pop out of existence (no smoke material, no embers, no flicker).
- Camera is fixed, whole-world framing; no follow, no zoom, no shake, no particles, no audio.

## 3. Honest gap list vs. Noita's look/feel (our ranking, challenge it)

1. **Zero light transport.** Noita's fire/lava/explosions *light their surroundings* and bloom. Ours emit nothing — a burning pile looks identical at midnight. Likely the single biggest missing system.
2. **Static pixels.** Our grain is frozen by design (determinism); Noita's materials visibly boil, flicker, pulse, and shimmer. We need *deterministic-looking* animation (tick-driven hash flicker is allowed — wall clock is not) or presentation-layer animation that doesn't touch sim state.
3. **One-tone materials.** Every solid is a flat fill; Noita's dirt/stone/wood have multi-tone noisy texture with structure at 2–4 px scale. Our schema has one RGB per material — probably needs a texture/pattern extension.
4. **No atmosphere.** Void background, no parallax, no ambient particles, no smoke/embers/sparks from combustion, no dig/place juice. Combustion currently produces nothing but disappearing pixels.
5. **Framing.** Full-world fixed camera at 1 cell ≈ 1–2 px. Noita frames a character at ~2–4× that pixel scale with a following camera; pixel density on screen is a huge part of the feel.
6. **Thin content.** 10 materials vs. hundreds; no melting/freezing/phase-change rows yet (designed, roster B), no material variants (dirt, coal, snow, acid, blood), so emergent chains run out after one step.

## 4. Constraints on any proposal

- Sim must stay deterministic, data-driven, and Godot-free; new visuals should live in `bridge/`+shaders unless a sim feature (e.g. a `glow`/`light` material property, smoke emission on burnout) is genuinely required — and then it needs data + test + save-compat per our agent guide.
- 60 Hz budget with ~7 ms already burned in sim; full-screen post effects need a perf note.
- Small team, incremental phases with exit criteria (P3 Rendering is next, unscoped).

## 5. Questions for the advising model

1. In what order would you attack §3 for maximum Noita-feel per unit effort, and what is the smallest shippable first step?
2. How would you add fire/light/glow so light affects nearby pixels — cheap 2D light propagation on the grid, a shader-only bloom/glow pass, or a sim `temperature→light` field — given the §4 constraints?
3. How do we get lively (flickering, boiling, pulsing) materials without breaking determinism or save identity — tick-hash animation in the painter, TIME-based shaders, or something else? Where exactly should the animation live?
4. Is one-flat-color-per-material + code grain a dead end — should the material schema gain multi-tone/noise textures, and what is the minimal version of that?
5. Any rendering architecture risks in the two-layer dirty-chunk snapshot approach as we scale toward lighting/particles/camera?

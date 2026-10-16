# Noita Sim Reference SPEC (clean-room, R9)

Extracted via REA from installed Noita data files as *roles and ranges* for
Ashfall's sim-v2 design. Not copied tuning: re-tune every number for Ashfall.
Noita assets, code, layouts, and exact values stay out of this repo.

Source files (local install, not vendored):
`data_unpacked/materials.xml`, `data_unpacked/shaders/*` (23 files),
`noita.exe` shader-path string table.

## Material model

- ~224 base `CellData` + ~245 `CellDataChild` overrides; `cell_type` counts:
  liquid 154, solid 40, gas 21, fire 4. Static rock is `cell_type=liquid` +
  `liquid_static=1` + `solid_static_type` — static-ness is a flag, not a type.
- Attribute roles worth adopting in `MaterialSpec` v2:
  - `density` — sink/float layering only. Fire/gas ~bottom, water-like low,
    sand mid, rock-like high, exotics highest.
  - `liquid_gravity` — fall-speed multiplier. Lava near 0 (slow), water above
    mid, sand below 1.
  - `liquid_viscosity` — flow resistance; only thick fluids set it, default
    free flow.
  - `stickyness` — ceiling/wall cling; niche, keep 0–1.
  - `lifetime` — self-decay timer for gases/smokes/stains.
  - `hp` — erase/damage HP (dirt low, rock very high); `durability` — separate
    tool/explosion tier. Split them: HP is not hardness.
  - `temperature_of_fire` — emitted heat class; `autoignition_temperature` —
    ignition threshold (non-flammables pin at ceiling).
  - `fire_hp` — burn-duration budget; -1 means eternal.
  - `gfx_glow` — bloom-mask brightness; only fire/lava/hot metals set it.
  - `liquid_sticks_to_ceiling / liquid_stains / solid_friction /
    solid_restitution / platform_type` — small flags with visible payoff.
- Inheritance: `CellDataChild(_parent, _inherit_reactions)` — adopt as
  profile inheritance, not per-material copy/paste.

## Reactions

- 18 attribute roles: `input_cell1..3 / output_cell1..3` (literal or `[tag]`
  wildcard), `probability` (per-step chance), `direction` (gravity-biased),
  `fast_reaction` (extra solver passes), `req_lifetime`, `blob_radius1/2`
  (+restrict: clump size that stays in place),
  `destroy_horizontally_lonely_pixels`, `cosmetic_particle`, `entity`
  (material-to-entity gateway, e.g. powders spawning critters), `audio_fx`.
- Patterns: hot-rock + water → rock + steam (patch-sized blobs); flame-tag as
  unconsumed catalyst for melt/freeze tags; acid-tag persists while emitting
  gas (fast, small blobs); steam + static → water (low-p condensation);
  powder + air → entity spawn.
- Ashfall mapping: our `reactions.json` already has probability + tag-or-id +
  positional products + deterministic rolls — the missing knobs are
  `direction`, `fast`, `blob radius`, and the entity gateway.

## Shader pipeline order (structure only)

1. `default.vert` transform/UV passthrough.
2. `sprite_*` cell/sprite decoders (pixel-art filter, stain blend, tile masks).
3. `process_cellgrid_expand_colors` — neighbor-fill transparent texels
   (anti-fringe for upscale).
4. `post_cell_res_blit` — cell-res upscale blit.
5. `post_glow1` temporal decay ← prev frame; `post_glow2` 2-tap blur +
   afterimage suppression → glow textures.
6. `post_final` composite: bg + fg + lights + skylight + glow + fog +
   dithering + grading. `sky_gradient` feeds sky tint.
7. `imgui_*` UI icons/text on top.
- Our Ashfall equivalent today: identity LUT → profile shader → water
  overlay → clustered 2D lights → restrained bloom. Keep that order; add the
  temporal-decay glow only when flicker forces it.

## Sim-v2 field proposal (names are ours)

`phase{fluid,powder,static,gas,flame}`, `density`, `gravity_scale`,
`viscosity` (default 0), `stick` 0–1, `static` + `platform`, `hp` + `hardness`,
`burn{ignition_temp, fire_temp, fuel}`, `thermal{freeze_to, melt_to}`,
`glow{intensity,color}`, `stains{}`, `tags[]`, `lifetime_frames|null`.
Reactions gain `direction/fast/blob_r/entity/sfx` gating.

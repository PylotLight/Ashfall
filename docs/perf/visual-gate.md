# Visual Gate (R9)

Six fixed viewpoints, bloom-off, 1280x720, same xvfb harness as the P4A
sign-off:

```sh
xvfb-run -a godot --audio-driver Dummy --path . --resolution 1280x720 --windowed --script tests/godot_visual_gate.gd
```

Frames land in `docs/perf/captures/2026-10-16-gate-*-bloom-off.png`
(spawn, mouth, chamber, spring, east, exit). Each frame also runs a coarse
luminance-spread probe (`hi - lo > 0.12` over the world area) that fails on
flat-fog regressions without asserting exact pixels — llvmpipe vs Mac GPU
output legitimately differs, so the gate checks readability, and humans judge
beauty against the reference board.

What R9 changed and why (external feedback round):

- Camera `view_h_cells` 120 → 176 (≈ 4 px/cell at 720p): the wizard reads
  against the room instead of filling it. Asserted in
  `godot_p3a_exit_verify.gd`.
- Wizard marker matches the 2.9 x 5.2-cell collider exactly and snaps to
  integer screen pixels (no more blurry mismatch).
- Veins are strata-following lenses (wide, thin, tilted, feathered) instead
  of round coin stamps.
- Terrain shader: lit top edge on exposed surfaces + crevice AO for tucked
  cells (`top_light`, `crevice_ao`), still branch-disciplined (neighbor-id
  difference only, no material named).
- Backdrop: structured wall (strata bands + ridged cracks + mortar courses)
  instead of smooth fog.
- Sim-v2 reference: `docs/NOITA_SIM_SPEC.md` (roles/ranges, clean-room).

Explicitly not in R9: sim rewrite (liquids film stays until sim v2),
worldgen/player in C++ (1555-line GDScript controller stays), second-cave
generation, Mac GPU profiling.

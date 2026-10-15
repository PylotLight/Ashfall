# `data/material_visuals.json` schema (P3A Slice B1)

Presentation-owned, data-only render profiles. Keyed by stable material NAME
(never table index), so palette edits never become a sim schema migration.
Consumed by `scripts/material_visuals.gd` (`MaterialVisuals.load_from_file` /
`load_from_text`); nothing in `engine/` or `bridge/` reads this file.

## Document

| Key | Type | Rule |
|---|---|---|
| `version` | int | Must be `1`. |
| `visuals` | array | Non-empty, at most 64 entries, one object per material. |

Unknown keys rejected at every level (document, entry, `emission`).

## Entry (one per material)

| Field | Type | Rule |
|---|---|---|
| `material` | string | Stable material name, `[a-z][a-z0-9_-]*`, unique in file, must match a known material (link stage); every known material must be covered. |
| `palette` | array of string | 2–4 colors, each `"#rrggbb"` hex. |
| `pattern` | string | One of `strata`, `grain`, `clusters`, `quiet`, `plume`, `crust`. No material-ID branches downstream — shaders consume profiles only. |
| `pattern_scale` | int | 1–16 (cells). Integral floats accepted (`4.0` == `4`; Godot JSON decodes all numbers as float). |
| `detail_strength` | number | 0–1 (small-scale variation amount; §6: restrained per-cell detail). |
| `edge_response` | number | 0–1 (exposed-boundary treatment strength). |
| `opacity` | number | 0–1. |
| `opacity_mode` | string | One of `opaque`, `translucent`, `additive`. |
| `emission` | object | Exactly `{color, strength}`: `color` `"#rrggbb"`, `strength` number 0–4. Intrinsic emission only — heat glow is added by the shader from sim temperature, never from this file (review §4.4: hot water must not glow). |
| `animation` | string | One of `static`, `flame`, `ember`, `flow`, `wisp` (tick-driven shader-side cadence; world clock, never blanket `TIME`). |

## Limits

Input ≤ 1 MiB, entries ≤ 64, palette 2–4, any single string ≤ 256 chars.

## Error + fallback contract

- Every schema failure names its path: `visuals[3].palette[1]: ...`, `(root): ...` for the document. Syntax failures carry the JSON line.
- ANY failure (missing file, garbage, empty file, unknown key, bad name, bad color, missing field, limit breach, unknown/link material, missing coverage) returns `ok == false` with the named `error` and `profiles` filled with safe defaults for every roster entry (flat opaque mid-grey, non-emissive, static).
- `load_from_file` additionally logs loudly (`push_warning` on missing file, `push_error` on rejection). Invalid visuals never block boot and never touch sim state.

## Known deviation

Godot's JSON parser resolves duplicate object keys last-wins (the Group-3 C++
subset parser rejects them). This presentation loader cannot observe
duplicates; authors must not rely on them.

## R7 depth rate (no JSON field, by design)

There is deliberately NO `depth_rate` entry field: the overburden curve
(`exp(-depth*rate)` in `shaders/material_profile.gdshader` §9) rides LUT
`tex3.g` (tex3.r stays the palette count), filled by
`scripts/material_profile_glue.gd` with `DEFAULT_DEPTH_RATE` (0.04) for every
row. The shader also applies a 0.30 global minimum attenuation so deep cave
walls stay distinct from the atmospheric void. Per-material depth tuning
therefore means touching the glue/shader defaults or extending
`build_params_image` — never this schema — so LUT shapes/bytes pinned by the E2
suite stay green.

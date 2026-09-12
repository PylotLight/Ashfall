extends RefCounted

# P3A Slice B2 — GDScript glue feeding loaded profiles to the profile shader.
#
# Presentation-only (no sim rules, no scene/controller edits — callers wire
# the returned ShaderMaterial when their slice owns the view; B2 proves the
# path headless). Pipeline:
#   Visuals.load_from_file (scripts/material_visuals.gd, keyed by stable NAME)
#     -> build_palette_image / build_params_image (row = table id 0..255)
#     -> make_material (ShaderMaterial over shaders/material_profile.gdshader)
# The TextureRect showing this material MUST use the bridge identity texture
# as its texture with NEAREST filtering + no mipmaps (categorical identity);
# seed_tex/profile LUTs ride as shader parameters (also nearest, set here).
#
# eval_cell_cpu mirrors the shader's static (anim_mix = 0) evaluation so the
# headless probe can assert profile fidelity per pixel without a GPU.
# eval_anim_cpu / eval_anim_parts_cpu mirror shader section 7 (Slice D1) for
# the tick-driven proof: inputs change with tick, freeze with tick.
#
# P3A Slice D, task D3 — halo/gutter rules (measured by the throwaway
# /tmp/d3_dirty_proof.gd against the live cave; dirty px/frame, submits, and
# seam scans — not asserted). 64-cell seams cannot appear by construction:
#   H1 CPU paint samples VERTICALLY ONLY (surface y-1, depth y+1..+5); no
#      horizontal neighbor reads exist, so no +-cx halo is needed or allowed.
#   H2 Every vertical read radius r must be covered by repaint expansion: the
#      bridge expands dirty +-1 cy (64 rows) >> the 5-row paint halo. Never
#      shrink the expansion below 6 rows without a re-proof.
#   H3 Shader-side neighbor sampling is FORBIDDEN on cropped views: profile +
#      water shading stay per-texel center-sample-only, and update_present
#      maps view_origin/view_size in sim space so crops stay consecutive.
#   H4 Camera moves (AtlasTexture region / view_origin) must never dirty the
#      sim or trigger rebakes — pan/zoom across chunk edges submits nothing.
#   H5 Animation inputs are uniforms only (present_tick/phase, anim_mix);
#      identity/seed/snapshot bytes are never mutated for looks — animated
#      (flickering) frames submit 0 uploads at 0 dirty px.
#   H6 GPU upload stays monolithic per painted flush (full-texture update x3,
#      P1.2 design); batching rule: exactly one flush_present per frame, and
#      idle frames submit nothing.

const PROFILE_SHADER: Shader = preload("res://shaders/material_profile.gdshader")
# A2 precedent: preload const, no class_name (parallel-slice safe).
const Visuals = preload("res://scripts/material_visuals.gd")

const PALETTE_W: int = 4
const PARAM_W: int = 4
const PROFILE_ROWS: int = 256

const PATTERN_INDEX := {"quiet": 0.0, "strata": 1.0, "grain": 2.0, "clusters": 3.0, "plume": 4.0, "crust": 5.0}
const OPACITY_INDEX := {"opaque": 0.0, "translucent": 1.0, "additive": 2.0}
const ANIM_INDEX := {"static": 0.0, "flame": 1.0, "ember": 2.0, "flow": 3.0, "wisp": 4.0}

const BG_COLOR := Color(18.0 / 255.0, 20.0 / 255.0, 24.0 / 255.0)


## Loaded-profile result straight from the B1 loader (roster = live table names).
static func load_profiles(world: AshfallWorld) -> Dictionary:
	var roster: Array = []
	for id in world.get_material_ids():
		roster.append(world.get_material_name(id))
	return Visuals.load_from_file("res://data/material_visuals.json", roster)


## 4x256 RGBA8: one row per table id, palette padded by repeating last color.
static func build_palette_image(profiles: Dictionary, world: AshfallWorld) -> Image:
	var img := Image.create(PALETTE_W, PROFILE_ROWS, false, Image.FORMAT_RGBA8)
	for id in PROFILE_ROWS:
		var cols := row_palette(profiles, world, id)
		for x in PALETTE_W:
			img.set_pixel(x, id, cols[x])
	return img


## 4x256 RGBAF: tex0=(pattern_mode,pattern_scale,detail,edge)
## tex1=(opacity,opacity_mode,emission_strength,anim)
## tex2=(emission rgb,0) tex3=(palette_count,0,0,0).
static func build_params_image(profiles: Dictionary, world: AshfallWorld) -> Image:
	var img := Image.create(PARAM_W, PROFILE_ROWS, false, Image.FORMAT_RGBAF)
	for id in PROFILE_ROWS:
		var prof := row_profile(profiles, world, id)
		var pal: Array = prof["palette"]
		var em: Dictionary = prof["emission"]
		var emc: Color = em["color"]
		img.set_pixel(0, id, Color(PATTERN_INDEX[prof["pattern"]], float(prof["pattern_scale"]),
			prof["detail_strength"], prof["edge_response"]))
		img.set_pixel(1, id, Color(prof["opacity"], OPACITY_INDEX[prof["opacity_mode"]],
			em["strength"], ANIM_INDEX[prof["animation"]]))
		img.set_pixel(2, id, Color(emc.r, emc.g, emc.b, 0.0))
		img.set_pixel(3, id, Color(float(pal.size()), 0.0, 0.0, 0.0))
	return img


## ShaderMaterial over the profile shader, LUTs baked, clock at world tick.
## view_origin/view_size default to the full texture (camera slices narrow them).
static func make_material(world: AshfallWorld, profiles: Dictionary,
		view_origin: Vector2i = Vector2i.ZERO, view_size: Vector2i = Vector2i.ZERO) -> ShaderMaterial:
	var size := Vector2i(world.get_world_width(), world.get_world_height())
	var region := view_size if view_size != Vector2i.ZERO else size
	var sm := ShaderMaterial.new()
	sm.shader = PROFILE_SHADER
	sm.set_shader_parameter("seed_tex", world.get_seed_texture())
	sm.set_shader_parameter("profile_palette",
		ImageTexture.create_from_image(build_palette_image(profiles, world)))
	sm.set_shader_parameter("profile_params",
		ImageTexture.create_from_image(build_params_image(profiles, world)))
	sm.set_shader_parameter("world_size", Vector2(size))
	sm.set_shader_parameter("view_origin", Vector2(view_origin))
	sm.set_shader_parameter("view_size", Vector2(region))
	sm.set_shader_parameter("bg_color", Vector3(BG_COLOR.r, BG_COLOR.g, BG_COLOR.b))
	update_present(sm, world, view_origin, region)
	sm.set_shader_parameter("emission_mix", 1.0) # C1 enabled: intrinsic-only, no heat input
	return sm


## Slice D1 animation clock. update_present feeds present_tick from the world
## tick every flush; present_phase is the optional 0..1 interpolation fraction
## between sim ticks (held at 0.0 — no interpolation — until a slice owns it);
## anim_mix is 1.0 live (0 selects the bit-exact static reference).
## Forced capture values for E2 visual regression (determinism):
##   set_animation_override(0, 0.0, 0.0) -> static bit-identical reference
##   set_animation_override(N, 0.0, 1.0) -> forced animated frame at tick N
## A negative channel selects the live value for that channel.
static var _anim_override_tick := -1
static var _anim_override_phase := -1.0
static var _anim_override_mix := -1.0


static func set_animation_override(tick := -1, phase := -1.0, mix := -1.0) -> void:
	_anim_override_tick = tick
	_anim_override_phase = phase
	_anim_override_mix = mix


static func clear_animation_override() -> void:
	_anim_override_tick = -1
	_anim_override_phase = -1.0
	_anim_override_mix = -1.0


## Resolved (tick, phase, mix) uniforms: live world values unless overridden.
static func resolve_animation(world: AshfallWorld) -> Dictionary:
	var tick := int(world.get_tick())
	if _anim_override_tick >= 0:
		tick = _anim_override_tick
	var phase := 0.0
	if _anim_override_phase >= 0.0:
		phase = clampf(_anim_override_phase, 0.0, 1.0)
	var mix := 1.0
	if _anim_override_mix >= 0.0:
		mix = clampf(_anim_override_mix, 0.0, 1.0)
	return {"tick": tick, "phase": phase, "mix": mix}


## Per-frame present refresh: world clock only (tick/phase declared for Slice D).
static func update_present(sm: ShaderMaterial, world: AshfallWorld,
		view_origin: Vector2i = Vector2i.ZERO, view_size: Vector2i = Vector2i.ZERO) -> void:
	var a := resolve_animation(world)
	sm.set_shader_parameter("present_tick", int(a["tick"]))
	sm.set_shader_parameter("present_phase", float(a["phase"]))
	sm.set_shader_parameter("anim_mix", float(a["mix"]))
	var region := view_size
	if region == Vector2i.ZERO:
		region = Vector2i(world.get_world_width(), world.get_world_height())
	sm.set_shader_parameter("view_origin", Vector2(view_origin))
	sm.set_shader_parameter("view_size", Vector2(region))


## Deliberate categorical decode of an R8 identity pixel (mirrors the shader).
static func decode_identity(px: Color) -> int:
	return int(floor(px.r * 255.0 + 0.5))


## GDScript mirror of AshfallWorld::stable_seed (uint32 wrap per op).
static func stable_seed(x: int, y: int) -> int:
	var h: int = (x * 0x9E3779B1) & 0xFFFFFFFF
	var t: int = ((y * 0x85EBCA77) + 0x9E3779B9 + ((h << 6) & 0xFFFFFFFF) + (h >> 2)) & 0xFFFFFFFF
	h = (h ^ t) & 0xFFFFFFFF
	h = (h ^ (h >> 13)) & 0xFFFFFFFF
	h = (h ^ (h >> 7)) & 0xFFFFFFFF
	return int(h & 0xFF)


## CPU mirror of the shader's static evaluation (anim_mix = 0, emission on:
## emission_mix = 1 intrinsic-only — no temperature input exists, so heat
## contributes nothing by construction). Mirrors shader section 6 exactly:
## luminance weight ((lum-0.25)/0.55 clamped, squared), core whitening
## (emission color lerped toward (1,0.96,0.88) by weight*0.45), gated on
## rendered coverage (air/translucent cells return background, unlit).
static func eval_cell_cpu(profiles: Dictionary, mat_name: String, x: int, y: int, seed_byte: int) -> Color:
	var prof: Dictionary = profiles.get(mat_name, Visuals.default_profile(mat_name))
	var pal: Array = row_palette_for(prof)
	var mode: float = PATTERN_INDEX[prof["pattern"]]
	var pscale: float = maxf(float(prof["pattern_scale"]), 1.0)
	var detail: float = clampf(float(prof["detail_strength"]), 0.0, 1.0)
	var count: float = maxf(float(pal.size()), 1.0)
	var seed01 := float(seed_byte) / 255.0
	var sstep := float(seed_byte)
	var cell := Vector2(x, y)
	var pidx := 0.0
	if mode < 0.5:
		pidx = 0.0
	elif mode < 1.5:
		pidx = fposmod(floor((cell.y + fposmod(sstep, pscale)) / pscale), count)
	elif mode < 2.5:
		var wob := (_hash12(floor(cell.x / 4.0), floor(cell.y / (pscale * 2.0))) - 0.5) * pscale * 0.5
		pidx = fposmod(floor((cell.y + wob) / maxf(pscale * 0.5, 1.0)), count)
	elif mode < 3.5:
		var blk := Vector2(floor(cell.x / pscale), floor(cell.y / pscale))
		pidx = floor(_hash12(blk.x, blk.y) * count)
	elif mode < 4.5:
		var t := _fract((cell.y + (_hash12(cell.x, 0.0) - 0.5) * pscale) / maxf(pscale * 2.0, 1.0))
		pidx = floor(t * count)
	else:
		var blk2 := Vector2(floor(cell.x / pscale), floor(cell.y / pscale))
		var h := _hash12(blk2.x + 7.3, blk2.y + 7.3)
		pidx = 0.0 if h < 0.7 else (1.0 if h < 0.9 else count - 1.0)
	var base: Color = pal[clampi(int(floor(pidx + 0.5)), 0, 3)]
	var col := base * (1.0 + (seed01 - 0.5) * 2.0 * detail)
	if float(prof["opacity"]) < 0.01:
		return BG_COLOR
	var opmode: float = OPACITY_INDEX[prof["opacity_mode"]]
	if opmode > 0.5 and opmode < 1.5:
		return BG_COLOR
	# Section 6 mirror: intrinsic emission only (emission_mix = 1).
	var em: Dictionary = prof["emission"]
	var emc: Color = em["color"]
	var estr := float(em["strength"])
	var cc := Color(clampf(col.r, 0.0, 1.0), clampf(col.g, 0.0, 1.0), clampf(col.b, 0.0, 1.0))
	var elum := clampf(0.299 * cc.r + 0.587 * cc.g + 0.114 * cc.b, 0.0, 1.0)
	var eweight := clampf((elum - 0.25) / 0.55, 0.0, 1.0)
	eweight *= eweight
	var ecolor: Color = emc.lerp(Color(1.0, 0.96, 0.88), eweight * 0.45)
	var e := estr * eweight # emission_mix = 1, coverage already gated above
	return Color(col.r + ecolor.r * e, col.g + ecolor.g * e, col.b + ecolor.b * e, 1.0)


# --- internals -------------------------------------------------------------
## CPU mirror of shader section 7 (Slice D1): raw amod before the coverage
## gate and anim_mix, for (anim profile, cell, static seed, tick+phase).
## Headless proof only — never drives rendering.
static func eval_anim_cpu(anim: String, x: int, y: int, seed_byte: int, tick: int, phase: float) -> float:
	var total := 0.0
	for _k in eval_anim_parts_cpu(anim, x, y, seed_byte, tick, phase).values():
		total += float(_k)
	return total


## Per-component mirror of section 7, so the proof can compare regional vs
## cell structure directly. Keys per anim: flame -> {region, cell, rise},
## ember -> {region}, flow -> {ripple, spark}, wisp -> {drift}, static -> {}.
static func eval_anim_parts_cpu(anim: String, x: int, y: int, seed_byte: int, tick: int, phase: float) -> Dictionary:
	var at := float(tick) + clampf(phase, 0.0, 1.0)
	var seed01 := float(seed_byte) / 255.0
	var cell := Vector2(x, y)
	match anim:
		"flame":
			var rphase := _hash12(floor(cell.x / 4.0), floor(cell.y / 4.0)) * 6.2831
			return {
				"region": sin(at * 0.9 + rphase) * 0.10,
				"cell": sin(at * 2.7 + seed01 * 6.2831) * 0.03,
				"rise": sin((cell.y + at * 3.0) * 0.9 + (_hash12(cell.x, 3.7) - 0.5) * 4.0) * 0.04,
			}
		"ember":
			var rphase := _hash12(floor(cell.x / 4.0), floor(cell.y / 4.0)) * 6.2831
			return {"region": sin(at * 0.25 + rphase) * 0.05}
		"flow":
			var ripple := sin((cell.x + cell.y) * 0.35 + at * 0.5) * 0.015
			var gate := 0.5 + 0.5 * sin(at * 0.5 + _hash12(floor(cell.x / 3.0), floor(cell.y / 3.0)) * 6.2831)
			return {"ripple": ripple, "spark": gate * gate * gate * gate * 0.05}
		"wisp":
			return {"drift": sin((cell.y + at * 1.0) * 0.5 + _hash12(cell.x, 9.1) * 6.2831) * 0.03}
	return {}

static func row_profile(profiles: Dictionary, world: AshfallWorld, id: int) -> Dictionary:
	var mat_name := ""
	if id >= 0 and id <= 255:
		mat_name = world.get_material_name(id)
	if profiles.has(mat_name):
		return profiles[mat_name]
	return Visuals.default_profile(mat_name)


static func row_palette(profiles: Dictionary, world: AshfallWorld, id: int) -> Array:
	return row_palette_for(row_profile(profiles, world, id))


static func row_palette_for(prof: Dictionary) -> Array:
	var pal: Array = (prof["palette"] as Array).duplicate()
	while pal.size() < 4:
		pal.append(pal[pal.size() - 1])
	return pal


static func _fract(v: float) -> float:
	return v - floor(v)


static func _hash12(px: float, py: float) -> float:
	var p3x := _fract(px * 0.1031)
	var p3y := _fract(py * 0.1031)
	var p3z := _fract(px * 0.1031)
	var d := p3x * (p3y + 33.33) + p3y * (p3z + 33.33) + p3z * (p3x + 33.33)
	p3x += d
	p3y += d
	p3z += d
	return _fract((p3x + p3y) * p3z)

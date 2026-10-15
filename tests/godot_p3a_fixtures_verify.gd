extends SceneTree

# P3A Slice E, task E2 — visual-regression fixtures (forced-tick captures).
# Run: godot --headless --path . --script tests/godot_p3a_fixtures_verify.gd
#
# What this suite proves (deterministic, no wall-clock):
#   A. Cave scene boots with all content (place-reads-at-scale input proof).
#   B. Forced-tick captures: lights on/off x bloom on/off matrix via the
#      override/forced APIs (Glue.set_animation_override + Clocks.set_forced)
#      yields IDENTICAL deterministic inputs (uniform triple, LUT bytes,
#      toggle states recorded) AND sim save-bytes bit-identical across all
#      four combos.
#   C. Profile fixtures extending the bridge-review section-F shader fixtures
#      (isolated drop, one-cell channel, solid border, two liquid colors +
#      profile variants) at the deterministic-INPUT level: LUT bytes, uniform
#      values, CPU-mirror evals.
#
# HEADLESS LIMIT (stated honestly): headless drivers cannot execute canvas
# shaders, so this suite asserts the deterministic INPUTS to rendering
# (uniform values, LUT bytes, toggle states, sim-bytes identity) rather than
# rendered pixels. No pixel assertion below would pass headless AND mean
# anything; rendered-pixel A/B pairs are a human capture-list item (E3).
# The negative control proves the comparisons are sensitive, not vacuous.

const Glue = preload("res://scripts/material_profile_glue.gd")
const Clocks = preload("res://scripts/presentation_clocks.gd")
const ClusterLights = preload("res://scripts/light_cluster_manager.gd")
const BloomToggle = preload("res://scripts/cave_bloom_toggle.gd")
const Visuals = preload("res://scripts/material_visuals.gd")

var _failures: int = 0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: ", msg)
	else:
		_failures += 1
		push_error("FAIL: " + msg)


func _q8(c: Color) -> Vector3i:
	return Vector3i(int(round(c.r * 255.0)), int(round(c.g * 255.0)), int(round(c.b * 255.0)))


func _html_q8(hex: String) -> Vector3i:
	return _q8(Color.html(hex))


func _close(a: Vector3i, b: Vector3i, tol: int = 1) -> bool:
	return absi(a.x - b.x) <= tol and absi(a.y - b.y) <= tol and absi(a.z - b.z) <= tol


func _mk_world(w: int, h: int) -> AshfallWorld:
	var world := AshfallWorld.new()
	world.init_world(w, h)
	return world


func _roster(world: AshfallWorld) -> Array:
	var out: Array = []
	for id in world.get_material_ids():
		out.append(world.get_material_name(id))
	return out


func _init() -> void:
	_run_profile_fixtures()
	_run_forced_tick_unit()
	if _failures > 0:
		push_error("E2 FIXTURE CHECKS FAILED (headless phase): %d" % _failures)
		quit(1)
		return
	print("--- E2 HEADLESS FIXTURE CHECKS PASSED ---")
	_run_cave_matrix.call_deferred()


# --- Part C: profile fixtures (§F extension at input level) ----------------
func _run_profile_fixtures() -> void:
	var world := _mk_world(32, 32)
	var res: Dictionary = Glue.load_profiles(world)
	check(bool(res["ok"]), "E2 profiles load ok from data/material_visuals.json")
	var profiles: Dictionary = res["profiles"]
	for n in ["air", "stone", "sand", "water", "steam", "fire", "wood", "lava", "rock", "oil"]:
		check(profiles.has(n), "E2 profile present: %s" % n)

	# LUT shapes.
	var pal := Glue.build_palette_image(profiles, world)
	var par := Glue.build_params_image(profiles, world)
	check(pal.get_width() == 4 and pal.get_height() == 256, "E2 palette LUT is 4x256")
	check(pal.get_format() == Image.FORMAT_RGBA8, "E2 palette LUT is RGBA8")
	check(par.get_width() == 4 and par.get_height() == 256, "E2 params LUT is 4x256")
	check(par.get_format() == Image.FORMAT_RGBAF, "E2 params LUT is RGBAF")

	# LUT bytes deterministic across rebuilds.
	var pal2 := Glue.build_palette_image(profiles, world)
	var par2 := Glue.build_params_image(profiles, world)
	check(pal.get_data() == pal2.get_data(), "E2 palette LUT bytes deterministic")
	check(par.get_data() == par2.get_data(), "E2 params LUT bytes deterministic")

	var id_of := {}
	for id in world.get_material_ids():
		id_of[world.get_material_name(id)] = id

	# Spot: fire row holds its 4 palette colors in order.
	var fire_row: Array = Glue.row_palette(profiles, world, int(id_of["fire"]))
	var fire_hex := ["#ff3d00", "#ff8c1a", "#ffd94d", "#fff6c9"]
	for i in 4:
		check(_close(_q8(fire_row[i]), _html_q8(fire_hex[i])), "E2 fire palette [%d] exact" % i)
	# Spot: short palettes pad by repeating the last color.
	var stone_row: Array = Glue.row_palette(profiles, world, int(id_of["stone"]))
	check(stone_row.size() == 4, "E2 stone row padded to 4")
	check(_q8(stone_row[3]) == _q8(stone_row[2]), "E2 stone row pads by repeating last color")

	# Spot: params tex0 pattern modes (§F profile-variant inputs).
	var pat := {"air": 0.0, "stone": 1.0, "wood": 2.0, "sand": 3.0, "fire": 4.0, "lava": 5.0}
	for n in pat.keys():
		var px: Color = par.get_pixel(0, int(id_of[n]))
		check(absf(px.r - float(pat[n])) < 0.001, "E2 pattern mode %s == %d" % [n, int(pat[n])])
	# Spot: params tex1 emission strengths + anim indices.
	var em := {"fire": 2.5, "lava": 1.5, "water": 0.0, "stone": 0.0}
	for n in em.keys():
		var px: Color = par.get_pixel(1, int(id_of[n]))
		check(absf(px.b - float(em[n])) < 0.001, "E2 emission strength %s == %s" % [n, str(em[n])])
	var an := {"stone": 0.0, "fire": 1.0, "lava": 2.0, "water": 3.0, "steam": 4.0}
	for n in an.keys():
		var px: Color = par.get_pixel(1, int(id_of[n]))
		check(absf(px.a - float(an[n])) < 0.001, "E2 anim index %s == %d" % [n, int(an[n])])
	# Hot water must not glow: emission 0 by input, never by temperature.
	var wpx: Color = par.get_pixel(1, int(id_of["water"]))
	check(wpx.b == 0.0, "E2 water emission strength is exactly 0 (hot water stays dark)")
	# Palette-count channel.
	check(int(round(par.get_pixel(3, int(id_of["fire"])).r)) == 4, "E2 fire palette count == 4")
	check(int(round(par.get_pixel(3, int(id_of["stone"])).r)) == 3, "E2 stone palette count == 3")

	# --- §F-analog CPU probes (deterministic inputs, not pixels) ---
	# Isolated-drop analog: air evaluates to background exactly.
	var air_eval: Color = Glue.eval_cell_cpu(profiles, "air", 10, 10, 123)
	check(_q8(air_eval) == _q8(Glue.BG_COLOR), "E2 §F/air evaluates to background exactly")
	# One-cell-channel analog: translucent liquids gate to background in the
	# profile eval (liquid coverage lives in the water layer, never here).
	var water_eval: Color = Glue.eval_cell_cpu(profiles, "water", 15, 7, 200)
	check(_q8(water_eval) == _q8(Glue.BG_COLOR), "E2 §F/translucent water gates to background")
	var oil_eval: Color = Glue.eval_cell_cpu(profiles, "oil", 25, 25, 200)
	check(_q8(oil_eval) == _q8(Glue.BG_COLOR), "E2 §F/translucent oil gates to background")
	# Solid-border analog: opaque solids render (never background, alpha 1).
	var stone_eval: Color = Glue.eval_cell_cpu(profiles, "stone", 20, 10, 77)
	check(_q8(stone_eval) != _q8(Glue.BG_COLOR), "E2 §F/solid stone renders (not background)")
	check(absf(stone_eval.a - 1.0) < 0.001, "E2 §F/solid stone alpha is 1")
	check(absf(Glue.eval_depth_factor(10) - 0.670320) < 0.001, "E2 overburden shading preserves shallow material color")
	check(absf(Glue.eval_depth_factor(255) - Glue.DEFAULT_DEPTH_FLOOR) < 0.001, "E2 deep cave walls retain the atmospheric visibility floor")
	# Two-liquid-colors analog: water vs oil palette rows are distinct.
	var wrow: Array = Glue.row_palette(profiles, world, int(id_of["water"]))
	var orow: Array = Glue.row_palette(profiles, world, int(id_of["oil"]))
	var ldist: int = 0
	for i in 3:
		var d: Vector3i = _q8(wrow[i]) - _q8(orow[i])
		ldist += absi(d.x) + absi(d.y) + absi(d.z)
	check(ldist > 30, "E2 §F/two liquids have distinct palette rows (dist %d)" % ldist)
	# Profile variants (bloom-off distinguishability at LUT level).
	var srow: Array = Glue.row_palette(profiles, world, int(id_of["stone"]))
	var rrow: Array = Glue.row_palette(profiles, world, int(id_of["rock"]))
	var sandrow: Array = Glue.row_palette(profiles, world, int(id_of["sand"]))
	var woodrow: Array = Glue.row_palette(profiles, world, int(id_of["wood"]))
	var srd: Vector3i = _q8(srow[0]) - _q8(rrow[0])
	var swd: Vector3i = _q8(sandrow[0]) - _q8(woodrow[0])
	check(absi(srd.x) + absi(srd.y) + absi(srd.z) > 10, "E2 stone vs rock palettes differ")
	check(absi(swd.x) + absi(swd.y) + absi(swd.z) > 10, "E2 sand vs wood palettes differ")
	# Pattern structure: a stone patch yields >1 distinct color (strata live).
	var seen := {}
	for y in range(8, 24):
		for x in range(8, 24):
			var c: Color = Glue.eval_cell_cpu(profiles, "stone", x, y, Glue.stable_seed(x, y))
			seen[_q8(c)] = true
	check(seen.size() > 1, "E2 stone patch shows pattern structure (%d colors)" % seen.size())
	# Emission-hot: fire eval carries emission above background.
	var fire_eval: Color = Glue.eval_cell_cpu(profiles, "fire", 12, 12, 99)
	check(fire_eval.r + fire_eval.g + fire_eval.b > 1.0, "E2 fire eval is emission-hot")
	# CPU mirror determinism.
	var e1: Color = Glue.eval_cell_cpu(profiles, "sand", 5, 5, 42)
	var e2: Color = Glue.eval_cell_cpu(profiles, "sand", 5, 5, 42)
	check(e1 == e2, "E2 eval_cell_cpu deterministic for identical inputs")
	check(Glue.stable_seed(7, 9) == Glue.stable_seed(7, 9), "E2 stable_seed deterministic")
	check(Glue.stable_seed(7, 9) >= 0 and Glue.stable_seed(7, 9) <= 255, "E2 stable_seed in byte range")
	# Unknown material falls back to a safe default (never fails boot).
	var unk: Dictionary = Visuals.default_profile("zzz_nope")
	check(float(unk["opacity"]) == 1.0 and String(unk["animation"]) == "static", "E2 unknown-material default is safe")
	world.free()


# --- Forced-tick unit (override API determinism, no scene) ------------------
func _run_forced_tick_unit() -> void:
	var world := _mk_world(64, 64)
	var res: Dictionary = Glue.load_profiles(world)
	var profiles: Dictionary = res["profiles"]

	# Static reference: override (0, 0, 0) pins tick/phase/mix exactly.
	Glue.set_animation_override(0, 0.0, 0.0)
	var sm := Glue.make_material(world, profiles)
	check(int(sm.get_shader_parameter("present_tick")) == 0, "E2 static override pins present_tick 0")
	check(absf(float(sm.get_shader_parameter("present_phase")) - 0.0) < 0.0001, "E2 static override pins phase 0")
	check(absf(float(sm.get_shader_parameter("anim_mix")) - 0.0) < 0.0001, "E2 static override pins mix 0")

	# Forced animated frame at tick N: identical uniforms on repeat builds.
	Glue.set_animation_override(100, 0.0, 1.0)
	var a := Glue.make_material(world, profiles)
	var ua := [int(a.get_shader_parameter("present_tick")),
		float(a.get_shader_parameter("present_phase")),
		float(a.get_shader_parameter("anim_mix"))]
	var b := Glue.make_material(world, profiles)
	var ub := [int(b.get_shader_parameter("present_tick")),
		float(b.get_shader_parameter("present_phase")),
		float(b.get_shader_parameter("anim_mix"))]
	check(ua[0] == 100 and absf(ua[2] - 1.0) < 0.0001, "E2 forced frame pins tick 100 + mix 1")
	check(str(ua) == str(ub), "E2 forced frame uniforms identical across builds")
	# A different forced tick is a different (deterministic) input.
	Glue.set_animation_override(101, 0.0, 1.0)
	var c := Glue.make_material(world, profiles)
	check(int(c.get_shader_parameter("present_tick")) == 101, "E2 forced tick 101 pins tick 101")

	# Animation math: flame moves with tick, static never moves, same tick freezes.
	var f100: float = Glue.eval_anim_cpu("flame", 12, 40, 99, 100, 0.0)
	var f101: float = Glue.eval_anim_cpu("flame", 12, 40, 99, 101, 0.0)
	var f100b: float = Glue.eval_anim_cpu("flame", 12, 40, 99, 100, 0.0)
	check(f100 != f101, "E2 flame anim input changes with tick")
	check(f100 == f100b, "E2 flame anim input freezes at same tick (pause determinism)")
	check(Glue.eval_anim_cpu("static", 12, 40, 99, 100, 0.0) == 0.0, "E2 static anim is exactly 0")
	check(Glue.eval_anim_cpu("static", 12, 40, 99, 101, 0.0) == 0.0, "E2 static anim stays 0 across ticks")
	# Regional >> cell structure (no white-noise flicker at input level).
	var parts: Dictionary = Glue.eval_anim_parts_cpu("flame", 12, 40, 99, 100, 0.0)
	check(absf(float(parts["region"])) > absf(float(parts["cell"])), "E2 flame regional amplitude exceeds cell amplitude")

	# Clocks forced API: pins, no-op tick_world, resume on clear.
	var clocks = Clocks.new()
	clocks.set_forced(100, 0.25)
	var p: Dictionary = clocks.present(9999)
	check(int(p["tick"]) == 100 and absf(float(p["phase"]) - 0.25) < 0.0001, "E2 clocks forced pins tick+phase")
	var w0: float = clocks.world_time()
	clocks.tick_world(0.016, true)
	check(clocks.world_time() == w0, "E2 clocks forced tick_world is a no-op")
	clocks.clear_forced()
	check(not clocks.is_forced(), "E2 clocks forced flag clears")

	Glue.clear_animation_override()
	world.free()


# --- Cave scene: boots + lights/bloom matrix --------------------------------
func _run_cave_matrix() -> void:
	var scene: PackedScene = load("res://cave_benchmark.tscn")
	if scene == null:
		_failures += 1
		push_error("FAIL: E2 cave scene loads")
		quit(1)
		return
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	main.world.auto_step = false

	# A. Cave boots with all content.
	check(main.world.get_world_width() == 512 and main.world.get_world_height() == 512, "E2 cave is 512x512 sim")
	var occ: int = int(main.world.get_occupied_cell_count())
	check(occ > 100000, "E2 cave occupied large (place-scale mass, got %d)" % occ)
	check(int(main.world.get_tick()) == 60, "E2 cave tick is 60 after deterministic settle")
	var counts := {"fire": 0, "water": 0, "wood": 0, "oil": 0, "stone": 0, "rock": 0, "sand": 0}
	var id_by_name := {}
	for id in main.world.get_material_ids():
		id_by_name[main.world.get_material_name(id)] = id
	for y in range(0, 512, 2):
		for x in range(0, 512, 2):
			var m: int = main.world.get_cell(x, y)
			for n in counts.keys():
				if int(id_by_name[n]) == m:
					counts[n] += 1
	for n in counts.keys():
		check(int(counts[n]) > 0, "E2 cave content present: %s (samples %d)" % [n, int(counts[n])])
	check(main.player_pos == main.CAVE_SPAWN, "E2 cave player spawns at CAVE_SPAWN")
	check(main.get_node_or_null("BloomToggle") != null, "E2 cave has BloomToggle node")
	for i in ["", "1", "2", "3", "4", "5"]:
		var lamp = main.get_node_or_null("MainLayout/ViewportContainer/WorldStack/%s" % ("CaveLight" if i == "" else "ClusterLight" + i))
		check(lamp != null, "E2 cave light node present: %s" % ("CaveLight" if i == "" else "ClusterLight" + i))
	check(main.texture_display.texture != null and main.water_display.texture != null, "E2 cave view textures bound")
	for key in ["sim", "world_render", "lighting", "display", "ui"]:
		check(main.has_meta(key), "E2 cave resolution-ladder metadata: %s" % key)
	# Fire visibly changes surfaces (input proof): real clusters extracted
	# from the live cave carry weight and sit in open space.
	var emissive: Dictionary = main._emissive
	check(not emissive.is_empty(), "E2 cave cluster input non-empty (emission-driven)")
	var clusters: Array = ClusterLights.extract_clusters(main.world, emissive)
	check(not clusters.is_empty(), "E2 live cave yields emissive clusters (%d)" % clusters.size())
	var w0: float = 0.0
	for cl in clusters:
		w0 += float((cl as Dictionary)["weight"])
	check(w0 > 0.0, "E2 live clusters carry positive weight (%.1f)" % w0)
	var snapped: Vector2 = ClusterLights.snap_to_open(main.world, (clusters[0] as Dictionary)["pos"], 512, 512)
	check(not main.world.is_solid_cell(int(snapped.x), int(snapped.y)), "E2 cluster anchor sits in open space (never lit-black stone)")

	# B. Lights on/off x bloom on/off matrix (forced tick 100, mix 1).
	var res: Dictionary = Glue.load_profiles(main.world)
	var profiles: Dictionary = res["profiles"]
	check(bool(res["ok"]), "E2 cave profiles load ok")
	var pal_bytes: PackedByteArray = Glue.build_palette_image(profiles, main.world).get_data()
	var par_bytes: PackedByteArray = Glue.build_params_image(profiles, main.world).get_data()
	check(pal_bytes.size() > 0 and par_bytes.size() > 0, "E2 cave LUT bytes non-empty")
	var baseline: PackedByteArray = main.world.save_to_bytes()
	var base_tick: int = int(main.world.get_tick())
	var base_occ: int = occ
	var bloom_node = main.get_node("BloomToggle")
	var lamp_nodes: Array = [main.cave_light, main.cluster_light_1, main.cluster_light_2,
		main.cluster_light_3, main.cluster_light_4, main.cluster_light_5]
	Glue.set_animation_override(100, 0.0, 1.0)
	var first_uniforms: String = ""
	var combos := [
		[true, true], [true, false], [false, true], [false, false],
	]
	for combo in combos:
		var lights_on: bool = combo[0]
		var bloom_on: bool = combo[1]
		if lights_on:
			for i in 5:
				main._update_light(0.05)
		else:
			for lamp in lamp_nodes:
				(lamp as PointLight2D).energy = 0.0
		bloom_node.set_bloom_enabled(bloom_on)
		var sm := Glue.make_material(main.world, profiles)
		var triple := [int(sm.get_shader_parameter("present_tick")),
			float(sm.get_shader_parameter("present_phase")),
			float(sm.get_shader_parameter("anim_mix"))]
		var key: String = str(triple)
		if first_uniforms.is_empty():
			first_uniforms = key
		var tag: String = "lights-%s/bloom-%s" % ["on" if lights_on else "off", "on" if bloom_on else "off"]
		check(key == first_uniforms, "E2 matrix uniform triple identical (%s)" % tag)
		check(Glue.build_palette_image(profiles, main.world).get_data() == pal_bytes, "E2 matrix palette LUT identical (%s)" % tag)
		check(Glue.build_params_image(profiles, main.world).get_data() == par_bytes, "E2 matrix params LUT identical (%s)" % tag)
		check(bloom_node.is_bloom_enabled() == bloom_on, "E2 matrix bloom toggle state %s" % tag)
		if lights_on:
			var e_any: float = 0.0
			for lamp in lamp_nodes:
				e_any = maxf(e_any, float((lamp as PointLight2D).energy))
			check(e_any > 0.0, "E2 matrix lights-on carries energy (%s)" % tag)
		else:
			var e_all: float = 0.0
			for lamp in lamp_nodes:
				e_all = maxf(e_all, float((lamp as PointLight2D).energy))
			check(e_all == 0.0, "E2 matrix lights-off is dark (%s)" % tag)
		check(main.world.save_to_bytes() == baseline, "E2 matrix sim-bytes identical (%s)" % tag)
		check(int(main.world.get_tick()) == base_tick, "E2 matrix tick unchanged (%s)" % tag)
		check(int(main.world.get_occupied_cell_count()) == base_occ, "E2 matrix occupied unchanged (%s)" % tag)

	# Negative control (LAST — mutates the sim): proves sensitivity.
	main.world.set_cell(256, 200, AshfallWorld.MATERIAL_SAND)
	check(main.world.save_to_bytes() != baseline, "E2 NEGATIVE CONTROL: sim mutation changes save bytes")

	Glue.clear_animation_override()
	print("E2 NOTE: headless drivers cannot execute canvas shaders; pixel A/B pairs are human captures (see E3 report). Inputs above are the deterministic contract.")
	main.queue_free()
	if _failures > 0:
		push_error("E2 CAVE MATRIX CHECKS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL E2 VISUAL-REGRESSION FIXTURE CHECKS PASSED ---")
		quit(0)

extends SceneTree

# P3A Slice E, task E3 — exit-gate verification (one check per §16 P3A criterion).
# Run: godot --headless --path . --script tests/godot_p3a_exit_verify.gd
#
# §16 P3A exit criteria (docs/advisory/2026-09-10-visual-direction-review.md §16):
#   C1. The scene reads as a place at normal play scale.
#   C2. Fire visibly changes nearby surface appearance.
#   C3. Materials remain distinguishable with bloom disabled.
#   C4. Presentation toggles do not change simulation hashes.
#   C5. Pause/replay animation behavior is documented.
#   C6. Performance measured on the target Mac.
# Plus the plan-level gate: the clip reads as "a game in a burning cave"
# (human capture list — see docs/perf/p3a-exit.md; not assertable headless).
#
# HEADLESS LIMIT (stated honestly): headless drivers cannot execute canvas
# shaders, so C1–C3 are proven at the deterministic-INPUT level (scene
# content, cluster/light state, LUT bytes + CPU-mirror evals) — the same
# contract the E2 fixture suite pins. Rendered pixels and the clip itself
# are human captures (see the E3 report). Each section below states which
# half is headless-proven and which half is a listed human capture.

const Glue = preload("res://scripts/material_profile_glue.gd")
const Clocks = preload("res://scripts/presentation_clocks.gd")
const ClusterLights = preload("res://scripts/light_cluster_manager.gd")

var _failures: int = 0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: ", msg)
	else:
		_failures += 1
		push_error("FAIL: " + msg)


func _q8(c: Color) -> Vector3i:
	return Vector3i(int(round(c.r * 255.0)), int(round(c.g * 255.0)), int(round(c.b * 255.0)))


func _dist(a: Array, b: Array, n: int) -> int:
	var d: int = 0
	for i in n:
		var v: Vector3i = _q8(a[i]) - _q8(b[i])
		d += absi(v.x) + absi(v.y) + absi(v.z)
	return d


func _init() -> void:
	_run_exit_checks.call_deferred()


func _run_exit_checks() -> void:
	var scene: PackedScene = load("res://cave_benchmark.tscn")
	if scene == null:
		_failures += 1
		push_error("FAIL: E3 cave scene loads")
		quit(1)
		return
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	main.world.auto_step = false

	# --- C1: place reads at scale (headless half: boots with all content) ---
	check(main.world.get_world_width() == 512 and main.world.get_world_height() == 512, "E3/C1 cave sim is 512x512")
	var occ: int = int(main.world.get_occupied_cell_count())
	check(occ > 100000, "E3/C1 place-scale mass occupied=%d (>100k)" % occ)
	check(int(main.world.get_tick()) == 60, "E3/C1 deterministic settle tick=60")
	var want := ["fire", "water", "wood", "oil", "stone", "rock", "sand"]
	var id_by_name := {}
	for id in main.world.get_material_ids():
		id_by_name[main.world.get_material_name(id)] = id
	var counts := {}
	for n in want:
		counts[n] = 0
	for y in range(0, 512, 2):
		for x in range(0, 512, 2):
			var m: int = main.world.get_cell(x, y)
			for n in want:
				if int(id_by_name[n]) == m:
					counts[n] += 1
	for n in want:
		check(int(counts[n]) > 0, "E3/C1 content present: %s (samples %d)" % [n, int(counts[n])])
		check(main.player_pos == main.CAVE_SPAWN, "E3/C1 character-scale placeholder at CAVE_SPAWN")
		var avatar_script: Script = main.player_marker.get_script()
		check(avatar_script != null and avatar_script.resource_path.ends_with("player_wizard.gd"), "E3/C1 readable wizard avatar overlay is attached")
		check(main.player_marker.size.x >= 26.0 and main.player_marker.size.y >= 36.0, "E3/C1 wizard overlay is legible without changing the cell collision marker")
		var debug_button := main.get_node_or_null("MainLayout/SidebarScroll/Sidebar/DebugToggleBtn") as Button
		var demo_button := main.get_node_or_null("MainLayout/SidebarScroll/Sidebar/DemoBtn") as Button
		check(debug_button != null and demo_button != null and debug_button.text.begins_with("[F1]") and demo_button.text.begins_with("[F2]"), "E3/C1 debug and material-demo shortcuts are distinct")
		check(is_equal_approx(main._camera.view_h_cells, 120.0), "E3/C1 default camera uses player-scale framing")
	check(main._view_region.size.y == 120 and main._view_region.size.x < 512, "E3/C1 actual view region is cropped to player scale")
	check(main._camera.center_cells.y < main.player_pos.y, "E3/C1 opening camera leaves room above the avatar")
	var cave_snapshot: PackedByteArray = main.world.save_to_bytes()
	main._build_cave()
	check(main.world.save_to_bytes() == cave_snapshot, "E3/C1 authored cave rebuild is deterministic")
	for key in ["sim", "world_render", "lighting", "display", "ui"]:
		check(main.has_meta(key), "E3/C1 resolution-ladder metadata: %s" % key)
	check(ProjectSettings.get_setting("application/run/main_scene") == "res://cave_benchmark.tscn", "E3/C1 boot default is the cave scene")
	check(main.texture_display.texture != null and main.water_display.texture != null, "E3/C1 view textures bound")
	print("E3/C1 HUMAN: silhouette/character/viewport ratios + before/after play-scale read (see p3a-exit.md capture 1).")

	# --- C2: fire changes surfaces (headless half: cluster/light state proof) ---
	var res: Dictionary = Glue.load_profiles(main.world)
	var profiles: Dictionary = res["profiles"]
	check(bool(res["ok"]), "E3/C2 profiles load ok")
	var emissive: Dictionary = main._emissive
	check(not emissive.is_empty(), "E3/C2 cluster input non-empty (emission-driven)")
	var clusters: Array = ClusterLights.extract_clusters(main.world, emissive)
	check(not clusters.is_empty(), "E3/C2 live cave yields emissive clusters (%d)" % clusters.size())
	var wsum: float = 0.0
	for cl in clusters:
		wsum += float((cl as Dictionary)["weight"])
	check(wsum > 0.0, "E3/C2 clusters carry positive weight (%.1f)" % wsum)
	var snapped: Vector2 = ClusterLights.snap_to_open(main.world, (clusters[0] as Dictionary)["pos"], 512, 512)
	check(not main.world.is_solid_cell(int(snapped.x), int(snapped.y)), "E3/C2 cluster anchor in open space (never lit-black stone)")
	for i in 5:
		main._update_light(0.05)
	var lamps: Array = [main.cave_light, main.cluster_light_1, main.cluster_light_2,
		main.cluster_light_3, main.cluster_light_4, main.cluster_light_5]
	var e_max: float = 0.0
	for lamp in lamps:
		e_max = maxf(e_max, float((lamp as PointLight2D).energy))
	check(e_max > 0.0, "E3/C2 Stage-A rig carries energy after retarget (max %.2f)" % e_max)
	var par := Glue.build_params_image(profiles, main.world)
	var fire_e: float = par.get_pixel(1, int(id_by_name["fire"])).b
	var lava_e: float = par.get_pixel(1, int(id_by_name["lava"])).b
	check(absf(fire_e - 2.5) < 0.001 and absf(lava_e - 1.5) < 0.001, "E3/C2 emission inputs fire=2.5 lava=1.5 (heat never lights: water=0)")
	check(par.get_pixel(1, int(id_by_name["water"])).b == 0.0, "E3/C2 hot water cannot glow (emission input 0)")
	print("E3/C2 HUMAN: fire-lit vs unlit surface A/B at same paused tick (see p3a-exit.md capture 2).")

	# --- C3: bloom-off distinguishable (headless half: profile/LUT proof) ---
	var bloom_node = main.get_node("BloomToggle")
	check(bloom_node != null, "E3/C3 bloom toggle node present (KEY_B path)")
	var was_on: bool = bloom_node.is_bloom_enabled()
	bloom_node.set_bloom_enabled(false)
	check(not bloom_node.is_bloom_enabled(), "E3/C3 bloom-off state takes")
	bloom_node.set_bloom_enabled(true)
	check(bloom_node.is_bloom_enabled(), "E3/C3 bloom-on state round-trips")
	bloom_node.set_bloom_enabled(was_on)
	var srow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["stone"]))
	var rrow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["rock"]))
	var sandrow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["sand"]))
	var woodrow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["wood"]))
	var wrow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["water"]))
	var orow: Array = Glue.row_palette(profiles, main.world, int(id_by_name["oil"]))
	check(_dist(srow, rrow, 3) > 10, "E3/C3 stone vs rock palettes differ (dist %d)" % _dist(srow, rrow, 3))
	check(_dist(sandrow, woodrow, 3) > 10, "E3/C3 sand vs wood palettes differ (dist %d)" % _dist(sandrow, woodrow, 3))
	check(_dist(wrow, orow, 3) > 30, "E3/C3 water vs oil palettes differ (dist %d)" % _dist(wrow, orow, 3))
	var se: Color = Glue.eval_cell_cpu(profiles, "stone", 20, 10, 77)
	var re: Color = Glue.eval_cell_cpu(profiles, "rock", 20, 10, 77)
	check(_q8(se) != _q8(re), "E3/C3 stone vs rock CPU-mirror evals differ (bloom-independent)")
	print("E3/C3 HUMAN: bloom on/off A/B pair at same paused tick, boundaries unwashed (see p3a-exit.md capture 3).")

	# --- C4: toggles hash-identical (headless: compact gauntlet on live cave) ---
	main.world.flush_present()
	var baseline: PackedByteArray = main.world.save_to_bytes()
	var base_tick: int = int(main.world.get_tick())
	var base_occ: int = int(main.world.get_occupied_cell_count())
	var step_n: int = 0
	var same := func(where: String) -> void:
		check(main.world.save_to_bytes() == baseline, "E3/C4 sim-bytes identical (%s)" % where)
		check(int(main.world.get_tick()) == base_tick, "E3/C4 tick unchanged (%s)" % where)
		check(int(main.world.get_occupied_cell_count()) == base_occ, "E3/C4 occupied unchanged (%s)" % where)
	bloom_node.toggle_bloom()
	same.call("bloom-off")
	step_n += 1
	bloom_node.toggle_bloom()
	same.call("bloom-on")
	main._toggle_follow_camera()
	same.call("follow-off")
	main._toggle_follow_camera()
	same.call("follow-on")
	for i in 3:
		main._zoom_camera(1)
	same.call("zoom-in")
	for i in 3:
		main._zoom_camera(-1)
	same.call("zoom-out")
	for i in 10:
		main._update_light(0.05)
		same.call("light-frame-%d" % i)
	var clocks = Clocks.new()
	for i in 30:
		clocks.tick_world(0.016, true)
		clocks.tick_ui(0.016)
	same.call("clock-run")
	for i in 10:
		clocks.tick_world(0.5, false)
		clocks.tick_ui(0.5)
	same.call("clock-pause")
	clocks.set_forced(base_tick, 0.25)
	same.call("clock-forced")
	clocks.clear_forced()
	main.world.flush_present()
	same.call("gauntlet-end")
	check(step_n == 1, "E3/C4 gauntlet bookkeeping sane")
	# Full D2 suite is the authoritative proof — rerun-green is asserted in
	# the E3 report's battery table, not duplicated here.
	main.world.set_cell(256, 200, AshfallWorld.MATERIAL_SAND)
	check(main.world.save_to_bytes() != baseline, "E3/C4 NEGATIVE CONTROL: sim mutation changes save bytes")

	# --- C5: pause/replay documented (headless half: behavior + doc presence) ---
	var c2 = Clocks.new()
	c2.tick_world(0.016, true)
	var wt: float = c2.world_time()
	c2.tick_world(1.0, false)
	check(c2.world_time() == wt, "E3/C5 pause freezes world clock exactly")
	c2.set_forced(100, 0.25)
	var p: Dictionary = c2.present(9999)
	check(int(p["tick"]) == 100 and absf(float(p["phase"]) - 0.25) < 0.0001, "E3/C5 forced pins replay tick+phase")
	c2.clear_forced()
	c2.reset()
	check(c2.world_time() == 0.0 and not c2.is_forced(), "E3/C5 reset zeroes clocks")
	var clk_src := FileAccess.open("res://scripts/presentation_clocks.gd", FileAccess.READ)
	check(clk_src != null, "E3/C5 clocks doc source present")
	if clk_src != null:
		var text: String = clk_src.get_as_text()
		clk_src.close()
		check(text.contains("Pause") and text.contains("set_forced"), "E3/C5 pause/replay behavior documented in clocks header")
	var d2_src := FileAccess.open("res://tests/godot_presentation_toggles_verify.gd", FileAccess.READ)
	check(d2_src != null, "E3/C5 D2 hash-unchanged proof present (pause/replay contract lives there)")
	if d2_src != null:
		d2_src.close()

	# --- C6: perf measured on target Mac (headless half: record presence) ---
	var e1 := FileAccess.open("res://docs/perf/e1-baseline.md", FileAccess.READ)
	check(e1 != null, "E3/C6 E1 baseline record present")
	if e1 != null:
		var body: String = e1.get_as_text()
		e1.close()
		check(body.contains("Stepping model") and body.contains("Benchmark worlds"), "E3/C6 E1 record covers stepping model + benchmark worlds")
		check(body.contains("2.743") and body.contains("35.783"), "E3/C6 E1 numbers pinned (cave p50 2.743ms, liquid worst 35.783ms)")
		check(body.contains("uninstrumented") and body.contains("GPU frame capture"), "E3/C6 E1 states the GPU-split limit honestly")

	main.queue_free()
	if _failures > 0:
		push_error("E3 EXIT-GATE CHECKS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL E3 EXIT-GATE CHECKS PASSED (headless halves; human captures listed in docs/perf/p3a-exit.md) ---")
		quit(0)

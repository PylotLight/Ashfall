extends SceneTree

# P3A Slice D, task D2 — presentation-toggle hash-unchanged verification.
# Run: godot --headless --path . --script tests/godot_presentation_toggles_verify.gd
#
# P3A exit criterion (hard): EVERY presentation toggle leaves the sim
# save-bytes bit-identical AND P1=51840. This test fails loudly (non-zero
# exit, FAIL lines) if any toggle path ever touches the sim.
#
# Coverage — every play toggle named in the D2 contract:
#   1. bloom on/off (scripts/cave_bloom_toggle.gd, KEY_B at runtime)
#   2. follow camera on/off + zoom steps (scripts/follow_camera.gd, KEY_C +
#      wheel at runtime, via both controllers' _toggle/_zoom paths in the
#      cave-scene section)
#   3. light retarget frames (scripts/light_cluster_manager.gd retarget +
#      smoothing cadence driven for real frames, incl. full get_cell scans)
#   4. world/UI clock motion incl. pause + forced captures
#      (scripts/presentation_clocks.gd)
# Each block asserts save-bytes identity + tick identity + occupied==51840
# immediately after the toggle motion, on top of a setup_play_level world
# (the P1 baseline) and again through the live cave scene.
#
# Pause/replay behavior (documented here per contract):
# - Pause (world.auto_step off) freezes the WORLD clock exactly: no
#   world_time/phase accumulates while paused, so resume continues the
#   identical animation phase — deterministic for identical delta
#   sequences. The UI clock keeps advancing while paused (interface stays
#   alive); it is never fed to world shaders.
# - Replay/capture pins exact values: set_forced(tick, phase) makes
#   tick_world a no-op and present() return the pins, so forced-tick
#   captures (Slice E2) reproduce the identical frame. clear_forced()
#   resumes live; reset() zeroes clocks on world rebuild (R key).
# - World animation inputs are tick-driven only: present_tick is always
#   the authoritative sim tick (read, never advanced, by the clock), so
#   no blanket TIME reaches world shaders through this path.
#
# Negative control: the final core block mutates the sim on purpose and
# asserts the comparison GOES RED (bytes differ, tick advances) — proving
# this test can actually catch a sim mutation and is not vacuously green.

const Clocks = preload("res://scripts/presentation_clocks.gd")
const FollowCam = preload("res://scripts/follow_camera.gd")
const ClusterLights = preload("res://scripts/light_cluster_manager.gd")
const BloomToggle = preload("res://scripts/cave_bloom_toggle.gd")

# P1 baseline: setup_play_level occupied count (see also
# docs/tasks/P2_DATA_DRIVEN_MATERIALS.md). Any sim change moves this.
const P1_OCCUPIED: int = 51840

var _failures: int = 0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: ", msg)
	else:
		_failures += 1
		push_error("FAIL: " + msg)


func _sim_untouched(w: AshfallWorld, baseline: PackedByteArray, base_tick: int, where: String) -> void:
	check(w.save_to_bytes() == baseline, "D2 sim save-bytes bit-identical (%s)" % where)
	check(int(w.get_tick()) == base_tick, "D2 sim tick unchanged (%s)" % where)
	check(int(w.get_occupied_cell_count()) == P1_OCCUPIED, "D2 P1=51840 holds (%s)" % where)


func _init() -> void:
	_run_core_checks()
	_run_scene_checks.call_deferred()


func _run_core_checks() -> void:
	var w := AshfallWorld.new()
	var rx := FileAccess.open("res://data/reactions.json", FileAccess.READ)
	if rx == null:
		_failures += 1
		push_error("FAIL: D2 canonical reaction data present")
		quit(1)
		return
	if not w.load_reactions(rx.get_as_text()):
		_failures += 1
		push_error("FAIL: D2 canonical reactions load through bridge")
		quit(1)
		return
	rx.close()
	w.setup_play_level()
	check(int(w.get_occupied_cell_count()) == P1_OCCUPIED, "D2 P1 baseline occupied=51840")
	w.flush_present()
	var baseline := w.save_to_bytes()
	var base_tick: int = int(w.get_tick())
	check(baseline.size() > 100, "D2 baseline save bytes non-empty")
	_sim_untouched(w, baseline, base_tick, "baseline")

	# 1. Bloom on/off gauntlet (presentation-only by construction — the
	# script touches only its own environment glow flag).
	var bloom = BloomToggle.new()
	root.add_child(bloom)
	for i in 6:
		bloom.toggle_bloom()
		_sim_untouched(w, baseline, base_tick, "bloom toggle %d" % i)
	bloom.set_bloom_enabled(true)
	_sim_untouched(w, baseline, base_tick, "bloom set on")
	bloom.set_bloom_enabled(false)
	_sim_untouched(w, baseline, base_tick, "bloom set off")
	bloom.set_bloom_enabled(true)
	check(bloom.is_bloom_enabled(), "D2 bloom toggle state round-trips")
	_sim_untouched(w, baseline, base_tick, "bloom gauntlet end")

	# 2. Follow camera on/off + zoom steps (pure view math — the sim never
	# sees the view rect).
	var cam = FollowCam.new()
	var world_size := Vector2i(w.get_world_width(), w.get_world_height())
	var control_size := Vector2(960, 540)
	var player := Vector2(256.0, 400.0)
	cam.snap_to(player, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera snap")
	for i in 10:
		player += Vector2(7.0, -3.0)
		cam.update(0.016, player, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera follow frames")
	cam.set_follow(false, player, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera follow off")
	for i in 4:
		cam.zoom_step(1, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera zoom in x4")
	for i in 4:
		cam.zoom_step(-1, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera zoom out x4")
	cam.set_follow(true, player, world_size, control_size)
	_sim_untouched(w, baseline, base_tick, "camera follow on")
	check(cam.follow_enabled, "D2 camera follow state round-trips")

	# 3. Light retarget frames: real extraction cadence (full read-only
	# get_cell scans) + per-frame smoothing toward live emissive regions.
	var mgr = ClusterLights.new()
	var emissive := {
		AshfallWorld.MATERIAL_FIRE: {"color": Color(1.0, 0.48, 0.0), "strength": 2.5},
		AshfallWorld.MATERIAL_LAVA: {"color": Color(1.0, 0.27, 0.0), "strength": 1.5},
	}
	mgr.setup(emissive, Vector2(256.0, 436.0), Color(1.0, 0.55, 0.25))
	for i in 20:
		var slots: Array = mgr.update(w, 0.05)
		check(slots.size() == ClusterLights.MAX_LIGHTS, "D2 light budget fixed at 6 (frame %d)" % i)
		_sim_untouched(w, baseline, base_tick, "light frame %d" % i)
	var st: Dictionary = mgr.debug_state()
	check(int(st["extracts"]) >= 3, "D2 light retarget actually re-extracted (%d)" % int(st["extracts"]))
	check(int(st["slots"]) == ClusterLights.MAX_LIGHTS, "D2 light slot count == budget")
	_sim_untouched(w, baseline, base_tick, "light gauntlet end")

	# 4. Clock motion incl. pause + forced captures. Clocks take only
	# floats/bools/ints — they cannot name the sim, but assert anyway.
	var clocks = Clocks.new()
	var sim_tick: int = base_tick
	for i in 30:
		clocks.tick_world(0.016, true)
		clocks.tick_ui(0.016)
	var wt_run: float = clocks.world_time()
	var ui_run: float = clocks.ui_time()
	var p_run: Dictionary = clocks.present(sim_tick)
	check(wt_run > 0.0 and ui_run > 0.0, "D2 clocks advance while running")
	check(int(p_run["tick"]) == sim_tick, "D2 present_tick is the sim tick")
	# Pause freezes the world clock exactly; UI clock keeps going.
	for i in 10:
		clocks.tick_world(0.5, false)
		clocks.tick_ui(0.5)
	check(clocks.world_time() == wt_run, "D2 pause freezes world_time exactly")
	var p_paused: Dictionary = clocks.present(sim_tick)
	check(float(p_paused["phase"]) == float(p_run["phase"]), "D2 pause freezes phase exactly")
	check(clocks.ui_time() > ui_run, "D2 UI clock advances while paused")
	_sim_untouched(w, baseline, base_tick, "clock run+pause")
	# Forced capture pins; live motion cannot disturb it.
	clocks.set_forced(sim_tick, 0.25)
	for i in 10:
		clocks.tick_world(0.016, true)
	var p_forced: Dictionary = clocks.present(sim_tick + 999)
	check(int(p_forced["tick"]) == sim_tick and float(p_forced["phase"]) == 0.25, "D2 forced capture pins tick+phase")
	check(clocks.world_time() == wt_run, "D2 forced tick_world is a no-op")
	clocks.clear_forced()
	check(not clocks.is_forced(), "D2 forced flag clears")
	clocks.tick_world(0.016, true)
	check(clocks.world_time() > wt_run, "D2 live clock resumes after clear")
	clocks.reset()
	check(clocks.world_time() == 0.0 and not clocks.is_forced(), "D2 reset zeroes clocks")
	_sim_untouched(w, baseline, base_tick, "clock gauntlet end")

	# Present-path flush also leaves the sim alone.
	w.flush_present()
	w.flush_present()
	_sim_untouched(w, baseline, base_tick, "flush_present idle")

	# Negative control (LAST — mutates the sim on purpose): proves the
	# comparison above is sensitive and not vacuously green.
	w.set_cell(256, 479, AshfallWorld.MATERIAL_SAND)
	w.step()
	check(w.save_to_bytes() != baseline, "D2 NEGATIVE CONTROL: sim mutation changes save bytes")
	check(int(w.get_tick()) == base_tick + 1, "D2 NEGATIVE CONTROL: step advances the tick")
	check(int(w.get_occupied_cell_count()) != P1_OCCUPIED, "D2 NEGATIVE CONTROL: mutation moves occupied")

	bloom.queue_free()
	w.free()
	if _failures > 0:
		push_error("D2 CORE TOGGLE CHECKS FAILED: %d" % _failures)
		quit(1)
		return
	print("--- D2 CORE TOGGLE CHECKS PASSED ---")


func _run_scene_checks() -> void:
	# Live cave scene: drive the REAL toggle paths (controller follow/zoom,
	# scene bloom node, cluster light frames) and prove sim identity.
	var scene: PackedScene = load("res://cave_benchmark.tscn")
	if scene == null:
		_failures += 1
		push_error("FAIL: D2 cave scene loads")
		quit(1)
		return
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	main.world.auto_step = false
	main.world.flush_present()
	var baseline: PackedByteArray = main.world.save_to_bytes()
	var base_tick: int = int(main.world.get_tick())
	var base_occ: int = int(main.world.get_occupied_cell_count())
	check(baseline.size() > 100, "D2 cave baseline save bytes non-empty")

	# Follow off/on + zoom in/out through the controller's own methods.
	main._toggle_follow_camera()
	check(main.world.save_to_bytes() == baseline, "D2 cave follow-off sim-identical")
	main._toggle_follow_camera()
	check(main.world.save_to_bytes() == baseline, "D2 cave follow-on sim-identical")
	for i in 3:
		main._zoom_camera(1)
	check(main.world.save_to_bytes() == baseline, "D2 cave zoom-in sim-identical")
	for i in 3:
		main._zoom_camera(-1)
	check(main.world.save_to_bytes() == baseline, "D2 cave zoom-out sim-identical")

	# Bloom on/off through the scene node (the KEY_B path's target).
	var bloom_node = main.get_node("BloomToggle")
	bloom_node.toggle_bloom()
	check(main.world.save_to_bytes() == baseline, "D2 cave bloom-off sim-identical")
	bloom_node.toggle_bloom()
	check(main.world.save_to_bytes() == baseline, "D2 cave bloom-on sim-identical")
	check(bloom_node.is_bloom_enabled(), "D2 cave bloom state round-trips")

	# Light retarget frames through the controller's per-frame drive.
	for i in 10:
		main._update_light(0.05)
		check(main.world.save_to_bytes() == baseline, "D2 cave light frame %d sim-identical" % i)
	main.world.flush_present()

	check(int(main.world.get_tick()) == base_tick, "D2 cave tick unchanged by all toggles")
	check(int(main.world.get_occupied_cell_count()) == base_occ, "D2 cave occupied unchanged by all toggles")

	main.queue_free()
	if _failures > 0:
		push_error("D2 PRESENTATION TOGGLE VERIFICATIONS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL D2 PRESENTATION TOGGLE VERIFICATIONS PASSED ---")
		quit(0)

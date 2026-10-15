extends SceneTree

## P3A Slice S, task S1 — Stage-B spike probe (PROTOTYPE ONLY, NEW file).
##
## Run: godot --headless --path . --script res://scripts/stage_b_spike_probe.gd
##
## 1. Synthetic headless checks (no scene): field resolution math, bounded
##    propagation travel, mirror symmetry across a 64-cell chunk boundary,
##    hard solid masking.
## 2. Cave A/B: boots cave_benchmark.tscn headless with the spike OFF (8 idle
##    frames) then ON (8 frames with a live rebuild per frame over the live
##    camera view). Asserts sim save-bytes + base/water image bytes are
##    bit-identical across the toggle (spike cannot perturb the Stage-A pixel
##    path), the spike toggle-state reads false->true, and the field is
##    non-trivial when on. Prints the headless-measurable comparison block
##    (resolution, propagation bound, rebuild us, upload bytes, present-timer
##    context) for the written verdict.

const Spike = preload("res://scripts/stage_b_light_field_spike.gd")
const ClusterMgr = preload("res://scripts/light_cluster_manager.gd")

const OFF_FRAMES: int = 8
const ON_FRAMES: int = 8

var _failures: int = 0


func check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: ", msg)
	else:
		_failures += 1
		push_error("FAIL: " + msg)


func _fail(msg: String) -> void:
	_failures += 1
	push_error("FAIL: " + msg)


func _init() -> void:
	_synthetic_checks()
	if _failures > 0:
		push_error("S1 SYNTHETIC CHECKS FAILED: %d" % _failures)
		quit(1)
		return
	print("--- S1 SYNTHETIC CHECKS PASSED ---")
	_cave_ab.call_deferred()


func _emissive_fire() -> Dictionary:
	return {AshfallWorld.MATERIAL_FIRE: {"color": Color(1.0, 0.5, 0.1), "strength": 2.5}}


func _open_world() -> AshfallWorld:
	var w := AshfallWorld.new()
	w.init_world(128, 128)
	w.fill_rect(0, 0, 127, 127, AshfallWorld.MATERIAL_AIR)
	return w


func _field_energy(spike, rect: Rect2i, x: int, y: int) -> float:
	var c: Color = spike.sample_cell(Vector2i(
		rect.position.x + x * Spike.CELL_PER_TEXEL + 1,
		rect.position.y + y * Spike.CELL_PER_TEXEL + 1))
	return maxf(c.r, maxf(c.g, c.b))


func _max_in_rect(spike, rect: Rect2i, x0: int, x1: int) -> float:
	var m: float = 0.0
	var res := Spike.field_resolution(rect)
	for ty in res.y:
		for tx in range(x0, x1):
			m = maxf(m, _field_energy(spike, rect, tx, ty))
	return m


func _synthetic_checks() -> void:
	# Resolution math: 128x64-cell rect -> 32x16 texels.
	var rect := Rect2i(0, 32, 128, 64)
	var res := Spike.field_resolution(rect)
	check(res == Vector2i(32, 16), "S1 field resolution 128x64 cells -> 32x16 texels (got %s)" % str(res))
	check(Spike.TRAVEL_CELLS == Spike.PROPAGATION_PASSES * Spike.CELL_PER_TEXEL,
		"S1 travel bound is passes x texel size (%d cells)" % Spike.TRAVEL_CELLS)
	check(Spike.ENABLED_DEFAULT == false, "S1 spike defaults OFF")

	# Seam: two identical sources mirrored about the x=63|64 chunk boundary
	# (47 cells apart, beyond the 40-cell Stage-A merge radius, so both feed
	# the field). Field must be exactly mirror-symmetric: chunk lines cannot
	# imprint.
	var w := _open_world()
	w.set_cell(40, 64, AshfallWorld.MATERIAL_FIRE)
	w.set_cell(87, 64, AshfallWorld.MATERIAL_FIRE)
	var clusters: Array = ClusterMgr.merge_clusters(ClusterMgr.extract_clusters(w, _emissive_fire()))
	check(clusters.size() == 2, "S1 seam setup holds 2 unmerged sources (got %d)" % clusters.size())
	var spike = Spike.new()
	spike.set_enabled(true)
	var m: Dictionary = spike.rebuild(w, clusters, rect)
	check(int(m["texels"]) == 512, "S1 seam field holds 512 texels")
	var worst: float = 0.0
	for ty in res.y:
		for tx in res.x / 2:
			var a: float = _field_energy(spike, rect, tx, ty)
			var b: float = _field_energy(spike, rect, res.x - 1 - tx, ty)
			worst = maxf(worst, absf(a - b))
	check(worst < 1e-5, "S1 no seam at 64-cell boundary (mirror diff %f)" % worst)

	# Masking: 4-wide solid wall across the field, one fire left of it.
	# Nothing may read right of the wall (single-texel splat + blocked relay).
	var w2 := _open_world()
	w2.fill_rect(64, 32, 67, 95, AshfallWorld.MATERIAL_STONE)
	w2.set_cell(40, 64, AshfallWorld.MATERIAL_FIRE)
	var clusters2: Array = ClusterMgr.merge_clusters(ClusterMgr.extract_clusters(w2, _emissive_fire()))
	var spike2 = Spike.new()
	spike2.set_enabled(true)
	spike2.rebuild(w2, clusters2, rect)
	var left: float = _max_in_rect(spike2, rect, 0, 16)
	var right: float = _max_in_rect(spike2, rect, 17, 32)
	check(left > 0.0, "S1 fire side of wall is lit (max %f)" % left)
	check(right == 0.0, "S1 wall masks fully (right max %f)" % right)

	# Bound: same single-fire field; texels beyond PASSES texels of travel
	# read exactly 0, while the wavefront edge (6 texels out) is lit.
	var fire_tx: int = 40 / Spike.CELL_PER_TEXEL
	var edge: float = _field_energy(spike2, rect, fire_tx - Spike.PROPAGATION_PASSES, 8)
	var beyond: float = _field_energy(spike2, rect, fire_tx - Spike.PROPAGATION_PASSES - 2, 8)
	check(edge > 0.0, "S1 propagation reaches the %d-texel wavefront (%f)" % [Spike.PROPAGATION_PASSES, edge])
	check(beyond == 0.0, "S1 propagation bounded (beyond wavefront %f)" % beyond)
	w.free()
	w2.free()


func _cave_ab() -> void:
	var scene: PackedScene = load("res://cave_benchmark.tscn")
	check(scene != null, "S1 cave scene loads headless")
	if scene == null:
		push_error("S1 CAVE BOOT FAILED")
		quit(1)
		return
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	check(main.world.get_world_width() == 512 and main.world.get_world_height() == 512,
		"S1 cave boots 512x512 headless (occupied=%d)" % main.world.get_occupied_cell_count())
	check(main.world.get_occupied_cell_count() > 1000, "S1 cave populated headless")

	var spike = Spike.new()
	check(bool(spike.debug_state()["enabled"]) == false, "S1 spike toggle reads OFF at boot")

	# OFF phase: Stage A only, spike never rebuilt.
	for i in OFF_FRAMES:
		await process_frame
	var sim_off: PackedByteArray = main.world.save_to_bytes()
	var base_off: PackedByteArray = main.world.get_world_image().get_data()
	var water_off: PackedByteArray = main.world.get_water_image().get_data()
	var cluster_off: Dictionary = main._cluster_mgr.debug_state()
	check(int(cluster_off["slots"]) == 6 and int(cluster_off["budget"]) == 6,
		"S1 Stage-A budget holds 6 slots while spike is off")

	# ON phase: same scene, spike rebuilt every frame over the live view.
	spike.set_enabled(true)
	var last_metrics: Dictionary = {}
	for i in ON_FRAMES:
		var clusters: Array = ClusterMgr.merge_clusters(
			ClusterMgr.extract_clusters(main.world, main._emissive))
		var ws := Vector2i(main.world.get_world_width(), main.world.get_world_height())
		var frect: Rect2i = Spike.field_rect_for_view(main._view_region, ws)
		last_metrics = spike.rebuild(main.world, clusters, frect)
		await process_frame
	var sim_on: PackedByteArray = main.world.save_to_bytes()
	var base_on: PackedByteArray = main.world.get_world_image().get_data()
	var water_on: PackedByteArray = main.world.get_water_image().get_data()
	var cluster_on: Dictionary = main._cluster_mgr.debug_state()
	var spike_on: Dictionary = spike.debug_state()

	check(sim_off == sim_on, "S1 sim bytes identical with spike on vs off")
	check(base_off == base_on, "S1 base pixel path identical with spike on vs off")
	check(water_off == water_on, "S1 water pixel path identical with spike on vs off")
	check(bool(spike_on["enabled"]) == true and int(spike_on["rebuilds"]) == ON_FRAMES,
		"S1 spike toggle reads ON and rebuilt every frame (%d)" % int(spike_on["rebuilds"]))
	check(float(spike_on["max_energy"]) > 0.0,
		"S1 field non-trivial on the cave (max %f, seed %f)" % [float(spike_on["max_energy"]), float(spike_on["seed_energy"])])
	check(int(cluster_on["slots"]) == 6, "S1 Stage-A slot count untouched by spike")

	# Headless-measurable comparison block for the written verdict.
	var frect: Rect2i = Spike.field_rect_for_view(
		main._view_region, Vector2i(main.world.get_world_width(), main.world.get_world_height()))
	var fres := Spike.field_resolution(frect)
	print("S1 CAVE view=%s field_rect=%s texels=%s upload_rgb8=%dB travel=%dcells" % [
		str(main._view_region), str(frect), str(fres),
		int(last_metrics["upload_bytes_rgb8"]), Spike.TRAVEL_CELLS])
	print("S1 CAVE samples fire=%s water=%s stone_floor=%s" % [
		str(spike.sample_cell(Vector2i(256, 436))),
		str(spike.sample_cell(Vector2i(367, 428))),
		str(spike.sample_cell(Vector2i(256, 460)))])
	var acc_extract: int = 0
	var acc_field: int = 0
	var clusters_last: Array = []
	for i in 20:
		var t0: int = Time.get_ticks_usec()
		clusters_last = ClusterMgr.merge_clusters(
			ClusterMgr.extract_clusters(main.world, main._emissive))
		acc_extract += int(Time.get_ticks_usec() - t0)
		var t1: int = Time.get_ticks_usec()
		spike.rebuild(main.world, clusters_last, frect)
		acc_field += int(Time.get_ticks_usec() - t1)
	print("S1 CAVE extract_merge_avg_us=%d field_rebuild_avg_us=%d over 20 (cluster input shared with Stage A)" % [acc_extract / 20, acc_field / 20])
	# Present-timer context: one real sim step + flush through the shipping path.
	main.world.step()
	main.world.flush_present()
	print("S1 STAGEA-PATH step+flush dirty_pixels=%d submits=%d paint_ms=%.3f collect_ms=%.3f last_step_ms=%.3f" % [
		int(main.world.get_present_dirty_pixels()),
		int(main.world.get_present_texture_submits()),
		main.world.get_present_paint_ms(),
		main.world.get_present_dirty_collect_ms(),
		main.world.get_last_step_ms()])

	main.queue_free()
	if _failures > 0:
		push_error("S1 SPIKE PROBE FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL S1 SPIKE CHECKS PASSED ---")
		quit(0)

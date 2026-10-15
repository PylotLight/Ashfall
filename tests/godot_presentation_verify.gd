extends SceneTree

# Group 3, Task B8 — presentation verification (bridge review §F1–F7).
# Run: godot --headless --path . --script tests/godot_presentation_verify.gd
#
# Cadence note (Task B4): step()/edits only accumulate engine dirtiness;
# every check below presents explicitly with flush_present().

var _failures: int = 0

func check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: ", msg)
	else:
		_failures += 1
		push_error("FAIL: " + msg)

func _grain(x: int, y: int) -> int:
	# GDScript mirror of the bridge paint grain (uint32 wrap per op).
	var h: int = (x * 0x9E3779B1) & 0xFFFFFFFF
	var t: int = ((y * 0x85EBCA77) + 0x9E3779B9 + ((h << 6) & 0xFFFFFFFF) + (h >> 2)) & 0xFFFFFFFF
	h = (h ^ t) & 0xFFFFFFFF
	h = (h ^ (h >> 13)) & 0xFFFFFFFF
	return int(h % 13) - 6

func _chan(c: Color, ch: String) -> int:
	match ch:
		"r":
			return int(round(c.r * 255.0))
		"g":
			return int(round(c.g * 255.0))
		"b":
			return int(round(c.b * 255.0))
	return int(round(c.a * 255.0))

func _snap(w: AshfallWorld) -> Array:
	return [w.get_world_image().get_data(), w.get_water_image().get_data()]

func _init() -> void:
	_f1_partial_equals_full()
	_f2_clearing()
	_f3_no_work_no_submit()
	_f4_one_flush()
	_f5_recovery()
	_f6_pixel_fixtures()
	if _failures > 0:
		push_error("PRESENTATION VERIFICATIONS FAILED: %d" % _failures)
		quit(1)
		return
	print("--- CORE PRESENTATION VERIFICATIONS PASSED ---")
	_run_host_checks.call_deferred()

# F1: partial repaint == forced full repaint, byte-compare of both layers.
func _build_case(w: AshfallWorld, idx: int) -> void:
	var S: int = AshfallWorld.MATERIAL_SAND
	var W: int = AshfallWorld.MATERIAL_WATER
	var T: int = AshfallWorld.MATERIAL_STONE
	var A: int = AshfallWorld.MATERIAL_AIR
	match idx:
		0: # insert across the 63/64 chunk boundary
			w.fill_rect(60, 60, 70, 72, S)
		1: # delete: fill then clear part to air
			w.fill_rect(20, 20, 40, 40, S)
			w.fill_rect(25, 25, 45, 45, A)
		2: # liquid -> solid swap
			w.fill_rect(90, 90, 110, 110, W)
			w.fill_rect(95, 95, 105, 105, T)
		3: # chunk-boundary liquid column, lower half erased (re-shades above)
			w.fill_rect(60, 40, 68, 90, W)
			w.fill_rect(60, 64, 68, 90, A)
		4: # threshold region: two adjacent dirty chunks (partial path)
			w.fill_rect(0, 0, 2, 2, S)
			w.fill_rect(64, 0, 66, 2, W)
		5: # threshold crossing: dirt spread so the halo exceeds 8 chunks
			for i in range(6):
				w.fill_rect(i * 64, 4, i * 64 + 2, 6, S)

func _case_world(idx: int) -> AshfallWorld:
	# Cases 0-3 live on 128x128 (2x2 chunks, always partial unless forced);
	# cases 4-5 need room for the 8-chunk threshold on 320x192 (5x3 chunks).
	var w := AshfallWorld.new()
	if idx <= 3:
		w.init_world(128, 128)
	else:
		w.init_world(320, 192)
	return w

func _f1_partial_equals_full() -> void:
	var names := ["insert", "delete", "liquid-to-solid", "chunk-boundary", "threshold-partial", "threshold-crossing"]
	for idx in names.size():
		var a := _case_world(idx)
		_build_case(a, idx)
		a.flush_present()
		var snap_a := _snap(a)
		var pix_a: int = a.get_present_dirty_pixels()
		a.free()
		var b := _case_world(idx)
		_build_case(b, idx)
		b.debug_force_full_repaint()
		b.flush_present()
		var snap_b := _snap(b)
		var pix_b: int = b.get_present_dirty_pixels()
		b.free()
		check(snap_a[0] == snap_b[0], "F1 base layer partial==full (%s)" % names[idx])
		check(snap_a[1] == snap_b[1], "F1 water layer partial==full (%s)" % names[idx])
		if idx == 4:
			check(pix_a < pix_b, "F1 threshold-partial really took the partial path")
		if idx == 5:
			check(pix_a == pix_b, "F1 threshold-crossing really took the full path")

# F2: deletion / reset / load clear old pixels on BOTH layers.
func _f2_clearing() -> void:
	var w := AshfallWorld.new()
	w.init_world(64, 64)
	var S: int = AshfallWorld.MATERIAL_SAND
	w.set_cell(10, 10, S)
	w.flush_present()
	var painted: Color = w.get_world_image().get_pixel(10, 10)
	check(abs(painted.r - 18.0 / 255.0) > 0.05, "F2 painted cell differs from background")
	check(_chan(w.get_water_image().get_pixel(10, 10), "a") == 0, "F2 sand leaves water layer transparent")
	w.set_cell(10, 10, AshfallWorld.MATERIAL_AIR)
	w.flush_present()
	var cleared: Color = w.get_world_image().get_pixel(10, 10)
	check(_chan(cleared, "r") == 18 and _chan(cleared, "g") == 20 and _chan(cleared, "b") == 24, "F2 deletion restores background exactly")
	check(_chan(w.get_water_image().get_pixel(10, 10), "a") == 0, "F2 deletion leaves water layer transparent")
	# Reset clears too.
	w.set_cell(20, 20, S)
	w.flush_present()
	w.init_world(64, 64)
	w.flush_present()
	var reset_px: Color = w.get_world_image().get_pixel(20, 20)
	check(_chan(reset_px, "r") == 18 and _chan(reset_px, "g") == 20, "F2 reset clears old pixels")
	check(_chan(w.get_water_image().get_pixel(20, 20), "a") == 0, "F2 reset clears water layer")
	# Load restores the saved pixels exactly.
	w.set_cell(30, 30, S)
	w.flush_present()
	var saved := w.save_to_bytes()
	w.set_cell(30, 30, AshfallWorld.MATERIAL_STONE)
	w.flush_present()
	check(w.load_from_bytes(saved), "F2 load of own save succeeds")
	w.flush_present()
	var loaded: Color = w.get_world_image().get_pixel(30, 30)
	check(abs(loaded.r - 18.0 / 255.0) > 0.05, "F2 load repaints the restored pixels")
	check(w.get_cell(30, 30) == S, "F2 load restores sim state")
	w.free()
	print("PASS: F2 deletion/reset/load clearing verified")

# F3: no work => image bytes + submit counter unchanged.
func _f3_no_work_no_submit() -> void:
	var w := AshfallWorld.new()
	w.init_world(64, 64)
	w.fill_rect(5, 5, 10, 10, AshfallWorld.MATERIAL_SAND)
	w.flush_present()
	var before := _snap(w)
	var submits: int = int(w.get_present_texture_submits())
	var flushes: int = int(w.get_present_flushes())
	w.flush_present()
	check(int(w.get_present_texture_submits()) == submits, "F3 idle flush submits nothing")
	check(int(w.get_present_flushes()) == flushes, "F3 idle flush counts no paint")
	check(w.get_world_image().get_data() == before[0], "F3 idle flush preserves base bytes")
	check(w.get_water_image().get_data() == before[1], "F3 idle flush preserves water bytes")
	w.free()

# F4: many ticks + edits before a frame => exactly one flush paints.
func _f4_one_flush() -> void:
	var w := AshfallWorld.new()
	w.init_world(64, 64)
	w.flush_present()
	var f0: int = int(w.get_present_flushes())
	w.step()
	w.step()
	w.step()
	w.set_cell(8, 8, AshfallWorld.MATERIAL_SAND)
	w.fill_rect(20, 20, 22, 22, AshfallWorld.MATERIAL_WATER)
	check(int(w.get_present_flushes()) == f0, "F4 steps+edits present nothing by themselves")
	w.flush_present()
	check(int(w.get_present_flushes()) == f0 + 1, "F4 accumulated work paints in exactly one flush")
	w.flush_present()
	check(int(w.get_present_flushes()) == f0 + 1, "F4 follow-up flush with no work paints nothing")
	w.free()

# F5: presentation recovery preserves sim bytes.
func _f5_recovery() -> void:
	var w := AshfallWorld.new()
	w.init_world(64, 64)
	w.fill_rect(10, 10, 20, 20, AshfallWorld.MATERIAL_SAND)
	w.fill_rect(30, 30, 40, 40, AshfallWorld.MATERIAL_WATER)
	for i in 3:
		w.step()
	w.flush_present()
	var saved := w.save_to_bytes()
	var tick: int = int(w.get_tick())
	w.debug_drop_presentation()
	check(w.get_world_image() == null, "F5 debug hook really drops presentation objects")
	w.flush_present()
	check(int(w.get_tick()) == tick, "F5 recovery never steps the sim")
	check(w.save_to_bytes() == saved, "F5 recovery preserves authoritative sim bytes")
	check(w.get_world_texture() != null and w.get_water_texture() != null, "F5 recovery rebinds both layers")
	w.free()

# F6: CPU pixel fixtures — the shader contract from the CPU side.
# Straight alpha, center sample only: solid-adjacent texels render exactly
# what the layer holds (no dilation halo); water alpha is 205/255.
func _f6_pixel_fixtures() -> void:
	var w := AshfallWorld.new()
	w.init_world(32, 32)
	var WATER: int = AshfallWorld.MATERIAL_WATER
	var STONE: int = AshfallWorld.MATERIAL_STONE
	var OIL: int = 9
	# Isolated drop: surface highlight +22 over the data color + grain.
	w.set_cell(10, 10, WATER)
	# One-cell channel: vertical line, middle cell shaded by depth.
	w.fill_rect(15, 5, 15, 9, WATER)
	# Solid border: stone directly beside water must stay exact stone.
	w.set_cell(20, 10, STONE)
	w.set_cell(19, 10, WATER)
	# Second liquid for distinct-color proof.
	w.set_cell(25, 25, OIL)
	w.flush_present()
	var wimg := w.get_water_image()
	var bimg := w.get_world_image()
	var wc: Color = w.get_material_color(WATER)
	var wbr := int(round(wc.r * 255.0))
	var wbg := int(round(wc.g * 255.0))
	var wbb := int(round(wc.b * 255.0))
	var gr: int = _grain(10, 10)
	var drop := wimg.get_pixel(10, 10)
	check(_chan(drop, "a") == 205, "F6 isolated drop coverage alpha is 205")
	check(_chan(drop, "r") == wbr + gr + 22, "F6 isolated drop red exact (data+grain+highlight)")
	check(_chan(drop, "g") == wbg + gr + 22, "F6 isolated drop green exact")
	check(_chan(drop, "b") == wbb + gr + 22, "F6 isolated drop blue exact")
	var drop_base := bimg.get_pixel(10, 10)
	check(_chan(drop_base, "r") == 18 and _chan(drop_base, "g") == 20 and _chan(drop_base, "b") == 24, "F6 liquid leaves base layer background")
	# No dilation: air beside the drop is transparent on the water layer.
	check(_chan(wimg.get_pixel(11, 10), "a") == 0, "F6 air beside liquid stays transparent (no dilation)")
	check(_chan(wimg.get_pixel(10, 11), "a") == 0, "F6 air below liquid stays transparent (no dilation)")
	# Channel middle is covered but shaded darker than the surface top.
	var top := wimg.get_pixel(15, 5)
	var mid := wimg.get_pixel(15, 7)
	check(_chan(top, "a") == 205 and _chan(mid, "a") == 205, "F6 channel cells covered")
	check(mid.r + mid.g + mid.b < top.r + top.g + top.b, "F6 channel depth shading darkens with depth")
	# Solid border: exact stone on base, transparent water above it.
	var sc: Color = w.get_material_color(STONE)
	var sgr: int = _grain(20, 10)
	var stone_px := bimg.get_pixel(20, 10)
	check(_chan(stone_px, "r") == int(round(sc.r * 255.0)) + sgr, "F6 solid-adjacent texel exact red")
	check(_chan(stone_px, "g") == int(round(sc.g * 255.0)) + sgr, "F6 solid-adjacent texel exact green")
	check(_chan(wimg.get_pixel(20, 10), "a") == 0, "F6 overlay never covers solids")
	# Water beside stone keeps its own color (dilation removal proof).
	check(_chan(wimg.get_pixel(19, 10), "a") == 205, "F6 liquid beside solid keeps coverage")
	# Two liquids render distinct colors.
	var oil_px := wimg.get_pixel(25, 25)
	check(_chan(oil_px, "a") == 205, "F6 oil covers the overlay")
	check(abs(oil_px.r - drop.r) + abs(oil_px.b - drop.b) > 0.1, "F6 two liquids render distinct colors")
	w.free()

# F7: host save envelope — truncation, lengths, floats, remap. Needs the
# live scene (main_controller save/load path). Deferred like the P1 scene
# checks so _ready completes first.
func _run_host_checks() -> void:
	var scene: PackedScene = load("res://main.tscn")
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	var SAVE: String = "user://ashfall_save.bin"
	if FileAccess.file_exists(SAVE):
		DirAccess.remove_absolute(SAVE)
	main.player_pos = Vector2(100.0, 150.0)
	main.current_material = AshfallWorld.MATERIAL_WATER
	main._save_game()
	check(main._transient_msg == "SAVED", "F7 save reports SAVED")
	check(FileAccess.file_exists(SAVE), "F7 save file written")

	# Round-trip restores avatar + held material + world.
	main.player_pos = Vector2(10.0, 10.0)
	main.world.set_cell(100, 100, AshfallWorld.MATERIAL_STONE)
	main._load_game()
	check(main._transient_msg == "LOADED", "F7 load reports LOADED")
	check(main.player_pos == Vector2(100.0, 150.0), "F7 load restores avatar position")
	check(main.current_material == AshfallWorld.MATERIAL_WATER, "F7 load restores held material by name")
	check(main.world.get_cell(100, 100) != AshfallWorld.MATERIAL_STONE, "F7 load restores exact world")

	# Truncation (below the 18-byte minimum) rejected, world untouched.
	var f := FileAccess.open(SAVE, FileAccess.READ)
	var good: PackedByteArray = f.get_buffer(int(f.get_length()))
	f.close()
	var occ: int = int(main.world.get_occupied_cell_count())
	var fw := FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_buffer(good.slice(0, 5))
	fw.close()
	main._load_game()
	check(main._transient_msg == "SAVE CORRUPT", "F7 truncated save rejected")
	check(int(main.world.get_occupied_cell_count()) == occ, "F7 rejected load commits nothing")

	# Trailing bytes rejected (strict envelope).
	fw = FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_buffer(good)
	fw.store_8(0xAB)
	fw.close()
	main._load_game()
	check(main._transient_msg == "SAVE CORRUPT", "F7 trailing byte rejected")

	# Non-finite player coords rejected.
	var badf := good.duplicate()
	var sb := StreamPeerBuffer.new()
	sb.put_float(0.0 / 0.0)
	var nan_bytes: PackedByteArray = sb.data_array
	for i in 4:
		badf[1 + i] = nan_bytes[i]
	fw = FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_buffer(badf)
	fw.close()
	main._load_game()
	check(main._transient_msg == "SAVE CORRUPT", "F7 NaN coords rejected")

	# Excessive sim_len rejected before any buffer read.
	var nlen: int = int(good.decode_u32(9))
	var sim: PackedByteArray = good.slice(13 + nlen + 4)
	fw = FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_8(2)
	fw.store_float(100.0)
	fw.store_float(150.0)
	fw.store_pascal_string("sand")
	fw.store_32(0x1000000)
	fw.store_buffer(sim)
	fw.close()
	main._load_game()
	check(main._transient_msg == "SAVE CORRUPT", "F7 excessive sim_len rejected")

	# Unknown material name falls back to the first table material.
	fw = FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_8(2)
	fw.store_float(100.0)
	fw.store_float(150.0)
	fw.store_pascal_string("zzz_nope")
	fw.store_32(sim.size())
	fw.store_buffer(sim)
	fw.close()
	main._load_game()
	var first_id: int = main.world.get_material_ids()[0]
	check(main.current_material == first_id, "F7 unknown material falls back to first table entry")
	check(main._transient_msg.begins_with("MATERIAL"), "F7 fallback carries a transient notice")

	# Stale v1 envelope (raw id, not name) no longer accepted.
	fw = FileAccess.open(SAVE, FileAccess.WRITE)
	fw.store_8(1)
	fw.store_float(100.0)
	fw.store_float(150.0)
	fw.store_8(3)
	fw.store_32(sim.size())
	fw.store_buffer(sim)
	fw.close()
	main._load_game()
	check(main._transient_msg == "SAVE VERSION?", "F7 stale v1 envelope rejected")

	if FileAccess.file_exists(SAVE):
		DirAccess.remove_absolute(SAVE)
	var tmp: String = "user://ashfall_save.bin.tmp"
	if FileAccess.file_exists(tmp):
		DirAccess.remove_absolute(tmp)
	main.queue_free()
	if _failures > 0:
		push_error("PRESENTATION VERIFICATIONS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL PRESENTATION VERIFICATIONS PASSED ---")
		quit(0)

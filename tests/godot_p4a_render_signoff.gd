extends SceneTree

# GUI-rendered P4A interaction pass (not a headless image approximation).
# Run: xvfb-run -a godot --path . --resolution 1280x720 --windowed --script tests/godot_p4a_render_signoff.gd
const CAPTURE_DIR := "res://docs/perf/captures"
const INTERACTION_CAPTURE := "res://docs/perf/captures/2026-10-15-p4a-bloom-off-interaction.png"
const FINISH_CAPTURE := "res://docs/perf/captures/2026-10-15-p4a-bloom-off-route-cleared.png"
var _failures: int = 0

func _init() -> void:
	_run.call_deferred()

func check(condition: bool, message: String) -> void:
	if condition:
		print("PASS: ", message)
	else:
		_failures += 1
		push_error("FAIL: " + message)

func _run() -> void:
	var packed: PackedScene = load("res://cave_benchmark.tscn")
	if packed == null:
		push_error("FAIL: playable cave scene loads")
		quit(1)
		return
	var main = packed.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	var bloom = main.get_node_or_null("BloomToggle")
	check(bloom != null, "P4A rendered scene has its bloom toggle")
	if bloom != null:
		bloom.set_bloom_enabled(false)
		check(not bloom.is_bloom_enabled() and not bloom.environment.glow_enabled, "P4A sign-off explicitly disables bloom")
	check(root.size == Vector2i(1280, 720), "P4A rendered viewport is exactly 1280x720 (actual %s)" % root.size)
	main.world.auto_step = false
	main._reset_cave_run()

	# Follow the authored route to the spring gate using the same side-view
	# integrator used by the fixed 60 Hz gameplay controller.
	for i in 400:
		main._integrate_player(1.0 / 60.0, 1.0, false)
	check(main.player_pos.x > 360.0 and main.player_pos.x < 376.0, "P4A rendered pass traverses to the timber gate")
	main._gameplay_awake = true
	main._simulation_manually_paused = false
	main._apply_wand_impact(Vector2i(350, 241))
	for i in 10:
		main.world.step()
		main._advance_projectiles(1.0 / 60.0)
	var steam_count: int = 0
	for y in range(220, 330):
		for x in range(325, 390):
			if main.world.get_cell(x, y) == AshfallWorld.MATERIAL_STEAM:
				steam_count += 1
	check(steam_count > 0, "P4A rendered pass releases water into the native fire/steam reaction")
	main._update_camera(1.0 / 60.0)
	main._update_marker()
	main._update_hud()
	main.world.flush_present()
	main._refresh_textures()
	check(await _capture(INTERACTION_CAPTURE), "P4A bloom-off interaction frame saved")

	# Open the eastern grille and bring the player to the authored portal for a
	# separate finish-state frame through actual side-view movement.
	main._apply_wand_impact(Vector2i(378, 314))
	main.world.flush_present()
	main.player_pos = Vector2(373.6, 321.0)
	main.player_velocity = Vector2.ZERO
	main.player_on_ground = true
	var gate_jump_started := false
	var shoulder_jump_started := false
	for i in 420:
		var axis_x := 1.0
		var request_jump := false
		if not gate_jump_started:
			gate_jump_started = true
			request_jump = true
		elif not shoulder_jump_started and main.player_pos.x >= 407.0:
			axis_x = 0.0
			if main.player_on_ground:
				shoulder_jump_started = true
				axis_x = 1.0
				request_jump = true
		main._integrate_player(1.0 / 60.0, axis_x, request_jump)
		main._check_finish_trigger()
		if main._run_finished:
			break
	check(main.player_pos.x >= 477.0 and main._box_passable(main.player_pos.x, main.player_pos.y), "P4A rendered pass moves through the breached grille to the eastern portal")
	check(main._run_finished and main._finish_overlay.visible, "P4A rendered pass reaches the explicit route-cleared state")
	main._update_camera(1.0 / 60.0)
	main._update_marker()
	main._update_hud()
	main.world.flush_present()
	main._refresh_textures()
	check(await _capture(FINISH_CAPTURE), "P4A bloom-off finish frame saved")

	main.queue_free()
	if _failures > 0:
		push_error("P4A RENDER SIGN-OFF FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- P4A RENDERED SIGN-OFF CAPTURES COMPLETE ---")
		quit(0)

func _capture(path: String) -> bool:
	await process_frame
	await RenderingServer.frame_post_draw
	var image: Image = root.get_texture().get_image()
	if image == null or image.get_width() != 1280 or image.get_height() != 720:
		push_error("Capture viewport mismatch: %s" % ("null" if image == null else str(image.get_size())))
		return false
	var absolute_dir := ProjectSettings.globalize_path(CAPTURE_DIR)
	if DirAccess.make_dir_recursive_absolute(absolute_dir) != OK and not DirAccess.dir_exists_absolute(absolute_dir):
		push_error("Unable to create capture directory: " + absolute_dir)
		return false
	var absolute_path := ProjectSettings.globalize_path(path)
	var error: Error = image.save_png(absolute_path)
	if error != OK:
		push_error("Unable to save capture: " + absolute_path)
		return false
	print("CAPTURE: %s (%dx%d)" % [absolute_path, image.get_width(), image.get_height()])
	return true

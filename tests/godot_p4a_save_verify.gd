extends SceneTree

# Host-envelope persistence for player/tool transients and native-world continuation.
# Run: godot --headless --path . --script tests/godot_p4a_save_verify.gd
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
	main.world.auto_step = false
	main._reset_cave_run()

	main.player_pos = Vector2(50.0, 80.0)
	main.player_velocity = Vector2(13.0, -41.0)
	main.player_on_ground = false
	main._aim_direction = Vector2(0.6, -0.8).normalized()
	main._aim_target = Vector2(150.0, 95.0)
	main._wand_cooldown = 0.17
	main._gameplay_awake = true
	main._simulation_manually_paused = true
	main._run_finished = false
	main._gameplay_tick = 777
	main.jump_requested = true
	main._pause_toggle_requested = false
	main._single_step_requested = true
	main._wand_fire_requested = true
	main._wand_fire_target = Vector2(160.0, 96.0)
	main.current_material = AshfallWorld.MATERIAL_WATER
	main._projectiles.clear()
	main._projectiles.append({
		"pos": Vector2(51.25, 77.5),
		"velocity": Vector2(120.0, -90.0),
		"age": 0.5,
	})
	main._impact_effects.clear()
	main._impact_effects.append({"pos": Vector2(61.0, 82.0), "age": 0.12})
	main._impact_effects.append({"pos": Vector2(66.0, 86.0), "age": 0.28})
	main.world.set_cell(90, 90, AshfallWorld.MATERIAL_WATER)
	main.world.flush_present()
	var expected_world: PackedByteArray = main.world.save_to_bytes()
	var expected_projectiles: Array[Dictionary] = main._projectiles.duplicate(true)
	var expected_impacts: Array[Dictionary] = main._impact_effects.duplicate(true)

	main._save_game()
	var save_file := FileAccess.open(main.SAVE_PATH, FileAccess.READ)
	check(save_file != null and save_file.get_8() == 3, "P4A transient snapshot is written with save-envelope version 3")
	if save_file != null:
		save_file.close()
	check(main._transient_msg == "SAVED", "P4A atomic host save completes")

	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	main.player_on_ground = true
	main._aim_direction = Vector2.RIGHT
	main._aim_target = Vector2(100.0, 80.0)
	main._wand_cooldown = 0.0
	main._gameplay_awake = false
	main._simulation_manually_paused = false
	main._gameplay_tick = 0
	main.jump_requested = false
	main._single_step_requested = false
	main._wand_fire_requested = false
	main._projectiles.clear()
	main._impact_effects.clear()
	main._recorded_commands.append({"tick": 1})
	main.world.set_cell(90, 90, AshfallWorld.MATERIAL_AIR)
	main._load_game()

	check(main._transient_msg == "LOADED", "P4A version-3 host state loads successfully")
	check(main.world.save_to_bytes() == expected_world, "P4A version-3 load restores exact native simulation bytes")
	check(main.player_pos == Vector2(50.0, 80.0) and main.player_velocity == Vector2(13.0, -41.0) and not main.player_on_ground, "P4A load restores player position, velocity, and grounded state")
	check(main._aim_direction.is_equal_approx(Vector2(0.6, -0.8).normalized()) and main._aim_target == Vector2(150.0, 95.0), "P4A load restores wand aim state")
	check(is_equal_approx(main._wand_cooldown, 0.17) and main._gameplay_awake and main._simulation_manually_paused, "P4A load restores cooldown and simulation run state")
	check(main._gameplay_tick == 777 and main.jump_requested and main._single_step_requested and main._wand_fire_requested, "P4A load restores tick position and queued next-tick inputs")
	check(main._wand_fire_target == Vector2(160.0, 96.0), "P4A load restores a queued wand-click target")
	var projectile_match: bool = main._projectiles.size() == expected_projectiles.size()
	if projectile_match and not expected_projectiles.is_empty():
		var loaded_bolt: Dictionary = main._projectiles[0]
		var original_bolt: Dictionary = expected_projectiles[0]
		projectile_match = (loaded_bolt["pos"] as Vector2).distance_to(original_bolt["pos"] as Vector2) < 0.0001
		projectile_match = projectile_match and (loaded_bolt["velocity"] as Vector2).distance_to(original_bolt["velocity"] as Vector2) < 0.0001
		projectile_match = projectile_match and absf(float(loaded_bolt["age"]) - float(original_bolt["age"])) < 0.000001
	var impact_match: bool = main._impact_effects.size() == expected_impacts.size()
	for i in range(mini(main._impact_effects.size(), expected_impacts.size())):
		var loaded_impact: Dictionary = main._impact_effects[i]
		var original_impact: Dictionary = expected_impacts[i]
		impact_match = impact_match and (loaded_impact["pos"] as Vector2).distance_to(original_impact["pos"] as Vector2) < 0.0001
		impact_match = impact_match and absf(float(loaded_impact["age"]) - float(original_impact["age"])) < 0.000001
	check(projectile_match and impact_match, "P4A load restores in-flight bolts and impact feedback")
	check(main.current_material == AshfallWorld.MATERIAL_WATER and not main.world.is_auto_step(), "P4A load restores material choice without wall-clock simulation stepping")
	check(int(main._replay_mode) == 1 and main._recorded_commands.is_empty(), "P4A loading a save invalidates the seed-based replay log")

	# Build a legacy v2 envelope; older saves contained only player feet, material,
	# and native-world bytes, so transient fields take safe authored defaults.
	var legacy_sim: PackedByteArray = main.world.save_to_bytes()
	var legacy := FileAccess.open(main.SAVE_PATH, FileAccess.WRITE)
	legacy.store_8(2)
	legacy.store_float(44.0)
	legacy.store_float(80.0)
	legacy.store_pascal_string("sand")
	legacy.store_32(legacy_sim.size())
	legacy.store_buffer(legacy_sim)
	legacy.close()
	main._load_game()
	check(main.player_pos == main.CAVE_SPAWN and main.player_velocity == Vector2.ZERO, "P4A version-2 migration restores position with safe motion defaults")
	check(main._projectiles.is_empty() and main._impact_effects.is_empty() and main._wand_cooldown == 0.0, "P4A version-2 migration defaults transient effects and cooldown")
	check(int(main._replay_mode) == 1, "P4A version-2 migration does not claim a replayable input history")

	var corrupt := FileAccess.open(main.SAVE_PATH, FileAccess.WRITE)
	corrupt.store_8(3)
	corrupt.store_8(0)
	corrupt.close()
	main._load_game()
	check(main._transient_msg == "SAVE CORRUPT", "P4A truncated version-3 envelope is rejected without applying state")

	main.queue_free()
	var save_path := ProjectSettings.globalize_path(main.SAVE_PATH)
	var tmp_path := ProjectSettings.globalize_path(main.SAVE_TMP_PATH)
	if FileAccess.file_exists(main.SAVE_PATH):
		DirAccess.remove_absolute(save_path)
	if FileAccess.file_exists(main.SAVE_TMP_PATH):
		DirAccess.remove_absolute(tmp_path)
	if _failures > 0:
		push_error("P4A SAVE CHECKS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL P4A SAVE CHECKS PASSED ---")
		quit(0)

extends SceneTree

# Deterministic command-log check for the P4A host-side player and Spring Wand.
# Run: godot --headless --path . --script tests/godot_p4a_replay_verify.gd
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

	# Build a 120-tick recording containing movement, jump, aim/fire, world pause,
	# and one explicit native single-step while paused.
	for i in range(120):
		var tick: int = i + 1
		var axis := 1.0 if tick <= 35 else (-0.5 if tick >= 56 and tick <= 75 else 0.0)
		var aim := Vector2i(60, 80) if tick < 120 else Vector2i(70, 0)
		var command := {
			"tick": tick,
			"move_x": axis,
			"jump": tick == 36 or tick == 82,
			"aim": aim,
			"fire": tick == 3 or tick == 70 or tick == 120,
			"pause": tick == 40 or tick == 55,
			"step": tick == 48,
		}
		main._gameplay_tick += 1
		main._recorded_commands.append(command)
		main._apply_gameplay_command(command)

	var expected_world: PackedByteArray = main.world.save_to_bytes()
	var expected_world_tick: int = main.world.get_tick()
	var expected_player: Vector2 = main.player_pos
	var expected_velocity: Vector2 = main.player_velocity
	var expected_shots: Array[Dictionary] = main._projectiles.duplicate(true)
	var expected_awake: bool = main._gameplay_awake
	var expected_paused: bool = main._simulation_manually_paused
	check(expected_shots.size() == 1, "P4A test recording ends with one in-flight projectile")
	check(main._recorded_commands.size() == 120, "P4A recording contains one command for each indexed gameplay tick")
	check(int(main._recorded_commands[0]["tick"]) == 1 and int(main._recorded_commands[119]["tick"]) == 120, "P4A command indices are contiguous and one-based")
	check(expected_world_tick > 60 and expected_awake, "P4A replay scenario advanced the native world through fixed gameplay ticks")

	main._start_replay()
	var safety := 0
	while int(main._replay_mode) == 2 and safety < 130:
		main._physics_process(1.0 / 60.0)
		safety += 1
	check(int(main._replay_mode) == 3, "P4A playback consumes the complete indexed command stream")
	check(main._gameplay_tick == 120 and main._replay_index == 120, "P4A replay executes each command at its recorded gameplay tick")
	check(main.world.get_tick() == expected_world_tick, "P4A replay matches the native simulation tick")
	check(main.world.save_to_bytes() == expected_world, "P4A replay reproduces the exact native-world serialization")
	check(main.player_pos.is_equal_approx(expected_player) and main.player_velocity.is_equal_approx(expected_velocity), "P4A replay reproduces player position and velocity")
	check(main._gameplay_awake == expected_awake and main._simulation_manually_paused == expected_paused, "P4A replay reproduces simulation wake/pause state")
	check(main._projectiles == expected_shots, "P4A replay reproduces transient projectile state")
	check(not main.world.is_auto_step(), "P4A replay never delegates simulation timing to the wall-clock accumulator")

	main._gameplay_tick = 17
	main.jump_requested = true
	main._wand_fire_requested = true
	main._wand_fire_target = Vector2(63.0, 79.0)
	var captured: Dictionary = main._capture_live_command()
	check(int(captured["tick"]) == 17 and bool(captured["jump"]) and bool(captured["fire"]), "P4A live input is sampled into one tick-indexed command")
	check(captured["aim"] == Vector2i(63, 79), "P4A wand click records its intended aim cell on the firing tick")

	main.queue_free()
	if _failures > 0:
		push_error("P4A REPLAY CHECKS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL P4A REPLAY CHECKS PASSED ---")
		quit(0)

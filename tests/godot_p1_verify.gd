extends SceneTree

# P1 gameplay verification: play level, avatar collision, persistence.
# Run: godot --headless --path . -s tests/godot_p1_verify.gd

func _init() -> void:
	var world := AshfallWorld.new()
	world.setup_play_level()
	assert(world.get_occupied_cell_count() > 1000, "play level populated")

	var spawn: Vector2i = world.get_player_spawn()
	assert(spawn == Vector2i(256, 480), "spawn matches engine level")
	assert(world.can_move_to_box(255, 478, 256, 479), "spawn avatar box passable")
	assert(not world.can_move_to_box(255, 479, 256, 480), "floor blocks avatar box")
	assert(not world.can_move_to_box(0, 200, 16, 220), "side wall blocks")
	assert(world.is_solid_cell(256, 480), "floor cell solid")
	assert(not world.is_solid_cell(256, 479), "spawn feet cell free")

	# Rain pocket stays contained below its tub (settled at setup).
	var leaks := 0
	for y in range(263, 331):
		for x in range(290, 421):
			if world.get_cell(x, y) == AshfallWorld.MATERIAL_WATER:
				leaks += 1
	assert(leaks == 0, "pocket water contained until dug")

	var saved := world.save_to_bytes()
	assert(saved.size() > 100, "save bytes non-empty")
	world.set_cell(256, 479, AshfallWorld.MATERIAL_SAND)
	assert(world.load_from_bytes(saved), "load valid bytes succeeds")
	assert(world.get_cell(256, 479) == AshfallWorld.MATERIAL_AIR, "load restores exact state")
	assert(world.save_to_bytes() == saved, "save bytes identical after round-trip")
	assert(not world.load_from_bytes(PackedByteArray()), "empty load rejected")

	print("P1 verification passed; occupied=", world.get_occupied_cell_count())
	world.free()
	_run_scene_checks.call_deferred()

# Scene level: avatar collision, avatar edits, save/load identity. Deferred
# so the instantiated scene completes _ready before we drive it.
func _run_scene_checks() -> void:
	var scene: PackedScene = load("res://main.tscn")
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	assert(main.player_pos == Vector2(256.0, 480.0), "avatar spawns at play-level spawn")
	assert(not main._box_passable(8.0, 400.0), "stone wall blocks avatar box")
	assert(main._box_passable(200.0, 300.0), "open air admits avatar box")

	# UI fits the base window: the sidebar must scroll instead of forcing the
	# layout taller than the 720px canvas (regression: min-height 979 pushed
	# the sidebar outside the screen).
	var layout: Control = main.get_node("MainLayout")
	assert(layout.get_minimum_size().y <= 720.0, "layout fits base window height")
	var scroll: Control = main.get_node("MainLayout/SidebarScroll")
	assert(scroll.get_rect().end.y <= 720.0, "sidebar stays on screen")
	# Water overlay is a second view of the same grid: any rect drift renders
	# water offset from the cells the brush edits.
	var base_tex: TextureRect = main.get_node("MainLayout/ViewportContainer/WorldStack/WorldTexture")
	var water_tex: TextureRect = main.get_node("MainLayout/ViewportContainer/WorldStack/WaterOverlay")
	assert(water_tex.get_rect() == base_tex.get_rect(), "water overlay aligned with base layer")

	main.player_pos = Vector2(60.0, 440.0)
	var before: int = main.world.get_occupied_cell_count()
	main._dig_at_player()
	assert(main.world.get_occupied_cell_count() < before, "avatar dig removes sand")
	main.current_material = AshfallWorld.MATERIAL_WATER
	main._place_at_player()
	assert(main.world.get_occupied_cell_count() > 0, "avatar place adds water")

	main.player_pos = Vector2(100.0, 150.0)
	main._save_game()
	main.player_pos = Vector2(10.0, 10.0)
	main.world.set_cell(100, 100, AshfallWorld.MATERIAL_STONE)
	main._load_game()
	assert(main.player_pos == Vector2(100.0, 150.0), "load restores avatar position")
	assert(main.current_material == AshfallWorld.MATERIAL_WATER, "load restores held material")
	assert(main.world.get_cell(100, 100) != AshfallWorld.MATERIAL_STONE, "load restores exact world")
	if FileAccess.file_exists("user://ashfall_save.bin"):
		DirAccess.remove_absolute("user://ashfall_save.bin")

	print("P1 scene verification passed")
	quit(0)

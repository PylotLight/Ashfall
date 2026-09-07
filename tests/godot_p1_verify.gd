extends SceneTree

# P1 gameplay verification: play level, avatar collision, persistence.
# Run: godot --headless --path . -s tests/godot_p1_verify.gd

func _init() -> void:
	var world := AshfallWorld.new()
	world.setup_play_level()
	assert(world.get_occupied_cell_count() > 1000, "play level populated")

	var spawn: Vector2i = world.get_player_spawn()
	assert(spawn == Vector2i(128, 240), "spawn matches engine level")
	assert(world.can_move_to_box(127, 238, 128, 239), "spawn avatar box passable")
	assert(not world.can_move_to_box(127, 239, 128, 240), "floor blocks avatar box")
	assert(not world.can_move_to_box(0, 100, 8, 110), "side wall blocks")
	assert(world.is_solid_cell(128, 240), "floor cell solid")
	assert(not world.is_solid_cell(128, 239), "spawn feet cell free")

	var saved := world.save_to_bytes()
	assert(saved.size() > 100, "save bytes non-empty")
	world.set_cell(128, 239, AshfallWorld.MATERIAL_SAND)
	assert(world.load_from_bytes(saved), "load valid bytes succeeds")
	assert(world.get_cell(128, 239) == AshfallWorld.MATERIAL_AIR, "load restores exact state")
	assert(world.save_to_bytes() == saved, "save bytes identical after round-trip")
	assert(not world.load_from_bytes(PackedByteArray()), "empty load rejected")

	print("P1 verification passed; occupied=", world.get_occupied_cell_count())
	world.free()
	quit(0)

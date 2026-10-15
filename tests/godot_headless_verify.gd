extends SceneTree

func _init() -> void:
	print("--- Starting Ashfall P0 Headless Verification ---")
	var world = AshfallWorld.new()
	assert(world != null, "AshfallWorld must instantiate")
	print("AshfallWorld instantiated successfully.")

	# 1. Initialize 256x256 world
	world.init_world(256, 256)
	assert(world.get_world_width() == 256, "World width must be 256")
	assert(world.get_world_height() == 256, "World height must be 256")
	print("World initialized to 256x256.")

	# 2. Setup reference world
	world.setup_reference_world()
	var count = world.get_occupied_cell_count()
	print("Reference world populated. Occupied cells: ", count)
	assert(count > 0, "Occupied cells must be > 0")

	# 3. Test modifying cells through bridge
	world.set_cell(128, 5, AshfallWorld.MATERIAL_SAND)
	assert(world.get_cell(128, 5) == AshfallWorld.MATERIAL_SAND, "Cell at (128, 5) must be sand")
	print("Cell modification verified.")

	# 4. Step simulation
	var initial_tick = world.get_tick()
	world.step()
	var next_tick = world.get_tick()
	assert(next_tick == initial_tick + 1, "Tick must advance on step")
	print("Stepped simulation from tick ", initial_tick, " to ", next_tick)

	# Verify sand fell from y=5 to y=6
	assert(world.get_cell(128, 6) == AshfallWorld.MATERIAL_SAND, "Sand must fall to y=6")
	print("Native sand fall verified through bridge.")

	# 5. Verify texture snapshot generation
	var tex = world.update_world_texture()
	assert(tex != null, "Texture snapshot must not be null")
	assert(tex.get_width() == 256, "Texture width must be 256")
	assert(tex.get_height() == 256, "Texture height must be 256")
	print("Native 256x256 texture snapshot generated successfully.")

	world.free()
	print("--- ALL P0 VERIFICATIONS PASSED ---")
	quit(0)

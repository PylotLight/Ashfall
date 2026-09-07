extends SceneTree

func _init() -> void:
	var world := AshfallWorld.new()
	world.init_world(256, 256)
	world.fill_rect(32, 216, 223, 223, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(32, 32, 39, 223, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(216, 32, 223, 223, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(72, 64, 119, 111, AshfallWorld.MATERIAL_SAND)
	world.fill_rect(136, 64, 183, 111, AshfallWorld.MATERIAL_WATER)

	for _step in 180:
		world.step()

	world.update_world_texture()
	var image := world.get_world_image()
	var output_path := "user://ashfall_visual_simulation.png"
	assert(image.save_png(output_path) == OK, "Visual simulation PNG must save")
	var stone_color := image.get_pixel(32, 216)
	assert(stone_color.r > 0.1 and abs(stone_color.r - stone_color.g) < 0.1 and abs(stone_color.g - stone_color.b) < 0.1,
		"Stone must render gray")
	assert(world.get_occupied_cell_count() > 0, "Scenario must contain material")
	print("Visual simulation passed; PNG written to ", output_path)
	world.free()
	quit(0)

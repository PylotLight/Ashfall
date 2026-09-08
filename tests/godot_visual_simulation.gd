extends SceneTree

func _init() -> void:
	var world := AshfallWorld.new()
	world.init_world(512, 512)
	world.fill_rect(64, 432, 446, 447, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(64, 64, 78, 447, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(432, 64, 446, 447, AshfallWorld.MATERIAL_STONE)
	world.fill_rect(144, 128, 238, 222, AshfallWorld.MATERIAL_SAND)
	world.fill_rect(272, 128, 366, 222, AshfallWorld.MATERIAL_WATER)

	for _step in 400:
		world.step()

	world.update_world_texture()
	var image := world.get_world_image()
	var output_path := "user://ashfall_visual_simulation.png"
	assert(image.save_png(output_path) == OK, "Visual simulation PNG must save")
	var stone_color := image.get_pixel(64, 432)
	assert(stone_color.r > 0.1 and abs(stone_color.r - stone_color.g) < 0.1 and abs(stone_color.g - stone_color.b) < 0.1,
		"Stone must render gray")
	assert(world.get_occupied_cell_count() > 0, "Scenario must contain material")
	# Water lives on its own layer: shaded (b clearly above r), translucent.
	var water_image := world.get_water_image()
	var found_water := false
	for y in water_image.get_height():
		for x in water_image.get_width():
			var c := water_image.get_pixel(x, y)
			if c.b - c.r > 0.12:
				found_water = true
				assert(c.a > 0.01 and c.a < 0.99, "Water layer must be translucent")
				break
		if found_water:
			break
	assert(found_water, "Water layer must contain visible water")
	print("Visual simulation passed; PNG written to ", output_path)
	world.free()

	# Falling water must not shade inverted (dark-on-top): a floating column
	# is thin in both directions, so its top must render as light as or
	# lighter than its middle.
	var fall := AshfallWorld.new()
	fall.init_world(16, 16)
	fall.fill_rect(7, 5, 9, 11, AshfallWorld.MATERIAL_WATER)
	fall.update_world_texture()
	var fall_image := fall.get_water_image()
	var top := fall_image.get_pixel(8, 5)
	var mid := fall_image.get_pixel(8, 8)
	assert(top.r + top.g + top.b >= mid.r + mid.g + mid.b,
		"Floating water top must not shade darker than its middle")
	fall.free()
	quit(0)

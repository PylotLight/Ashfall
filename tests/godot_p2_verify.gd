extends SceneTree

# P2.3 renderer-palette verification: colors/names from data, liquid shading
# by movement class, dynamic sidebar palette, table-order shortcuts.
# Run: godot --headless --path . -s tests/godot_p2_verify.gd

func _init() -> void:
	_run_core_checks()
	_run_scene_checks.call_deferred()

func _recolor(material_name: String, r: int, g: int, b: int) -> String:
	var f := FileAccess.open("res://data/materials.json", FileAccess.READ)
	assert(f != null, "canonical data present")
	var text: String = f.get_as_text()
	f.close()
	var anchor := text.find('"name": "%s"' % material_name)
	assert(anchor > 0, "material found in data")
	var color_key := text.find('"color"', anchor)
	var open_br := text.find('[', color_key)
	var close_br := text.find(']', open_br)
	return text.substr(0, open_br + 1) + "%d, %d, %d" % [r, g, b] + text.substr(close_br)

func _run_core_checks() -> void:
	var world := AshfallWorld.new()
	world.init_world(64, 64)
	assert(world.get_material_count() == 9, "default table has nine materials")
	assert(world.get_material_ids() == PackedInt32Array([0, 1, 2, 3, 4, 5, 6, 7, 8]), "ids follow table order")
	assert(world.get_material_name(2) == "sand", "sand name from data")
	assert(world.get_material_movement(3) == 2, "water movement class is liquid")
	assert(world.get_material_name(4) == "steam", "steam name from data")
	assert(world.get_material_movement(4) == 3, "steam movement class is gas")
	assert(world.get_material_movement(7) == 2, "lava movement class is liquid")
	var steam_c: Color = world.get_material_color(4)
	assert(steam_c.r > 0.75 and steam_c.g > 0.75 and steam_c.b > 0.75, "steam renders its data color")
	var rx := FileAccess.open("res://data/reactions.json", FileAccess.READ)
	assert(rx != null, "canonical reaction data present")
	assert(world.load_reactions(rx.get_as_text()), "canonical reactions load through bridge")
	rx.close()
	assert(world.get_material_movement(42) == -1, "unknown id movement is -1")
	assert(world.get_material_name(42) == "", "unknown id name is empty")

	# Data-driven color change: sand redefined red, painted block reads red.
	assert(world.load_materials(_recolor("sand", 255, 0, 0)), "recolored table loads")
	world.fill_rect(10, 10, 20, 20, 2)
	world.update_world_texture()
	var c: Color = world.get_world_image().get_pixel(15, 15)
	assert(c.r > 0.9 and c.g < 0.2 and c.b < 0.2, "sand renders the data color")

	# Liquid shading follows the table color, not hardcoded constants:
	# water redefined orange; a deep column shades darker than its surface.
	assert(world.load_materials(_recolor("water", 200, 100, 50)), "orange water loads")
	world.fill_rect(30, 10, 30, 40, 3)
	world.update_world_texture()
	var wimg := world.get_water_image()
	var surface: Color = wimg.get_pixel(30, 10)
	var deep: Color = wimg.get_pixel(30, 30)
	assert(surface.a > 0.5, "surface water covers the overlay")
	assert(deep.r + deep.g + deep.b < surface.r + surface.g + surface.b,
		"liquid depth shading derives from the data color")
	world.free()
	print("P2 core palette verification passed")

func _run_scene_checks() -> void:
	var scene: PackedScene = load("res://main.tscn")
	var main = scene.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	var box: VBoxContainer = main.get_node("MainLayout/SidebarScroll/Sidebar/PaletteBox")
	assert(box.get_child_count() == 9, "palette built from table")
	var first: Button = box.get_child(0)
	assert(first.text == "[1] Air (Eraser)", "first button follows table order")
	assert(main.palette_ids == [0, 1, 2, 3, 4, 5, 6, 7, 8], "shortcuts map in table order")
	main._select_palette(2)
	assert(main.current_material == 2, "table-order shortcut selects sand")
	main._select_palette(99)
	assert(main.current_material == 2, "out-of-range shortcut ignored")
	assert(not main.has_node("MainLayout/SidebarScroll/Sidebar/SandBtn"), "static SandBtn removed")
	var sand_btn: Button = box.get_child(2)
	sand_btn.pressed.emit()
	assert(main.current_material == 2, "palette button selects its material")
	var fire_btn: Button = box.get_child(5)
	assert(fire_btn.text == "[6] Fire", "fire button follows table order")
	main._select_palette(5)
	assert(main.current_material == 5, "table-order shortcut selects fire")
	main.queue_free()
	print("P2 scene palette verification passed")
	quit(0)

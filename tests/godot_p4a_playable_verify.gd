extends SceneTree

# P4A first-room playable slice: surface entrance + grounded movement + water wand.
# Run: godot --headless --path . --script tests/godot_p4a_playable_verify.gd
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

	# Authored place: fixed-seed surface, a sky-connected entrance, a broad
	# first chamber and a small, deliberately gated water spring.
	check(main.world.get_world_width() == 512 and main.world.get_world_height() == 512, "P4A world remains 512x512")
	check(main.world.get_cell(44, 79) == AshfallWorld.MATERIAL_AIR and main.world.get_cell(44, 80) != AshfallWorld.MATERIAL_AIR, "P4A spawn patch has air above solid surface")
	check(main.world.get_cell(101, 72) == AshfallWorld.MATERIAL_AIR, "P4A cave mouth intersects open sky")
	check(main.world.get_cell(218, 194) == AshfallWorld.MATERIAL_AIR and main.world.get_cell(218, 280) != AshfallWorld.MATERIAL_AIR, "P4A first chamber has open interior and closed natural floor")
	check(main.world.get_cell(350, 238) == AshfallWorld.MATERIAL_WATER and main.world.get_cell(350, 241) == AshfallWorld.MATERIAL_WOOD, "P4A spring is held behind a breakable timber sill")
	check(main.world.get_cell(378, 300) == AshfallWorld.MATERIAL_WOOD, "P4A eastern passage is blocked by a floor-anchored timber grille")
	check(main.player_pos == main.CAVE_SPAWN and main._box_passable(main.player_pos.x, main.player_pos.y), "P4A character begins in the world with a passable body")
	check(not main.world.is_auto_step(), "P4A reactive world starts settled until the player reaches the spring route")

	# Physics: feet remain grounded, walk horizontally, jump with an upward
	# impulse, then return to the terrain; solids prevent passing through walls.
	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	main._integrate_player(1.0 / 60.0, 0.0, false)
	check(main.player_on_ground and absf(main.player_pos.y - main.CAVE_SPAWN.y) < 0.05, "P4A gravity resolves onto the surface floor")
	var x_start: float = main.player_pos.x
	main._integrate_player(0.05, 1.0, false)
	check(main.player_pos.x > x_start and main.player_on_ground, "P4A grounded lateral movement works")
	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	main._integrate_player(1.0 / 60.0, 0.0, true)
	check(main.player_pos.y < main.CAVE_SPAWN.y and main.player_velocity.y < 0.0, "P4A jump applies upward velocity")
	for i in 90:
		main._integrate_player(1.0 / 60.0, 0.0, false)
	check(main.player_on_ground and absf(main.player_pos.y - main.CAVE_SPAWN.y) < 0.05, "P4A jump falls back onto the surface floor")
	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	for i in 400:
		main._integrate_player(1.0 / 60.0, 1.0, false)
	check(main.player_pos.x > 360.0 and main.player_pos.x < 376.0 and main._box_passable(main.player_pos.x, main.player_pos.y), "P4A surface slope and cave route are traversable up to the timber gate (x=%.1f y=%.1f)" % [main.player_pos.x, main.player_pos.y])
	for y in range(74, 86):
		main.world.set_cell(50, y, AshfallWorld.MATERIAL_STONE)
	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	for i in 40:
		main._integrate_player(0.05, 1.0, false)
	check(main.player_pos.x < 49.0, "P4A collision box cannot move through a solid wall")
	for y in range(74, 86):
		main.world.set_cell(50, y, AshfallWorld.MATERIAL_AIR)

	# Wand: fire creates a moving bolt; its impact opens soft terrain and adds
	# real water cells, leaving the native simulation to create reactions.
	var sill_wood_before: int = 0
	for x in range(347, 355):
		if main.world.get_cell(x, 241) == AshfallWorld.MATERIAL_WOOD:
			sill_wood_before += 1
	main._apply_wand_impact(Vector2i(350, 241))
	var sill_wood_after: int = 0
	for x in range(347, 355):
		if main.world.get_cell(x, 241) == AshfallWorld.MATERIAL_WOOD:
			sill_wood_after += 1
	check(sill_wood_before > 0 and sill_wood_after < sill_wood_before, "P4A wand impact removes the timber spring cap")
	check(main.world.get_cell(350, 241) == AshfallWorld.MATERIAL_WATER, "P4A wand impact writes water into the opened cell world")
	for i in 10:
		main.world.step()
	var steam_count: int = 0
	for y in range(220, 330):
		for x in range(325, 390):
			if main.world.get_cell(x, y) == AshfallWorld.MATERIAL_STEAM:
				steam_count += 1
	check(steam_count > 0, "P4A released water produces visible steam through native fire reactions")
	main._apply_wand_impact(Vector2i(378, 314))
	check(main._box_passable(378.0, 317.0), "P4A second wand breach opens a character-sized route through the grille")
	main.player_pos = main.CAVE_SPAWN
	main.player_velocity = Vector2.ZERO
	main._wand_cooldown = 0.0
	var fired: bool = main._fire_wand_at(Vector2(95.0, 74.0))
	check(fired and main._projectiles.size() == 1, "P4A mouse aim spawns a wand bolt")
	var before_pos: Vector2 = main._projectiles[0]["pos"]
	main._advance_projectiles(0.05)
	check(main._projectiles.size() == 1 and (main._projectiles[0]["pos"] as Vector2).distance_to(before_pos) > 5.0, "P4A wand bolt advances in world space")
	check(main.get_node_or_null("MainLayout/ViewportContainer/WorldStack/GameplayFX") != null, "P4A projectile and impact FX layer is wired")
	check(main.get_node_or_null("MainLayout/SidebarScroll/Sidebar/ToolReadout") != null, "P4A visible HUD explains the Spring Wand")
	check(main.palette_box.get_parent().name == "DebugBox", "P4A material-paint tools stay behind the debug opener")

	main.queue_free()
	if _failures > 0:
		push_error("P4A PLAYABLE CHECKS FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- ALL P4A PLAYABLE CHECKS PASSED ---")
		quit(0)

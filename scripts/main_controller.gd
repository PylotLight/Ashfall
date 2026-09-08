extends Control

# Ashfall Host Controller
# Presentation and input forwarding only.
# No simulation rules or world state are implemented here.

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WorldTexture
@onready var water_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WaterOverlay
@onready var player_marker: ColorRect = $MainLayout/ViewportContainer/WorldStack/PlayerMarker
@onready var tick_label: Label = $MainLayout/Sidebar/InfoBox/TickValue
@onready var cells_label: Label = $MainLayout/Sidebar/InfoBox/CellsValue
@onready var status_label: Label = $MainLayout/Sidebar/InfoBox/StatusValue
@onready var material_label: Label = $MainLayout/Sidebar/InfoBox/MaterialValue
@onready var player_label: Label = $MainLayout/Sidebar/InfoBox/PlayerValue
@onready var fps_label: Label = $MainLayout/Sidebar/InfoBox/FpsValue
@onready var frame_label: Label = $MainLayout/Sidebar/InfoBox/FrameValue
@onready var sim_label: Label = $MainLayout/Sidebar/InfoBox/SimValue

var current_material: int = AshfallWorld.MATERIAL_SAND
var brush_size: int = 4
var is_drawing: bool = false
var draw_material: int = AshfallWorld.MATERIAL_SAND
var _hud_cooldown: float = 0.0
var _frame_ms_ema: float = 16.0
const HUD_REFRESH_INTERVAL: float = 0.25

# Player avatar state. Position is in cell units, anchored at the feet center.
# The sim owns all cells; the avatar is host-side and only queries native
# stone collision (can_move_to_box) before translating.
var player_pos: Vector2 = Vector2(256.0, 480.0)
const PLAYER_HALF_W: float = 0.4
const PLAYER_H: float = 1.8
const PLAYER_SPEED: float = 90.0
const SAVE_PATH: String = "user://ashfall_save.bin"

func _ready() -> void:
	# Boot the hand-authored P1 play level in C++ (deterministic content).
	world.setup_play_level()
	world.auto_step = false
	world.set_sim_rate_hz(60.0)
	world.set_max_steps_per_physics_frame(2)
	_respawn_player()
	texture_display.texture = world.get_world_texture()
	water_display.texture = world.get_water_texture()
	_update_hud()
	_update_marker()

func _process(delta: float) -> void:
	# Frame-time EMA for the perf HUD (cheap, every frame).
	if delta > 0.0:
		_frame_ms_ema = lerpf(_frame_ms_ema, delta * 1000.0, 0.08)
	_move_player(delta)
	_update_marker()
	# Throttled HUD: occupied-cell query is O(1) but label churn every frame
	# is still wasteful. Refresh at 4Hz.
	_hud_cooldown -= delta
	if _hud_cooldown <= 0.0:
		_hud_cooldown = HUD_REFRESH_INTERVAL
		_update_hud()
	else:
		# Cheap tick-only refresh keeps the counter live without cell query.
		tick_label.text = str(world.get_tick())

func _respawn_player() -> void:
	var spawn: Vector2i = world.get_player_spawn()
	player_pos = Vector2(float(spawn.x), float(spawn.y))

func _box_passable(px: float, py: float) -> bool:
	var x1: int = floori(px - PLAYER_HALF_W)
	var x2: int = ceili(px + PLAYER_HALF_W) - 1
	var y1: int = floori(py - PLAYER_H)
	var y2: int = ceili(py) - 1
	return world.can_move_to_box(x1, y1, x2, y2)

func _move_player(delta: float) -> void:
	var dir := Vector2.ZERO
	if Input.is_physical_key_pressed(KEY_A) or Input.is_physical_key_pressed(KEY_LEFT):
		dir.x -= 1.0
	if Input.is_physical_key_pressed(KEY_D) or Input.is_physical_key_pressed(KEY_RIGHT):
		dir.x += 1.0
	if Input.is_physical_key_pressed(KEY_W) or Input.is_physical_key_pressed(KEY_UP):
		dir.y -= 1.0
	if Input.is_physical_key_pressed(KEY_S) or Input.is_physical_key_pressed(KEY_DOWN):
		dir.y += 1.0
	if dir == Vector2.ZERO:
		return
	dir = dir.normalized()
	var world_w: float = float(world.get_world_width())
	var world_h: float = float(world.get_world_height())
	var nx: float = clampf(player_pos.x + dir.x * PLAYER_SPEED * delta, 0.5, world_w - 0.5)
	if _box_passable(nx, player_pos.y):
		player_pos.x = nx
	var ny: float = clampf(player_pos.y + dir.y * PLAYER_SPEED * delta, PLAYER_H, world_h - 0.5)
	if _box_passable(player_pos.x, ny):
		player_pos.y = ny

func _player_center_cell() -> Vector2i:
	return Vector2i(roundi(player_pos.x), roundi(player_pos.y - PLAYER_H * 0.5))

func _dig_at_player() -> void:
	# Dig sand/water around the avatar; stone is too hard to dig by hand.
	var c := _player_center_cell()
	for dy in range(-2, 3):
		for dx in range(-2, 3):
			var m: int = world.get_cell(c.x + dx, c.y + dy)
			if m != AshfallWorld.MATERIAL_AIR and m != AshfallWorld.MATERIAL_STONE:
				world.set_cell(c.x + dx, c.y + dy, AshfallWorld.MATERIAL_AIR)
	world.update_world_texture()
	_update_hud()

func _place_at_player() -> void:
	# Pour the held material around the avatar, only into empty cells.
	if current_material == AshfallWorld.MATERIAL_AIR:
		return
	var c := _player_center_cell()
	for dy in range(-2, 3):
		for dx in range(-2, 3):
			if world.get_cell(c.x + dx, c.y + dy) == AshfallWorld.MATERIAL_AIR:
				world.set_cell(c.x + dx, c.y + dy, current_material)
	world.update_world_texture()
	_update_hud()

func _save_game() -> void:
	var sim := world.save_to_bytes()
	var f := FileAccess.open(SAVE_PATH, FileAccess.WRITE)
	if f == null:
		status_label.text = "SAVE FAILED"
		return
	f.store_8(1) # save format version
	f.store_float(player_pos.x)
	f.store_float(player_pos.y)
	f.store_8(current_material)
	f.store_32(sim.size())
	f.store_buffer(sim)
	f.close()
	_update_hud()

func _load_game() -> void:
	if not FileAccess.file_exists(SAVE_PATH):
		status_label.text = "NO SAVE"
		return
	var f := FileAccess.open(SAVE_PATH, FileAccess.READ)
	if f == null or f.get_length() < 11:
		status_label.text = "SAVE CORRUPT"
		return
	if f.get_8() != 1:
		status_label.text = "SAVE VERSION?"
		return
	var px: float = f.get_float()
	var py: float = f.get_float()
	var mat: int = f.get_8()
	var sim_len: int = int(f.get_32())
	var sim := f.get_buffer(sim_len)
	f.close()
	if sim.size() != sim_len or not world.load_from_bytes(sim):
		status_label.text = "LOAD FAILED"
		return
	player_pos = Vector2(px, py)
	current_material = mat
	_update_marker()
	_update_hud()

func _update_hud() -> void:
	tick_label.text = str(world.get_tick())
	cells_label.text = str(world.get_occupied_cell_count())
	status_label.text = "RUNNING" if world.is_auto_step() else "PAUSED"
	fps_label.text = "%d fps" % Engine.get_frames_per_second()
	frame_label.text = "%.2f ms" % _frame_ms_ema
	sim_label.text = "%.2f ms" % world.get_last_step_ms()
	player_label.text = "(%d, %d)" % [roundi(player_pos.x), roundi(player_pos.y)]

func _display_scale() -> Vector2:
	# KEEP_ASPECT_CENTERED letterbox: uniform scale plus centered offset.
	var cs: Vector2 = texture_display.size
	var world_w: float = float(world.get_world_width())
	var world_h: float = float(world.get_world_height())
	if cs.x <= 0.0 or cs.y <= 0.0:
		return Vector2.ZERO
	var s: float = minf(cs.x / world_w, cs.y / world_h)
	return Vector2(s, s)

func _display_offset(scale: Vector2) -> Vector2:
	var cs: Vector2 = texture_display.size
	return Vector2(
		(cs.x - float(world.get_world_width()) * scale.x) * 0.5,
		(cs.y - float(world.get_world_height()) * scale.y) * 0.5)

func _world_to_display(cell: Vector2) -> Vector2:
	var s := _display_scale()
	var o := _display_offset(s)
	return Vector2(o.x + cell.x * s.x, o.y + cell.y * s.y)

func _display_to_world(local_pos: Vector2) -> Vector2i:
	var s := _display_scale()
	var o := _display_offset(s)
	var world_w: int = world.get_world_width()
	var world_h: int = world.get_world_height()
	if s.x <= 0.0:
		return Vector2i(-1, -1)
	var cell_x: int = clampi(int(floor((local_pos.x - o.x) / s.x)), 0, world_w - 1)
	var cell_y: int = clampi(int(floor((local_pos.y - o.y) / s.y)), 0, world_h - 1)
	return Vector2i(cell_x, cell_y)

func _update_marker() -> void:
	# Avatar box in display space (same letterbox as the world texture).
	var top_left := _world_to_display(Vector2(player_pos.x - PLAYER_HALF_W, player_pos.y - PLAYER_H))
	var bottom_right := _world_to_display(Vector2(player_pos.x + PLAYER_HALF_W, player_pos.y))
	player_marker.position = top_left
	player_marker.size = bottom_right - top_left
	match current_material:
		AshfallWorld.MATERIAL_SAND:
			material_label.text = "Sand"
		AshfallWorld.MATERIAL_WATER:
			material_label.text = "Water"
		AshfallWorld.MATERIAL_STONE:
			material_label.text = "Stone"
		AshfallWorld.MATERIAL_AIR:
			material_label.text = "Air (Eraser)"

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.is_pressed() and not event.is_echo():
		match event.keycode:
			KEY_SPACE:
				world.auto_step = not world.is_auto_step()
				_update_hud()
			KEY_S:
				world.step()
				_update_hud()
			KEY_R:
				world.setup_play_level()
				_respawn_player()
				_update_marker()
				_update_hud()
			KEY_E:
				_dig_at_player()
			KEY_F:
				_place_at_player()
			KEY_F5:
				_save_game()
			KEY_F9:
				_load_game()
			KEY_1:
				current_material = AshfallWorld.MATERIAL_SAND
				_update_hud()
			KEY_2:
				current_material = AshfallWorld.MATERIAL_WATER
				_update_hud()
			KEY_3:
				current_material = AshfallWorld.MATERIAL_STONE
				_update_hud()
			KEY_4:
				current_material = AshfallWorld.MATERIAL_AIR
				_update_hud()

func _on_world_texture_gui_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			is_drawing = event.is_pressed()
			draw_material = current_material
			if is_drawing:
				_draw_at(event.position)
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			is_drawing = event.is_pressed()
			draw_material = AshfallWorld.MATERIAL_AIR
			if is_drawing:
				_draw_at(event.position)
	elif event is InputEventMouseMotion and is_drawing:
		_draw_at(event.position)

func _draw_at(local_pos: Vector2) -> void:
	var cell := _display_to_world(local_pos)
	if cell.x < 0:
		return
	var world_w: int = world.get_world_width()
	var world_h: int = world.get_world_height()

	var x1: int = maxi(0, cell.x - brush_size)
	var y1: int = maxi(0, cell.y - brush_size)
	var x2: int = mini(world_w - 1, cell.x + brush_size)
	var y2: int = mini(world_h - 1, cell.y + brush_size)

	world.fill_rect(x1, y1, x2, y2, draw_material)
	world.update_world_texture()
	_update_hud()

func _on_sand_btn_pressed() -> void:
	current_material = AshfallWorld.MATERIAL_SAND
	_update_hud()

func _on_water_btn_pressed() -> void:
	current_material = AshfallWorld.MATERIAL_WATER
	_update_hud()

func _on_stone_btn_pressed() -> void:
	current_material = AshfallWorld.MATERIAL_STONE
	_update_hud()

func _on_eraser_btn_pressed() -> void:
	current_material = AshfallWorld.MATERIAL_AIR
	_update_hud()

func _on_step_btn_pressed() -> void:
	world.step()
	_update_hud()

func _on_play_btn_pressed() -> void:
	world.auto_step = not world.is_auto_step()
	_update_hud()

func _on_reset_btn_pressed() -> void:
	world.setup_play_level()
	_respawn_player()
	_update_marker()
	_update_hud()

func _on_save_btn_pressed() -> void:
	_save_game()

func _on_load_btn_pressed() -> void:
	_load_game()

extends Control

# Ashfall Cave Benchmark Controller (P3A Slice A1, task A1+A3).
# Presentation and input forwarding only. No simulation rules here.
#
# The cave is authored procedurally at boot through the public AshfallWorld
# API (init_world/fill_rect/set_cell/step) — no engine, bridge, or data
# changes. Content: irregular rock silhouette with stone lining, background
# rock mass (distinct wall tones), wood platform structure, water reservoir,
# oil + wood combustible area with a fresh fire bed, warm light source
# (PointLight2D + additive glow decal over the fire pit), character-sized
# player placeholder, stable pixel scaling (nearest world, letterboxed).
#
# Resolution ladder (P3A task A3, review 12.2) for this scene:
#   sim           512x512 cells (1 cell = 1 sim unit, authoritative)
#   world-render  512x512 px textures, 1 px per cell, NEAREST (texture_filter)
#   lighting      canvas-resolution Godot 2D light + glow decal (<= display)
#   display       1280x720 viewport, KEEP_ASPECT_CENTERED letterbox upscale
#   UI            same-canvas vector controls, resolution-independent, crisp
# World instruments read near pixel-art scale; the final world upscale is
# nearest so base pixels stay crisp (water overlay alone stays linear).
# Follow-camera-ready (A2): the world_to_display/display_to_world mapping
# below is the anchor a Camera2D follow will reuse; marker, light, and glow
# already track cell space every frame.
#
# Controls: WASD/arrows = move avatar; N = single sim tick; SPACE =
# play/pause; R = rebuild cave; E = dig at avatar; F = place held material;
# F5 = save; F9 = load; F1 = open material demo (demo stays bootable);
# 1..0 = palette shortcuts; mouse paint on the world view.

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WorldTexture
@onready var water_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WaterOverlay
@onready var cave_light: PointLight2D = $MainLayout/ViewportContainer/WorldStack/CaveLight
@onready var fire_glow: TextureRect = $MainLayout/ViewportContainer/WorldStack/FireGlow
@onready var player_marker: ColorRect = $MainLayout/ViewportContainer/WorldStack/PlayerMarker
@onready var tick_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/TickValue
@onready var cells_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/CellsValue
@onready var status_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/StatusValue
@onready var material_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/MaterialValue
@onready var player_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/PlayerValue
@onready var fps_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/FpsValue
@onready var frame_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/FrameValue
@onready var sim_label: Label = $MainLayout/SidebarScroll/Sidebar/InfoBox/SimValue
@onready var palette_box: VBoxContainer = $MainLayout/SidebarScroll/Sidebar/PaletteBox

var current_material: int = AshfallWorld.MATERIAL_SAND
var brush_size: int = 4
var is_drawing: bool = false
var draw_material: int = AshfallWorld.MATERIAL_SAND
var palette_ids: Array[int] = []
var _hud_cooldown: float = 0.0
var _frame_ms_ema: float = 16.0
const HUD_REFRESH_INTERVAL: float = 0.25

# Avatar state: cell units, feet center. Same play scale as the demo
# (0.8 wide x 1.8 tall) so character/viewport ratios match across scenes.
var player_pos: Vector2 = Vector2(240.0, 330.0)
const PLAYER_HALF_W: float = 0.4
const PLAYER_H: float = 1.8
const PLAYER_SPEED: float = 90.0
# Feet rest on the wood platform beam (top face y = 330).
const CAVE_SPAWN: Vector2 = Vector2(240.0, 330.0)
# Warm-light anchor: just above the fire bed (fire cells y 442..447).
const FIRE_ANCHOR: Vector2 = Vector2(256.0, 436.0)
const SAVE_PATH: String = "user://ashfall_save.bin"
const SAVE_TMP_PATH: String = "user://ashfall_save.bin.tmp"
# Host save envelope v2 (same contract as the demo controller): version
# u8=2 | px float | py float | material NAME (pascal string) | sim_len u32
# | sim bytes. Minimum 18 bytes; atomic write via tmp + rename.
const HOST_SAVE_VERSION: int = 2
const HOST_SAVE_MIN_BYTES: int = 18
const HOST_SAVE_SIM_MAX: int = 64 * 1024 * 1024
var _transient_msg: String = ""
var _transient_ttl: float = 0.0
const TRANSIENT_SECS: float = 3.0
var _marker_mat: int = -1

# P3A A2 wiring (integrator): shared follow-camera + debug-panel helpers.
# Mirrors main_controller.gd; the helpers are scene-agnostic (NodePath-guarded).
const FollowCamera = preload("res://scripts/follow_camera.gd")
const HudPanels = preload("res://scripts/hud_panels.gd")
var _camera = null
var _base_atlas: AtlasTexture
var _water_atlas: AtlasTexture
var _view_region: Rect2i = Rect2i(0, 0, 0, 0)
var _debug_panel: PopupPanel

func _ready() -> void:
	get_window().focus_exited.connect(_on_focus_lost)
	var data_file := FileAccess.open("res://data/materials.json", FileAccess.READ)
	if data_file != null:
		if not world.load_materials(data_file.get_as_text()):
			push_error("cave_benchmark: data/materials.json rejected, using built-in defaults")
		data_file.close()
	else:
		push_error("cave_benchmark: data/materials.json missing, using built-in defaults")
	var rx_file := FileAccess.open("res://data/reactions.json", FileAccess.READ)
	if rx_file != null:
		if not world.load_reactions(rx_file.get_as_text()):
			push_error("cave_benchmark: data/reactions.json rejected, reactions disabled")
		rx_file.close()
	else:
		push_error("cave_benchmark: data/reactions.json missing, reactions disabled")
	_build_cave()
	world.auto_step = false
	world.set_sim_rate_hz(60.0)
	world.set_max_steps_per_physics_frame(2)
	_respawn_player()
	texture_display.texture = world.get_world_texture()
	water_display.texture = world.get_water_texture()
	_build_palette()
	_update_hud()
	_init_camera()
	_update_marker()
	_init_hud_separation()

# Deterministic per-row jitter in [0, 1): integer hash, no RNG state, so
# the silhouette is identical every boot.
func _row_hash(y: int) -> float:
	var h: int = (y * 1103515245 + 12345) & 0x7fffffff
	return float((h >> 7) % 100) / 100.0

func _mat_id(mat_name: String, fallback: int) -> int:
	for id in world.get_material_ids():
		if world.get_material_name(id) == mat_name:
			return id
	return fallback

func _build_cave() -> void:
	var AIR: int = AshfallWorld.MATERIAL_AIR
	var STONE: int = AshfallWorld.MATERIAL_STONE
	var SAND: int = AshfallWorld.MATERIAL_SAND
	var WATER: int = AshfallWorld.MATERIAL_WATER
	var FIRE: int = AshfallWorld.MATERIAL_FIRE
	var WOOD: int = AshfallWorld.MATERIAL_WOOD
	var ROCK: int = AshfallWorld.MATERIAL_ROCK
	var OIL: int = _mat_id("oil", 9)
	world.init_world(512, 512)
	# Background mass: everything starts as dark rock; the carved cavern
	# reads against it as the background wall (rock outside, stone lining
	# inside = two distinct wall tones).
	world.fill_rect(0, 0, 511, 511, ROCK)
	# Main cavern: ellipse with jittered edges (irregular silhouette).
	var cx: float = 256.0
	var cy: float = 285.0
	var rx: float = 200.0
	var ry: float = 160.0
	for y in range(126, 445):
		var ny: float = (float(y) - cy) / ry
		var half: float = rx * sqrt(maxf(0.0, 1.0 - ny * ny))
		var j: float = (_row_hash(y) - 0.5) * 24.0
		var x0: int = clampi(int(cx - half + j), 1, 510)
		var x1: int = clampi(int(cx + half + j), 1, 510)
		world.fill_rect(x0, y, x1, y, AIR)
		# Stone lining ring just outside the carve (foreground rock layer).
		world.fill_rect(maxi(0, x0 - 6), y, x0 - 1, y, STONE)
		world.fill_rect(x1 + 1, y, mini(511, x1 + 6), y, STONE)
	# Chimney vent to the surface (smoke/ember path for later slices).
	world.fill_rect(246, 40, 266, 130, AIR)
	world.fill_rect(240, 40, 245, 130, STONE)
	world.fill_rect(267, 40, 272, 130, STONE)
	# Stalactites: static teeth hanging from the ceiling.
	world.fill_rect(180, 125, 190, 160, STONE)
	world.fill_rect(300, 125, 308, 150, STONE)
	world.fill_rect(340, 125, 346, 170, STONE)
	# Floor slab across the cavern foot.
	world.fill_rect(56, 448, 456, 470, STONE)
	# Water reservoir (right): explicitly walled basin so containment never
	# depends on where the ellipse edge lands.
	world.fill_rect(330, 370, 405, 447, AIR)
	world.fill_rect(324, 370, 329, 447, STONE)
	world.fill_rect(406, 370, 411, 447, STONE)
	world.fill_rect(330, 410, 405, 447, WATER)
	# Wood structure: platform beam on two posts down to the floor.
	world.fill_rect(150, 330, 330, 336, WOOD)
	world.fill_rect(150, 337, 156, 447, WOOD)
	world.fill_rect(324, 337, 330, 447, WOOD)
	# Combustible area (left of the fire bed): oil pool + wood pile.
	world.fill_rect(120, 441, 185, 447, OIL)
	world.fill_rect(96, 425, 114, 447, WOOD)
	world.fill_rect(120, 435, 140, 440, WOOD)
	# Dune patch for powder texture at play scale.
	world.fill_rect(60, 442, 95, 447, SAND)
	# Settle liquids/powders (fixed count => deterministic) BEFORE placing
	# fire, so boot shows fresh flames instead of pre-burned fuel.
	for i in 60:
		world.step()
	# Fire bed with stone curb; the warm light anchor sits just above it.
	world.fill_rect(238, 444, 243, 447, STONE)
	world.fill_rect(269, 444, 274, 447, STONE)
	world.fill_rect(244, 442, 268, 447, FIRE)
	world.flush_present()

func _process(delta: float) -> void:
	if delta > 0.0:
		_frame_ms_ema = lerpf(_frame_ms_ema, delta * 1000.0, 0.08)
	_move_player(delta)
	_update_camera(delta)
	_update_marker()
	_update_light()
	if is_drawing and not (Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		is_drawing = false
	world.flush_present()
	_refresh_textures()
	if _transient_ttl > 0.0:
		_transient_ttl -= delta
		if _transient_ttl <= 0.0:
			_transient_msg = ""
	_hud_cooldown -= delta
	if _hud_cooldown <= 0.0:
		_hud_cooldown = HUD_REFRESH_INTERVAL
		_update_hud()
	else:
		tick_label.text = str(world.get_tick())

func _set_transient(msg: String) -> void:
	_transient_msg = msg
	_transient_ttl = TRANSIENT_SECS
	status_label.text = msg

func _respawn_player() -> void:
	player_pos = CAVE_SPAWN
	_snap_camera()

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
	var c := _player_center_cell()
	for dy in range(-2, 3):
		for dx in range(-2, 3):
			var m: int = world.get_cell(c.x + dx, c.y + dy)
			if m != AshfallWorld.MATERIAL_AIR and m != AshfallWorld.MATERIAL_STONE:
				world.set_cell(c.x + dx, c.y + dy, AshfallWorld.MATERIAL_AIR)
	_update_hud()

func _place_at_player() -> void:
	if current_material == AshfallWorld.MATERIAL_AIR:
		return
	var c := _player_center_cell()
	for dy in range(-2, 3):
		for dx in range(-2, 3):
			if world.get_cell(c.x + dx, c.y + dy) == AshfallWorld.MATERIAL_AIR:
				world.set_cell(c.x + dx, c.y + dy, current_material)
	_update_hud()

func _build_palette() -> void:
	for child in palette_box.get_children():
		child.queue_free()
	palette_ids.clear()
	var ids: PackedInt32Array = world.get_material_ids()
	for i in ids.size():
		var id: int = ids[i]
		palette_ids.append(id)
		var btn := Button.new()
		var label: String = "[%d] %s" % [i + 1, world.get_material_name(id).capitalize()]
		if id == AshfallWorld.MATERIAL_AIR:
			label += " (Eraser)"
		btn.text = label
		btn.name = "PaletteBtn%d" % id
		btn.pressed.connect(_on_palette_btn_pressed.bind(id))
		palette_box.add_child(btn)

func _select_palette(index: int) -> void:
	if index >= 0 and index < palette_ids.size():
		current_material = palette_ids[index]
		_update_hud()

func _on_palette_btn_pressed(id: int) -> void:
	current_material = id
	_update_hud()

func _save_game() -> void:
	var sim := world.save_to_bytes()
	var mat_name: String = world.get_material_name(current_material)
	var f := FileAccess.open(SAVE_TMP_PATH, FileAccess.WRITE)
	if f == null:
		_set_transient("SAVE FAILED")
		return
	f.store_8(HOST_SAVE_VERSION)
	f.store_float(player_pos.x)
	f.store_float(player_pos.y)
	f.store_pascal_string(mat_name)
	f.store_32(sim.size())
	f.store_buffer(sim)
	f.close()
	if DirAccess.rename_absolute(SAVE_TMP_PATH, SAVE_PATH) != OK:
		_set_transient("SAVE FAILED")
		return
	_set_transient("SAVED")
	_update_hud()

func _load_game() -> void:
	if not FileAccess.file_exists(SAVE_PATH):
		_set_transient("NO SAVE")
		return
	var f := FileAccess.open(SAVE_PATH, FileAccess.READ)
	if f == null or f.get_length() < HOST_SAVE_MIN_BYTES:
		_set_transient("SAVE CORRUPT")
		return
	if f.get_8() != HOST_SAVE_VERSION:
		_set_transient("SAVE VERSION?")
		return
	var px: float = f.get_float()
	var py: float = f.get_float()
	var mat_name: String = f.get_pascal_string()
	var sim_len: int = int(f.get_32())
	var world_w: int = world.get_world_width()
	var world_h: int = world.get_world_height()
	var meta_ok: bool = is_finite(px) and is_finite(py)
	meta_ok = meta_ok and px >= 0.0 and px <= float(world_w)
	meta_ok = meta_ok and py >= 0.0 and py <= float(world_h)
	meta_ok = meta_ok and not mat_name.is_empty()
	meta_ok = meta_ok and sim_len >= 0 and sim_len <= HOST_SAVE_SIM_MAX
	meta_ok = meta_ok and sim_len <= int(f.get_length() - f.get_position())
	if not meta_ok:
		_set_transient("SAVE CORRUPT")
		return
	var sim := f.get_buffer(sim_len)
	if sim.size() != sim_len or f.get_position() != f.get_length():
		_set_transient("SAVE CORRUPT")
		return
	f.close()
	if not world.load_from_bytes(sim):
		_set_transient("LOAD FAILED")
		return
	player_pos = Vector2(px, py)
	current_material = _material_id_for_name(mat_name)
	_snap_camera()
	_update_marker()
	_update_hud()
	if _pending_material_notice.is_empty():
		_set_transient("LOADED")
	else:
		_set_transient(_pending_material_notice)
		_pending_material_notice = ""

var _pending_material_notice: String = ""

func _material_id_for_name(mat_name: String) -> int:
	var ids: PackedInt32Array = world.get_material_ids()
	for id in ids:
		if world.get_material_name(id) == mat_name:
			return id
	var first: int = ids[0] if ids.size() > 0 else AshfallWorld.MATERIAL_AIR
	_pending_material_notice = "MATERIAL '%s' ? -> %s" % [mat_name, world.get_material_name(first).capitalize()]
	return first

func _open_demo() -> void:
	# The material demo stays bootable one key/click away (owner decision);
	# the boot-default switch itself belongs to the integrator (project.godot
	# run/main_scene). This direction needs no existing-file edits.
	if get_tree().change_scene_to_file("res://main.tscn") != OK:
		_set_transient("NO DEMO SCENE")

func _update_hud() -> void:
	tick_label.text = str(world.get_tick())
	cells_label.text = str(world.get_occupied_cell_count())
	if _transient_msg.is_empty():
		status_label.text = "RUNNING" if world.is_auto_step() else "PAUSED"
	else:
		status_label.text = _transient_msg
	fps_label.text = "%d fps%s" % [Engine.get_frames_per_second(),
		"" if DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED else " (uncapped)"]
	frame_label.text = "%.2f ms" % _frame_ms_ema
	sim_label.text = "%.2f ms" % world.get_last_step_ms()
	player_label.text = "(%d, %d)" % [roundi(player_pos.x), roundi(player_pos.y)]

func _world_size() -> Vector2i:
	return Vector2i(world.get_world_width(), world.get_world_height())

func _init_camera() -> void:
	# Same contract as main_controller._init_camera: cropped AtlasTexture
	# views over the live bridge textures; center-sample-only water shader
	# is crop-safe.
	_camera = FollowCamera.new()
	_base_atlas = AtlasTexture.new()
	_water_atlas = AtlasTexture.new()
	_refresh_textures()
	texture_display.texture = _base_atlas
	water_display.texture = _water_atlas
	_view_region = _camera.snap_to(player_pos, _world_size(), texture_display.size)
	_apply_view_region()

func _update_camera(delta: float) -> void:
	if _camera == null:
		return
	var region: Rect2i = _camera.update(delta, player_pos, _world_size(), texture_display.size)
	if region != _view_region:
		_view_region = region
		_apply_view_region()

func _snap_camera() -> void:
	if _camera == null:
		return
	_view_region = _camera.snap_to(player_pos, _world_size(), texture_display.size)
	_apply_view_region()

func _zoom_camera(dir: int) -> void:
	if _camera == null:
		return
	_view_region = _camera.zoom_step(dir, _world_size(), texture_display.size)
	_apply_view_region()

func _toggle_follow_camera() -> void:
	if _camera == null:
		return
	_view_region = _camera.set_follow(not _camera.follow_enabled, player_pos, _world_size(), texture_display.size)
	_apply_view_region()
	_set_transient("CAM: FOLLOW ON" if _camera.follow_enabled else "CAM: FOLLOW OFF")

func _apply_view_region() -> void:
	_base_atlas.region = Rect2(_view_region)
	_water_atlas.region = Rect2(_view_region)

func _refresh_textures() -> void:
	var base_tex := world.get_world_texture()
	if _base_atlas == null:
		texture_display.texture = base_tex
		water_display.texture = world.get_water_texture()
		return
	if _base_atlas.atlas != base_tex:
		_base_atlas.atlas = base_tex
	var water_tex := world.get_water_texture()
	if _water_atlas.atlas != water_tex:
		_water_atlas.atlas = water_tex

func _init_hud_separation() -> void:
	var sidebar: VBoxContainer = get_node_or_null("MainLayout/SidebarScroll/Sidebar")
	if sidebar == null:
		return
	_debug_panel = HudPanels.build_debug_panel(self, sidebar)
	var opener: Button = sidebar.get_node_or_null(NodePath(HudPanels.OPENER_NAME))
	if opener != null:
		opener.pressed.connect(_toggle_debug_panel)

func _toggle_debug_panel() -> void:
	HudPanels.toggle_debug_panel(_debug_panel)

func _display_scale() -> Vector2:
	return FollowCamera.view_scale(texture_display.size, _view_region)

func _display_offset(scale: Vector2) -> Vector2:
	return FollowCamera.view_offset(texture_display.size, _view_region, scale)

func _world_to_display(cell: Vector2) -> Vector2:
	return FollowCamera.world_to_screen(cell, texture_display.size, _view_region)

func _display_to_world(local_pos: Vector2) -> Vector2i:
	return FollowCamera.screen_to_world(local_pos, texture_display.size, _view_region, _world_size())

func _update_marker() -> void:
	var top_left := _world_to_display(Vector2(player_pos.x - PLAYER_HALF_W, player_pos.y - PLAYER_H))
	var bottom_right := _world_to_display(Vector2(player_pos.x + PLAYER_HALF_W, player_pos.y))
	player_marker.position = top_left
	player_marker.size = bottom_right - top_left
	if current_material == _marker_mat:
		return
	_marker_mat = current_material
	if current_material == AshfallWorld.MATERIAL_AIR:
		material_label.text = "Air (Eraser)"
	else:
		material_label.text = world.get_material_name(current_material).capitalize()

func _update_light() -> void:
	# Warm source tracks the fire pit in display space (same letterbox as
	# the world texture). C2 will replace this single light with clustered
	# lights; the anchor math stays.
	var center := _world_to_display(FIRE_ANCHOR)
	cave_light.position = center
	fire_glow.position = center - fire_glow.size * 0.5

func _on_focus_lost() -> void:
	is_drawing = false

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.is_pressed() and not event.is_echo():
		match event.keycode:
			KEY_SPACE:
				world.auto_step = not world.is_auto_step()
				_update_hud()
			KEY_N:
				world.step()
				_update_hud()
			KEY_R:
				_build_cave()
				_respawn_player()
				_update_marker()
				_update_hud()
			KEY_E:
				_dig_at_player()
			KEY_F:
				_place_at_player()
			KEY_C:
				_toggle_follow_camera()
			KEY_F1:
				_open_demo()
			KEY_F5:
				_save_game()
			KEY_F9:
				_load_game()
			KEY_V:
				if DisplayServer.window_get_vsync_mode() == DisplayServer.VSYNC_DISABLED:
					DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_ENABLED)
					_set_transient("VSYNC ON — render capped to display (V to uncap)")
				else:
					DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
					_set_transient("VSYNC OFF — render uncapped (V to re-cap)")
				_update_hud()
			KEY_1:
				_select_palette(0)
			KEY_2:
				_select_palette(1)
			KEY_3:
				_select_palette(2)
			KEY_4:
				_select_palette(3)
			KEY_5:
				_select_palette(4)
			KEY_6:
				_select_palette(5)
			KEY_7:
				_select_palette(6)
			KEY_8:
				_select_palette(7)
			KEY_9:
				_select_palette(8)
			KEY_0:
				_select_palette(9)

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
		elif event.button_index == MOUSE_BUTTON_WHEEL_UP and event.is_pressed():
			_zoom_camera(1)
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN and event.is_pressed():
			_zoom_camera(-1)
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
	_update_hud()

func _on_step_btn_pressed() -> void:
	world.step()
	_update_hud()

func _on_play_btn_pressed() -> void:
	world.auto_step = not world.is_auto_step()
	_update_hud()

func _on_reset_btn_pressed() -> void:
	_build_cave()
	_respawn_player()
	_update_marker()
	_update_hud()

func _on_save_btn_pressed() -> void:
	_save_game()

func _on_load_btn_pressed() -> void:
	_load_game()

func _on_demo_btn_pressed() -> void:
	_open_demo()

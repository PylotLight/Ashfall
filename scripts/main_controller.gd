extends Control

# Ashfall Host Controller
# Presentation and input forwarding only.
# No simulation rules or world state are implemented here.
#
# Controls: WASD/arrows = move avatar (S keeps move-down); N = single sim
# tick (rebound from S in Group 3 — S both moved down and stepped, so the
# step key moved to the unused N); SPACE = play/pause; R = reset level;
# E = dig at avatar; F = place held material; C = follow-camera on/off;
# mouse wheel over the world view = camera zoom; F1 = debug panel (the
# sim-control buttons live there, hidden from the play view); F5 = save;
# F9 = load; 1..0 = palette shortcuts in table order; mouse paint on the
# world view.
#
# Presentation cadence (Group 3, Task B4): sim/edit paths only accumulate
# engine dirtiness; _process calls world.flush_present() exactly once per
# rendered frame and rebinds both texture refs (rebind covers bridge-side
# presentation recovery, which replaces texture objects).

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WorldTexture
@onready var water_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WaterOverlay
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

# P3A Slice A, Task A2: follow camera + HUD separation live in NEW script
# files (region lock); this controller only wires them. No class_name on
# those scripts (parallel A1 must not collide); preload consts instead.
const FollowCamera = preload("res://scripts/follow_camera.gd")
const HudPanels = preload("res://scripts/hud_panels.gd")
const ProfileGlue = preload("res://scripts/material_profile_glue.gd")

# Camera state: cropped AtlasTexture views over the live bridge textures
# (TextureRect stack stays for P3A — no world-space refactor). The view
# region is the single source of truth for display mapping below.
var _camera = null
var _base_atlas: AtlasTexture
var _water_atlas: AtlasTexture
var _view_region: Rect2i = Rect2i(0, 0, 0, 0)
# Debug panel (sim-control buttons, hidden from the play view).
var _debug_panel: PopupPanel

var current_material: int = AshfallWorld.MATERIAL_SAND
var brush_size: int = 4
var is_drawing: bool = false
var draw_material: int = AshfallWorld.MATERIAL_SAND
var palette_ids: Array[int] = []
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
const SAVE_TMP_PATH: String = "user://ashfall_save.bin.tmp"
# Host save envelope v2 (Group 3, Task B6): version u8=2 | px float | py
# float | material NAME (pascal string) | sim_len u32 | sim bytes. The v1
# header was exactly 14 bytes (1+8+1+4); v2's minimum is 18 (1+8+5+4 with a
# 1-char name). The material travels by stable NAME so table-id remaps (core
# v3 roster) cannot misroute it; unknown names fall back to the first table
# material with a transient message. Writes are atomic (tmp + rename).
const HOST_SAVE_VERSION: int = 2
const HOST_SAVE_MIN_BYTES: int = 18
const HOST_SAVE_SIM_MAX: int = 64 * 1024 * 1024
# Transient status line (Group 3, Task B6/B7): save/load/material notices
# live here instead of being overwritten by the next throttled HUD refresh.
var _transient_msg: String = ""
var _transient_ttl: float = 0.0
const TRANSIENT_SECS: float = 3.0
# Marker label cache (Group 3, Task B7): material lookup + label assignment
# run only when the held material changes, not every frame.
var _marker_mat: int = -1

func _ready() -> void:
	# Release-outside safety: gui_input only fires inside the control, so a
	# release outside would otherwise leave is_drawing stuck on. The _process
	# poll below is the backstop; focus loss clears immediately.
	get_window().focus_exited.connect(_on_focus_lost)
	# Material table comes from versioned data (P2); defaults match, so this
	# only proves the production file-to-core path every boot.
	var data_file := FileAccess.open("res://data/materials.json", FileAccess.READ)
	if data_file != null:
		if not world.load_materials(data_file.get_as_text()):
			push_error("main_controller: data/materials.json rejected, using built-in defaults")
		data_file.close()
	else:
		push_error("main_controller: data/materials.json missing, using built-in defaults")
	# Reaction rules resolve against the loaded table (P2.5); a missing or
	# invalid file only disables reactions, never blocks boot.
	var rx_file := FileAccess.open("res://data/reactions.json", FileAccess.READ)
	if rx_file != null:
		if not world.load_reactions(rx_file.get_as_text()):
			push_error("main_controller: data/reactions.json rejected, reactions disabled")
		rx_file.close()
	else:
		push_error("main_controller: data/reactions.json missing, reactions disabled")
	# Boot the hand-authored P1 play level in C++ (deterministic content).
	world.setup_play_level()
	world.auto_step = false
	world.set_sim_rate_hz(60.0)
	world.set_max_steps_per_physics_frame(2)
	_respawn_player()
	texture_display.texture = world.get_world_texture()
	water_display.texture = world.get_water_texture()
	_build_palette()
	_update_hud()
	_init_camera()
	_init_profile_material()
	_update_marker()
	_refresh_group3_labels()
	_init_hud_separation()

func _process(delta: float) -> void:
	# Frame-time EMA for the perf HUD (cheap, every frame).
	if delta > 0.0:
		_frame_ms_ema = lerpf(_frame_ms_ema, delta * 1000.0, 0.08)
	_move_player(delta)
	_update_camera(delta)
	_update_marker()
	# Group 3, Task B7: drawing state tracks the physical buttons, so a
	# release outside the drawing control still ends the stroke.
	if is_drawing and not (Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		is_drawing = false
	# Group 3, Task B4: exactly one present flush per rendered frame, then
	# rebind both layers through the camera atlases (rebind covers
	# bridge-side presentation recovery, which replaces texture objects).
	world.flush_present()
	_refresh_textures()
	if _profile_mat != null:
		ProfileGlue.update_present(_profile_mat, world, _view_region.position, _view_region.size)
	if _transient_ttl > 0.0:
		_transient_ttl -= delta
		if _transient_ttl <= 0.0:
			_transient_msg = ""
	# Throttled HUD: occupied-cell query is O(1) but label churn every frame
	# is still wasteful. Refresh at 4Hz.
	_hud_cooldown -= delta
	if _hud_cooldown <= 0.0:
		_hud_cooldown = HUD_REFRESH_INTERVAL
		_update_hud()
	else:
		# Cheap tick-only refresh keeps the counter live without cell query.
		tick_label.text = str(world.get_tick())

func _set_transient(msg: String) -> void:
	_transient_msg = msg
	_transient_ttl = TRANSIENT_SECS
	status_label.text = msg

func _respawn_player() -> void:
	var spawn: Vector2i = world.get_player_spawn()
	player_pos = Vector2(float(spawn.x), float(spawn.y))
	_snap_camera()

func _world_size() -> Vector2i:
	return Vector2i(world.get_world_width(), world.get_world_height())

func _init_camera() -> void:
	# P3A A2: the TextureRects show cropped AtlasTexture views over the
	# live bridge textures. The center-sample-only water shader (no
	# neighborhood reads) is crop-safe; plain texture() keeps working
	# because AtlasTexture remaps UVs to the region.
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
	# Player teleports (spawn/reset/load): jump-cut, never smooth-pan.
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
	# AtlasTexture.region is Rect2; the Rect2i stays the source of truth.
	_base_atlas.region = Rect2(_view_region)
	_water_atlas.region = Rect2(_view_region)

# P3A R7: the DISPLAYED base layer is the identity texture under the profile
# ShaderMaterial (see _init_profile_material). The CPU RGBA layer keeps
# painting every flush as the static reference (fixtures depend on it) but is
# no longer shown. Water overlay is untouched.
var _profiles: Dictionary = {}
var _profile_mat: ShaderMaterial


func _init_profile_material() -> void:
	var res: Dictionary = ProfileGlue.load_profiles(world)
	if not bool(res["ok"]):
		push_warning("main_controller: material_visuals rejected (%s), profile LUTs are safe defaults" % String(res["error"]))
	_profiles = res["profiles"]
	_profile_mat = ProfileGlue.make_material(world, _profiles, _view_region.position, _view_region.size)
	texture_display.material = _profile_mat
	_refresh_textures()


func _refresh_textures() -> void:
	# Rebind through the atlases so bridge recovery (replaced texture
	# objects) keeps working; skip no-op assignments.
	var base_tex := world.get_identity_texture()
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
	# P3A A2: sim-control buttons leave the play sidebar for a hidden
	# debug panel. Play HUD (InfoBox), palette, title, and help stay.
	var sidebar: VBoxContainer = get_node_or_null("MainLayout/SidebarScroll/Sidebar")
	if sidebar == null:
		return
	_debug_panel = HudPanels.build_debug_panel(self, sidebar)
	var opener: Button = sidebar.get_node_or_null(NodePath(HudPanels.OPENER_NAME))
	if opener != null:
		opener.pressed.connect(_toggle_debug_panel)

func _toggle_debug_panel() -> void:
	HudPanels.toggle_debug_panel(_debug_panel)

func _refresh_group3_labels() -> void:
	# Group-3 leftover that bites (fixed here in code — A2 region lock
	# forbids scene edits): the step key moved S -> N, but the button label
	# and help text still said S. Deliberately LEFT ALONE (verified, no
	# bite): S move-down vs N single-step dispatch, outside-image click
	# rejection, focus-loss drawing reset, marker throttle — all still
	# correct, now camera-aware where mapping is involved.
	var sidebar: Node = get_node_or_null("MainLayout/SidebarScroll/Sidebar")
	if sidebar == null:
		return
	var step_btn: Button = sidebar.get_node_or_null("StepBtn")
	if step_btn != null:
		step_btn.text = "[N] Single Step"
	var help_label: Label = sidebar.get_node_or_null("HelpText")
	if help_label != null:
		help_label.text = "Controls:\n• WASD / Arrows: Move avatar (stone blocks)\n• Left Click/Drag: Paint material\n• Right Click: Erase cells\n• Wheel over world: Zoom camera\n• E: Dig sand/water at avatar\n• F: Place material at avatar\n• Space: Toggle simulation\n• N: Step single tick\n• C: Follow camera on/off\n• F1: Debug panel (sim controls)\n• R: Reset level\n• F5 / F9: Save / Load"

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
	# Group 3, Task B4: no direct present here — the per-frame flush in
	# _process presents the accumulated dirtiness.
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
	# Group 3, Task B4: no direct present here — the per-frame flush in
	# _process presents the accumulated dirtiness.
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
	f.store_8(HOST_SAVE_VERSION) # host save format version (v2: name, not id)
	f.store_float(player_pos.x)
	f.store_float(player_pos.y)
	f.store_pascal_string(mat_name)
	f.store_32(sim.size())
	f.store_buffer(sim)
	f.close()
	# Atomic write: the previous good save survives a crash mid-write.
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
	# Parse + validate ALL host metadata BEFORE the engine commit below.
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
	# Domain for player coords (Group 3, Task B6): the world rect.
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
		# Short read, or trailing bytes (strict envelope: reject).
		_set_transient("SAVE CORRUPT")
		return
	f.close()
	if not world.load_from_bytes(sim):
		_set_transient("LOAD FAILED")
		return
	player_pos = Vector2(px, py)
	_snap_camera()
	# Stable-name remap: unknown names fall back to the first table material
	# with a transient notice (UX fallback, not sim integrity).
	current_material = _material_id_for_name(mat_name)
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

func _update_hud() -> void:
	tick_label.text = str(world.get_tick())
	cells_label.text = str(world.get_occupied_cell_count())
	# A live transient notice owns the status line until it expires.
	if _transient_msg.is_empty():
		status_label.text = "RUNNING" if world.is_auto_step() else "PAUSED"
	else:
		status_label.text = _transient_msg
	fps_label.text = "%d fps%s" % [Engine.get_frames_per_second(),
		"" if DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED else " (uncapped)"]
	frame_label.text = "%.2f ms" % _frame_ms_ema
	sim_label.text = _sim_split_text()
	sim_label.tooltip_text = _sim_split_tooltip()
	player_label.text = "(%d, %d)" % [roundi(player_pos.x), roundi(player_pos.y)]

# E1 split-timer display (§10 CPU split; display only, no sim here).
# Stepping model: fixed 60Hz physics accumulator (max 2 steps per physics
# frame, backlog dropped — never spiral); exactly one flush_present() per
# rendered frame. sim ms = last step() span ONLY, directly comparable to
# bench_step/bench_e1 ms/step; present cost is separate (see tooltip).
func _sim_split_text() -> String:
	var ms: float = world.get_last_step_ms()
	var ph: Dictionary = world.get_sim_phases()
	if ph.is_empty():
		return "%.2f ms" % ms
	var react: float = float(ph.get("reactions_ms", 0.0))
	var move: float = float(ph.get("powder_ms", 0.0)) + float(ph.get("liquid_ms", 0.0)) + float(ph.get("gas_ms", 0.0))
	var temp: float = float(ph.get("heat_carry_ms", 0.0)) + float(ph.get("conduct_ms", 0.0))
	var maint: float = float(ph.get("clear_reserve_ms", 0.0)) + float(ph.get("classify_ms", 0.0)) + float(ph.get("ordering_ms", 0.0)) + float(ph.get("reconcile_ms", 0.0))
	return "%.2f ms (R %.2f M %.2f T %.2f A %.2f)" % [ms, react, move, temp, maint]

func _sim_split_tooltip() -> String:
	var tip: String = "stepping: fixed 60Hz physics accumulator, max 2 steps/physics frame, backlog dropped; 1 present flush per rendered frame; sim = last step() span (bench-comparable)"
	var ph: Dictionary = world.get_sim_phases()
	if not ph.is_empty():
		tip += "\nsim splits ms: react %.3f clear %.3f classify %.3f order %.3f powder %.3f liquid %.3f gas %.3f reconcile %.3f heatcarry %.3f conduct %.3f" % [float(ph.get("reactions_ms", 0.0)), float(ph.get("clear_reserve_ms", 0.0)), float(ph.get("classify_ms", 0.0)), float(ph.get("ordering_ms", 0.0)), float(ph.get("powder_ms", 0.0)), float(ph.get("liquid_ms", 0.0)), float(ph.get("gas_ms", 0.0)), float(ph.get("reconcile_ms", 0.0)), float(ph.get("heat_carry_ms", 0.0)), float(ph.get("conduct_ms", 0.0))]
	tip += "\npresent splits ms: dirty %.3f read %.3f paint %.3f setdata %.3f submit %.3f (submit = CPU ImageTexture::update call, not GPU execution)" % [world.get_present_dirty_collect_ms(), world.get_present_material_read_ms(), world.get_present_paint_ms(), world.get_present_set_data_ms(), world.get_present_submit_ms()]
	return tip

func _display_scale() -> Vector2:
	# P3A A2: KEEP_ASPECT_CENTERED letterbox for the CURRENT camera view
	# (was: whole world). Follow-off shows the full grid, pixel-identical
	# to the old fit math.
	return FollowCamera.view_scale(texture_display.size, _view_region)

func _display_offset(scale: Vector2) -> Vector2:
	return FollowCamera.view_offset(texture_display.size, _view_region, scale)

func _world_to_display(cell: Vector2) -> Vector2:
	return FollowCamera.world_to_screen(cell, texture_display.size, _view_region)

func _display_to_world(local_pos: Vector2) -> Vector2i:
	return FollowCamera.screen_to_world(local_pos, texture_display.size, _view_region, _world_size())

func _update_marker() -> void:
	# Avatar box in display space (same letterbox as the world texture).
	# Position is cheap and follows every frame; the material lookup + label
	# assignment run only when the held material changes (Group 3, Task B7).
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

func _on_focus_lost() -> void:
	# Group 3, Task B7: alt-tab (or any window focus loss) mid-stroke must
	# not leave a stuck brush painting on return.
	is_drawing = false

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.is_pressed() and not event.is_echo():
		match event.keycode:
			KEY_SPACE:
				world.auto_step = not world.is_auto_step()
				_update_hud()
			# Group 3, Task B7: S keeps move-down (see _move_player); the
			# single-step key is N (unused elsewhere in the input map).
			KEY_N:
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
			KEY_C:
				_toggle_follow_camera()
			KEY_F1:
				_toggle_debug_panel()
			KEY_F5:
				_save_game()
			KEY_F9:
				_load_game()
			KEY_V:
				# Perf probe: the 120 figure is display vsync (ProMotion), not a
				# setting. Uncap render fps to expose true frame overhead and
				# future-load headroom. Sim stepping is physics-driven (max 2
				# steps per physics frame), so render rate never changes sim
				# speed; _process still flushes exactly once per rendered frame.
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
	# Group 3, Task B4: no direct present here — the per-frame flush in
	# _process presents the accumulated dirtiness.
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

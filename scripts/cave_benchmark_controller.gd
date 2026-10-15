extends Control

# Ashfall Cave Benchmark Controller (P3A Slice A1, task A1+A3).
# Presentation and input forwarding only. No simulation rules here.
#
# The cave is authored procedurally at boot through the public AshfallWorld
# API (init_world/fill_rect/set_cell/step) — no engine, bridge, or data
# changes. Content (R8 Noita-style): topsoil band, jittered chambers linked
# by winding tunnels + vertical shafts, stone rind on rock/air borders,
# stalactites/stalagmites, sand + lava veins sealed in rock, walled
# reservoir + sealed lake + lipped pool, wood platform structure, oil + wood
# combustible area with a fresh fire bed, warm light pool over the fire pit
# (textured PointLight2D budget-6 cluster rig + additive glow decal),
# character-sized player placeholder, stable pixel scaling (nearest world,
# letterboxed).
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
# F1 = debug panel; F2 = material demo; F5 = save; F9 = load;
# 1..0 = palette shortcuts; mouse paint on the world view.

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WorldTexture
@onready var water_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WaterOverlay
@onready var cave_light: PointLight2D = $MainLayout/ViewportContainer/WorldStack/CaveLight
@onready var cluster_light_1: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight1
@onready var cluster_light_2: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight2
@onready var cluster_light_3: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight3
@onready var cluster_light_4: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight4
@onready var cluster_light_5: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight5
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
# Keep the avatar in the lower third so the spawn-room ceiling and route ahead
# enter the first frame; this changes framing only, never simulation state.
const CAMERA_LOOK_AHEAD_CELLS: float = -30.0
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
const ClusterLights = preload("res://scripts/light_cluster_manager.gd")
const MatVisuals = preload("res://scripts/material_visuals.gd")
const ProfileGlue = preload("res://scripts/material_profile_glue.gd")
var _camera = null
var _cluster_mgr = null
var _emissive: Dictionary = {}
var _light_nodes: Array = []
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
	_init_profile_material()
	_init_cluster_lights()
	_update_marker()
	_init_hud_separation()

# Deterministic 2D hash in [0, 1): integer math only, no RNG state, so the
# cave is identical every boot. Salt decorrelates independent features.
func _hash2(x: int, y: int, salt: int) -> float:
	var h: int = ((x * 0x8DA6B343) ^ (y * 0xD8163841) ^ (salt * 0x9E3779B1)) & 0xFFFFFFFF
	h = (h ^ (h >> 13)) & 0xFFFFFFFF
	h = (h ^ (h >> 7)) & 0xFFFFFFFF
	return float(h % 1000) / 1000.0

## Smooth value noise for the chamber silhouette; unlike a per-row offset,
## adjacent rows stay coherent and the cave stops reading as horizontal bands.
func _noise1d(value: float, salt: int) -> float:
	var i := floori(value)
	var t := value - float(i)
	t = t * t * (3.0 - 2.0 * t)
	return lerpf(_hash2(i, 0, salt), _hash2(i + 1, 0, salt), t)

func _mat_id(mat_name: String, fallback: int) -> int:
	for id in world.get_material_ids():
		if world.get_material_name(id) == mat_name:
			return id
	return fallback

func _build_cave() -> void:
	# Noita-style authoring (R8): a mostly-solid rock mass holding topsoil,
	# jittered chambers, winding connecting tunnels + vertical shafts, a
	# stone rind pass, ceiling/floor teeth, ore-style veins sealed in rock
	# (sand lenses read gold-ish, lava lenses glow), a walled reservoir plus
	# a sealed lake and a lipped chamber pool, and the preserved play
	# structures (spawn platform, fire pit, fuel, dune). Deterministic:
	# _hash2 only, no randf. Spawn (CAVE_SPAWN, feet on the beam top y=330
	# inside the spawn hall) and FIRE_ANCHOR are unchanged.
	var AIR: int = AshfallWorld.MATERIAL_AIR
	var STONE: int = AshfallWorld.MATERIAL_STONE
	var SAND: int = AshfallWorld.MATERIAL_SAND
	var WATER: int = AshfallWorld.MATERIAL_WATER
	var FIRE: int = AshfallWorld.MATERIAL_FIRE
	var WOOD: int = AshfallWorld.MATERIAL_WOOD
	var ROCK: int = AshfallWorld.MATERIAL_ROCK
	var LAVA: int = AshfallWorld.MATERIAL_LAVA
	var OIL: int = _mat_id("oil", 9)
	world.init_world(512, 512)
	world.fill_rect(0, 0, 511, 511, ROCK)
	_build_topsoil(SAND, STONE, WOOD, ROCK)
	# Chambers (cx, cy, rx, ry, salt): spawn hall first (must contain the
	# compact bridge x194..286 @ y330..334 and the avatar box y328..329).
	_carve_chamber(240.0, 298.0, 82.0, 58.0, 11, 0.35)
	_carve_chamber(105.0, 250.0, 55.0, 38.0, 21, 0.30)
	_carve_chamber(390.0, 255.0, 68.0, 44.0, 31, 0.30)
	_carve_chamber(335.0, 118.0, 48.0, 24.0, 41, 0.30)
	_carve_chamber(135.0, 398.0, 58.0, 34.0, 51, 0.30)
	_carve_chamber(395.0, 395.0, 62.0, 38.0, 61, 0.30)
	_carve_chamber(256.0, 415.0, 45.0, 25.0, 71, 0.30)
	# Sealed lake pocket (no tunnels touch it): water stays put.
	_carve_chamber(70.0, 175.0, 30.0, 18.0, 81, 0.25)
	# Chimney vent to the surface (smoke/ember path), jittered walls.
	for y in range(10, 131):
		var j: float = (_hash2(256, y, 91) - 0.5) * 6.0
		var x0: int = clampi(int(246.0 + j), 1, 510)
		var x1: int = clampi(int(266.0 + j), 1, 510)
		world.fill_rect(x0, y, x1, y, AIR)
	world.fill_rect(240, 10, 245, 130, STONE)
	world.fill_rect(267, 10, 272, 130, STONE)
	# Tunnels/shafts (winding walks, overlapping discs => connected).
	_carve_tunnel(256, 130, 300, 118, 3, 101) # chimney -> surface pocket
	_carve_tunnel(360, 130, 390, 215, 3, 102) # surface pocket -> east hall
	_carve_tunnel(100, 288, 120, 364, 3, 103) # west hall -> deep west shaft
	_carve_tunnel(256, 356, 256, 392, 3, 104) # spawn hall -> deep central shaft
	_carve_tunnel(300, 415, 335, 405, 3, 105) # deep central -> deep east
	_carve_tunnel(330, 350, 350, 370, 3, 106) # crawl space -> deep east
	# Explicit reservoir cut (merged with the deep-east chamber).
	world.fill_rect(330, 370, 405, 447, AIR)
	# Fire nook under the deep-central chamber (merges via overlap).
	world.fill_rect(238, 438, 274, 447, AIR)
	_build_stone_rind(STONE, ROCK, AIR)
	_build_teeth(STONE, AIR)
	_build_veins(SAND, LAVA, ROCK)
	_build_structures(STONE, WOOD, OIL, SAND, AIR)
	# Water: walled reservoir + sealed lake + lipped east-hall pool.
	world.fill_rect(324, 370, 329, 447, STONE)
	world.fill_rect(406, 370, 411, 447, STONE)
	world.fill_rect(330, 410, 405, 447, WATER)
	world.fill_rect(42, 182, 98, 190, WATER)
	world.fill_rect(368, 290, 370, 300, STONE)
	world.fill_rect(400, 290, 402, 300, STONE)
	world.fill_rect(371, 294, 399, 298, WATER)
	# Settle liquids/powders (fixed count => deterministic) BEFORE placing
	# fire, so boot shows fresh flames instead of pre-burned fuel.
	for i in 60:
		world.step()
	# Starting-room landmarks: a small wick and a shallow seep, visible in
	# the opening camera crop and framed by the two broken stone shelves.
	world.fill_rect(176, 294, 177, 297, WOOD)
	world.fill_rect(176, 291, 177, 293, FIRE)
	world.fill_rect(305, 284, 312, 287, WATER)
	# Fire bed with stone curb; the warm light anchor sits just above it.
	world.fill_rect(238, 444, 243, 447, STONE)
	world.fill_rect(269, 444, 274, 447, STONE)
	world.fill_rect(244, 442, 268, 447, FIRE)
	world.flush_present()


## Topsoil: ragged sand (dirt) band with stone speckle + wood root flecks.
func _build_topsoil(sand_mat: int, stone_mat: int, wood_mat: int, rock_mat: int) -> void:
	for y in range(4, 70):
		var edge: float = 58.0 + (_hash2(7, y, 11) - 0.5) * 12.0
		if float(y) < edge:
			world.fill_rect(1, y, 510, y, sand_mat)
	for y in range(4, 66):
		for x in range(1, 511, 3):
			var h: float = _hash2(x, y, 12)
			if h < 0.05 and world.get_cell(x, y) == sand_mat:
				world.set_cell(x, y, stone_mat)
			elif h > 0.985 and world.get_cell(x, y) == sand_mat:
				world.set_cell(x, y, rock_mat)
	for x in range(10, 505, 11):
		if _hash2(x, 3, 13) < 0.55:
			var y0: int = 10 + int(_hash2(x, 9, 14) * 30.0)
			var ln: int = 3 + int(_hash2(x, 17, 15) * 5.0)
			world.fill_rect(x, y0, x, mini(68, y0 + ln), wood_mat)


## Organic chamber carve: left and right edges wander independently at two
## scales, so the negative-space silhouette reads as eroded rock, not bands.
func _carve_chamber(cx: float, cy: float, rx: float, ry: float, salt: int, edge: float) -> void:
	var AIR: int = AshfallWorld.MATERIAL_AIR
	var y_lo: int = maxi(1, int(cy - ry) - 4)
	var y_hi: int = mini(510, int(cy + ry) + 4)
	for y in range(y_lo, y_hi + 1):
		var ny: float = (float(y) - cy) / ry
		var s: float = 1.0 - ny * ny
		if s <= 0.0:
			continue
		var half: float = rx * sqrt(s)
		var broad: float = minf(10.0, rx * edge * 0.12)
		var left_wander: float = (_noise1d(float(y) / 11.0, salt) - 0.5) * 2.0 * broad
		var right_wander: float = (_noise1d(float(y) / 13.0, salt + 1) - 0.5) * 2.0 * broad
		var left_chip: float = (_hash2(int(cx), y, salt + 2) - 0.5) * 3.0
		var right_chip: float = (_hash2(int(cx), y, salt + 3) - 0.5) * 3.0
		var x0: int = clampi(int(cx - half + left_wander + left_chip), 1, 510)
		var x1: int = clampi(int(cx + half + right_wander + right_chip), 1, 510)
		if x1 >= x0:
			world.fill_rect(x0, y, x1, y, AIR)


## Winding tunnel: overlapping rounded stamps along a gently wandering path.
## Rounded walls remove the square ledges left by the old rectangular stamps.
func _carve_tunnel(ax: int, ay: int, bx: int, by: int, rad: int, salt: int) -> void:
	var AIR: int = AshfallWorld.MATERIAL_AIR
	var dx: int = bx - ax
	var dy: int = by - ay
	var n: int = maxi(maxi(absi(dx), absi(dy)) * 2, 8)
	var px: float = -float(dy)
	var py: float = float(dx)
	var pl: float = sqrt(px * px + py * py)
	if pl > 0.001:
		px /= pl
		py /= pl
	for i in range(n + 1):
		var t: float = float(i) / float(n)
		var wob: float = (_hash2(i, salt, 77) - 0.5) * 10.0 * sin(t * 3.14159)
		var cx: int = clampi(int(lerpf(float(ax), float(bx), t) + px * wob), 2, 509)
		var cy: int = clampi(int(lerpf(float(ay), float(by), t) + py * wob), 2, 509)
		var stamp_r: float = maxf(2.5, float(rad) + (_hash2(i, salt, 78) - 0.5) * 1.0)
		for oy in range(-ceili(stamp_r), ceili(stamp_r) + 1):
			var ny: float = float(oy) / stamp_r
			if absf(ny) > 1.0:
				continue
			var span: int = int(floor(stamp_r * sqrt(1.0 - ny * ny)))
			world.fill_rect(maxi(1, cx - span), maxi(1, cy + oy), mini(510, cx + span), mini(510, cy + oy), AIR)


## Stone rind: rock touching air reads as worked stone (Noita wall tone).
func _build_stone_rind(stone_mat: int, rock_mat: int, air_mat: int) -> void:
	for y in range(1, 511):
		for x in range(1, 511):
			if world.get_cell(x, y) != rock_mat:
				continue
			if world.get_cell(x - 1, y) != air_mat and world.get_cell(x + 1, y) != air_mat \
					and world.get_cell(x, y - 1) != air_mat and world.get_cell(x, y + 1) != air_mat:
				continue
			if _hash2(x, y, 5) < 0.85:
				world.set_cell(x, y, stone_mat)


## Stalactites (ceilings) + stalagmites (floors) inside the big chambers.
func _build_teeth(stone_mat: int, air_mat: int) -> void:
	var boxes := [
		Rect2i(145, 240, 190, 116), Rect2i(50, 212, 110, 76),
		Rect2i(322, 211, 136, 88), Rect2i(77, 364, 116, 68),
		Rect2i(333, 357, 124, 76),
	]
	for b in boxes:
		var x: int = b.position.x + 3
		while x < b.end.x - 3:
			_place_tooth(x, b, stone_mat, air_mat, true)
			_place_tooth(x + 3, b, stone_mat, air_mat, false)
			x += 9 + int(_hash2(x, b.position.y, 201) * 7.0)


## One tooth: scan for the ceiling (down=true) or floor, hash-gated, capped.
func _place_tooth(x: int, b: Rect2i, stone_mat: int, air_mat: int, down: bool) -> void:
	if _hash2(x, b.position.y if down else b.end.y, 202) < 0.35:
		return
	if down:
		for y in range(b.position.y, b.end.y):
			if world.get_cell(x, y) == air_mat and world.get_cell(x, y - 1) != air_mat:
				var ln: int = mini(4 + int(_hash2(x, y, 203) * 8.0), 12)
				world.fill_rect(x - 1, y, x + 1, mini(b.end.y, y + ln * 2 / 3), stone_mat)
				world.fill_rect(x, y, x, mini(b.end.y, y + ln), stone_mat)
				return
	else:
		for y in range(b.end.y - 1, b.position.y - 1, -1):
			if world.get_cell(x, y) == air_mat and world.get_cell(x, y + 1) != air_mat:
				var ln2: int = mini(3 + int(_hash2(x, y, 204) * 6.0), 9)
				world.fill_rect(x - 1, maxi(b.position.y, y - ln2 * 2 / 3), x + 1, y, stone_mat)
				world.fill_rect(x, maxi(b.position.y, y - ln2), x, y, stone_mat)
				return


## Ore-style veins sealed in rock: sand lenses (gold-ish) + lava lenses.
## Writes land on ROCK only (lenses never breach air: per-cell enclosure).
func _build_veins(sand_mat: int, lava_mat: int, rock_mat: int) -> void:
	var AIR: int = AshfallWorld.MATERIAL_AIR
	for k in 44:
		var cx: int = 10 + int(_hash2(k, 1, 301) * 492.0)
		var cy: int = 90 + int(_hash2(k, 2, 302) * 351.0)
		var r: int = 2 + int(_hash2(k, 3, 303) * 4.0)
		for dy in range(-r, r + 1):
			for dx in range(-r, r + 1):
				if dx * dx + dy * dy > r * r:
					continue
				var x: int = cx + dx
				var y: int = cy + dy
				if x >= 1 and x <= 510 and y >= 1 and y <= 510 \
						and world.get_cell(x, y) == rock_mat:
					world.set_cell(x, y, sand_mat)
	for k in 8:
		var lx: int = 60 + int(_hash2(k, 4, 304) * 392.0)
		var ly: int = 380 + int(_hash2(k, 5, 305) * 62.0)
		var lr: int = 2 + int(_hash2(k, 6, 306) * 2.0)
		for dy2 in range(-lr, lr + 1):
			for dx2 in range(-lr, lr + 1):
				if dx2 * dx2 + dy2 * dy2 > lr * lr:
					continue
				var x2: int = lx + dx2
				var y2: int = ly + dy2
				if x2 < 1 or x2 > 510 or y2 < 1 or y2 > 510:
					continue
				if world.get_cell(x2, y2) != rock_mat:
					continue
				if world.get_cell(x2 - 1, y2) == AIR or world.get_cell(x2 + 1, y2) == AIR \
						or world.get_cell(x2, y2 - 1) == AIR or world.get_cell(x2, y2 + 1) == AIR:
					continue
				world.set_cell(x2, y2, lava_mat)


## Preserved structures: lower floor/fuel, plus a compact broken spawn bridge.
func _build_structures(stone_mat: int, wood_mat: int, oil_mat: int, sand_mat: int, _air: int) -> void:
	world.fill_rect(56, 448, 456, 470, stone_mat)
	world.fill_rect(194, 330, 222, 331, wood_mat)
	world.fill_rect(223, 330, 257, 331, stone_mat)
	world.fill_rect(258, 330, 286, 331, wood_mat)
	world.fill_rect(194, 332, 286, 334, stone_mat)
	world.fill_rect(202, 335, 205, 343, wood_mat)
	world.fill_rect(275, 335, 278, 343, wood_mat)
	world.fill_rect(169, 298, 189, 300, stone_mat)
	world.fill_rect(292, 288, 313, 290, stone_mat)
	world.fill_rect(120, 441, 185, 447, oil_mat)
	world.fill_rect(96, 425, 114, 447, wood_mat)
	world.fill_rect(120, 435, 140, 440, wood_mat)
	world.fill_rect(60, 442, 95, 447, sand_mat)

func _process(delta: float) -> void:
	if delta > 0.0:
		_frame_ms_ema = lerpf(_frame_ms_ema, delta * 1000.0, 0.08)
	_move_player(delta)
	_update_camera(delta)
	_update_marker()
	_update_light(delta)
	if is_drawing and not (Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) or Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT)):
		is_drawing = false
	world.flush_present()
	_refresh_textures()
	if _profile_mat != null:
		ProfileGlue.update_present(_profile_mat, world, _view_region.position, _view_region.size)
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

func _world_size() -> Vector2i:
	return Vector2i(world.get_world_width(), world.get_world_height())

func _init_camera() -> void:
	# Same contract as main_controller._init_camera: cropped AtlasTexture
	# views over the live bridge textures; center-sample-only water shader
	# is crop-safe.
	_camera = FollowCamera.new()
	# Tighter authored play framing: enough of the room to read its route, with
	# the player no longer disappearing into full-world benchmark scale.
	_camera.view_h_cells = 120.0
	_base_atlas = AtlasTexture.new()
	_water_atlas = AtlasTexture.new()
	_refresh_textures()
	texture_display.texture = _base_atlas
	water_display.texture = _water_atlas
	_view_region = _camera.snap_to(_camera_target(), _world_size(), texture_display.size)
	_apply_view_region()

func _update_camera(delta: float) -> void:
	if _camera == null:
		return
	var region: Rect2i = _camera.update(delta, _camera_target(), _world_size(), texture_display.size)
	if region != _view_region:
		_view_region = region
		_apply_view_region()

func _snap_camera() -> void:
	if _camera == null:
		return
	_view_region = _camera.snap_to(_camera_target(), _world_size(), texture_display.size)
	_apply_view_region()

func _camera_target() -> Vector2:
	return player_pos + Vector2(0.0, CAMERA_LOOK_AHEAD_CELLS)

func _zoom_camera(dir: int) -> void:
	if _camera == null:
		return
	_view_region = _camera.zoom_step(dir, _world_size(), texture_display.size)
	_apply_view_region()

func _toggle_follow_camera() -> void:
	if _camera == null:
		return
	_view_region = _camera.set_follow(not _camera.follow_enabled, _camera_target(), _world_size(), texture_display.size)
	_apply_view_region()
	_set_transient("CAM: FOLLOW ON" if _camera.follow_enabled else "CAM: FOLLOW OFF")

func _apply_view_region() -> void:
	_base_atlas.region = Rect2(_view_region)
	_water_atlas.region = Rect2(_view_region)

# P3A R7: the DISPLAYED base layer is the identity texture under the profile
# ShaderMaterial (see _init_profile_material). The CPU RGBA layer keeps
# painting every flush as the static reference (fixtures depend on it) but is
# no longer shown. Water overlay, lights, and bloom are untouched.
var _profiles: Dictionary = {}
var _profile_mat: ShaderMaterial


func _init_profile_material() -> void:
	var res: Dictionary = ProfileGlue.load_profiles(world)
	if not bool(res["ok"]):
		push_warning("cave_benchmark: material_visuals rejected (%s), profile LUTs are safe defaults" % String(res["error"]))
	_profiles = res["profiles"]
	_profile_mat = ProfileGlue.make_material(world, _profiles, _view_region.position, _view_region.size)
	texture_display.material = _profile_mat
	_refresh_textures()


func _refresh_textures() -> void:
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
	# The collider is deliberately small, but a game character cannot be a
	# sub-cell red sliver. Keep the crisp wizard glyph readable at play scale.
	var feet := _world_to_display(Vector2(player_pos.x, player_pos.y))
	var cell_px: float = maxf(_display_scale().x, 1.0)
	var sprite_size := Vector2(maxf(26.0, cell_px * 4.0), maxf(36.0, cell_px * 5.8))
	player_marker.size = sprite_size
	player_marker.position = feet - Vector2(sprite_size.x * 0.5, sprite_size.y * 0.94)
	if Input.is_physical_key_pressed(KEY_A) or Input.is_physical_key_pressed(KEY_LEFT):
		player_marker.call("set_facing_right", false)
	elif Input.is_physical_key_pressed(KEY_D) or Input.is_physical_key_pressed(KEY_RIGHT):
		player_marker.call("set_facing_right", true)
	if current_material == _marker_mat:
		return
	_marker_mat = current_material
	if current_material == AshfallWorld.MATERIAL_AIR:
		material_label.text = "Air (Eraser)"
	else:
		material_label.text = world.get_material_name(current_material).capitalize()

func _update_light(delta: float) -> void:
	# Warm pool over the fire pit: the glow decal holds the FIRE_ANCHOR
	# math every frame (that anchor stays), while the fixed-budget
	# PointLight2D rig (slot 0 seeded at the anchor) is eased by the
	# Stage-A cluster manager toward live emissive regions. Lights are
	# presentation-only — the sim never sees them.
	var center := _world_to_display(FIRE_ANCHOR)
	fire_glow.position = center - fire_glow.size * 0.5
	if _cluster_mgr == null or _light_nodes.is_empty():
		cave_light.position = center
		return
	var slots: Array = _cluster_mgr.update(world, delta)
	for i in _light_nodes.size():
		var lamp: PointLight2D = _light_nodes[i]
		if i < slots.size():
			var s: Dictionary = slots[i]
			lamp.position = _world_to_display(s["pos"] as Vector2)
			lamp.color = s["color"] as Color
			lamp.energy = float(s["energy"])
		else:
			lamp.energy = 0.0

## P3A Slice C, task C2: cluster input comes from the presentation-owned
## material_visuals.json emission fields (never sim temperature, never RGB
## recovery — hot water stays dark because its emission strength is 0).
## Rejected/missing data falls back to the built-in fire/lava emission so
## the warm pool survives bad data; boot never fails here.
func _init_cluster_lights() -> void:
	_light_nodes = [cave_light, cluster_light_1, cluster_light_2,
		cluster_light_3, cluster_light_4, cluster_light_5]
	var ids: PackedInt32Array = world.get_material_ids()
	var names: Array = []
	var id_by_name := {}
	for id in ids:
		var mat_name: String = world.get_material_name(id)
		names.append(mat_name)
		id_by_name[mat_name] = id
	var res: Dictionary = MatVisuals.load_from_file("res://data/material_visuals.json", names)
	_emissive.clear()
	var profiles: Dictionary = res["profiles"]
	for key in profiles.keys():
		if not id_by_name.has(key):
			continue
		var prof: Dictionary = profiles[key]
		var em: Dictionary = prof["emission"]
		if float(em["strength"]) > 0.0:
			_emissive[int(id_by_name[key])] = {
				"color": em["color"] as Color,
				"strength": float(em["strength"]),
			}
	if not bool(res["ok"]):
		push_warning("cave_benchmark: material_visuals rejected (%s)" % String(res["error"]))
	if _emissive.is_empty():
		_emissive[AshfallWorld.MATERIAL_FIRE] = {"color": Color(1.0, 0.48, 0.0), "strength": 2.5}
		_emissive[AshfallWorld.MATERIAL_LAVA] = {"color": Color(1.0, 0.27, 0.0), "strength": 1.5}
	_cluster_mgr = ClusterLights.new()
	_cluster_mgr.setup(_emissive, FIRE_ANCHOR, Color(1.0, 0.55, 0.25))

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
				_toggle_debug_panel()
			KEY_F2:
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

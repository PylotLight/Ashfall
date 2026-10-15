extends Control

# Ashfall first-room controller: procedural world authoring, host-side
# platform movement and wand input, plus the existing presentation pipeline.
# Native simulation rules remain in the portable C++ engine. The original
# fixed-seed room is built through AshfallWorld's public API; player, projectile,
# and tool commands advance on a fixed 60 Hz tick-indexed host-side boundary.
#
# Resolution ladder (P3A task A3, review 12.2) for this scene:
#   sim           512x512 cells (1 cell = 1 sim unit, authoritative)
#   world-render  512x512 px textures, 1 px per cell, NEAREST (texture_filter)
#   lighting      canvas-resolution Godot 2D light + glow decal (<= display)
#   display       1280x720 viewport, KEEP_ASPECT_CENTERED letterbox upscale
#   UI            same-canvas vector controls, resolution-independent, crisp
# World instruments read near pixel-art scale; the final world upscale is
# nearest for both the solid base and liquid overlay, preserving a unified
# crisp pixel grid at the camera crop.
# The cropped follow-camera mapping keeps the wizard, light and projectile
# effects aligned to authoritative cell coordinates.
#
# Controls: A/D or arrows = move; SPACE/W/UP = jump; mouse = aim;
# left-click = water wand; wheel = camera zoom; R = rebuild; F1 = debug;
# F2 = material demo; F5/F9 = save/load; F6/F7 = stop/replay; P = pause.

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WorldTexture
@onready var water_display: TextureRect = $MainLayout/ViewportContainer/WorldStack/WaterOverlay
@onready var cave_backdrop: ColorRect = $MainLayout/ViewportContainer/WorldStack/CaveBackdrop
@onready var cave_light: PointLight2D = $MainLayout/ViewportContainer/WorldStack/CaveLight
@onready var cluster_light_1: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight1
@onready var cluster_light_2: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight2
@onready var cluster_light_3: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight3
@onready var cluster_light_4: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight4
@onready var cluster_light_5: PointLight2D = $MainLayout/ViewportContainer/WorldStack/ClusterLight5
@onready var fire_glow: TextureRect = $MainLayout/ViewportContainer/WorldStack/FireGlow
@onready var player_marker: ColorRect = $MainLayout/ViewportContainer/WorldStack/PlayerMarker
@onready var gameplay_fx: Control = $MainLayout/ViewportContainer/WorldStack/GameplayFX
@onready var tool_readout: Label = $MainLayout/SidebarScroll/Sidebar/ToolReadout
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

# Avatar state uses cell-space feet coordinates. The collider and the rendered
# wizard now agree at roughly 3x5 cells instead of the old sub-cell placeholder.
var player_pos: Vector2 = Vector2(44.0, 80.0)
var player_velocity: Vector2 = Vector2.ZERO
var player_on_ground: bool = false
var jump_requested: bool = false
var _aim_direction: Vector2 = Vector2.RIGHT
var _aim_target: Vector2 = Vector2(100.0, 80.0)
var _projectiles: Array[Dictionary] = []
var _impact_effects: Array[Dictionary] = []
var _wand_cooldown: float = 0.0
var _gameplay_awake: bool = false
var _simulation_manually_paused: bool = false
enum ReplayMode { RECORDING, IDLE, PLAYBACK, PLAYBACK_FINISHED }
var _replay_mode: ReplayMode = ReplayMode.RECORDING
var _gameplay_tick: int = 0
var _recorded_commands: Array[Dictionary] = []
var _replay_commands: Array[Dictionary] = []
var _replay_index: int = 0
var _pause_toggle_requested: bool = false
var _single_step_requested: bool = false
var _wand_fire_requested: bool = false
var _wand_fire_target: Vector2 = Vector2(100.0, 80.0)
const PLAYER_HALF_W: float = 1.45
const PLAYER_H: float = 5.2
const PLAYER_SPEED: float = 64.0
const PLAYER_GRAVITY: float = 210.0
const PLAYER_JUMP_SPEED: float = 94.0
const PLAYER_TERMINAL_SPEED: float = 150.0
const PLAYER_STEP_HEIGHT: int = 2
const GAMEPLAY_TICK_RATE: int = 60
const GAMEPLAY_DT: float = 1.0 / float(GAMEPLAY_TICK_RATE)
const REPLAY_MAX_TICKS: int = GAMEPLAY_TICK_RATE * 60 * 10
const CAVE_SPAWN: Vector2 = Vector2(44.0, 80.0)
const SURFACE_LEVEL: float = 80.0
const EXIT_TRIGGER: Rect2 = Rect2(477.0, 332.0, 23.0, 30.0)
const EXIT_MARKER: Vector2 = Vector2(488.0, 351.0)
const WAND_SPEED: float = 150.0
const WAND_COOLDOWN: float = 0.28
const WAND_MAX_AGE: float = 1.6
# Frame the sky, the first slope and the entrance together at spawn.
const CAMERA_LOOK_AHEAD_CELLS: float = -20.0
# Warm light sits over the spring-fed timber choke deeper in the room.
const FIRE_ANCHOR: Vector2 = Vector2(378.0, 293.0)
const SAVE_PATH: String = "user://ashfall_save.bin"
const SAVE_TMP_PATH: String = "user://ashfall_save.bin.tmp"
## Host save envelope v3 adds movement, aim, run, command-pending, and transient FX state.
## Version 2 remains readable with deterministic defaults for fields it did not own.
const HOST_SAVE_VERSION: int = 3
const HOST_SAVE_MIN_BYTES: int = 18
const HOST_SAVE_SIM_MAX: int = 64 * 1024 * 1024
const HOST_SAVE_MAX_PROJECTILES: int = 8
const HOST_SAVE_MAX_IMPACTS: int = 256
var _transient_msg: String = ""
var _transient_ttl: float = 0.0
const TRANSIENT_SECS: float = 3.0
var _marker_mat: int = -1
var _run_finished: bool = false
var _finish_overlay: Control

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
	_create_finish_overlay()
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
	_update_tool_readout()

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
	# Fixed-seed surface geology: a broken ridge of ochre soil over cool
	# stratified rock, opening into a slanted mouth, one roomy landmark chamber,
	# a small optional spring pocket, then a lower eastern route. The silhouette
	# is original to Ashfall; all variation comes from the integer hash/noise.
	var AIR: int = AshfallWorld.MATERIAL_AIR
	var STONE: int = AshfallWorld.MATERIAL_STONE
	var SAND: int = AshfallWorld.MATERIAL_SAND
	var WATER: int = AshfallWorld.MATERIAL_WATER
	var FIRE: int = AshfallWorld.MATERIAL_FIRE
	var WOOD: int = AshfallWorld.MATERIAL_WOOD
	var ROCK: int = AshfallWorld.MATERIAL_ROCK
	var OIL: int = _mat_id("oil", 9)
	world.init_world(512, 512)
	world.fill_rect(0, 0, 511, 511, ROCK)
	for x in range(512):
		var surface: int = _surface_height(x)
		world.fill_rect(x, 0, x, surface - 1, AIR)
		world.fill_rect(x, surface, x, surface + 4, SAND)
		world.fill_rect(x, surface + 5, x, surface + 10, STONE)
		world.fill_rect(x, surface + 11, x, 511, ROCK)
	# A thin exposed bedrock lip gives the spawn stable footing while the
	# surrounding ridge keeps its loose soil and irregular silhouette.
	world.fill_rect(36, int(SURFACE_LEVEL), 52, int(SURFACE_LEVEL), STONE)
	# Main entry: the mouth is broad at the sky line and narrows as it bends
	# below the ridge into the first chamber.
	_carve_tunnel(101, 72, 164, 150, 15, 311)
	_carve_chamber(218.0, 194.0, 84.0, 68.0, 321, 0.28)
	# Optional high pocket: a spring tucked above the main route.
	_carve_tunnel(255, 169, 272, 148, 5, 331)
	_carve_chamber(291.0, 147.0, 29.0, 22.0, 341, 0.22)
	# The lower passage kinks east around a rock shoulder into an asymmetric
	# second room and a short, clearly lit eastern spur.
	_carve_tunnel(276, 226, 345, 254, 13, 351)
	_carve_chamber(385.0, 276.0, 59.0, 45.0, 361, 0.27)
	_carve_tunnel(424, 296, 464, 323, 10, 371)
	# Preserve a walkable connector where the chamber floor rises into the east
	# tunnel; without it the two authored voids nearly touch but leave a soft lock.
	_carve_tunnel(410, 309, 430, 300, 9, 401)
	_carve_chamber(465.0, 334.0, 34.0, 27.0, 381, 0.20)
	_carve_chamber(351.0, 226.0, 25.0, 18.0, 391, 0.18)
	_build_stone_rind(STONE, ROCK, AIR)
	_build_veins(SAND, AshfallWorld.MATERIAL_LAVA, ROCK)
	# A narrow timber grille marks the route; a water pocket is held just above
	# a weak timber sill. The Spring Wand can cut sand/wood and release it.
	world.fill_rect(376, 273, 379, 322, WOOD)
	# The basin stores water behind a closed one-cell spill sill. Opening it
	# releases the water; the following fire/steam outcomes use native reactions.
	world.fill_rect(326, 241, 376, 241, STONE)
	world.fill_rect(347, 241, 354, 241, WOOD)
	world.fill_rect(333, 222, 369, 239, WATER)
	world.fill_rect(367, 311, 374, 315, OIL)
	# A little eroded rubble, a submerged seep and warm fuel make the room feel
	# inhabited; keep the short timber ledge west of the traversable connector.
	world.fill_rect(235, 255, 250, 257, SAND)
	world.fill_rect(298, 244, 308, 247, WATER)
	world.fill_rect(395, 305, 408, 307, WOOD)
	# Preserve the deterministic settle-tick contract for existing render and
	# presentation checks. Stage fresh fire only after settling the liquid.
	for i in 60:
		world.step()
	world.fill_rect(373, 286, 374, 290, FIRE)
	world.fill_rect(380, 289, 381, 293, FIRE)
	world.fill_rect(375, 269, 377, 272, FIRE)
	world.flush_present()


func _surface_height(x: int) -> int:
	# Anchor the spawn patch exactly level, then let the ridge wander on two
	# deterministic scales so the skyline stays walkable rather than noisy.
	if abs(x - int(CAVE_SPAWN.x)) <= 7:
		return int(SURFACE_LEVEL)
	var broad: float = _noise1d(float(x) / 55.0, 401) - _noise1d(CAVE_SPAWN.x / 55.0, 401)
	var fine: float = _noise1d(float(x) / 17.0, 402) - _noise1d(CAVE_SPAWN.x / 17.0, 402)
	return int(SURFACE_LEVEL) + roundi(broad * 7.0 + fine * 2.0)


## Player-scale rockwork frames the spawn lane with ceiling ribs and buttresses.
func _build_opening_rockwork(rock_mat: int) -> void:
	_stamp_tapered_rock(219.0, 258, 35, 6.0, 2.0, 181, rock_mat)
	_stamp_tapered_rock(261.0, 258, 28, 5.0, 2.0, 191, rock_mat)
	_stamp_tapered_rock(188.0, 315, 32, 2.0, 5.0, 201, rock_mat)
	_stamp_tapered_rock(292.0, 315, 32, 2.0, 5.0, 211, rock_mat)


## One-row tapered rock stamp; world-anchored noise keeps authored ribs organic.
func _stamp_tapered_rock(cx: float, y0: int, length: int, half_top: float, half_bottom: float, salt: int, rock_mat: int) -> void:
	for i in range(length):
		var t: float = float(i) / float(maxi(length - 1, 1))
		var half_width: float = maxf(1.0, lerpf(half_top, half_bottom, t) + (_noise1d(float(i) / 5.0, salt) - 0.5) * 1.2)
		var center: float = cx + (_noise1d(float(i) / 11.0, salt + 1) - 0.5) * 2.0
		world.fill_rect(int(floor(center - half_width)), y0 + i, int(ceil(center + half_width)), y0 + i, rock_mat)


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
		var broad: float = minf(16.0, rx * edge * 0.24)
		var left_wander: float = (_noise1d(float(y) / 29.0, salt + 4) - 0.5) * 2.0 * broad * 0.7
		left_wander += (_noise1d(float(y) / 9.0, salt) - 0.5) * 2.0 * broad * 0.3
		var right_wander: float = (_noise1d(float(y) / 31.0, salt + 5) - 0.5) * 2.0 * broad * 0.7
		right_wander += (_noise1d(float(y) / 11.0, salt + 1) - 0.5) * 2.0 * broad * 0.3
		var left_chip: float = (_hash2(int(cx), y, salt + 2) - 0.5) * 2.0
		var right_chip: float = (_hash2(int(cx), y, salt + 3) - 0.5) * 2.0
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
		var slow_wobble: float = (_noise1d(float(i) / 18.0, salt + 77) - 0.5) * 2.0
		var fine_wobble: float = (_noise1d(float(i) / 5.0, salt + 78) - 0.5) * 2.0
		var wob: float = (slow_wobble * 0.72 + fine_wobble * 0.28) * 11.0 * sin(t * 3.14159)
		var cx: int = clampi(int(lerpf(float(ax), float(bx), t) + px * wob), 2, 509)
		var cy: int = clampi(int(lerpf(float(ay), float(by), t) + py * wob), 2, 509)
		var stamp_r: float = maxf(2.5, float(rad) + (_noise1d(float(i) / 9.0, salt + 79) - 0.5) * 1.2)
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

func _physics_process(_delta: float) -> void:
	if _run_finished or _replay_mode == ReplayMode.PLAYBACK_FINISHED:
		return
	_gameplay_tick += 1
	var command: Dictionary
	if _replay_mode == ReplayMode.PLAYBACK:
		if _replay_index >= _replay_commands.size():
			_finish_replay()
			return
		command = _replay_commands[_replay_index]
		if int(command.get("tick", -1)) != _gameplay_tick:
			push_error("Replay tick mismatch: expected %d, got %d" % [_gameplay_tick, int(command.get("tick", -1))])
			_finish_replay()
			return
		_replay_index += 1
	else:
		_aim_from_cursor()
		command = _capture_live_command()
		if _replay_mode == ReplayMode.RECORDING:
				if _recorded_commands.size() >= REPLAY_MAX_TICKS:
					_replay_mode = ReplayMode.IDLE
					_set_transient("REPLAY LIMIT · 10 MINUTES")
				else:
					_recorded_commands.append(command.duplicate(true))
	_apply_gameplay_command(command)
	if _replay_mode == ReplayMode.PLAYBACK and (_replay_index >= _replay_commands.size() or _run_finished):
		_finish_replay()


func _capture_live_command() -> Dictionary:
	var axis_x := 0.0
	if Input.is_physical_key_pressed(KEY_A) or Input.is_physical_key_pressed(KEY_LEFT):
		axis_x -= 1.0
	if Input.is_physical_key_pressed(KEY_D) or Input.is_physical_key_pressed(KEY_RIGHT):
		axis_x += 1.0
	var aim_to_record: Vector2 = _wand_fire_target if _wand_fire_requested else _aim_target
	var command := {
		"tick": _gameplay_tick,
		"move_x": axis_x,
		"jump": jump_requested,
		"aim": Vector2i(roundi(aim_to_record.x), roundi(aim_to_record.y)),
		"fire": _wand_fire_requested,
		"pause": _pause_toggle_requested,
		"step": _single_step_requested,
	}
	jump_requested = false
	_wand_fire_requested = false
	_pause_toggle_requested = false
	_single_step_requested = false
	return command


func _apply_gameplay_command(command: Dictionary) -> void:
	if bool(command.get("pause", false)):
		_simulation_manually_paused = not _simulation_manually_paused
	var target: Vector2 = Vector2(command.get("aim", _aim_target))
	_aim_target = target
	var origin := Vector2(player_pos.x, player_pos.y - PLAYER_H * 0.58)
	var aim_delta := target - origin
	if aim_delta.length_squared() > 0.01:
		_aim_direction = aim_delta.normalized()
	_integrate_player(GAMEPLAY_DT, float(command.get("move_x", 0.0)), bool(command.get("jump", false)))
	if not _gameplay_awake and not _simulation_manually_paused and player_pos.x >= 340.0:
		_wake_gameplay_world()
	if bool(command.get("fire", false)):
		_fire_wand_at(target)
	_advance_projectiles(GAMEPLAY_DT)
	_check_finish_trigger()
	if bool(command.get("step", false)):
		world.step()
	elif not _run_finished and _gameplay_awake and not _simulation_manually_paused:
		world.step()


func _stop_recording() -> void:
	if _replay_mode != ReplayMode.RECORDING:
		return
	_replay_mode = ReplayMode.IDLE
	_set_transient("RECORDED %d TICKS · F7 TO REPLAY" % _recorded_commands.size())
	_update_hud()


func _start_replay() -> void:
	if _recorded_commands.is_empty():
		_set_transient("NO RECORDED RUN · PRESS R FIRST")
		return
	var commands: Array[Dictionary] = _recorded_commands.duplicate(true)
	for i in commands.size():
		if int(commands[i].get("tick", -1)) != i + 1:
			_set_transient("REPLAY LOG INVALID")
			return
	_replay_commands = commands
	_build_cave()
	_respawn_player()
	_gameplay_tick = 0
	_replay_index = 0
	_recorded_commands.clear()
	_replay_mode = ReplayMode.PLAYBACK
	jump_requested = false
	_pause_toggle_requested = false
	_single_step_requested = false
	_wand_fire_requested = false
	_update_marker()
	_update_hud()
	_set_transient("REPLAYING %d TICKS" % _replay_commands.size())


func _finish_replay() -> void:
	if _replay_mode != ReplayMode.PLAYBACK:
		return
	_replay_mode = ReplayMode.PLAYBACK_FINISHED
	if not _run_finished:
		_set_transient("REPLAY COMPLETE · %d TICKS" % _replay_index)
	_update_hud()


func _reset_cave_run() -> void:
	_build_cave()
	_respawn_player()
	_gameplay_tick = 0
	_recorded_commands.clear()
	_replay_commands.clear()
	_replay_index = 0
	_replay_mode = ReplayMode.RECORDING
	jump_requested = false
	_pause_toggle_requested = false
	_single_step_requested = false
	_wand_fire_requested = false
	_update_marker()
	_update_hud()


func _process(delta: float) -> void:
	if delta > 0.0:
		_frame_ms_ema = lerpf(_frame_ms_ema, delta * 1000.0, 0.08)
	if _replay_mode != ReplayMode.PLAYBACK:
		_aim_from_cursor()
	_update_camera(delta)
	_update_marker()
	_update_light(delta)
	_update_gameplay_fx()
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
				_update_tool_readout()
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
	tool_readout.text = msg


func _update_tool_readout() -> void:
	if _transient_ttl > 0.0 and not _transient_msg.is_empty():
		tool_readout.text = _transient_msg
	else:
		tool_readout.text = "SPRING WAND  ·  WATER BOLT\nAim with mouse  ·  Left-click to cast\nF6 ends recording  ·  F7 replays"

func _respawn_player() -> void:
	player_pos = CAVE_SPAWN
	player_velocity = Vector2.ZERO
	player_on_ground = true
	jump_requested = false
	_aim_direction = Vector2.RIGHT
	_aim_target = Vector2(CAVE_SPAWN.x + 56.0, CAVE_SPAWN.y)
	_projectiles.clear()
	_impact_effects.clear()
	_wand_cooldown = 0.0
	_gameplay_awake = false
	_simulation_manually_paused = false
	_run_finished = false
	if _finish_overlay != null:
		_finish_overlay.visible = false
	world.auto_step = false
	_snap_camera()


func _create_finish_overlay() -> void:
	var shade := ColorRect.new()
	shade.name = "FinishOverlay"
	shade.color = Color(0.025, 0.02, 0.035, 0.52)
	shade.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	shade.z_index = 100
	shade.visible = false
	add_child(shade)
	_finish_overlay = shade
	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	shade.add_child(center)
	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(420.0, 150.0)
	center.add_child(panel)
	var stack := VBoxContainer.new()
	stack.alignment = BoxContainer.ALIGNMENT_CENTER
	stack.add_theme_constant_override("separation", 10)
	panel.add_child(stack)
	var title := Label.new()
	title.text = "THE GROTTO FALLS QUIET"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_color_override("font_color", Color("#ffc869"))
	title.add_theme_font_size_override("font_size", 24)
	stack.add_child(title)
	var detail := Label.new()
	detail.text = "ROUTE CLEARED  ·  THE SPRING FOUND ITS WAY HOME"
	detail.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	detail.add_theme_color_override("font_color", Color("#d5e8df"))
	stack.add_child(detail)
	var restart := Label.new()
	restart.text = "Press R to begin another run"
	restart.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	restart.add_theme_color_override("font_color", Color("#b6aba0"))
	stack.add_child(restart)


func _check_finish_trigger() -> void:
	if _run_finished:
		return
	var body_center := Vector2(player_pos.x, player_pos.y - PLAYER_H * 0.5)
	if EXIT_TRIGGER.has_point(body_center):
		_run_finished = true
		world.auto_step = false
		_finish_overlay.visible = true
		_set_transient("ROUTE CLEARED")
		_update_hud()

func _box_passable(px: float, py: float) -> bool:
	var x1: int = floori(px - PLAYER_HALF_W)
	var x2: int = ceili(px + PLAYER_HALF_W) - 1
	var y1: int = floori(py - PLAYER_H)
	var y2: int = ceili(py) - 1
	return world.can_move_to_box(x1, y1, x2, y2)

func _move_player(delta: float) -> void:
	var axis_x: float = 0.0
	if Input.is_physical_key_pressed(KEY_A) or Input.is_physical_key_pressed(KEY_LEFT):
		axis_x -= 1.0
	if Input.is_physical_key_pressed(KEY_D) or Input.is_physical_key_pressed(KEY_RIGHT):
		axis_x += 1.0
	_integrate_player(delta, axis_x, jump_requested)


func _integrate_player(delta: float, axis_x: float, request_jump: bool) -> void:
	# Fixed-size spatial substeps stop fast frames tunnelling through native
	# cells. Gravity and horizontal motion remain host-side prototype state;
	# all terrain collision still queries the authoritative C++ world.
	if delta <= 0.0:
		jump_requested = false
		return
	delta = minf(delta, 0.08)
	axis_x = clampf(axis_x, -1.0, 1.0)
	player_on_ground = not _box_passable(player_pos.x, player_pos.y + 0.12)
	if player_on_ground and player_velocity.y >= 0.0:
		player_pos.y = roundf(player_pos.y)
		player_velocity.y = 0.0
	if request_jump and player_on_ground:
		player_velocity.y = -PLAYER_JUMP_SPEED
		player_on_ground = false
	player_velocity.x = axis_x * PLAYER_SPEED
	var dx: float = player_velocity.x * delta
	var x_steps: int = maxi(1, ceili(absf(dx) / 0.4))
	for i in x_steps:
		var nx: float = player_pos.x + dx / float(x_steps)
		if _box_passable(nx, player_pos.y):
			player_pos.x = nx
			continue
		var stepped := false
		if player_on_ground and absf(dx) > 0.0:
			for lift in range(1, PLAYER_STEP_HEIGHT + 1):
				if _box_passable(nx, player_pos.y - float(lift)):
					player_pos.x = nx
					player_pos.y -= float(lift)
					stepped = true
					break
		if not stepped:
			player_velocity.x = 0.0
			break
	player_velocity.y = minf(player_velocity.y + PLAYER_GRAVITY * delta, PLAYER_TERMINAL_SPEED)
	var dy: float = player_velocity.y * delta
	var y_steps: int = maxi(1, ceili(absf(dy) / 0.4))
	for i in y_steps:
		var ny: float = clampf(player_pos.y + dy / float(y_steps), PLAYER_H, float(world.get_world_height()) - 0.5)
		if _box_passable(player_pos.x, ny):
			player_pos.y = ny
		else:
			if dy > 0.0:
				player_on_ground = true
			player_velocity.y = 0.0
			break
	player_on_ground = not _box_passable(player_pos.x, player_pos.y + 0.12)
	if player_on_ground and player_velocity.y >= 0.0:
		player_pos.y = roundf(player_pos.y)
		player_velocity.y = 0.0
	if player_pos.y >= float(world.get_world_height()) - 1.0:
		_respawn_player()
	jump_requested = false


func _wake_gameplay_world() -> void:
	if _simulation_manually_paused:
		return
	_gameplay_awake = true
	world.auto_step = false
	_set_transient("THE SPRING ROOM STIRS")


func _aim_from_cursor() -> void:
	if texture_display == null or _view_region.size == Vector2i.ZERO:
		return
	var cell: Vector2i = _display_to_world(texture_display.get_local_mouse_position())
	if cell.x < 0:
		return
	_aim_target = Vector2(cell)
	var origin := Vector2(player_pos.x, player_pos.y - PLAYER_H * 0.58)
	var delta_to_target := _aim_target - origin
	if delta_to_target.length_squared() > 0.01:
		_aim_direction = delta_to_target.normalized()


func _fire_wand_at(target: Vector2) -> bool:
	if _wand_cooldown > 0.0 or _projectiles.size() >= 8:
		return false
	var origin := Vector2(player_pos.x, player_pos.y - PLAYER_H * 0.58)
	var delta_to_target := target - origin
	if delta_to_target.length_squared() < 0.01:
		delta_to_target = _aim_direction
	_aim_direction = delta_to_target.normalized()
	player_marker.call("set_aim_direction", _aim_direction)
	_projectiles.append({
		"pos": origin + _aim_direction * 3.0,
		"velocity": _aim_direction * WAND_SPEED,
		"age": 0.0,
	})
	_wand_cooldown = WAND_COOLDOWN
	if not _gameplay_awake:
		_wake_gameplay_world()
	return true


func _advance_projectiles(delta: float) -> void:
	_wand_cooldown = maxf(0.0, _wand_cooldown - delta)
	for i in range(_impact_effects.size() - 1, -1, -1):
		var effect: Dictionary = _impact_effects[i]
		effect["age"] = float(effect["age"]) + delta
		if float(effect["age"]) >= 0.34:
			_impact_effects.remove_at(i)
		else:
			_impact_effects[i] = effect
	for i in range(_projectiles.size() - 1, -1, -1):
		var bolt: Dictionary = _projectiles[i]
		var position: Vector2 = bolt["pos"]
		var velocity: Vector2 = bolt["velocity"]
		var age: float = float(bolt["age"]) + delta
		var distance: float = velocity.length() * minf(delta, 0.08)
		var steps: int = maxi(1, ceili(distance / 0.75))
		var impacted := false
		for step in steps:
			var next_position: Vector2 = position + velocity * (minf(delta, 0.08) / float(steps))
			var cell := Vector2i(floori(next_position.x), floori(next_position.y))
			if cell.x < 1 or cell.y < 1 or cell.x >= world.get_world_width() - 1 or cell.y >= world.get_world_height() - 1:
				position = next_position
				impacted = true
				break
			var material: int = world.get_cell(cell.x, cell.y)
			if material != AshfallWorld.MATERIAL_AIR and material != AshfallWorld.MATERIAL_STEAM:
				position = next_position
				_apply_wand_impact(cell)
				impacted = true
				break
			position = next_position
		if not impacted and age >= WAND_MAX_AGE:
			var cell := Vector2i(floori(position.x), floori(position.y))
			_apply_wand_impact(cell)
			impacted = true
		if impacted:
			_projectiles.remove_at(i)
		else:
			bolt["pos"] = position
			bolt["age"] = age
			_projectiles[i] = bolt


func _apply_wand_impact(center: Vector2i) -> void:
	if center.x < 1 or center.y < 1 or center.x >= world.get_world_width() - 1 or center.y >= world.get_world_height() - 1:
		return
	var struck: int = world.get_cell(center.x, center.y)
	var cut_terrain: bool = struck == AshfallWorld.MATERIAL_WOOD or struck == AshfallWorld.MATERIAL_SAND
	var cut_count: int = 0
	if cut_terrain:
		for dy in range(-4, 5):
			for dx in range(-4, 5):
				if dx * dx + dy * dy > 16:
					continue
				var x: int = center.x + dx
				var y: int = center.y + dy
				var material: int = world.get_cell(x, y)
				if material == AshfallWorld.MATERIAL_WOOD or material == AshfallWorld.MATERIAL_SAND:
					world.set_cell(x, y, AshfallWorld.MATERIAL_AIR)
					cut_count += 1
	# A small, real water burst is written to the authoritative cell world. The
	# following fixed native ticks handle falling, spreading, quench and steam.
	var offsets: Array[Vector2i] = [Vector2i.ZERO, Vector2i(0, -1), Vector2i(-1, 0), Vector2i(1, 0), Vector2i(0, 1), Vector2i(0, -2)]
	var placed: int = 0
	for offset in offsets:
		var x: int = center.x + offset.x
		var y: int = center.y + offset.y
		if world.get_cell(x, y) == AshfallWorld.MATERIAL_AIR:
			world.set_cell(x, y, AshfallWorld.MATERIAL_WATER)
			placed += 1
	if placed > 0 or cut_count > 0:
		_impact_effects.append({"pos": Vector2(center), "age": 0.0})
		if cut_count > 0 and center.y < 250:
			_set_transient("SPRING OPENED  ·  WATER RELEASED")
		else:
			_set_transient("WATER SPLASH  ·  WATCH FOR STEAM")


func _update_gameplay_fx() -> void:
	if gameplay_fx == null:
		return
	var shots: Array[Dictionary] = []
	for bolt in _projectiles:
		shots.append({"pos": _world_to_display(bolt["pos"] as Vector2), "dir": (bolt["velocity"] as Vector2).normalized()})
	var impacts: Array[Dictionary] = []
	for effect in _impact_effects:
		impacts.append({"pos": _world_to_display(effect["pos"] as Vector2), "age": float(effect["age"])})
	var cursor := _world_to_display(_aim_target)
	gameplay_fx.call("set_frame", shots, impacts, cursor, _world_to_display(EXIT_MARKER))

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
	if _projectiles.size() > HOST_SAVE_MAX_PROJECTILES or _impact_effects.size() > HOST_SAVE_MAX_IMPACTS:
		_set_transient("SAVE FAILED · TOO MANY TRANSIENT EFFECTS")
		return
	var sim := world.save_to_bytes()
	var mat_name: String = world.get_material_name(current_material)
	var f := FileAccess.open(SAVE_TMP_PATH, FileAccess.WRITE)
	if f == null:
		_set_transient("SAVE FAILED")
		return
	f.store_8(HOST_SAVE_VERSION)
	f.store_float(player_pos.x)
	f.store_float(player_pos.y)
	f.store_float(player_velocity.x)
	f.store_float(player_velocity.y)
	f.store_8(1 if player_on_ground else 0)
	f.store_float(_aim_direction.x)
	f.store_float(_aim_direction.y)
	f.store_float(_aim_target.x)
	f.store_float(_aim_target.y)
	f.store_float(_wand_cooldown)
	f.store_8(1 if _gameplay_awake else 0)
	f.store_8(1 if _simulation_manually_paused else 0)
	f.store_8(1 if _run_finished else 0)
	f.store_64(_gameplay_tick)
	f.store_8(1 if jump_requested else 0)
	f.store_8(1 if _pause_toggle_requested else 0)
	f.store_8(1 if _single_step_requested else 0)
	f.store_8(1 if _wand_fire_requested else 0)
	f.store_float(_wand_fire_target.x)
	f.store_float(_wand_fire_target.y)
	f.store_pascal_string(mat_name)
	f.store_8(_projectiles.size())
	for bolt in _projectiles:
		var pos: Vector2 = bolt["pos"]
		var velocity: Vector2 = bolt["velocity"]
		f.store_float(pos.x)
		f.store_float(pos.y)
		f.store_float(velocity.x)
		f.store_float(velocity.y)
		f.store_float(float(bolt["age"]))
	f.store_16(_impact_effects.size())
	for effect in _impact_effects:
		var pos: Vector2 = effect["pos"]
		f.store_float(pos.x)
		f.store_float(pos.y)
		f.store_float(float(effect["age"]))
	f.store_32(sim.size())
	f.store_buffer(sim)
	var write_error: Error = f.get_error()
	f.close()
	if write_error != OK:
		_set_transient("SAVE FAILED")
		return
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
	var version: int = f.get_8()
	if version != 2 and version != HOST_SAVE_VERSION:
		f.close()
		_set_transient("SAVE VERSION?")
		return
	var px: float = f.get_float()
	var py: float = f.get_float()
	var vx: float = 0.0
	var vy: float = 0.0
	var grounded_raw: int = 1
	var aim_x: float = 1.0
	var aim_y: float = 0.0
	var target_x: float = px + 56.0
	var target_y: float = py
	var cooldown: float = 0.0
	var awake_raw: int = 1 if px >= 340.0 else 0
	var paused_raw: int = 0
	var finished_raw: int = 0
	var gameplay_tick: int = 0
	var jump_raw: int = 0
	var pause_raw: int = 0
	var step_raw: int = 0
	var fire_raw: int = 0
	var fire_target_x: float = target_x
	var fire_target_y: float = target_y
	var saved_projectiles: Array[Dictionary] = []
	var saved_impacts: Array[Dictionary] = []
	if version == HOST_SAVE_VERSION:
		vx = f.get_float()
		vy = f.get_float()
		grounded_raw = int(f.get_8())
		aim_x = f.get_float()
		aim_y = f.get_float()
		target_x = f.get_float()
		target_y = f.get_float()
		cooldown = f.get_float()
		awake_raw = int(f.get_8())
		paused_raw = int(f.get_8())
		finished_raw = int(f.get_8())
		gameplay_tick = int(f.get_64())
		jump_raw = int(f.get_8())
		pause_raw = int(f.get_8())
		step_raw = int(f.get_8())
		fire_raw = int(f.get_8())
		fire_target_x = f.get_float()
		fire_target_y = f.get_float()
	var mat_name: String = f.get_pascal_string()
	if version == HOST_SAVE_VERSION:
		var projectile_count: int = int(f.get_8())
		if projectile_count > HOST_SAVE_MAX_PROJECTILES or projectile_count * 20 > int(f.get_length() - f.get_position()):
			f.close()
			_set_transient("SAVE CORRUPT")
			return
		for i in projectile_count:
			var bolt_pos := Vector2(f.get_float(), f.get_float())
			var bolt_velocity := Vector2(f.get_float(), f.get_float())
			var bolt_age: float = f.get_float()
			saved_projectiles.append({"pos": bolt_pos, "velocity": bolt_velocity, "age": bolt_age})
		var impact_count: int = int(f.get_16())
		if impact_count > HOST_SAVE_MAX_IMPACTS or impact_count * 12 > int(f.get_length() - f.get_position()):
			f.close()
			_set_transient("SAVE CORRUPT")
			return
		for i in impact_count:
			var impact_pos := Vector2(f.get_float(), f.get_float())
			var impact_age: float = f.get_float()
			saved_impacts.append({"pos": impact_pos, "age": impact_age})
	var sim_len: int = int(f.get_32())
	var world_w: int = world.get_world_width()
	var world_h: int = world.get_world_height()
	var meta_ok: bool = f.get_error() == OK
	meta_ok = meta_ok and is_finite(px) and is_finite(py)
	meta_ok = meta_ok and px >= 0.0 and px <= float(world_w)
	meta_ok = meta_ok and py >= 0.0 and py <= float(world_h)
	meta_ok = meta_ok and not mat_name.is_empty()
	meta_ok = meta_ok and sim_len >= 0 and sim_len <= HOST_SAVE_SIM_MAX
	meta_ok = meta_ok and sim_len <= int(f.get_length() - f.get_position())
	if version == HOST_SAVE_VERSION:
		meta_ok = meta_ok and is_finite(vx) and is_finite(vy) and absf(vx) <= PLAYER_SPEED + 0.01 and absf(vy) <= PLAYER_TERMINAL_SPEED + 0.01
		meta_ok = meta_ok and grounded_raw >= 0 and grounded_raw <= 1
		meta_ok = meta_ok and is_finite(aim_x) and is_finite(aim_y) and Vector2(aim_x, aim_y).length_squared() > 0.001 and Vector2(aim_x, aim_y).length_squared() <= 1.2
		meta_ok = meta_ok and is_finite(target_x) and is_finite(target_y) and target_x >= 0.0 and target_x < float(world_w) and target_y >= 0.0 and target_y < float(world_h)
		meta_ok = meta_ok and is_finite(cooldown) and cooldown >= 0.0 and cooldown <= WAND_COOLDOWN
		meta_ok = meta_ok and awake_raw >= 0 and awake_raw <= 1 and paused_raw >= 0 and paused_raw <= 1 and finished_raw >= 0 and finished_raw <= 1
		meta_ok = meta_ok and gameplay_tick >= 0
		meta_ok = meta_ok and jump_raw >= 0 and jump_raw <= 1 and pause_raw >= 0 and pause_raw <= 1 and step_raw >= 0 and step_raw <= 1 and fire_raw >= 0 and fire_raw <= 1
		meta_ok = meta_ok and is_finite(fire_target_x) and is_finite(fire_target_y) and fire_target_x >= 0.0 and fire_target_x < float(world_w) and fire_target_y >= 0.0 and fire_target_y < float(world_h)
		for bolt in saved_projectiles:
			var bolt_pos: Vector2 = bolt["pos"]
			var bolt_velocity: Vector2 = bolt["velocity"]
			var bolt_age: float = float(bolt["age"])
			meta_ok = meta_ok and is_finite(bolt_pos.x) and is_finite(bolt_pos.y) and bolt_pos.x >= -1.0 and bolt_pos.x <= float(world_w) + 1.0 and bolt_pos.y >= -1.0 and bolt_pos.y <= float(world_h) + 1.0
			meta_ok = meta_ok and is_finite(bolt_velocity.x) and is_finite(bolt_velocity.y) and bolt_velocity.length() <= WAND_SPEED + 0.01
			meta_ok = meta_ok and is_finite(bolt_age) and bolt_age >= 0.0 and bolt_age <= WAND_MAX_AGE
		for effect in saved_impacts:
			var impact_pos: Vector2 = effect["pos"]
			var impact_age: float = float(effect["age"])
			meta_ok = meta_ok and is_finite(impact_pos.x) and is_finite(impact_pos.y) and impact_pos.x >= 0.0 and impact_pos.x < float(world_w) and impact_pos.y >= 0.0 and impact_pos.y < float(world_h)
			meta_ok = meta_ok and is_finite(impact_age) and impact_age >= 0.0 and impact_age < 0.34
	if not meta_ok:
		f.close()
		_set_transient("SAVE CORRUPT")
		return
	var sim := f.get_buffer(sim_len)
	if sim.size() != sim_len or f.get_position() != f.get_length():
		f.close()
		_set_transient("SAVE CORRUPT")
		return
	f.close()
	if not world.load_from_bytes(sim):
		_set_transient("LOAD FAILED")
		return
	player_pos = Vector2(px, py)
	if version == HOST_SAVE_VERSION:
		player_velocity = Vector2(vx, vy)
		player_on_ground = grounded_raw != 0
		_aim_direction = Vector2(aim_x, aim_y).normalized()
		_aim_target = Vector2(target_x, target_y)
		_wand_cooldown = cooldown
		_gameplay_awake = awake_raw != 0
		_simulation_manually_paused = paused_raw != 0
		_run_finished = finished_raw != 0
		_gameplay_tick = gameplay_tick
		jump_requested = jump_raw != 0
		_pause_toggle_requested = pause_raw != 0
		_single_step_requested = step_raw != 0
		_wand_fire_requested = fire_raw != 0
		_wand_fire_target = Vector2(fire_target_x, fire_target_y)
		_projectiles = saved_projectiles
		_impact_effects = saved_impacts
	else:
		player_velocity = Vector2.ZERO
		player_on_ground = true
		_aim_direction = Vector2.RIGHT
		_aim_target = Vector2(px + 56.0, py)
		_wand_cooldown = 0.0
		_gameplay_awake = px >= 340.0
		_simulation_manually_paused = false
		_run_finished = false
		_gameplay_tick = 0
		jump_requested = false
		_pause_toggle_requested = false
		_single_step_requested = false
		_wand_fire_requested = false
		_projectiles.clear()
		_impact_effects.clear()
	world.auto_step = false
	current_material = _material_id_for_name(mat_name)
	_replay_mode = ReplayMode.IDLE
	_recorded_commands.clear()
	_replay_commands.clear()
	_replay_index = 0
	if _finish_overlay != null:
		_finish_overlay.visible = _run_finished
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
		status_label.text = "RUNNING" if _gameplay_awake and not _simulation_manually_paused else "PAUSED"
	else:
		status_label.text = _transient_msg
	fps_label.text = "%d fps%s" % [Engine.get_frames_per_second(),
		"" if DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED else " (uncapped)"]
	frame_label.text = "%.2f ms" % _frame_ms_ema
	sim_label.text = _sim_split_text()
	sim_label.tooltip_text = _sim_split_tooltip()
	player_label.text = "(%d, %d)" % [roundi(player_pos.x), roundi(player_pos.y)]

# E1 split-timer display (§10 CPU split; display only, no sim here).
# Stepping model: one native step per active 60Hz gameplay command tick;
# exactly one flush_present() per rendered frame. sim ms = last step() span ONLY, directly comparable to
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
	var tip: String = "stepping: fixed 60Hz gameplay command tick; native world steps explicitly once per active/unpaused command; 1 present flush per rendered frame; sim = last step() span (bench-comparable)"
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
	var backdrop_material := cave_backdrop.material as ShaderMaterial
	if backdrop_material != null:
		backdrop_material.set_shader_parameter("view_origin", Vector2(_view_region.position))
		backdrop_material.set_shader_parameter("view_size", Vector2(_view_region.size))
		backdrop_material.set_shader_parameter("surface_level", SURFACE_LEVEL)

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
	player_marker.call("set_aim_direction", _aim_direction)
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
		if event.keycode == KEY_R:
			_reset_cave_run()
			return
		if event.keycode == KEY_F6:
			_stop_recording()
			return
		if event.keycode == KEY_F7:
			_start_replay()
			return
		if _replay_mode == ReplayMode.PLAYBACK or _replay_mode == ReplayMode.PLAYBACK_FINISHED or _run_finished:
			return
		match event.keycode:
			KEY_SPACE:
				jump_requested = true
			KEY_W, KEY_UP:
				jump_requested = true
			KEY_P:
				_pause_toggle_requested = true
			KEY_N:
				_single_step_requested = true
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
	if _replay_mode == ReplayMode.PLAYBACK or _replay_mode == ReplayMode.PLAYBACK_FINISHED or _run_finished:
		return
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			if event.is_pressed():
				if Input.is_key_pressed(KEY_SHIFT):
					is_drawing = true
					draw_material = current_material
					_draw_at(event.position)
				else:
					is_drawing = false
					var target := _display_to_world(event.position)
					if target.x >= 0:
						_wand_fire_requested = true
						_wand_fire_target = Vector2(target)
			else:
				is_drawing = false
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			if Input.is_key_pressed(KEY_SHIFT):
				is_drawing = event.is_pressed()
				draw_material = AshfallWorld.MATERIAL_AIR
				if is_drawing:
					_draw_at(event.position)
			else:
				is_drawing = false
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
	if _replay_mode != ReplayMode.PLAYBACK and not _run_finished:
		_single_step_requested = true

func _on_play_btn_pressed() -> void:
	if _replay_mode != ReplayMode.PLAYBACK and not _run_finished:
		_pause_toggle_requested = true

func _on_reset_btn_pressed() -> void:
	_reset_cave_run()

func _on_save_btn_pressed() -> void:
	_save_game()

func _on_load_btn_pressed() -> void:
	_load_game()

func _on_demo_btn_pressed() -> void:
	_open_demo()

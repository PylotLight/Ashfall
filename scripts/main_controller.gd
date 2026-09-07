extends Control

# Ashfall Host Controller
# Presentation and input forwarding only.
# No simulation rules or world state are implemented here.

@onready var world: AshfallWorld = $AshfallWorld
@onready var texture_display: TextureRect = $MainLayout/ViewportContainer/WorldTexture
@onready var tick_label: Label = $MainLayout/Sidebar/InfoBox/TickValue
@onready var cells_label: Label = $MainLayout/Sidebar/InfoBox/CellsValue
@onready var status_label: Label = $MainLayout/Sidebar/InfoBox/StatusValue
@onready var material_label: Label = $MainLayout/Sidebar/InfoBox/MaterialValue

var current_material: int = AshfallWorld.MATERIAL_SAND
var brush_size: int = 2
var is_drawing: bool = false
var draw_material: int = AshfallWorld.MATERIAL_SAND
var _hud_cooldown: float = 0.0
const HUD_REFRESH_INTERVAL: float = 0.25

func _ready() -> void:
	# Populate the authoritative 256x256 reference world in C++
	world.setup_reference_world()
	world.auto_step = false
	world.set_sim_rate_hz(30.0)
	world.set_max_steps_per_physics_frame(2)
	texture_display.texture = world.get_world_texture()
	_update_hud()

func _process(delta: float) -> void:
	if not world.auto_step:
		return
	# Throttled HUD: occupied-cell query is now O(1) but GDScript label churn
	# every frame is still wasteful. Refresh at 4Hz while running.
	_hud_cooldown -= delta
	if _hud_cooldown <= 0.0:
		_hud_cooldown = HUD_REFRESH_INTERVAL
		_update_hud()
	else:
		# Cheap tick-only refresh keeps the counter live without cell query.
		tick_label.text = str(world.get_tick())

func _update_hud() -> void:
	tick_label.text = str(world.get_tick())
	cells_label.text = str(world.get_occupied_cell_count())
	status_label.text = "RUNNING" if world.is_auto_step() else "PAUSED"
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
				world.setup_reference_world()
				_update_hud()
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
	var tex_size = texture_display.size
	if tex_size.x <= 0 or tex_size.y <= 0:
		return
	var world_w = world.get_world_width()
	var world_h = world.get_world_height()

	var cell_x = int(clamp((local_pos.x / tex_size.x) * world_w, 0, world_w - 1))
	var cell_y = int(clamp((local_pos.y / tex_size.y) * world_h, 0, world_h - 1))

	var x1 = max(0, cell_x - brush_size)
	var y1 = max(0, cell_y - brush_size)
	var x2 = min(world_w - 1, cell_x + brush_size)
	var y2 = min(world_h - 1, cell_y + brush_size)

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
	world.setup_reference_world()
	_update_hud()

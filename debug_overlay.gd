extends CanvasLayer

@onready var sim_backend = $"../../SimWorld/SimBackend"
@onready var player = $"../../Entities/Player"
@onready var input_router = $"../../Managers/InputRouter"
@onready var game_state = $"../../Managers/GameState"
@onready var stats_label: Label = $Stats

var show_overlay := true
var current_tool_name := ""

func _ready() -> void:
	if input_router:
		input_router.tool_changed.connect(func(_idx, tool_name): current_tool_name = tool_name)

func _process(_delta: float) -> void:
	if Input.is_action_just_pressed("debug_toggle"):
		show_overlay = not show_overlay
		visible = show_overlay

	if not show_overlay or stats_label == null:
		return

	var fps = Engine.get_frames_per_second()
	var player_world = player.global_position if player else Vector2.ZERO
	var player_cell = sim_backend.world_to_cell(player_world) if sim_backend else Vector2i.ZERO
	var cursor_world = player.get_aim_world_pos() if player else Vector2.ZERO
	var cursor_cell = sim_backend.world_to_cell(cursor_world) if sim_backend else Vector2i.ZERO
	var player_vel = player.velocity if player else Vector2.ZERO
	var player_grounded = player.is_grounded if player else false

	var grid_w = sim_backend.GRID_W if sim_backend else 0
	var grid_h = sim_backend.GRID_H if sim_backend else 0
	var substeps = game_state.substeps_per_frame if game_state else 0
	var paused_str = " (PAUSED)" if game_state and game_state.is_paused else ""

	stats_label.text = (
		"FPS: %d%s\n" +
		"Grid: %dx%d | Substeps: %d\n" +
		"Player World: (%.1f, %.1f) | Cell: (%d, %d)\n" +
		"Player Vel: (%.1f, %.1f) | Grounded: %s\n" +
		"Cursor World: (%.1f, %.1f) | Cell: (%d, %d)\n" +
		"Tool: %s (Radius: %d, [ / ] to resize)\n" +
		"F3 / ` : Toggle Debug Overlay | Esc: Pause"
	) % [fps, paused_str, grid_w, grid_h, substeps, player_world.x, player_world.y, player_cell.x, player_cell.y, player_vel.x, player_vel.y, "Yes" if player_grounded else "No", cursor_world.x, cursor_world.y, cursor_cell.x, cursor_cell.y, current_tool_name, player.interaction_radius if player else 0]

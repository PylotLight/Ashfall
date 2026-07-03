extends Node

signal tool_changed(tool_idx: int, tool_name: String)

enum Tool { DIG, PLACE_SAND, PLACE_WATER, PLACE_STONE }

var current_tool: Tool = Tool.PLACE_SAND
var tool_names := {
	Tool.DIG: "Dig / Erase",
	Tool.PLACE_SAND: "Place Sand",
	Tool.PLACE_WATER: "Place Water",
	Tool.PLACE_STONE: "Place Stone"
}

@onready var player = $"../../Entities/Player"
var debug_mode := true

func _ready() -> void:
	tool_changed.emit(current_tool, tool_names[current_tool])

func _unhandled_input(event: InputEvent) -> void:
	if player == null:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_BRACKETLEFT:
			player.interaction_radius = max(1, player.interaction_radius - 1)
		elif event.keycode == KEY_BRACKETRIGHT:
			player.interaction_radius = min(20, player.interaction_radius + 1)

func _process(_delta: float) -> void:
	if player == null:
		return

	if Input.is_action_just_pressed("debug_toggle"):
		debug_mode = not debug_mode

	if Input.is_action_just_pressed("tool_next"):
		_cycle_tool(1)
	elif Input.is_action_just_pressed("tool_prev"):
		_cycle_tool(-1)

	var is_primary = Input.is_action_pressed("interact_primary")
	var is_secondary = Input.is_action_pressed("interact_secondary")

	if is_primary or is_secondary:
		var target_pos = player.get_aim_world_pos()
		if player.validate_interaction(target_pos):
			if is_secondary:
				player.dig_requested.emit(target_pos, player.interaction_radius)
			else:
				match current_tool:
					Tool.DIG:
						player.dig_requested.emit(target_pos, player.interaction_radius)
					Tool.PLACE_SAND:
						player.place_requested.emit(target_pos, 2, player.interaction_radius)
					Tool.PLACE_WATER:
						player.place_requested.emit(target_pos, 3, player.interaction_radius)
					Tool.PLACE_STONE:
						if debug_mode:
							player.place_requested.emit(target_pos, 1, player.interaction_radius)

func _cycle_tool(dir: int) -> void:
	var next = (int(current_tool) + dir) % Tool.values().size()
	if next < 0:
		next = Tool.values().size() - 1
	current_tool = next as Tool
	tool_changed.emit(current_tool, tool_names[current_tool])


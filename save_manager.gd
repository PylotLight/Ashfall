extends Node

const SAVE_PATH := "user://save_game.json"

@onready var sim_backend = $"../../SimWorld/SimBackend"
@onready var player = $"../../Entities/Player"
@onready var input_router = $"../../Managers/InputRouter"

func capture_save_state() -> Dictionary:
	var sim_payload = sim_backend.capture_save_state() if sim_backend else {}
	var state := {
		"save_version": 1,
		"area_id": "test_chamber",
		"player_position": {
			"x": player.position.x if player else 0.0,
			"y": player.position.y if player else 0.0
		},
		"current_tool": input_router.current_tool if input_router else 0,
		"sim_payload": sim_payload
	}
	return state

func restore_save_state(data: Dictionary) -> void:
	if data.has("player_position") and player:
		var pos = data["player_position"]
		player.position = Vector2(pos["x"], pos["y"])
	
	if data.has("current_tool") and input_router:
		input_router.current_tool = data["current_tool"]
		input_router.tool_changed.emit(input_router.current_tool, input_router.tool_names[input_router.current_tool])
		
	if data.has("sim_payload") and sim_backend:
		sim_backend.restore_save_state(data["sim_payload"])
		
	print("Game state restored successfully.")

func save_game() -> void:
	var state = capture_save_state()
	var file = FileAccess.open(SAVE_PATH, FileAccess.WRITE)
	if file:
		var json_str = JSON.stringify(state)
		file.store_string(json_str)
		file.close()
		print("Game saved successfully to: ", SAVE_PATH)
	else:
		push_error("Failed to write save game file.")

func load_game() -> void:
	if not FileAccess.file_exists(SAVE_PATH):
		print("No save game file found.")
		return
		
	var file = FileAccess.open(SAVE_PATH, FileAccess.READ)
	if file:
		var json_str = file.get_as_text()
		file.close()
		var json = JSON.new()
		var error = json.parse(json_str)
		if error == OK:
			var data = json.data
			restore_save_state(data)
		else:
			push_error("JSON Parse Error: ", json.get_error_message(), " at line ", json.get_error_line())
	else:
		push_error("Failed to read save game file.")

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_F5:
			save_game()
		elif event.keycode == KEY_F9:
			load_game()

extends Node2D

@export var follow_smoothing := 10.0
@export var min_zoom := 0.2
@export var max_zoom := 8.0
@export var zoom_step := 0.1

@onready var camera_2d: Camera2D = $Camera2D
@onready var player = $"../Entities/Player"
@onready var sim_backend = $"../SimWorld/SimBackend"

var target_zoom := 1.0

func _ready() -> void:
	if camera_2d:
		camera_2d.zoom = Vector2(target_zoom, target_zoom)

func _process(delta: float) -> void:
	if player == null or sim_backend == null:
		return

	if Input.is_action_just_pressed("camera_zoom_in"):
		target_zoom = clampf(target_zoom + zoom_step, min_zoom, max_zoom)
	elif Input.is_action_just_pressed("camera_zoom_out"):
		target_zoom = clampf(target_zoom - zoom_step, min_zoom, max_zoom)

	if camera_2d:
		camera_2d.zoom = camera_2d.zoom.lerp(Vector2(target_zoom, target_zoom), follow_smoothing * delta)

	var target_pos = player.global_position
	global_position = global_position.lerp(target_pos, follow_smoothing * delta)

	var grid_size = sim_backend.get_grid_size()
	var cell_size = sim_backend.get_cell_size_world()
	var max_x = grid_size.x * cell_size
	var max_y = grid_size.y * cell_size

	global_position.x = clampf(global_position.x, 0.0, max_x)
	global_position.y = clampf(global_position.y, 0.0, max_y)

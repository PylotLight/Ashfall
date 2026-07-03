extends Node2D

const SimBrushOp = preload("res://sim_brush_op.gd")

@onready var sim_backend = $SimWorld/SimBackend
@onready var player = $Entities/Player

func _ready() -> void:
	if player:
		player.dig_requested.connect(_on_player_dig_requested)
		player.place_requested.connect(_on_player_place_requested)

func _on_player_dig_requested(world_pos: Vector2, radius: int) -> void:
	if sim_backend == null:
		return
	var cell = sim_backend.world_to_cell(world_pos)
	var op = SimBrushOp.new()
	op.mode = SimBrushOp.Mode.DIG
	op.material_id = 0
	sim_backend.modify_cells_circle(cell, radius, op)

func _on_player_place_requested(world_pos: Vector2, material_id: int, radius: int) -> void:
	if sim_backend == null:
		return
	var cell = sim_backend.world_to_cell(world_pos)
	var op = SimBrushOp.new()
	op.mode = SimBrushOp.Mode.PLACE
	op.material_id = material_id
	sim_backend.modify_cells_circle(cell, radius, op)

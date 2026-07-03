extends Node2D

@onready var player = $"../Entities/Player"
@onready var sim_backend = $"../SimWorld/SimBackend"

func _process(_delta: float) -> void:
	queue_redraw()

func _draw() -> void:
	if player == null or sim_backend == null or not sim_backend.is_ready():
		return
	
	var mouse_pos = get_global_mouse_position()
	var in_range = player.validate_interaction(mouse_pos)
	var color = Color(0.2, 0.8, 0.2, 0.6) if in_range else Color(0.8, 0.2, 0.2, 0.6)
	
	var radius_cells = player.interaction_radius
	var cell_size = sim_backend.get_cell_size_world()
	var radius_world = radius_cells * cell_size
	
	var cell = sim_backend.world_to_cell(mouse_pos)
	var cell_center = sim_backend.cell_to_world_center(cell)
	
	# Draw range limit circle around player
	draw_arc(player.global_position, player.interaction_range, 0, TAU, 64, Color(1.0, 1.0, 1.0, 0.15), 1.0)
	
	# Draw preview of brush
	draw_arc(cell_center, radius_world, 0, TAU, 32, color, 1.0)
	draw_circle(cell_center, 1.0, color)

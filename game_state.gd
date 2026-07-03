extends Node

@export var substeps_per_frame := 2

@onready var sim_backend = $"../../SimWorld/SimBackend"

var is_paused := false

func _process(_delta: float) -> void:
	if Input.is_action_just_pressed("pause"):
		is_paused = not is_paused

	if sim_backend != null and sim_backend.is_ready() and not is_paused:
		sim_backend.step_sim(substeps_per_frame)

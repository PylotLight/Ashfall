extends Sprite2D

@onready var sim_backend = $"../SimBackend"

func _ready() -> void:
	if sim_backend == null:
		return

	if not sim_backend.is_ready():
		await sim_backend.sim_ready

	var mat := ShaderMaterial.new()
	mat.shader = load("res://display.gdshader")
	material = mat
	texture = sim_backend.get_display_texture()
	texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	
	scale = Vector2.ONE
	centered = false

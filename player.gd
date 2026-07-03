extends CharacterBody2D

signal dig_requested(world_pos: Vector2, radius: int)
signal place_requested(world_pos: Vector2, material_id: int, radius: int)

# --- Tuning Values ---
@export_group("Movement")
@export var move_speed := 140.0
@export var acceleration := 800.0
@export var deceleration := 1000.0
@export var gravity := 900.0
@export var max_fall_speed := 500.0

@export_group("Jumping")
@export var jump_speed := 280.0
@export var coyote_time_duration := 0.15
@export var jump_buffer_duration := 0.15

@export_group("Collision & Interaction")
@export var collision_size := Vector2(10, 20)
@export var interaction_radius := 5
@export var interaction_range := 96.0

@onready var sim_backend = $"../../SimWorld/SimBackend"
@onready var visual = $Visual

var is_grounded := false
var coyote_timer := 0.0
var jump_buffer_timer := 0.0
var facing_direction := 1 # 1 = Right, -1 = Left

func _ready() -> void:
	if position == Vector2.ZERO:
		position = Vector2(1024, 1000)

func _physics_process(delta: float) -> void:
	if sim_backend == null or not sim_backend.is_ready():
		return

	# Handle Coyote Time Timer
	if is_grounded:
		coyote_timer = coyote_time_duration
	else:
		coyote_timer = max(0.0, coyote_timer - delta)

	# Handle Jump Buffer Timer
	if Input.is_action_just_pressed("jump"):
		jump_buffer_timer = jump_buffer_duration
	else:
		jump_buffer_timer = max(0.0, jump_buffer_timer - delta)

	# Horizontal movement with acceleration/deceleration
	var move_input = 0.0
	if Input.is_action_pressed("move_left"):
		move_input -= 1.0
	if Input.is_action_pressed("move_right"):
		move_input += 1.0

	if move_input != 0.0:
		velocity.x = move_toward(velocity.x, move_input * move_speed, acceleration * delta)
		facing_direction = 1 if move_input > 0 else -1
	else:
		velocity.x = move_toward(velocity.x, 0.0, deceleration * delta)

	# Apply simple facing direction visually via scale
	if visual:
		visual.scale.x = facing_direction

	# Gravity & Max Fall Speed
	if not is_grounded:
		velocity.y = min(max_fall_speed, velocity.y + gravity * delta)
	else:
		if velocity.y > 0:
			velocity.y = 0

	# Jump Logic (incorporating Coyote Time and Jump Buffering)
	if jump_buffer_timer > 0.0 and coyote_timer > 0.0:
		velocity.y = -jump_speed
		is_grounded = false
		coyote_timer = 0.0
		jump_buffer_timer = 0.0

	# Variable Jump Height (cut velocity when jump key is released early)
	if Input.is_action_just_released("jump") and velocity.y < 0.0:
		velocity.y = velocity.y * 0.5 # Halt jump momentum by half

	_move_and_resolve_collision(delta)

func _move_and_resolve_collision(delta: float) -> void:
	var next_x = position.x + velocity.x * delta
	var aabb_x = Rect2(Vector2(next_x - collision_size.x / 2.0, position.y - collision_size.y / 2.0), collision_size)
	if not sim_backend.is_solid_region(_world_rect_to_cell_rect(aabb_x)):
		position.x = next_x
	else:
		velocity.x = 0

	var next_y = position.y + velocity.y * delta
	var aabb_y = Rect2(Vector2(position.x - collision_size.x / 2.0, next_y - collision_size.y / 2.0), collision_size)
	if not sim_backend.is_solid_region(_world_rect_to_cell_rect(aabb_y)):
		position.y = next_y
	else:
		if velocity.y > 0:
			is_grounded = true
		velocity.y = 0

	var ground_probe = Rect2(Vector2(position.x - collision_size.x / 2.0, position.y + collision_size.y / 2.0), Vector2(collision_size.x, 1.0))
	is_grounded = sim_backend.is_solid_region(_world_rect_to_cell_rect(ground_probe))

func _world_rect_to_cell_rect(rect: Rect2) -> Rect2i:
	var start = sim_backend.world_to_cell(rect.position)
	var end = sim_backend.world_to_cell(rect.end)
	return Rect2i(start, end - start + Vector2i(1, 1))

func get_aim_world_pos() -> Vector2:
	return get_global_mouse_position()

func validate_interaction(target_world_pos: Vector2) -> bool:
	return position.distance_to(target_world_pos) <= interaction_range


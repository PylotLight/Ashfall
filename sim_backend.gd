extends Node

signal sim_ready
signal material_changed(region: Rect2i)
signal cell_region_modified(region: Rect2i)

const SimBrushOp = preload("res://sim_brush_op.gd")

const GRID_W := 2048
const GRID_H := 2048
const CELL_SIZE_WORLD := 1.0

const EMPTY := 0
const STONE := 1
const SAND := 2
const WATER := 3

var rd: RenderingDevice
var shader: RID
var pipeline: RID
var tex_a: RID
var tex_b: RID
var uniform_set_a_to_b: RID
var uniform_set_b_to_a: RID
var using_a_as_src := true
var frame_counter := 0
var display_texture: Texture2DRD

# NOTE: cpu_grid is a temporary CPU-side collision mask, NOT the final simulation state.
# It is updated on CPU by brush writes, but GPU simulation movement does not yet sync back.
var cpu_grid: PackedByteArray
var _is_ready := false

# Pending GPU brush operations
var pending_painting := 0
var pending_x := 0
var pending_y := 0
var pending_radius := 4
var pending_mat := 0

func _ready() -> void:
	rd = RenderingServer.get_rendering_device()
	if rd == null:
		push_error("No RenderingDevice available. Project Settings > Rendering > Renderer must be 'Forward+' or 'Mobile', not 'Compatibility'.")
		return

	_setup_compute()
	_setup_display()
	_is_ready = true
	sim_ready.emit()

func _setup_compute() -> void:
	var shader_file := load("res://sand_sim.glsl") as RDShaderFile
	var spirv := shader_file.get_spirv()
	shader = rd.shader_create_from_spirv(spirv)
	pipeline = rd.compute_pipeline_create(shader)

	var fmt := RDTextureFormat.new()
	fmt.width = GRID_W
	fmt.height = GRID_H
	fmt.format = RenderingDevice.DATA_FORMAT_R8_UNORM
	fmt.usage_bits = (
		RenderingDevice.TEXTURE_USAGE_STORAGE_BIT
		| RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		| RenderingDevice.TEXTURE_USAGE_CAN_UPDATE_BIT
		| RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	)
	var view := RDTextureView.new()

	cpu_grid = _make_initial_grid()
	tex_a = rd.texture_create(fmt, view, [cpu_grid])
	tex_b = rd.texture_create(fmt, view, [cpu_grid])

	uniform_set_a_to_b = _make_uniform_set(tex_a, tex_b)
	uniform_set_b_to_a = _make_uniform_set(tex_b, tex_a)

func _make_uniform_set(src: RID, dst: RID) -> RID:
	var u_src := RDUniform.new()
	u_src.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u_src.binding = 0
	u_src.add_id(src)

	var u_dst := RDUniform.new()
	u_dst.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u_dst.binding = 1
	u_dst.add_id(dst)

	return rd.uniform_set_create([u_src, u_dst], shader, 0)

func _make_initial_grid() -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(GRID_W * GRID_H)
	for y in range(GRID_H):
		for x in range(GRID_W):
			var idx := y * GRID_W + x
			if y == GRID_H - 1 or x == 0 or x == GRID_W - 1:
				data[idx] = STONE  # floor + side walls
				continue

			data[idx] = EMPTY

			# Low platform under spawn point
			if y == 1010 and x >= 900 and x <= 1150:
				data[idx] = STONE
				
			# Vertical wall
			if x == 950 and y >= 950 and y <= 1010:
				data[idx] = STONE
				
			# Sand container (bowl)
			if y == 980 and x >= 1000 and x <= 1040:
				data[idx] = STONE
			if x == 1000 and y >= 950 and y <= 980:
				data[idx] = STONE
			if x == 1040 and y >= 950 and y <= 980:
				data[idx] = STONE
				
			# Water container (bowl)
			if y == 980 and x >= 1060 and x <= 1100:
				data[idx] = STONE
			if x == 1060 and y >= 950 and y <= 980:
				data[idx] = STONE
			if x == 1100 and y >= 950 and y <= 980:
				data[idx] = STONE
				
			# Spawn some sand inside sand bowl
			if x > 1000 and x < 1040 and y > 950 and y < 980:
				data[idx] = SAND
				
			# Spawn some water inside water bowl
			if x > 1060 and x < 1100 and y > 950 and y < 980:
				data[idx] = WATER
	return data

func _setup_display() -> void:
	display_texture = Texture2DRD.new()
	_update_display_texture()

func _update_display_texture() -> void:
	if display_texture:
		display_texture.texture_rd_rid = tex_a if using_a_as_src else tex_b

func is_ready() -> bool:
	return _is_ready

func get_grid_size() -> Vector2i:
	return Vector2i(GRID_W, GRID_H)

func get_cell_size_world() -> float:
	return CELL_SIZE_WORLD

func world_to_cell(world_pos: Vector2) -> Vector2i:
	return Vector2i(floor(world_pos.x / CELL_SIZE_WORLD), floor(world_pos.y / CELL_SIZE_WORLD))

func cell_to_world(cell: Vector2i) -> Vector2:
	return Vector2(cell.x * CELL_SIZE_WORLD, cell.y * CELL_SIZE_WORLD)

func cell_to_world_center(cell: Vector2i) -> Vector2:
	return Vector2((cell.x + 0.5) * CELL_SIZE_WORLD, (cell.y + 0.5) * CELL_SIZE_WORLD)

func get_cell_material(cell: Vector2i) -> int:
	if cell.x < 0 or cell.x >= GRID_W or cell.y < 0 or cell.y >= GRID_H:
		return STONE
	return cpu_grid[cell.y * GRID_W + cell.x]

func is_solid_cell(cell: Vector2i) -> bool:
	return get_cell_material(cell) == STONE

func is_solid_region(cell_rect: Rect2i) -> bool:
	for y in range(cell_rect.position.y, cell_rect.end.y):
		for x in range(cell_rect.position.x, cell_rect.end.x):
			if is_solid_cell(Vector2i(x, y)):
				return true
	return false

func get_collision_mask_region(cell_rect: Rect2i) -> PackedByteArray:
	var region_data = PackedByteArray()
	region_data.resize(cell_rect.size.x * cell_rect.size.y)
	for y in range(cell_rect.size.y):
		var cy = cell_rect.position.y + y
		for x in range(cell_rect.size.x):
			var cx = cell_rect.position.x + x
			var val = STONE
			if cx >= 0 and cx < GRID_W and cy >= 0 and cy < GRID_H:
				val = cpu_grid[cy * GRID_W + cx]
			region_data[y * cell_rect.size.x + x] = val
	return region_data

func sync_collision_region(cell_rect: Rect2i) -> void:
	# Currently a stub. In the future, this will sync GPU sim results back to cpu_grid
	# for the specified bounding region to enable dynamic material collision.
	pass

func modify_cells_circle(center_cell: Vector2i, radius: int, op: Resource) -> void:
	pending_painting = 1
	pending_x = center_cell.x
	pending_y = center_cell.y
	pending_radius = radius
	pending_mat = op.material_id if op.mode != SimBrushOp.Mode.ERASE and op.mode != SimBrushOp.Mode.DIG else EMPTY

	_apply_brush_to_cpu(center_cell, radius, op)
	cell_region_modified.emit(Rect2i(center_cell.x - radius, center_cell.y - radius, radius * 2, radius * 2))

func modify_cells_rect(rect: Rect2i, op: Resource) -> void:
	var center = rect.position + rect.size / 2
	var radius = maxi(rect.size.x, rect.size.y) / 2
	modify_cells_circle(center, radius, op)

func _apply_brush_to_cpu(center_cell: Vector2i, radius: int, op: Resource) -> void:
	var mat_val = op.material_id
	if op.mode == SimBrushOp.Mode.ERASE or op.mode == SimBrushOp.Mode.DIG:
		mat_val = EMPTY

	var r2 = radius * radius
	for dy in range(-radius, radius + 1):
		for dx in range(-radius, radius + 1):
			if dx * dx + dy * dy <= r2:
				var cx = center_cell.x + dx
				var cy = center_cell.y + dy
				if cx > 0 and cx < GRID_W - 1 and cy > 0 and cy < GRID_H - 1:
					cpu_grid[cy * GRID_W + cx] = mat_val

func step_sim(substeps: int) -> void:
	if rd == null:
		return

	for i in range(substeps):
		_dispatch_step()

	_update_display_texture()
	pending_painting = 0

func _dispatch_step() -> void:
	var uniform_set = uniform_set_a_to_b if using_a_as_src else uniform_set_b_to_a

	var push := PackedByteArray()
	push.resize(32)
	push.encode_s32(0, GRID_W)
	push.encode_s32(4, GRID_H)
	push.encode_u32(8, frame_counter)
	push.encode_u32(12, pending_painting)
	push.encode_s32(16, pending_x)
	push.encode_s32(20, pending_y)
	push.encode_s32(24, pending_radius)
	push.encode_s32(28, pending_mat)

	var groups_x = int(ceil(float(GRID_W) / 8.0))
	var groups_y = int(ceil(float(GRID_H) / 8.0))

	var compute_list = rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	rd.compute_list_set_push_constant(compute_list, push, push.size())
	rd.compute_list_dispatch(compute_list, groups_x, groups_y, 1)
	rd.compute_list_end()

	using_a_as_src = not using_a_as_src
	frame_counter += 1

func get_display_texture() -> Texture2D:
	return display_texture

func capture_save_state() -> Dictionary:
	return {
		"cpu_grid_base64": Marshalls.raw_to_base64(cpu_grid)
	}

func restore_save_state(data: Dictionary) -> void:
	if data.has("cpu_grid_base64"):
		var raw = Marshalls.base64_to_raw(data["cpu_grid_base64"])
		if raw.size() == cpu_grid.size():
			cpu_grid = raw
			if rd != null and _is_ready:
				rd.texture_update(tex_a, 0, raw)
				rd.texture_update(tex_b, 0, raw)

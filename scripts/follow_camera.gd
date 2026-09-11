extends RefCounted

# Ashfall follow camera — P3A Slice A, Task A2.
#
# Presentation-only view state: the sim never sees the view rect, so camera
# motion cannot change sim hashes. The controller feeds player cell position
# + world size each frame and applies the returned region to cropped
# AtlasTexture views over the live bridge textures.
#
# Scene-tree convention (shared with the parallel A1 cave scene — this file
# deliberately does NOT depend on A1's files): every play scene is driven
# through main_controller.gd, which owns `player_pos` (cell units, feet
# center), `world` (AshfallWorld), and the TextureRect stack (WorldTexture +
# WaterOverlay filling the same control rect). This helper only needs
# player/world/control sizes, so it works unchanged in the demo and in any
# new scene that keeps that contract.
#
# World-space Node2D direction: the TextureRect stack STAYS for P3A (no
# refactor — deliberate per the A2 brief). A future Node2D world view can
# reuse the view-rect math here (snap/update/screen mapping) and apply the
# rect to a Camera2D; only the apply step in main_controller changes. HUD
# labels live outside the world stack, so the UI stays crisp independent of
# the world upscale.

const DEFAULT_VIEW_H_CELLS: float = 180.0
const MIN_VIEW_H_CELLS: float = 48.0
const ZOOM_STEP: float = 1.25
const SMOOTHING_RATE: float = 6.0
const OUTSIDE = Vector2i(-1, -1)

var follow_enabled: bool = true
var view_h_cells: float = DEFAULT_VIEW_H_CELLS
var center_cells: Vector2 = Vector2.ZERO
var _primed: bool = false


func snap_to(player_pos: Vector2, world_size: Vector2i, control_size: Vector2) -> Rect2i:
	center_cells = player_pos
	_primed = true
	return view_region(world_size, control_size)


func update(delta: float, player_pos: Vector2, world_size: Vector2i, control_size: Vector2) -> Rect2i:
	if not _primed:
		return snap_to(player_pos, world_size, control_size)
	if follow_enabled and delta > 0.0:
		var t: float = clampf(SMOOTHING_RATE * delta, 0.0, 1.0)
		center_cells = center_cells.lerp(player_pos, t)
	return view_region(world_size, control_size)


func set_follow(enabled: bool, player_pos: Vector2, world_size: Vector2i, control_size: Vector2) -> Rect2i:
	follow_enabled = enabled
	if enabled:
		return snap_to(player_pos, world_size, control_size)
	return view_region(world_size, control_size)


func zoom_step(dir: int, world_size: Vector2i, control_size: Vector2) -> Rect2i:
	if dir > 0:
		view_h_cells = maxf(view_h_cells / ZOOM_STEP, MIN_VIEW_H_CELLS)
	elif dir < 0:
		view_h_cells = minf(view_h_cells * ZOOM_STEP, float(maxi(world_size.y, 1)))
	return view_region(world_size, control_size)


func view_region(world_size: Vector2i, control_size: Vector2) -> Rect2i:
	var vw: int = world_size.x
	var vh: int = world_size.y
	if vw <= 0 or vh <= 0:
		return Rect2i(0, 0, 0, 0)
	if not follow_enabled:
		# Classic fit: the whole grid, letterboxed by the TextureRects.
		return Rect2i(0, 0, vw, vh)
	var want_h: float = clampf(view_h_cells, 1.0, float(vh))
	var aspect: float = 1.0
	if control_size.x > 0.0 and control_size.y > 0.0:
		aspect = control_size.x / control_size.y
	var want_w: float = want_h * aspect
	if want_w >= float(vw):
		# Narrow control: fit the width, grow the height to match.
		want_w = float(vw)
		want_h = minf(float(vw) / aspect, float(vh))
	var w: int = mini(int(round(want_w)), vw)
	var h: int = mini(int(round(want_h)), vh)
	var x0: int = 0
	var y0: int = 0
	if w < vw:
		var cx: float = clampf(center_cells.x, want_w * 0.5, float(vw) - want_w * 0.5)
		x0 = clampi(int(floor(cx - want_w * 0.5)), 0, vw - w)
	if h < vh:
		var cy: float = clampf(center_cells.y, want_h * 0.5, float(vh) - want_h * 0.5)
		y0 = clampi(int(floor(cy - want_h * 0.5)), 0, vh - h)
	return Rect2i(x0, y0, w, h)


# KEEP_ASPECT_CENTERED letterbox mapping for a view rect: uniform scale plus
# centered offset. Static so it is unit-testable without a scene.
static func view_scale(control_size: Vector2, view: Rect2i) -> Vector2:
	if control_size.x <= 0.0 or control_size.y <= 0.0:
		return Vector2.ZERO
	if view.size.x <= 0 or view.size.y <= 0:
		return Vector2.ZERO
	var s: float = minf(control_size.x / float(view.size.x), control_size.y / float(view.size.y))
	return Vector2(s, s)


static func view_offset(control_size: Vector2, view: Rect2i, scale: Vector2) -> Vector2:
	return Vector2(
		(control_size.x - float(view.size.x) * scale.x) * 0.5,
		(control_size.y - float(view.size.y) * scale.y) * 0.5)


static func world_to_screen(cell: Vector2, control_size: Vector2, view: Rect2i) -> Vector2:
	var s := view_scale(control_size, view)
	var o := view_offset(control_size, view, s)
	return Vector2(
		o.x + (cell.x - float(view.position.x)) * s.x,
		o.y + (cell.y - float(view.position.y)) * s.y)


static func screen_to_world(local_pos: Vector2, control_size: Vector2, view: Rect2i, world_size: Vector2i) -> Vector2i:
	# Group-3 outside-image rejection, camera-aware: letterbox clicks OUTSIDE
	# the image are rejected pre-clamp (clamping would paint border cells
	# from the margins).
	var s := view_scale(control_size, view)
	if s.x <= 0.0:
		return OUTSIDE
	var o := view_offset(control_size, view, s)
	var fx: float = float(view.position.x) + (local_pos.x - o.x) / s.x
	var fy: float = float(view.position.y) + (local_pos.y - o.y) / s.y
	if fx < float(view.position.x) or fy < float(view.position.y):
		return OUTSIDE
	if fx >= float(view.end.x) or fy >= float(view.end.y):
		return OUTSIDE
	if fx < 0.0 or fy < 0.0 or fx >= float(world_size.x) or fy >= float(world_size.y):
		return OUTSIDE
	return Vector2i(int(floor(fx)), int(floor(fy)))

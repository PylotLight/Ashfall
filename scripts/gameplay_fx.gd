extends Control
## Screen-space-only sprites for moving tool bolts and impact feedback.
## All impacts are visuals: the controller has already edited native cells.
var shots: Array[Dictionary] = []
var impacts: Array[Dictionary] = []
var cursor_pos := Vector2.ZERO
var exit_pos := Vector2.ZERO
var show_cursor := false

func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	z_index = 21

func set_frame(new_shots: Array[Dictionary], new_impacts: Array[Dictionary], cursor: Vector2, exit_marker: Vector2) -> void:
	shots = new_shots
	impacts = new_impacts
	cursor_pos = cursor
	exit_pos = exit_marker
	show_cursor = Rect2(Vector2.ZERO, size).has_point(cursor)
	queue_redraw()

func _draw() -> void:
	if Rect2(Vector2.ZERO, size).has_point(exit_pos):
		var portal := Rect2(exit_pos - Vector2(10.0, 21.0), Vector2(20.0, 42.0))
		draw_rect(portal.grow(4.0), Color(0.12, 0.55, 0.61, 0.10), true)
		draw_rect(portal, Color(0.36, 0.86, 0.89, 0.24), true)
		draw_rect(Rect2(portal.position, Vector2(3.0, portal.size.y)), Color(0.53, 0.91, 0.87, 0.95), true)
		draw_rect(Rect2(portal.position + Vector2(portal.size.x - 3.0, 0.0), Vector2(3.0, portal.size.y)), Color(0.53, 0.91, 0.87, 0.95), true)
		draw_arc(exit_pos + Vector2(0.0, -21.0), 10.0, PI, TAU, 16, Color(0.64, 0.96, 0.91, 0.95), 3.0, true)
		draw_string(ThemeDB.fallback_font, exit_pos + Vector2(-14.0, 31.0), "EXIT", HORIZONTAL_ALIGNMENT_LEFT, -1.0, 10, Color(0.75, 0.95, 0.9, 0.95))
	for shot in shots:
		var p: Vector2 = shot["pos"]
		var dir: Vector2 = shot["dir"]
		draw_line(p - dir * 12.0, p, Color(0.22, 0.75, 0.91, 0.45), 2.0, true)
		draw_circle(p, 4.0, Color(0.46, 0.87, 1.0, 0.95))
		draw_circle(p, 1.8, Color(0.89, 0.98, 1.0, 1.0))
	for impact in impacts:
		var p: Vector2 = impact["pos"]
		var phase: float = clampf(float(impact["age"]) / 0.34, 0.0, 1.0)
		var radius: float = lerpf(3.0, 18.0, phase)
		var color := Color(0.54, 0.88, 1.0, 1.0 - phase)
		draw_arc(p, radius, 0.0, TAU, 20, color, 2.0, true)
		draw_circle(p, 2.5 * (1.0 - phase), Color(0.88, 0.97, 1.0, 1.0 - phase))
	if show_cursor:
		draw_arc(cursor_pos, 7.0, 0.0, TAU, 16, Color(0.83, 0.89, 0.78, 0.55), 1.0, true)

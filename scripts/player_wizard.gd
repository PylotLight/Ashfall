extends ColorRect

## Presentation-only avatar glyph for the cave slice. The authoritative body
## remains the controller's cell-space box; this deliberately crisp overlay
## gives the player readable shape at the camera's pixel scale.

const INK := Color("#17131d")
const CLOAK := Color("#563648")
const CLOAK_LIGHT := Color("#92533e")
const CLOAK_DARK := Color("#342638")
const HOOD := Color("#242131")
const FACE := Color("#e6aa72")
const GOLD := Color("#ffc869")
const GLOW := Color(1.0, 0.35, 0.08, 0.24)

var facing_right := true

func _ready() -> void:
	color = Color(0.0, 0.0, 0.0, 0.0)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	z_index = 20

func set_facing_right(value: bool) -> void:
	if facing_right == value:
		return
	facing_right = value
	queue_redraw()

func _draw() -> void:
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var sx := size.x / 24.0
	var sy := size.y / 36.0
	var origin := Vector2.ZERO if facing_right else Vector2(size.x, 0.0)
	var scale := Vector2(sx, sy) if facing_right else Vector2(-sx, sy)
	draw_set_transform(origin, 0.0, scale)

	# Small ember and wand tip first, so the glow sits behind the silhouette.
	draw_circle(Vector2(22.0, 5.0), 5.0, GLOW)
	draw_line(Vector2(16.0, 19.0), Vector2(22.0, 7.0), INK, 3.0, true)
	draw_line(Vector2(16.0, 19.0), Vector2(22.0, 7.0), CLOAK_LIGHT, 1.2, true)
	draw_circle(Vector2(22.0, 5.0), 2.6, GOLD)
	draw_circle(Vector2(22.0, 5.0), 1.1, Color("#fff3c4"))

	# Boots and the broad, tapered travel cloak.
	draw_rect(Rect2(6.0, 31.0, 5.0, 3.0), INK)
	draw_rect(Rect2(14.0, 31.0, 5.0, 3.0), INK)
	draw_colored_polygon(PackedVector2Array([
		Vector2(7.0, 15.0), Vector2(16.0, 14.0), Vector2(19.0, 31.0),
		Vector2(4.0, 31.0), Vector2(5.0, 23.0),
	]), INK)
	draw_colored_polygon(PackedVector2Array([
		Vector2(8.0, 16.0), Vector2(15.0, 15.0), Vector2(17.0, 29.0),
		Vector2(6.0, 29.0), Vector2(7.0, 22.0),
	]), CLOAK)
	draw_colored_polygon(PackedVector2Array([
		Vector2(8.0, 17.0), Vector2(10.0, 18.0), Vector2(8.0, 28.0),
		Vector2(6.0, 28.0),
	]), CLOAK_LIGHT)
	draw_rect(Rect2(7.0, 27.0, 10.0, 2.0), CLOAK_DARK)
	draw_rect(Rect2(10.0, 27.0, 3.0, 2.0), GOLD)

	# Face, hood, and a bright cheek mark remain legible at small sizes.
	draw_rect(Rect2(9.0, 8.0, 8.0, 9.0), INK)
	draw_rect(Rect2(11.0, 10.0, 5.0, 6.0), FACE)
	draw_rect(Rect2(14.0, 12.0, 2.0, 2.0), CLOAK_DARK)
	draw_colored_polygon(PackedVector2Array([
		Vector2(7.0, 12.0), Vector2(8.0, 5.0), Vector2(13.0, 2.0),
		Vector2(19.0, 6.0), Vector2(20.0, 13.0), Vector2(16.0, 10.0),
		Vector2(11.0, 10.0),
	]), INK)
	draw_colored_polygon(PackedVector2Array([
		Vector2(9.0, 10.0), Vector2(10.0, 6.0), Vector2(13.0, 4.0),
		Vector2(17.0, 7.0), Vector2(17.0, 11.0), Vector2(14.0, 9.0),
		Vector2(11.0, 9.0),
	]), HOOD)
	draw_rect(Rect2(12.0, 8.0, 4.0, 2.0), CLOAK_LIGHT)
	draw_rect(Rect2(16.0, 13.0, 1.0, 1.0), Color("#fff1ca"))

	draw_set_transform(Vector2.ZERO, 0.0, Vector2.ONE)

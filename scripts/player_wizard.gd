extends ColorRect

## Crisp overlay avatar. The controller owns the cell-space body and collision;
## this glyph provides a readable silhouette and a wand that follows mouse aim.
const INK := Color("#17131d")
const CLOAK := Color("#563648")
const CLOAK_LIGHT := Color("#92533e")
const CLOAK_DARK := Color("#342638")
const HOOD := Color("#242131")
const FACE := Color("#e6aa72")
const GOLD := Color("#ffc869")
const GLOW := Color(1.0, 0.35, 0.08, 0.24)
var facing_right := true
var aim_direction := Vector2.RIGHT

func _ready() -> void:
	color = Color(0.0, 0.0, 0.0, 0.0)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	z_index = 20

func set_facing_right(value: bool) -> void:
	if facing_right == value:
		return
	facing_right = value
	queue_redraw()

func set_aim_direction(value: Vector2) -> void:
	if value.length_squared() < 0.001:
		return
	aim_direction = value.normalized()
	facing_right = aim_direction.x >= 0.0
	queue_redraw()

func _draw() -> void:
	if size.x <= 0.0 or size.y <= 0.0:
		return
	var sx := size.x / 24.0
	var sy := size.y / 36.0
	var origin := Vector2.ZERO if facing_right else Vector2(size.x, 0.0)
	var scale := Vector2(sx, sy) if facing_right else Vector2(-sx, sy)
	draw_set_transform(origin, 0.0, scale)
	var local_aim := Vector2(absf(aim_direction.x), aim_direction.y).normalized()
	var wand_base := Vector2(16.0, 19.0)
	var wand_tip := wand_base + local_aim * 11.0
	draw_circle(wand_tip, 5.0, GLOW)
	draw_line(wand_base, wand_tip, INK, 3.0, true)
	draw_line(wand_base, wand_tip, CLOAK_LIGHT, 1.2, true)
	draw_circle(wand_tip, 2.6, GOLD)
	draw_circle(wand_tip, 1.1, Color("#fff3c4"))

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

	# Face, hood, and bright cheek mark remain legible at small sizes.
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

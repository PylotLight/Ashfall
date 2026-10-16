extends SceneTree

# Visual quality gate (R9): six fixed viewpoints captured bloom-off at
# 1280x720 through the same xvfb harness as the P4A sign-off. Review the
# gate-*.png frames side by side against the reference board; no commit
# that makes them worse ships.
# Run: xvfb-run -a godot --audio-driver Dummy --path . --resolution 1280x720 --windowed --script tests/godot_visual_gate.gd
const CAPTURE_DIR := "res://docs/perf/captures"
const VIEWPOINTS: Array = [
	{"name": "gate-spawn", "player": Vector2(44.0, 80.0)},
	{"name": "gate-mouth", "player": Vector2(140.0, 120.0)},
	{"name": "gate-chamber", "player": Vector2(218.0, 200.0)},
	{"name": "gate-spring", "player": Vector2(350.0, 250.0)},
	{"name": "gate-east", "player": Vector2(420.0, 310.0)},
	{"name": "gate-exit", "player": Vector2(477.0, 340.0)},
]
var _failures: int = 0

func _init() -> void:
	_run.call_deferred()

func check(condition: bool, message: String) -> void:
	if condition:
		print("PASS: ", message)
	else:
		_failures += 1
		push_error("FAIL: " + message)

func _run() -> void:
	var packed: PackedScene = load("res://cave_benchmark.tscn")
	if packed == null:
		push_error("FAIL: playable cave scene loads")
		quit(1)
		return
	var main = packed.instantiate()
	root.add_child(main)
	await process_frame
	await process_frame
	var bloom = main.get_node_or_null("BloomToggle")
	if bloom != null:
		bloom.set_bloom_enabled(false)
	check(root.size == Vector2i(1280, 720), "visual gate viewport is exactly 1280x720 (actual %s)" % root.size)
	main.world.auto_step = false
	main._reset_cave_run()
	for i in 60:
		main.world.step()
	main.world.flush_present()
	for vp in VIEWPOINTS:
		var vname: String = vp["name"]
		main.player_pos = vp["player"]
		main.player_velocity = Vector2.ZERO
		main._snap_camera()
		main._update_marker()
		main.world.flush_present()
		main._refresh_textures()
		var path := "%s/2026-10-16-%s-bloom-off.png" % [CAPTURE_DIR, vname]
		check(await _capture(path), "visual gate frame saved: %s" % vname)
		check(_frame_has_range(path), "visual gate frame is not flat fog: %s" % vname)
	main.queue_free()
	if _failures > 0:
		push_error("VISUAL GATE FAILED: %d" % _failures)
		quit(1)
	else:
		print("--- VISUAL GATE CAPTURES COMPLETE ---")
		quit(0)

func _capture(path: String) -> bool:
	await process_frame
	await RenderingServer.frame_post_draw
	var image: Image = root.get_texture().get_image()
	if image == null or image.get_width() != 1280 or image.get_height() != 720:
		push_error("Capture viewport mismatch: %s" % ("null" if image == null else str(image.get_size())))
		return false
	var absolute_dir := ProjectSettings.globalize_path(CAPTURE_DIR)
	if DirAccess.make_dir_recursive_absolute(absolute_dir) != OK and not DirAccess.dir_exists_absolute(absolute_dir):
		push_error("Unable to create capture directory: " + absolute_dir)
		return false
	var absolute_path := ProjectSettings.globalize_path(path)
	if image.save_png(absolute_path) != OK:
		push_error("Unable to save capture: " + absolute_path)
		return false
	print("CAPTURE: %s (%dx%d)" % [absolute_path, image.get_width(), image.get_height()])
	return true

# Coarse readability probe: sample a grid over the world viewport area and
# require real luminance spread. Catches flat-fog regressions without
# asserting exact pixels (llvmpipe vs Mac GPU differ legitimately).
func _frame_has_range(path: String) -> bool:
	var absolute_path := ProjectSettings.globalize_path(path)
	var image: Image = Image.load_from_file(absolute_path)
	if image == null:
		return false
	var lo := 1.0
	var hi := 0.0
	for y in range(60, 660, 24):
		for x in range(60, 1000, 24):
			var c: Color = image.get_pixel(x, y)
			var lum: float = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b
			lo = minf(lo, lum)
			hi = maxf(hi, lum)
	return (hi - lo) > 0.12

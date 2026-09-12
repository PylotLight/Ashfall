class_name CaveBloomToggle
extends WorldEnvironment

## P3A Slice C, task C3 — restrained low-res bloom toggle (presentation-only).
##
## Conventional Godot 2D bloom: a scene-local WorldEnvironment whose
## Environment enables glow at reduced resolution, low strength, and a high
## HDR threshold so only emission-hot pixels bloom with a tight halo that
## must not wash out material boundaries. The curated defaults live on the
## Environment sub-resource in cave_benchmark.tscn; this script only flips
## the runtime flag.
##
## Sim-free by construction (Slice D2 contract): this file never references
## AshfallWorld, the bridge, engine, data, or any controller — it touches
## only its own `environment` glow flag. Toggling bloom therefore cannot
## alter sim bytes or the tick by construction.
##
## Toggle: `set_bloom_enabled(bool)` / `toggle_bloom()` / `is_bloom_enabled()`,
## plus KEY_B at runtime (scene-local input; the cave controller is untouched).

@export var bloom_enabled: bool = true


func _ready() -> void:
	_apply()


func set_bloom_enabled(value: bool) -> void:
	bloom_enabled = value
	_apply()


func toggle_bloom() -> bool:
	bloom_enabled = not bloom_enabled
	_apply()
	return bloom_enabled


func is_bloom_enabled() -> bool:
	return bloom_enabled


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.is_pressed() and not event.is_echo():
		if event.keycode == KEY_B:
			toggle_bloom()
			get_viewport().set_input_as_handled()


func _apply() -> void:
	if environment != null:
		environment.glow_enabled = bloom_enabled

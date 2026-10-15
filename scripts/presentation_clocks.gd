extends RefCounted

# P3A Slice D, task D2 — presentation clocks: world clock vs UI clock.
#
# Presentation-only (no sim rules, no scene/controller/shader/engine/bridge/
# data edits — callers preload this file and drive it from their own slice;
# D2 proves the separation headless in tests/godot_presentation_toggles_verify.gd).
#
# Design (review §5 animation policy):
# - WORLD clock drives world animation (shader present_tick/present_phase via
#   apply_to_material). It advances ONLY while the sim is running
#   (running = world.is_auto_step()); pause freezes it exactly — no time
#   accumulates, so resume continues the identical phase. present_tick is
#   always the authoritative sim tick (read by the caller, never stored or
#   advanced here), so world animation is tick-driven, never wall-clock.
# - UI clock drives interface motion only (HUD pulses, button feedback).
#   It advances on every frame regardless of pause — UI stays alive while
#   the world is frozen. It is never fed to any world shader.
# - No blanket TIME: world shaders receive only the explicit (tick, phase)
#   pair this clock supplies. The water_fx.gdshader TIME shimmer predates
#   P3A and is out of D2 scope (no shader edits per region lock).
# - Pause/replay: pause (auto_step off) freezes world_time + phase
#   deterministically — the same pause/resume delta sequence always yields
#   the same (tick, phase). Replay/capture pins exact values with
#   set_forced(tick, phase) (used for forced-tick captures, Slice E2);
#   clear_forced() resumes live. reset() zeroes both clocks on world
#   rebuild (R key) so a fresh world never inherits stale phase.
# - Sim-free by construction: this file never references AshfallWorld, the
#   bridge, engine, data, or any controller — tick_world/tick_ui take plain
#   floats/bools, present/apply take the tick as an int. Clock motion
#   therefore cannot alter sim bytes or the tick by construction.
#
# --- D1/D2 interface (convention: each worker owns its own block below;
# never edit the other worker's block) ---
# [D2-owned: clocks — this file]
#   D2 supplies the raw (tick, phase) pair: present(sim_tick) returns
#   {"tick": int, "phase": float in [0,1)}; apply_to_material(sm, sim_tick)
#   writes exactly the shader uniforms "present_tick" (int) and
#   "present_phase" (float). Phase advances at TICK_HZ while running and
#   freezes on pause/forced. D2 never touches shader animation math.
# [D1-owned: shader animation math — RESERVED, lives in D1's files]
#   (D1 worker fills this in; D2 does not edit below this line.)
#
# Wiring (if a controller routes toggles through the world clock, a few
# lines suffice — none required: clocks live entirely in this script):
#   clocks.tick_world(delta, world.is_auto_step())
#   clocks.tick_ui(delta)
#   clocks.apply_to_material(shader_material, world.get_tick())

# Sim tick rate the phase interpolates within (matches set_sim_rate_hz(60)).
const TICK_HZ: float = 60.0

var _world_time: float = 0.0
var _ui_time: float = 0.0
var _phase: float = 0.0
var _forced: bool = false
var _forced_tick: int = 0
var _forced_phase: float = 0.0


## Fresh world (rebuild): stale phase must never leak into a new run.
func reset() -> void:
	_world_time = 0.0
	_phase = 0.0
	_forced = false
	_forced_tick = 0
	_forced_phase = 0.0


## Advance the world clock. Frozen exactly unless running (pause freeze is
## deterministic: identical delta sequences give identical phase).
func tick_world(delta: float, running: bool) -> void:
	if _forced:
		return
	if running and delta > 0.0:
		_world_time += delta
		_phase = fposmod(_phase + delta * TICK_HZ, 1.0)


## Advance the UI clock. Always runs — interface stays alive while paused.
func tick_ui(delta: float) -> void:
	if delta > 0.0:
		_ui_time += delta


## The raw (tick, phase) pair for the world shaders. Pinned while forced.
func present(sim_tick: int) -> Dictionary:
	if _forced:
		return {"tick": _forced_tick, "phase": _forced_phase}
	return {"tick": sim_tick, "phase": _phase}


## Pin exact values for forced-tick captures (Slice E2 replay). While
## forced, tick_world is a no-op and present returns the pins.
func set_forced(tick: int, phase: float) -> void:
	_forced = true
	_forced_tick = tick
	_forced_phase = clampf(phase, 0.0, 1.0)


func clear_forced() -> void:
	_forced = false


func is_forced() -> bool:
	return _forced


func world_time() -> float:
	return _world_time


func ui_time() -> float:
	return _ui_time


## Write the world-clock pair onto a profile-shader material. Touches only
## the ShaderMaterial's uniforms — never the sim (tick arrives as an int).
func apply_to_material(sm: ShaderMaterial, sim_tick: int) -> void:
	var p := present(sim_tick)
	sm.set_shader_parameter("present_tick", int(p["tick"]))
	sm.set_shader_parameter("present_phase", float(p["phase"]))

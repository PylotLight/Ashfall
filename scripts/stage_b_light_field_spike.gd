class_name StageBLightFieldSpike
extends RefCounted

## P3A Slice S, task S1 — Stage-B coarse light-field SPIKE (PROTOTYPE ONLY).
##
## Coarse camera-local light field fed by the SAME cluster input as Stage A
## (LightClusterManager.extract_clusters + merge_clusters over the
## material_visuals.json emission map). Timeboxed prototype: CPU-side field,
## no shader application, no scene wiring, FLAG-GATED DEFAULT OFF.
##
## MUST-NOT constraint: this file only READS the Stage-A path (static calls
## into light_cluster_manager.gd, the controller's live _emissive dict). It
## edits no existing light/cluster/shader/controller/scene file, the cave
## controller never loads it, and enabling it cannot change sim bytes or the
## Stage-A pixel path (proven by scripts/stage_b_spike_probe.gd).
##
## Design (resolution / propagation / masking / cost):
##   - Resolution: ~1 texel per 4x4 cells (CELL_PER_TEXEL) over the camera
##     view rect + MARGIN_CELLS, clamped to the world. Origin is snapped DOWN
##     to multiples of CELL_PER_TEXEL, so texels are world-locked: no
##     sub-texel swimming while the camera drifts inside the margin, and no
##     alignment to the 64-cell dirty-chunk grid (chunk seams impossible by
##     construction — proven by the mirror test in the probe).
##   - Sources: merged Stage-A clusters seed the containing texel with
##     LightClusterManager.target_energy_for(weight) x cluster color, so both
##     stages speak the same brightness language. Accumulates, clamped.
##   - Masking: a texel is solid when its block-center cell is solid
##     (out-of-bounds reads solid). Solid texels pin 0 and never relay, so
##     walls cast hard shadows. Approximation: 1-cell walls whose center the
##     4x4 sampling misses can leak — stated prototype limitation.
##   - Propagation: PROPAGATION_PASSES relaxations of
##     new = max(old, max(4-neighbors) * FALLOFF) over open texels only.
##     Bounded travel = PASSES texels = TRAVEL_CELLS cells — proven by the
##     bound test in the probe.
##   - Cost: texels x passes x 3 channels of GDScript relax per rebuild, plus
##     one RGB8 upload (w*h*3 bytes) at rebuild cadence. Stage A by contrast
##     uploads 0 bytes (6 PointLight2D nodes, GPU-side falloff).

const ClusterMgr = preload("res://scripts/light_cluster_manager.gd")

const CELL_PER_TEXEL: int = 4
const MARGIN_CELLS: int = 32
const PROPAGATION_PASSES: int = 6
const PROPAGATION_FALLOFF: float = 0.72
const SPLAT_CAP: float = 4.0
const TRAVEL_CELLS: int = PROPAGATION_PASSES * CELL_PER_TEXEL
const ENABLED_DEFAULT: bool = false

var enabled: bool = ENABLED_DEFAULT

var _ox: int = 0
var _oy: int = 0
var _w: int = 0
var _h: int = 0
var _field: PackedColorArray = PackedColorArray()
var _solid: PackedByteArray = PackedByteArray()
var _rebuilds: int = 0
var _last_build_us: int = 0
var _last_seed_energy: float = 0.0
var _last_max_energy: float = 0.0


## Camera-local field rect: view + margin, origin snapped to the texel grid
## (world-locked texels), clamped to the world. Pure; unit-drivable.
static func field_rect_for_view(view: Rect2i, world_size: Vector2i) -> Rect2i:
	if view.size.x <= 0 or view.size.y <= 0:
		return Rect2i(0, 0, 0, 0)
	var x0: int = maxi(0, view.position.x - MARGIN_CELLS)
	var y0: int = maxi(0, view.position.y - MARGIN_CELLS)
	var x1: int = mini(world_size.x, view.end.x + MARGIN_CELLS)
	var y1: int = mini(world_size.y, view.end.y + MARGIN_CELLS)
	x0 = (x0 / CELL_PER_TEXEL) * CELL_PER_TEXEL
	y0 = (y0 / CELL_PER_TEXEL) * CELL_PER_TEXEL
	if x1 <= x0 or y1 <= y0:
		return Rect2i(0, 0, 0, 0)
	return Rect2i(x0, y0, x1 - x0, y1 - y0)


## Texel count for a field rect. Pure.
static func field_resolution(rect: Rect2i) -> Vector2i:
	if rect.size.x <= 0 or rect.size.y <= 0:
		return Vector2i.ZERO
	return Vector2i(
		(rect.size.x + CELL_PER_TEXEL - 1) / CELL_PER_TEXEL,
		(rect.size.y + CELL_PER_TEXEL - 1) / CELL_PER_TEXEL)


func set_enabled(v: bool) -> void:
	enabled = v


## Rebuild the field over rect from merged Stage-A clusters. Presentation
## math only — never touches the world or any scene node. Returns metrics.
func rebuild(world: AshfallWorld, clusters: Array, rect: Rect2i) -> Dictionary:
	var t0: int = Time.get_ticks_usec()
	var res := field_resolution(rect)
	_ox = rect.position.x
	_oy = rect.position.y
	_w = res.x
	_h = res.y
	_field = PackedColorArray()
	_field.resize(_w * _h)
	_solid = PackedByteArray()
	_solid.resize(_w * _h)
	var ww: int = world.get_world_width()
	var wh: int = world.get_world_height()
	var solid_texels: int = 0
	for ty in _h:
		for tx in _w:
			var cx: int = _ox + tx * CELL_PER_TEXEL + CELL_PER_TEXEL / 2
			var cy: int = _oy + ty * CELL_PER_TEXEL + CELL_PER_TEXEL / 2
			var blocked: bool = cx < 0 or cy < 0 or cx >= ww or cy >= wh or world.is_solid_cell(cx, cy)
			if blocked:
				_solid[ty * _w + tx] = 1
				solid_texels += 1
	var seed_energy: float = 0.0
	for c in clusters:
		var cd: Dictionary = c
		var p: Vector2 = cd["pos"]
		var tx: int = int(floor((p.x - float(_ox)) / float(CELL_PER_TEXEL)))
		var ty: int = int(floor((p.y - float(_oy)) / float(CELL_PER_TEXEL)))
		if tx < 0 or ty < 0 or tx >= _w or ty >= _h:
			continue
		if _solid[ty * _w + tx] == 1:
			continue
		var e: float = ClusterMgr.target_energy_for(float(cd["weight"]))
		var col: Color = cd["color"]
		var i: int = ty * _w + tx
		var cur: Color = _field[i]
		_field[i] = Color(
			minf(cur.r + col.r * e, SPLAT_CAP),
			minf(cur.g + col.g * e, SPLAT_CAP),
			minf(cur.b + col.b * e, SPLAT_CAP))
		seed_energy += e
	_last_seed_energy = seed_energy
	for p in PROPAGATION_PASSES:
		_relax_pass()
	var max_e: float = 0.0
	for i in _field.size():
		var c: Color = _field[i]
		max_e = maxf(max_e, maxf(c.r, maxf(c.g, c.b)))
	_last_max_energy = max_e
	_last_build_us = int(Time.get_ticks_usec() - t0)
	_rebuilds += 1
	return {
		"texels": _w * _h,
		"field_w": _w,
		"field_h": _h,
		"upload_bytes_rgb8": _w * _h * 3,
		"passes": PROPAGATION_PASSES,
		"travel_cells": TRAVEL_CELLS,
		"solid_texels": solid_texels,
		"seed_energy": seed_energy,
		"max_energy": max_e,
		"build_us": _last_build_us,
		"rebuilds": _rebuilds,
	}


func _relax_pass() -> void:
	var next := _field.duplicate()
	for ty in _h:
		for tx in _w:
			var i: int = ty * _w + tx
			if _solid[i] == 1:
				next[i] = Color(0, 0, 0)
				continue
			var best := _field[i]
			if tx > 0 and _solid[i - 1] == 0:
				best = _maxc(best, _field[i - 1] * PROPAGATION_FALLOFF)
			if tx + 1 < _w and _solid[i + 1] == 0:
				best = _maxc(best, _field[i + 1] * PROPAGATION_FALLOFF)
			if ty > 0 and _solid[i - _w] == 0:
				best = _maxc(best, _field[i - _w] * PROPAGATION_FALLOFF)
			if ty + 1 < _h and _solid[i + _w] == 0:
				best = _maxc(best, _field[i + _w] * PROPAGATION_FALLOFF)
			next[i] = best
	_field = next


static func _maxc(a: Color, b: Color) -> Color:
	return Color(maxf(a.r, b.r), maxf(a.g, b.g), maxf(a.b, b.b))


## Field sample at a cell coord (OOB / empty field reads black).
func sample_cell(cell: Vector2i) -> Color:
	if _w <= 0 or _h <= 0:
		return Color(0, 0, 0)
	var tx: int = int(floor((float(cell.x) - float(_ox)) / float(CELL_PER_TEXEL)))
	var ty: int = int(floor((float(cell.y) - float(_oy)) / float(CELL_PER_TEXEL)))
	if tx < 0 or ty < 0 or tx >= _w or ty >= _h:
		return Color(0, 0, 0)
	return _field[ty * _w + tx]


## Toggle-state + metrics introspection for the probe (presentation only).
func debug_state() -> Dictionary:
	return {
		"enabled": enabled,
		"texels": _w * _h,
		"rebuilds": _rebuilds,
		"last_build_us": _last_build_us,
		"seed_energy": _last_seed_energy,
		"max_energy": _last_max_energy,
		"travel_cells": TRAVEL_CELLS,
	}

class_name LightClusterManager
extends RefCounted

## P3A Slice C, task C2 — Stage-A clustered lights (presentation-only).
##
## Groups nearby emissive cells by coarse tile into clusters
## (representative pos/color/bounded intensity), merges them under a FIXED
## light budget, and smooths slot transitions so lights glide/fade instead of
## popping. The controller drives one Godot PointLight2D per slot — never one
## light per flame cell.
##
## Cluster input comes from `data/material_visuals.json` emission fields
## (via MaterialVisuals): only materials with emission strength > 0 count.
## Hot water must not glow — water's emission strength is 0, so temperature
## never enters this path.
##
## Solids receive light while blocking transmission by construction of the
## anchor: every cluster position is snapped to open space (nearest non-solid
## cell), so a light never sits inside stone/rock rendering a lit-black
## wall. (No occluders in P3A; radii stay bounded so leak stays local.)
##
## Budget + merge policy (fixed, stated here so S1 can reuse the input):
##   MAX_LIGHTS = 6 PointLight2Ds, no more, ever.
##   Merge: sort clusters by weight (cells x emission strength) descending;
##   fold each cluster into a kept light within MERGE_RADIUS_CELLS
##   (weighted-average pos, weight-summed, color blended by weight); spill
##   over the budget folds into the nearest kept light. Large lava fields
##   therefore degrade to a few bright representatives, never more nodes.
##   Intensity: energy = clamp(weight / REF_WEIGHT, 0, 1) * MAX_ENERGY with
##   a MIN_ACTIVE_ENERGY floor while a slot holds a target; slots that lose
##   their target decay to 0 (fade-out, never a hard cut).
##
## Cadence: extraction (the full get_cell scan, ~10 ms on 512x512) runs at
## EXTRACT_INTERVAL; per-frame update() only eases slots toward targets.
## Sim is never touched — presentation only.

const MAX_LIGHTS: int = 6
const TILE: int = 16
const EXTRACT_INTERVAL: float = 0.25
const SMOOTH_RATE: float = 5.0
const MERGE_RADIUS_CELLS: float = 40.0
const REF_WEIGHT: float = 250.0
const MAX_ENERGY: float = 1.5
const MIN_ACTIVE_ENERGY: float = 0.30
const FADE_ENERGY: float = 0.02
const SNAP_RADIUS: int = 16

# Slot: {"pos": Vector2 (cell space), "color": Color, "energy": float,
#         "weight": float, "count": int, "has_target": bool,
#         "target_pos": Vector2, "target_color": Color, "target_energy": float,
#         "target_weight": float}
var _slots: Array = []
var _emissive: Dictionary = {}
var _accum: float = 0.0
var _extracts: int = 0
var _last_cluster_count: int = 0


## Emissive map: {material_id: {"color": Color, "strength": float}} for
## strength > 0 only. seed_pos is cell space (e.g. the fire-pit anchor) so
## slot 0 shows the warm pool before the first extraction lands.
func setup(emissive: Dictionary, seed_pos: Vector2, seed_color: Color) -> void:
	_emissive = emissive
	_slots.clear()
	for i in MAX_LIGHTS:
		_slots.append({
			"pos": seed_pos,
			"color": seed_color,
			"energy": 0.0,
			"weight": 0.0,
			"count": 0,
			"has_target": false,
			"target_pos": seed_pos,
			"target_color": seed_color,
			"target_energy": 0.0,
			"target_weight": 0.0,
		})
	# Slot 0 opens on the seed (the guaranteed warm pool); the rest fade in
	# from 0 as real clusters claim them.
	_slots[0]["energy"] = MIN_ACTIVE_ENERGY
	_accum = EXTRACT_INTERVAL


## Per-frame drive. Returns the slot array (cell-space Dictionaries);
## the controller maps pos through world_to_display onto its light nodes.
func update(world: AshfallWorld, delta: float) -> Array:
	if _slots.is_empty():
		return _slots
	_accum += delta
	if _accum >= EXTRACT_INTERVAL:
		_accum = 0.0
		_retarget(world)
	var t: float = 1.0 - exp(-SMOOTH_RATE * maxf(delta, 0.0))
	for s in _slots:
		var slot: Dictionary = s
		if bool(slot["has_target"]):
			slot["pos"] = (slot["pos"] as Vector2).lerp(slot["target_pos"] as Vector2, t)
			slot["color"] = (slot["color"] as Color).lerp(slot["target_color"] as Color, t)
			slot["energy"] = lerpf(float(slot["energy"]), float(slot["target_energy"]), t)
			slot["weight"] = lerpf(float(slot["weight"]), float(slot["target_weight"]), t)
		else:
			slot["energy"] = lerpf(float(slot["energy"]), 0.0, t)
			slot["weight"] = lerpf(float(slot["weight"]), 0.0, t)
	return _slots


## Small introspection hook for the C2 proof script (counts, budget cap,
## merge behavior, smoothing) — presentation state only, never sim.
func debug_state() -> Dictionary:
	var active: int = 0
	for s in _slots:
		if float((s as Dictionary)["energy"]) > FADE_ENERGY:
			active += 1
	return {
		"slots": _slots.size(),
		"active": active,
		"extracts": _extracts,
		"last_clusters": _last_cluster_count,
		"budget": MAX_LIGHTS,
	}


func _retarget(world: AshfallWorld) -> void:
	var clusters: Array = extract_clusters(world, _emissive)
	_last_cluster_count = clusters.size()
	var w: int = world.get_world_width()
	var h: int = world.get_world_height()
	var targets: Array = merge_clusters(clusters)
	for t in targets:
		var td: Dictionary = t
		td["pos"] = snap_to_open(world, td["pos"] as Vector2, w, h)
		td["energy"] = target_energy_for(float(td["weight"]))
	# Greedy nearest-slot assignment so lights glide between regions
	# instead of swapping identities (no popping on reorder).
	var claimed: Array = []
	for i in _slots.size():
		claimed.append(false)
	for t in targets:
		var td: Dictionary = t
		var best: int = -1
		var best_d: float = INF
		for i in _slots.size():
			if claimed[i]:
				continue
			var d: float = ((_slots[i] as Dictionary)["pos"] as Vector2).distance_squared_to(td["pos"] as Vector2)
			if d < best_d:
				best_d = d
				best = i
		if best >= 0:
			claimed[best] = true
			(_slots[best] as Dictionary)["has_target"] = true
			(_slots[best] as Dictionary)["target_pos"] = td["pos"]
			(_slots[best] as Dictionary)["target_color"] = td["color"]
			(_slots[best] as Dictionary)["target_energy"] = td["energy"]
			(_slots[best] as Dictionary)["target_weight"] = td["weight"]
			(_slots[best] as Dictionary)["count"] = int(td["count"])
	for i in _slots.size():
		if not claimed[i]:
			(_slots[i] as Dictionary)["has_target"] = false
			(_slots[i] as Dictionary)["target_energy"] = 0.0
			(_slots[i] as Dictionary)["target_weight"] = 0.0
	_extracts += 1


static func target_energy_for(weight: float) -> float:
	if weight <= 0.0:
		return 0.0
	return maxf(MIN_ACTIVE_ENERGY, clampf(weight / REF_WEIGHT, 0.0, 1.0) * MAX_ENERGY)


## Coarse-tile grouping: one cluster per TILE x TILE tile that holds any
## emissive cell. Representative pos = emission-weighted centroid,
## color = strength-blended emission color, weight = sum(strength),
## count = emissive cells. Full get_cell scan (~10 ms on 512x512 at
## EXTRACT_INTERVAL cadence — never per-frame per-cell nodes).
static func extract_clusters(world: AshfallWorld, emissive: Dictionary) -> Array:
	var out: Array = []
	if emissive.is_empty():
		return out
	var w: int = world.get_world_width()
	var h: int = world.get_world_height()
	if w <= 0 or h <= 0:
		return out
	var ty: int = 0
	while ty < h:
		var tx: int = 0
		while tx < w:
			var count: int = 0
			var weight: float = 0.0
			var sx: float = 0.0
			var sy: float = 0.0
			var cr: float = 0.0
			var cg: float = 0.0
			var cb: float = 0.0
			var y1: int = mini(ty + TILE, h)
			var x1: int = mini(tx + TILE, w)
			for y in range(ty, y1):
				for x in range(tx, x1):
					var m: int = world.get_cell(x, y)
					if not emissive.has(m):
						continue
					var e: Dictionary = emissive[m]
					var st: float = float(e["strength"])
					var ec: Color = e["color"]
					count += 1
					weight += st
					sx += float(x) * st
					sy += float(y) * st
					cr += ec.r * st
					cg += ec.g * st
					cb += ec.b * st
			if count > 0:
				out.append({
					"pos": Vector2(sx / weight, sy / weight),
					"color": Color(cr / weight, cg / weight, cb / weight),
					"weight": weight,
					"count": count,
				})
			tx += TILE
		ty += TILE
	return out


## Fixed-budget merge (pure; unit-drivable without a scene). Sort by weight
## desc, fold neighbors within MERGE_RADIUS_CELLS, fold budget spill into
## the nearest kept light. Returns <= MAX_LIGHTS clusters.
static func merge_clusters(clusters: Array) -> Array:
	var sorted: Array = clusters.duplicate()
	sorted.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return float(a["weight"]) > float(b["weight"]))
	var kept: Array = []
	for c in sorted:
		var cd: Dictionary = c
		var best: int = -1
		var best_d: float = MERGE_RADIUS_CELLS
		for i in kept.size():
			var d: float = ((kept[i] as Dictionary)["pos"] as Vector2).distance_to(cd["pos"] as Vector2)
			if d < best_d:
				best_d = d
				best = i
		if best >= 0:
			_fold_into(kept[best] as Dictionary, cd)
		elif kept.size() < MAX_LIGHTS:
			kept.append({
				"pos": cd["pos"],
				"color": cd["color"],
				"weight": float(cd["weight"]),
				"count": int(cd["count"]),
			})
		else:
			# Budget spill: nearest kept light absorbs, no new node ever.
			var ni: int = 0
			var nd: float = INF
			for i in kept.size():
				var d: float = ((kept[i] as Dictionary)["pos"] as Vector2).distance_squared_to(cd["pos"] as Vector2)
				if d < nd:
					nd = d
					ni = i
			_fold_into(kept[ni] as Dictionary, cd)
	return kept


static func _fold_into(kept: Dictionary, other: Dictionary) -> void:
	var w0: float = float(kept["weight"])
	var w1: float = float(other["weight"])
	var total: float = w0 + w1
	if total <= 0.0:
		return
	kept["pos"] = ((kept["pos"] as Vector2) * w0 + (other["pos"] as Vector2) * w1) / total
	kept["color"] = (kept["color"] as Color) * (w0 / total) + (other["color"] as Color) * (w1 / total)
	kept["weight"] = total
	kept["count"] = int(kept["count"]) + int(other["count"])


## Open-space anchor: lights never sit inside solids (never lit-black
## stone). If the centroid is solid, spiral out to the nearest non-solid
## cell within SNAP_RADIUS; out-of-bounds reads as solid.
static func snap_to_open(world: AshfallWorld, pos: Vector2, w: int, h: int) -> Vector2:
	var cx: int = roundi(pos.x)
	var cy: int = roundi(pos.y)
	if _is_open(world, cx, cy, w, h):
		return Vector2(float(cx), float(cy))
	for r in range(1, SNAP_RADIUS + 1):
		for dy in range(-r, r + 1):
			for dx in range(-r, r + 1):
				if maxi(absi(dx), absi(dy)) != r:
					continue
				if _is_open(world, cx + dx, cy + dy, w, h):
					return Vector2(float(cx + dx), float(cy + dy))
	return Vector2(float(clampi(cx, 0, w - 1)), float(clampi(cy, 0, h - 1)))


static func _is_open(world: AshfallWorld, x: int, y: int, w: int, h: int) -> bool:
	if x < 0 or y < 0 or x >= w or y >= h:
		return false
	return not world.is_solid_cell(x, y)

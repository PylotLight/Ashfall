class_name MaterialVisuals
extends RefCounted

## P3A Slice B1 — presentation-owned material visual profiles (data-only).
##
## Loads `data/material_visuals.json`: per-material render profiles keyed by
## stable material NAME (never table index), mirroring the Group-3 hardened
## JSON conventions from the engine loaders (materials/reactions):
## parse -> validate -> link/apply.
##
## - parse: strict JSON via Godot's parser; syntax failures carry line info.
## - validate: strict unknown-key rejection at every level, closed enums,
##   numeric ranges, resource limits. Every schema failure names its path
##   (`visuals[3].palette[1]: ...`, `(root): ...` for the document object).
## - link: profile names resolve against the caller-supplied material roster
##   (normally the live table names, as reactions resolve against the loaded
##   material table). Unknown names reject; missing coverage rejects.
##
## Boot-safe fallback: ANY failure returns `ok == false` with a named `error`
## and `profiles` filled with safe defaults for every roster entry — never a
## boot failure. `load_from_file` additionally logs loudly (missing file or
## rejected content), matching the main_controller data-file pattern.
##
## Result Dictionary: `{ "ok": bool, "error": String, "profiles": Dictionary }`
## where `profiles[material_name]` is the NORMALIZED profile:
## `{ "material": String, "palette": Array[Color], "pattern": String,
##    "pattern_scale": int, "detail_strength": float, "edge_response": float,
##    "opacity": float, "opacity_mode": String,
##    "emission": { "color": Color, "strength": float },
##    "animation": String }`.
##
## NOTE (GDScript JSON limitation): Godot's JSON parser decodes every number
## as float, so `4` and `4.0` are indistinguishable. Integral floats are
## accepted for integer fields (pattern_scale, version); non-integral values
## reject. Duplicate object keys resolve last-wins per Godot JSON (the Group-3
## C++ parser rejects them; this presentation loader cannot observe them).

const SCHEMA_VERSION: int = 1
# Resource limits (mirror the Group-3 budget style: bounded input, bounded
# rows, bounded strings — this file never drives sim allocation, but a bad
# file must still fail fast instead of building garbage).
const MAX_INPUT_BYTES: int = 1 << 20
const MAX_ENTRIES: int = 64
const MAX_STRING_CHARS: int = 256

const PATTERNS: Array[String] = ["strata", "grain", "clusters", "quiet", "plume", "crust"]
const OPACITY_MODES: Array[String] = ["opaque", "translucent", "additive"]
const ANIMATIONS: Array[String] = ["static", "flame", "ember", "flow", "wisp"]

const MIN_PALETTE: int = 2
const MAX_PALETTE: int = 4
const PATTERN_SCALE_MIN: int = 1
const PATTERN_SCALE_MAX: int = 16
const EMISSION_STRENGTH_MAX: float = 4.0

const _ENTRY_KEYS: Array[String] = ["material", "palette", "pattern", "pattern_scale",
	"detail_strength", "edge_response", "opacity", "opacity_mode", "emission", "animation"]
const _EMISSION_KEYS: Array[String] = ["color", "strength"]


## File entry point. Missing/unreadable file => safe defaults + loud warning.
## Rejected content => safe defaults + loud error. Never fails boot.
static func load_from_file(path: String, known_names: Array) -> Dictionary:
	var roster: Array[String] = _to_roster(known_names)
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		var defaults := defaults_for(roster)
		push_warning("MaterialVisuals: file \"%s\" missing/unreadable, using safe defaults" % path)
		return {"ok": false, "error": "file \"%s\" missing or unreadable" % path, "profiles": defaults}
	var text: String = f.get_as_text()
	f.close()
	var result := load_from_text(text, roster)
	if not bool(result["ok"]):
		push_error("MaterialVisuals: \"%s\" rejected (%s), using safe defaults" % [path, String(result["error"])])
	return result


## Text entry point: parse -> validate -> link. Always returns profiles
## (normalized on success, safe defaults on failure).
static func load_from_text(text: String, known_names: Array) -> Dictionary:
	var roster: Array[String] = _to_roster(known_names)
	if text.to_utf8_buffer().size() > MAX_INPUT_BYTES:
		return _fail(roster, "limit: input exceeds %d bytes" % MAX_INPUT_BYTES)
	var parser := JSON.new()
	if parser.parse(text) != OK:
		return _fail(roster, "line %d: %s" % [parser.get_error_line(), parser.get_error_message()])
	var root: Variant = parser.data
	if typeof(root) != TYPE_DICTIONARY:
		return _fail(roster, "top-level value must be an object")
	var doc: Dictionary = root
	var key_error := _reject_unknown_keys(doc, "(root)", ["version", "visuals"])
	if not key_error.is_empty():
		return _fail(roster, key_error)
	if not doc.has("version"):
		return _fail(roster, "(root).version: missing (want %d)" % SCHEMA_VERSION)
	if not _is_int_number(doc["version"]) or int(doc["version"]) != SCHEMA_VERSION:
		return _fail(roster, "(root).version: unsupported data version (want %d)" % SCHEMA_VERSION)
	if not doc.has("visuals"):
		return _fail(roster, "\"visuals\" must be a non-empty array")
	var visuals: Variant = doc["visuals"]
	if typeof(visuals) != TYPE_ARRAY or (visuals as Array).is_empty():
		return _fail(roster, "\"visuals\" must be a non-empty array")
	var rows: Array = visuals
	if rows.size() > MAX_ENTRIES:
		return _fail(roster, "limit: visuals array exceeds %d entries" % MAX_ENTRIES)
	var profiles: Dictionary = {}
	var seen: Array[String] = []
	for i in rows.size():
		var row := "visuals[%d]" % i
		var entry: Variant = rows[i]
		if typeof(entry) != TYPE_DICTIONARY:
			return _fail(roster, row + ": visual entry must be an object")
		var profile := {}
		var perr := _parse_entry(entry, row, profile)
		if not perr.is_empty():
			return _fail(roster, perr)
		var mat_name: String = profile["material"]
		if seen.has(mat_name):
			return _fail(roster, row + ": duplicate material \"" + mat_name + "\"")
		seen.append(mat_name)
		profiles[mat_name] = profile
	# Link stage: every profile name must be a known material (mirrors the
	# reaction link check that rejects unknown material refs / dead tags).
	for i in rows.size():
		var row := "visuals[%d]" % i
		# NOTE: rows[i] passed _parse_entry above, so "material" is a String.
		var rd: Dictionary = rows[i]
		var mat_name: String = rd["material"]
		if not roster.has(mat_name):
			return _fail(roster, row + ": unknown material \"" + mat_name + "\"")
	# Link stage: full coverage — every known material needs a profile.
	var missing: Array[String] = []
	for n in roster:
		if not profiles.has(n):
			missing.append(n)
	if not missing.is_empty():
		return _fail(roster, "missing visuals for material(s): " + ", ".join(missing))
	return {"ok": true, "error": "", "profiles": profiles}


## Safe defaults for every roster entry (flat opaque mid-grey: renders as a
## readable solid, never transparent, never emissive, never animated).
static func defaults_for(known_names: Array) -> Dictionary:
	var roster: Array[String] = _to_roster(known_names)
	var out := {}
	for n in roster:
		out[n] = default_profile(n)
	return out


static func default_profile(material_name: String) -> Dictionary:
	return {
		"material": material_name,
		"palette": [Color(0.5, 0.5, 0.5), Color(0.6, 0.6, 0.6)],
		"pattern": "quiet",
		"pattern_scale": 2,
		"detail_strength": 0.1,
		"edge_response": 0.2,
		"opacity": 1.0,
		"opacity_mode": "opaque",
		"emission": {"color": Color(0, 0, 0), "strength": 0.0},
		"animation": "static",
	}


# --- internals -------------------------------------------------------------

static func _fail(roster: Array[String], error: String) -> Dictionary:
	return {"ok": false, "error": error, "profiles": defaults_for(roster)}


static func _to_roster(known_names: Array) -> Array[String]:
	var roster: Array[String] = []
	for n in known_names:
		roster.append(String(n))
	return roster


static func _reject_unknown_keys(obj: Dictionary, path: String, allowed: Array) -> String:
	for k in obj.keys():
		if not allowed.has(String(k)):
			return path + ": unknown key \"" + String(k) + "\""
	return ""


static func _is_int_number(v: Variant) -> bool:
	if typeof(v) == TYPE_INT:
		return true
	if typeof(v) == TYPE_FLOAT:
		var f: float = v
		return f == floor(f) and absf(f) < 9007199254740992.0
	return false


static func _is_number(v: Variant) -> bool:
	return typeof(v) == TYPE_INT or typeof(v) == TYPE_FLOAT


static func _is_identifier(s: String) -> bool:
	# Choice A identifiers, mirroring the engine loaders: [a-z][a-z0-9_-]*.
	if s.is_empty() or s.length() > MAX_STRING_CHARS:
		return false
	var first: int = s.unicode_at(0)
	if first < 97 or first > 122:
		return false
	for i in range(1, s.length()):
		var c: int = s.unicode_at(i)
		var ok := (c >= 97 and c <= 122) or (c >= 48 and c <= 57) or c == 95 or c == 45
		if not ok:
			return false
	return true


static func _is_hex_color(s: String) -> bool:
	if s.length() != 7 or not s.begins_with("#"):
		return false
	for i in range(1, 7):
		var c: int = s.unicode_at(i)
		var hex := (c >= 48 and c <= 57) or (c >= 65 and c <= 70) or (c >= 97 and c <= 102)
		if not hex:
			return false
	return true


static func _parse_entry(entry: Dictionary, row: String, out_profile: Dictionary) -> String:
	var ke := _reject_unknown_keys(entry, row, _ENTRY_KEYS)
	if not ke.is_empty():
		return ke
	# material: required, Choice A identifier (link stage checks roster).
	if not entry.has("material") or typeof(entry["material"]) != TYPE_STRING:
		return row + ".material: material \"material\" must be a non-empty string"
	var mat_name: String = entry["material"]
	if mat_name.is_empty():
		return row + ".material: material \"material\" must be a non-empty string"
	if mat_name.length() > MAX_STRING_CHARS:
		return row + ".material: string exceeds %d chars" % MAX_STRING_CHARS
	if not _is_identifier(mat_name):
		return row + ".material: \"" + mat_name + "\" is not a valid identifier (want [a-z][a-z0-9_-]*)"
	# palette: 2-4 hex colors.
	if not entry.has("palette") or typeof(entry["palette"]) != TYPE_ARRAY:
		return row + ".palette: palette must be an array of %d..%d hex colors" % [MIN_PALETTE, MAX_PALETTE]
	var pal: Array = entry["palette"]
	if pal.size() < MIN_PALETTE or pal.size() > MAX_PALETTE:
		return row + ".palette: palette must hold %d..%d colors (got %d)" % [MIN_PALETTE, MAX_PALETTE, pal.size()]
	var colors: Array = []
	for c in pal.size():
		if typeof(pal[c]) != TYPE_STRING or not _is_hex_color(String(pal[c])):
			return row + ".palette[%d]: color must be \"#rrggbb\" hex" % c
		colors.append(Color(String(pal[c])))
	# pattern: closed enum.
	if not entry.has("pattern") or typeof(entry["pattern"]) != TYPE_STRING or not PATTERNS.has(String(entry["pattern"])):
		return row + ".pattern: unknown pattern (want " + "/".join(PATTERNS) + ")"
	# pattern_scale: int 1..16.
	if not entry.has("pattern_scale") or not _is_int_number(entry["pattern_scale"]):
		return row + ".pattern_scale: must be an integer %d..%d" % [PATTERN_SCALE_MIN, PATTERN_SCALE_MAX]
	var pscale: int = int(entry["pattern_scale"])
	if pscale < PATTERN_SCALE_MIN or pscale > PATTERN_SCALE_MAX:
		return row + ".pattern_scale: must be an integer %d..%d" % [PATTERN_SCALE_MIN, PATTERN_SCALE_MAX]
	# 0..1 floats.
	for f in ["detail_strength", "edge_response", "opacity"]:
		if not entry.has(f) or not _is_number(entry[f]):
			return row + "." + f + ": must be a number 0..1"
		var fv: float = float(entry[f])
		if fv < 0.0 or fv > 1.0:
			return row + "." + f + ": must be a number 0..1"
	# opacity_mode: closed enum.
	if not entry.has("opacity_mode") or typeof(entry["opacity_mode"]) != TYPE_STRING or not OPACITY_MODES.has(String(entry["opacity_mode"])):
		return row + ".opacity_mode: unknown mode (want " + "/".join(OPACITY_MODES) + ")"
	# emission: exactly {color, strength}.
	if not entry.has("emission") or typeof(entry["emission"]) != TYPE_DICTIONARY:
		return row + ".emission: emission must be an object {color, strength}"
	var em: Dictionary = entry["emission"]
	var eke := _reject_unknown_keys(em, row + ".emission", _EMISSION_KEYS)
	if not eke.is_empty():
		return eke
	if not em.has("color") or typeof(em["color"]) != TYPE_STRING or not _is_hex_color(String(em["color"])):
		return row + ".emission.color: color must be \"#rrggbb\" hex"
	if not em.has("strength") or not _is_number(em["strength"]):
		return row + ".emission.strength: must be a number 0..4"
	var estr: float = float(em["strength"])
	if estr < 0.0 or estr > EMISSION_STRENGTH_MAX:
		return row + ".emission.strength: must be a number 0..4"
	# animation: closed enum.
	if not entry.has("animation") or typeof(entry["animation"]) != TYPE_STRING or not ANIMATIONS.has(String(entry["animation"])):
		return row + ".animation: unknown profile (want " + "/".join(ANIMATIONS) + ")"
	out_profile["material"] = mat_name
	out_profile["palette"] = colors
	out_profile["pattern"] = String(entry["pattern"])
	out_profile["pattern_scale"] = pscale
	out_profile["detail_strength"] = float(entry["detail_strength"])
	out_profile["edge_response"] = float(entry["edge_response"])
	out_profile["opacity"] = float(entry["opacity"])
	out_profile["opacity_mode"] = String(entry["opacity_mode"])
	out_profile["emission"] = {"color": Color(String(em["color"])), "strength": estr}
	out_profile["animation"] = String(entry["animation"])
	return ""

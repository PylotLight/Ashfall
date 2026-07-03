class_name SimBrushOp
extends Resource

enum Mode { SET_MATERIAL, ERASE, DIG, PLACE }

@export var mode: Mode = Mode.SET_MATERIAL
@export var material_id: int = 0
@export var respect_solid_boundaries := true
@export var affects_dynamic_materials := true

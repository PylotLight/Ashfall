extends RefCounted

# Play HUD vs debug panel separation — P3A Slice A, Task A2.
#
# Play view (sidebar) keeps: title, the InfoBox play HUD (tick / cells /
# status / material / player / fps-frame-sim), the material palette, and the
# help text. Sim-control widgets (play-pause, single-step, reset, save,
# load) move into a hidden PopupPanel = the debug panel. Key shortcuts keep
# working while it is hidden.
#
# Implemented by reparenting (A2 region lock: no scene-file edits — scenes
# stay byte-identical). Signal connections survive the move, and the
# controller's @onready refs were resolved before the move. Every lookup is
# guarded with get_node_or_null so a scene that follows the
# main_controller.gd convention but omits a widget still boots — this is the
# coordination contract with the parallel A1 cave scene.

# Sidebar child names that count as debug controls, in panel order.
const DEBUG_WIDGET_NAMES: PackedStringArray = [
	"ActionsHeader",
	"PlayBtn",
	"StepBtn",
	"ResetBtn",
	"SaveBtn",
	"LoadBtn",
]

const OPENER_NAME: StringName = &"DebugToggleBtn"
const OPENER_TEXT: String = "[F1] Debug..."


static func build_debug_panel(main: Node, sidebar: VBoxContainer) -> PopupPanel:
	var panel := PopupPanel.new()
	panel.name = "DebugPanel"
	main.add_child(panel)
	panel.hide()
	var box := VBoxContainer.new()
	box.name = "DebugBox"
	box.add_theme_constant_override("separation", 4)
	panel.add_child(box)
	var moved: int = 0
	for widget_name in DEBUG_WIDGET_NAMES:
		var w: Node = sidebar.get_node_or_null(NodePath(widget_name))
		if w == null:
			continue
		sidebar.remove_child(w)
		box.add_child(w)
		moved += 1
	if moved == 0:
		var empty := Label.new()
		empty.name = "DebugEmpty"
		empty.text = "(no debug controls in this scene)"
		box.add_child(empty)
	# Play-view chrome: a slim opener stays in the sidebar so the hidden
	# panel is discoverable without cluttering the play view.
	if sidebar.get_node_or_null(NodePath(OPENER_NAME)) == null:
		var opener := Button.new()
		opener.name = OPENER_NAME
		opener.text = OPENER_TEXT
		sidebar.add_child(opener)
		var help: Node = sidebar.get_node_or_null(NodePath("HelpText"))
		if help != null:
			sidebar.move_child(opener, help.get_index())
	return panel


static func toggle_debug_panel(panel: PopupPanel) -> void:
	if panel == null:
		return
	if panel.visible:
		panel.hide()
	else:
		panel.popup_centered()

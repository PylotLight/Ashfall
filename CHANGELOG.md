# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Implemented `cursor_preview.gd` providing a visual cursor preview clamped to interaction range.
- Added `get_collision_mask_region` and `sync_collision_region` methods to `sim_backend.gd`.
- Added support for brush size adjustments (using `[` and `]` keys) in `input_router.gd`.
- Pre-populated the simulation grid with a small hand-crafted test chamber layout.
- Added file serialization to `save_manager.gd` with versioned save format mapping F5/F9 keys.

### Changed
- Refined player movement with coyote time, jump buffering, acceleration, variable jump heights, and max fall speeds in `player.gd`.
- Updated `debug_overlay.gd` to display player velocity, grounded state, and current brush radius.
- Restructured `main.tscn` to include the `CursorPreview` node.

### Added (Previous)
- Created `sim_brush_op.gd` resource class defining simulation brushes.
- Created `sim_backend.gd` encapsulating compute shader pipeline, cell/world coordinate conversions, and CPU collision grid mirror.
- Created `sim_display.gd` to render the simulation texture with canvas_items nearest filter and 1:1 scale mapping.
- Created `player.gd` character controller using `CharacterBody2D` with custom grid AABB collision resolution on X/Y axes.
- Created `camera_rig.gd` offering smooth follow, zoom in/out, and clamping to sim grid boundaries.
- Created `debug_overlay.gd` displaying FPS, coordinates, active tools, and pause status.
- Created `input_router.gd` routing key bindings for movement, jumping, zoom, pause, and tool selection.
- Created `save_manager.gd` providing JSON serialization/deserialization stubs.
- Created `game_state.gd` managing simulation tick rate and pause cycles.

### Changed
- Restructured `main.tscn` to use the new modular GameRoot node and component scripts.
- Configured renderer explicitly to `forward_plus` and updated display viewport resolution settings in `project.godot`.

### Removed
- Removed obsolete single-file feasibility test script `main.gd`.

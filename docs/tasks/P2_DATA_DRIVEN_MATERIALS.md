# P2 — Data-Driven Materials and Reactions

## Goal & Exit Criterion

Load material definitions and reaction definitions from versioned data. Add
gases, compact per-cell state, temperature, fire, and reaction execution.
Introduce activation/sleeping for chunks only when profiling demonstrates a
need.

**Exit criterion**: a data-only material/reaction addition works through the
native core, renderer, save format, and deterministic tests without adding a
special material branch.

---

## Phased Task Breakdown

### Phase P2.1: Versioned Material Data + Loader (no sim change)
- **Status**: Ready for Execution
- **Ownership**: `engine/include/ashfall/material_data.hpp`, `engine/src/material_data.cpp`, `data/materials.json`, `tests/simulation_tests.cpp`
- **Description**: Strict JSON-subset parser (objects, arrays, strings, ints,
  bools; no floats, no deps) plus the versioned material schema (`version`,
  `materials[]` with `id`/`name`/`movement`/`solid`/`density`/`color`) and a
  validating loader. The simulation keeps its hardcoded table; this phase only
  adds the data library, the canonical `data/materials.json`, and tests.
- **Tasks**:
  1. Parser: `parse_material_data(text, table, error)` with line/col errors.
  2. Schema validation: `version == 1`; id 0 exists (air: static, non-solid,
     density 0); unique ids/names; id fits uint8; `movement` in
     {static, powder, liquid}; `solid` implies static; density 0..255; color
     3x 0..255.
  3. Canonical `data/materials.json` matching legacy air/stone/sand/water
     (ids, movement, solidity, density, renderer base colors).
  4. Deterministic tests: valid table parses; each malformed input rejected
     (bad JSON, unknown movement, duplicate id/name, missing air, solid
     powder, density/color out of range, wrong version).
  5. CMake: compile `material_data.cpp` into `ashfall_sim`.

### Phase P2.2: Engine Table from Data
- **Status**: Pending P2.1
- **Ownership**: `engine/`, `bridge/`
- **Description**: `SimulationWorld` builds its material table from a loaded
  `MaterialTable` instead of hardcoded `emplace` calls. Default construction
  preserves legacy behavior byte-for-byte. The Godot host reads
  `res://data/materials.json` via FileAccess and passes the string to the
  bridge (`load_materials`), so no engine code depends on working directory.
- **Tasks**:
  1. `SimulationWorld` constructor overload / loader taking `MaterialTable`;
     `set_cell`/`fill_rect` validate against loaded ids.
  2. Movement dispatch stays on the `Movement` enum (table-driven, no id
     branches).
  3. Bridge `load_materials(json_string)` + host `_ready` wiring; bridge
     material enum stays as named constants for the default ids.
  4. Tests: table with reordered ids behaves identically; unknown id
     rejected; all P1 suites green unchanged.

### Phase P2.3: Renderer Palette from Data
- **Status**: Pending P2.2
- **Ownership**: `bridge/`, `scripts/`
- **Description**: Base-layer colors and material names come from the loaded
  table. Water depth shading keys off the `liquid` movement class, not the
  water id. Sidebar palette buttons are generated from the material list so a
  data-only addition is paintable.
- **Tasks**:
  1. Bridge exposes material count/name/color; `paint_cell` uses table colors
     + grain; liquid shading by movement class.
  2. Sidebar builds palette buttons dynamically; keyboard shortcuts map in
     table order.
  3. Visual sim + headless verifies cover a data-driven color change.

### Phase P2.4: Per-Cell Temperature State
- **Status**: Pending P2.2
- **Ownership**: `engine/`
- **Description**: Compact temperature storage (sparse: only non-ambient cells
  stored, so the hot path and memory profile of ambient worlds are unchanged)
  with deterministic conduction, exposed via cell-state queries. Save format
  bump with explicit version/migration decision per `docs/AGENT_GUIDE.md`.
- **Tasks**:
  1. Sparse temp map + `set_temperature`/`temperature` + conduction rule.
  2. Save v2 carries temps (or explicit reset decision, documented).
  3. Tests: conduction converges, determinism, save round-trip, perf note.

### Phase P2.5: Gas Movement, Fire, Reaction Execution
- **Status**: Pending P2.4
- **Ownership**: `engine/`, `data/reactions.json`
- **Description**: `gas` movement (rises, disperses, dissipates at open sky),
  data-defined `fire`, and a data-defined reaction rule format (neighbor
  match + temperature gate + transform) executed deterministically in `step`.
  Proof reaction: fire heats fuel, water quenches fire to steam (gas), steam
  condenses/cools back to water.
- **Tasks**:
  1. Gas movement in step dispatch (table-driven, no id branch).
  2. Reaction schema + executor (deterministic order, mass-conserving).
  3. Fire + steam defined purely in `data/`; play-level fire demo area.
  4. Tests for each reaction step (ignite, quench, condense) + mass balance.

### Phase P2.6: P2 Verification & Exit Criterion Validation
- **Status**: Pending P2.5
- **Ownership**: Whole repo
- **Description**: Add one NEW material + reaction via data only (e.g. oil:
  flammable liquid; burns when hot) touching no C++ branch, and prove it
  end to end: core behavior test, renderer color, save/load round-trip,
  paintable in the host UI.
- **Tasks**:
  1. Data-only oil + burn reaction; tests green.
  2. Full suites (C++ sim, P0/P1/P2 headless verifies, visual sim, boot).
  3. `CHANGELOG.md` + sign off P2 exit criterion, establish P3 readiness.

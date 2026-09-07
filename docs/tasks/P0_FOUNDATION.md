# P0 — Native Bridge and Visible Reference World

## Goal & Exit Criterion

Build a `godot-cpp` GDExtension that owns one `SimulationWorld`, steps it at a
fixed rate, and presents a chunk snapshot to Godot.

**Exit criterion**: Launch Godot, instantiate a native 256 by 256 test world,
modify cells through the bridge, step it, and visibly observe the native
result. No simulation rule is implemented in GDScript.

---

## Phased Task Breakdown

### Phase P0.1: Engine Chunk Snapshot & Query API
- **Status**: Complete
- **Ownership**: `engine/` & `tests/`
- **Description**: Add public methods to `SimulationWorld` allowing caller systems to query active chunks and copy chunk / rect data into contiguous buffers without exposing internal containers or Godot types.
- **Tasks**:
  1. Add `bool read_chunk(CellPosition chunk_pos, std::span<MaterialId> out_buffer) const` to `SimulationWorld`.
  2. Add `bool has_chunk(CellPosition chunk_pos) const`.
  3. Add `std::vector<CellPosition> active_chunks() const`.
  4. Add `void copy_rect(CellPosition min_pos, CellPosition max_pos, std::span<MaterialId> out_buffer) const`.
  5. Add deterministic unit tests in `tests/simulation_tests.cpp`.

### Phase P0.2: GDExtension Build Infrastructure
- **Status**: Complete
- **Ownership**: Build system (`CMakeLists.txt`, `ashfall.gdextension`, `godot-cpp`)
- **Description**: Integrate `godot-cpp` bindings and configure build outputs for dynamic libraries.
- **Tasks**:
  1. Add `godot-cpp` as a submodule/dependency.
  2. Configure `CMakeLists.txt` to compile `ashfall_extension` shared library.
  3. Author `ashfall.gdextension` manifest linking platform dynamic libraries.

### Phase P0.3: Native Bridge Class (`AshfallWorld`)
- **Status**: Complete
- **Ownership**: `bridge/`
- **Description**: Expose an `AshfallWorld` Godot Node that owns a private `ashfall::SimulationWorld` instance.
- **Tasks**:
  1. Implement `AshfallWorld` inheriting from `godot::Node2D`.
  2. Expose `step()`, `set_cell()`, `get_cell()`, `fill_rect()`, `get_tick()`, `get_occupied_cell_count()`.
  3. Provide `get_world_image(int width, int height)` or texture snapshot renderer converting native material IDs to color pixels.
  4. Register module initialization and class binding in `register_types.cpp`.

### Phase P0.4: Godot Host Presentation & Reference Scene
- **Status**: Complete
- **Ownership**: `project.godot`, `main.tscn`, `scripts/main_controller.gd`
- **Description**: Construct the reference 256x256 test world scene.
- **Tasks**:
  1. Set up `main.tscn` with `AshfallWorld` and display canvas (`TextureRect`).
  2. Implement `scripts/main_controller.gd` to handle user input (placing materials, stepping simulation, pausing).
  3. Verify strictly no simulation rules in GDScript.

### Phase P0.5: Validation, Verification & CHANGELOG
- **Status**: Complete
- **Ownership**: Whole repo
- **Description**: Verify deterministic headless tests and visual interactive execution against P0 exit criterion.
- **Tasks**:
  1. Run `ctest` simulation test suite (100% passing).
  2. Verify Godot headless load and execution (`tests/godot_headless_verify.gd` passing).
  3. Update `CHANGELOG.md` under `[Unreleased]`.

---

## Exit Criterion Verification & Evidence

| Criterion Item | Requirement | Verification Result | Evidence / Artifact |
| :--- | :--- | :--- | :--- |
| **Launch Godot** | Godot project opens and loads cleanly | **PASS** | `main.tscn` runs headlessly and interactively via `/opt/homebrew/bin/godot` with zero errors. |
| **Instantiate 256x256 test world** | Native `SimulationWorld` allocated at 256x256 | **PASS** | `AshfallWorld::init_world(256, 256)` instantiates backing C++ simulation world and allocates 256x256 buffer. Verified in `tests/godot_headless_verify.gd`. |
| **Modify cells through bridge** | GDExtension bridge exposes modification API | **PASS** | `set_cell()`, `fill_rect()`, palette material mapping verified via both interactive UI and GDScript test harness. |
| **Step simulation** | Native fixed-tick simulation advances | **PASS** | `step()` advances tick count and executes native cellular automaton steps (sand falls downward, water spreads). Verified in `tests/simulation_tests.cpp` and `tests/godot_headless_verify.gd`. |
| **Visibly observe native result** | Bridge generates visual snapshot for Godot rendering | **PASS** | `update_world_texture()` transfers native cell IDs into RGBA8 `Image` / `ImageTexture` displayed in `TextureRect`. |
| **Zero simulation rules in GDScript** | GDScript must not contain simulation logic | **PASS** | `scripts/main_controller.gd` only forwards UI input and displays HUD values; all simulation state and mutation rules are 100% in C++. |

### Sign-Off
- **P0 Status**: **COMPLETE & VERIFIED**
- **Readiness for P1**: **READY TO PROCEED**


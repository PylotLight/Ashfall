# P1 — Deterministic Cellular World

## Goal & Exit Criterion

Complete air, stone, sand, and water in the C++ core. Add chunk-boundary,
mass-conservation, settling, command replay, and save/load tests. Replace the
test snapshot with dirty-chunk presentation.

**Exit criterion**: A player can alter sand and water in a hand-authored scene;
stone blocks movement; quitting and loading restores the exact authoritative
world and player state.

---

## Phased Task Breakdown

### Phase P1.1: Complete Core Cellular Simulation Dynamics
- **Status**: Not Started
- **Ownership**: `engine/include/ashfall/simulation.hpp`, `engine/src/simulation.cpp`
- **Description**: Finish four core materials (Air, Stone, Sand, Water) in the C++ simulation core.
- **Tasks**:
  1. Sand dynamics: falling, sliding down diagonals when obstructed, stacking at natural angle of repose.
  2. Water dynamics: downward flow, horizontal spreading across flat surfaces, filling basins, equalizing heights.
  3. Multi-material density interactions: sand falling through water to settle at the bottom; water displacing upward into air.
  4. Ensure consistent chunk boundary transitions without directional bias or seam artifacts.

### Phase P1.2: Dirty-Chunk Tracking & Incremental Presentation
- **Status**: Not Started
- **Ownership**: `engine/`, `bridge/`
- **Description**: Replace the full 256x256 monolithic rect copy with fine-grained dirty chunk tracking.
- **Tasks**:
  1. Add dirty flags or dirty chunk list to `SimulationWorld` set during step mutations and external cell edits.
  2. Clear dirty flags after queries/ticks.
  3. Expose dirty chunk query API to `AshfallWorld` (`get_dirty_chunks()`).
  4. Update Godot bridge to perform partial/dirty chunk updates rather than monolithic world texture regeneration.

### Phase P1.3: Deterministic Test Suite & Replay
- **Status**: Not Started
- **Ownership**: `tests/simulation_tests.cpp`
- **Description**: Author comprehensive deterministic tests verifying cellular automaton invariants.
- **Tasks**:
  1. **Chunk boundary tests**: Test sand and water moving seamlessly across positive and negative chunk boundaries.
  2. **Mass conservation tests**: Verify that total count of sand and water cells remains strictly invariant over $N$ simulation steps in enclosed containers.
  3. **Settling tests**: Verify that sand and water eventually reach steady state (quiescence / 0 active movements) in stable basins.
  4. **Command replay tests**: Log sequence of cell modifications / brush inputs and verify bitwise-identical world state when replayed from tick 0.
  5. **Save/load tests**: Test serialization and deserialization of active worlds with varied topology, verifying exact bitwise match.

### Phase P1.4: Hand-Authored Scene, Player Avatar & Stone Collision
- **Status**: Not Started
- **Ownership**: `engine/`, `bridge/`, `scripts/`, `scenes/`
- **Description**: Introduce a minimal authoritative player position/controller where stone blocks movement, in a hand-authored scene.
- **Tasks**:
  1. Implement native spatial query / point & AABB check (`is_passable(pos)`, `can_move_to(box)`).
  2. Add player movement handling in Godot host respecting native stone collision.
  3. Allow player avatar to alter sand and water in real time (e.g. dig sand, release water).
  4. Create a hand-authored level layout showcasing terrain, stone walls/obstacles, sand dunes, and water reservoirs.

### Phase P1.5: Authoritative Persistence (Save / Load) Integration
- **Status**: Not Started
- **Ownership**: `bridge/`, `scripts/`
- **Description**: Wire world and player persistence through GDExtension to Godot save files.
- **Tasks**:
  1. Expose `save_to_file(path)` and `load_from_file(path)` (or byte buffer methods) on `AshfallWorld`.
  2. Include player state (position, inventory/held material) in save metadata alongside authoritative `SimulationWorld` state.
  3. Add Save & Load actions in the UI/keyboard controls.
  4. Verify quitting and reloading restores the exact authoritative world and player state.

### Phase P1.6: P1 Verification & Exit Criterion Validation
- **Status**: Not Started
- **Ownership**: Whole repo
- **Description**: Validate all P1 requirements against exit criterion and document results.
- **Tasks**:
  1. Execute all C++ deterministic unit and scenario tests.
  2. Execute Godot automated/headless verification script testing player movement, stone blocking, sand/water modification, and save/load state identity.
  3. Update `CHANGELOG.md` under `[Unreleased]`.
  4. Sign off on P1 exit criterion and establish readiness for P2.

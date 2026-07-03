# Sand MVP — Compute Shader Feasibility Test

A minimal Godot 4 project that simulates falling sand/water via a GPU compute
shader, to test whether the compute-shader approach from the design doc is
actually viable before building anything bigger on it.

**I wrote this without being able to run Godot myself** (no GUI/network in my
sandbox) — treat it as a well-reasoned first draft, not a verified build. If
it throws an error, paste it back to me and I'll fix it.

## Setup (macOS)

1. Download Godot **4.3+** for macOS from godotengine.org (get the arm64
   build if you're on Apple Silicon).
2. Open Godot → Import → select the `project.godot` file in this folder.
3. **Before pressing Play**, check Project Settings → Rendering → Renderer.
   It must be `Forward+` or `Mobile`. If it says `Compatibility`, compute
   shaders won't work at all (plain OpenGL, no RenderingDevice) — the
   `project.godot` here already sets `forward_plus`, but double-check if
   anything goes wrong.
4. Press Play (F5). You should see a black grid with a stone floor.
   - **Left click** = drop sand
   - **Right click** = pour water
   - **Middle click** = erase

## What's actually being tested

The compute shader (`sand_sim.glsl`) uses a **pull-based update**: instead of
a cell pushing itself into a neighbor (which races when two cells target the
same destination), every destination cell asks "which neighbor, reading only
last frame's frozen state, would deterministically move into me?" Every GPU
thread computes that identically, so there's no duplication/loss without any
locks or atomics. That's the part worth stress-testing — it's the same
technique a full game would need at scale.

## How to actually run the feasibility test

The MVP intentionally simulates the *entire* grid every substep, with no
chunk/dirty-rect optimization (Section 3.1 of the design doc). That's the
right baseline: it tells you the worst case, and how much headroom chunking
would buy you later.

Methodology:
1. Watch the FPS counter at the default `GRID_W/GRID_H = 256`. Should be
   trivially high (hundreds of fps) — this is a sanity check, not the test.
2. Increase `GRID_W`/`GRID_H` in `main.gd` (try 512, then 1024, then 2048)
   and re-run. Note where FPS drops below 60, then below 30.
3. Increase `SUBSTEPS_PER_FRAME` (simulating faster than render rate, which
   real games do) and watch how that scales alongside grid size.
4. That FPS-vs-cell-count curve is your actual answer to "is this compute
   shader approach fast enough" — and tells you how urgently you'd need
   chunk-based dirty tracking once you're building the real thing.

## Known limitations (intentional, for MVP scope)

- No chunking — brute-force full-grid dispatch every substep.
- No fire, gas, temperature, or rigid-body extraction — just sand/water/stone.
- Mouse painting can overwrite the boundary walls (paints STONE tiles into
  something else) — cosmetic issue only, not a correctness bug in the sim.
- Water has no pressure/volume equalization, just the same fall+spread rule
  as sand with an extra horizontal option — visually close enough to test
  the architecture, not tuned for "correct" fluid behavior.

## Troubleshooting

- **"No RenderingDevice available"** → Renderer is set to Compatibility.
  Fix in Project Settings → Rendering → Renderer.
- **Shader fails to compile** → paste the exact Godot console error back to
  me; GLSL push-constant layout and RD API calls are the most likely place
  for a version-specific mismatch (I targeted 4.3 syntax).
- **Nothing visible / all black** → check that `Display` sprite's `scale`
  actually got set (should be 3x from `CELL_PIXEL_SIZE`) and that the window
  is large enough to show a 768x768 area at default grid size.
- **Sand/water don't fall** → likely a uniform-set binding mismatch between
  `binding = 0/1` in the shader and `u_src.binding`/`u_dst.binding` in
  `main.gd` — these must match exactly.

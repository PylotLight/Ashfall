# External Bridge/Host Review — raw feedback (2026-09-10)

Source: advisory model review of `bridge/src/ashfall_world.cpp`,
`bridge/include/ashfall_world.hpp`, `scripts/main_controller.gd`,
`shaders/water_fx.gdshader` (+ `main.tscn` scene context). Reproduced verbatim
for the local record. Follow-ups extracted into `docs/advisory/PERF_TASK_BACKLOG.md`
(bridge + host section). Note: `tests/simulation_tests.cpp` was NOT included in
this round — the test-quality audit is still open (see §F).

---

## Verdict

The bridge is a reasonable **512² prototype renderer**, but it does not yet have partial GPU updates, and two presentation issues deserve fixing before lighting:

1. **Partial repaint does not account for liquid shading’s cross-chunk dependencies**, unless the engine already supplies expanded dirty sets—which these files do not establish.
2. **The shader expands coverage without expanding the corresponding color**, producing dark halos and potentially painting over adjacent solids.

The larger architectural finding: **cells remain engine-owned, but the controller already owns player state and gameplay rules.** That contradicts “presentation and input forwarding only,” though it does not require moving everything immediately.

This is source inspection. I cannot quantify copy costs or GPU time without profiling, and `tests/simulation_tests.cpp` was not included.

---

# A. Dirty tracking: partial CPU painting, full texture submission

## What actually happens

For every `update_world_texture()` with a nonempty dirty set:

| Stage | Current behavior |
|---|---|
| Obtain dirty list | Copies the engine’s dirty positions into a vector |
| Read material state, ≤8 dirty chunks | Per-cell `world_.cell()` queries, including liquid-neighbor queries |
| Read material state, >8 dirty chunks | `copy_rect()` for the entire presentation window |
| Paint pixels | Dirty chunks or entire window |
| Refresh images | Two full-image `Image::set_data()` calls |
| Submit texture updates | Two full-image `ImageTexture::update()` calls |
| Consume dirty state | Clears the entire engine dirty set |

**There are no region uploads in this code.** `ImageTexture::update()` has no supplied dirty rectangle and is not being given separate chunk textures. Dirty tracking reduces CPU painting only.

### Upload payload

For two RGBA8 layers:

| Window | Payload per presentation update | At 60 updates/s |
|---|---:|---:|
| 512² | 2 MiB | 120 MiB/s |
| 1024² | 8 MiB | 480 MiB/s |

These are logical texture payload figures, **not measured bus traffic or GPU time**. Backend staging, unified memory, synchronization, and allocation/copy behavior affect the real cost.

At 512², this does not by itself justify a renderer rewrite. The more immediate waste is update frequency.

## Presentation currently happens too often

```cpp
void AshfallWorld::step() {
    world_.step();
    update_world_texture();
}
```

Consequently:

- Two catch-up simulation steps produce two presentation updates.
- Painting can produce an update for every mouse-motion event.
- Simulation and editing can both upload before the next rendered frame.

Only the last intermediate image is needed for normal display.

**First optimization: separate advance from presentation and flush at most once per rendered frame.** Accumulate dirty state across simulation ticks and edits.

For compatibility, a synchronous “step and present” helper can remain for tests or tools, but the production scheduler should not require it. Ensure a paused edit still schedules a presentation flush.

This changes no simulation rules.

## Split the timer before deciding what to optimize

`last_step_ms_` measures:

> simulation + dirty collection + material reads + pixel painting + image refresh + texture update calls

It is neither pure simulation time nor completed GPU execution time. The label “Sim Step” is misleading.

Instrument:

- `sim_ms`
- `dirty_collect_ms`
- `material_read_ms`
- `paint_ms`
- `image_set_data_ms`
- `texture_submit_ms`
- presentation updates per rendered frame
- visible dirty chunks / repainted pixels
- logical uploaded bytes

Use rendering/GPU profiling separately for actual GPU cost. A texture update call may also incur CPU-side staging or stalls; that still does not make its duration a clean GPU timer.

With ~7 ms already in simulation, the nominal remaining 60 Hz budget is only ~9.7 ms for everything else. A two-step catch-up callback is already tight before rendering.

## Copy-on-write trap on clean frames

These execute **before** the empty-dirty early return:

```cpp
uint8_t *w = pixel_bytes_.ptrw();
uint8_t *water_w = water_pixel_bytes_.ptrw();
```

If the packed arrays share backing storage with the images, writable access can trigger full-buffer copy-on-write detachment—even though no pixels will be painted.

Whether that happens here depends on Godot’s actual storage path, so do not file a measured full-copy claim without checking. But the safe change is unconditional:

**Return on no visible work before requesting writable pointers.**

Likewise, `set_data()` should not automatically be counted as a `memcpy`; measure it and any later detachment separately.

## The eight-chunk threshold is only a heuristic

At 512², eight chunks are 12.5% of the window; at 1024² they are 3.125%. The threshold is also based on **all dirty chunks**, not visible dirty chunks.

Currently:

- One offscreen dirty chunk can cause two full texture updates despite painting nothing.
- Nine offscreen dirty chunks can cause a full-window repaint and upload.

Build a visible presentation work set first, including shading dependencies, then choose partial versus full painting based on measured cost or affected area.

For the partial path, a contiguous material scratch tile with a halo is worth benchmarking against thousands of hash lookups. Use `copy_rect()` or chunk reads; do not make the bridge depend on `cells_`.

---

# B. Clearing and presentation correctness

## B1. Cross-chunk liquid shading needs expanded invalidation

Liquid color reads:

- The cell immediately above for `surface`.
- Up to five cells below.
- Up to five cells above.

Therefore, changing a cell can change the rendered color of **unchanged cells in a neighboring vertical chunk**.

Example:

- A liquid column crosses rows 63/64.
- Erase the liquid at row 64.
- Liquids above it may need a different depth shade.
- If only the chunk containing row 64 is repainted, row 63 remains stale.

The full repaint path will produce the new color; the partial path may not.

**Required contract:** the renderer expands simulation dirtiness to cover its own sampling dependencies. Do not make the simulation understand this particular shading algorithm.

A conservative chunk-level solution here is each dirty chunk plus its immediate vertical neighbors, clipped to the presentation window. A tighter implementation expands affected pixel rectangles by five rows.

**Regression test:** after boundary edits, compare both CPU image buffers byte-for-byte against a forced full repaint. Test insertion, deletion, liquid-to-solid changes, and transitions across the partial/full threshold.

This is a renderer invalidation issue, separate from the previously filed heat bug.

## B2. Deletion painting itself is sound

For a repainted air cell:

- Base becomes opaque background.
- Liquid layer becomes transparent black.

Thus the bridge does not require a removed chunk to remain allocated. The partial path reads `world_.cell()`; the full path uses `copy_rect()`.

**`read_chunk()` is not called anywhere in this renderer.** Its missing-chunk contract remains an engine API question, not something this packet proves.

What still needs verification in the engine:

1. Removing the final cell dirties the former chunk.
2. `reset()` dirties removed chunks.
3. Successful `load()` invalidates both old occupied regions and new occupied regions.
4. `copy_rect()` overwrites every requested output cell, including missing/air regions.

The code comments claim reset behavior, but comments are not a test.

## B3. Do not use world reset as texture recovery

```cpp
if (world_image_.is_null() || world_texture_.is_null()) {
    init_world(world_width_, world_height_);
}
```

`init_world()` resets authoritative simulation state.

That means a presentation-resource recovery path can erase the world. It also checks only base-layer resources, not the liquid resources.

Split these operations:

- Reset/create authoritative world.
- Allocate/recreate presentation resources.
- Force full repaint from the current world.

Resource recreation must not mutate simulation state.

`init_world()` also replaces texture objects. The controller assigns its texture references only in `_ready()`, so later reinitialization requires rebinding or an explicit resource-changed signal.

## B4. One dirty consumer, explicitly

The renderer clears all engine dirtiness, while GDScript exposes:

```cpp
get_dirty_chunks()
clear_dirty_chunks()
```

Any caller clearing dirtiness before presentation can cause stale pixels.

For now, make the bridge presenter the documented sole consumer. Do not later reuse this stream for lighting, saves, and collision caches without independent consumption semantics.

For a future moving presentation window, changes outside the current window may be discarded only if newly exposed regions are always repainted.

---

# C. Shader and scene fitness for lighting

## C1. This is dilation, not morphological closing

The shader performs a maximum over center and four neighbors, followed by thresholding.

A morphological closing requires dilation **and erosion**. Thresholding the dilated result does not supply the missing erosion.

At an air texel center directly beside liquid:

- Neighbor alpha is approximately `0.804`.
- `smoothstep(0.25, 0.65, 0.804)` is `1`.
- Output alpha becomes `0.88`.

Thus this can expand liquid by roughly a whole texel at those locations, not merely a half-texel meniscus.

Rename it if that expansion is intentional; otherwise change the operation.

## C2. Expanded alpha uses unexpanded color

Coverage comes from neighboring samples:

```glsl
a = max(a, texture(TEXTURE, UV + ...).a);
```

Color remains:

```glsl
vec4 c = texture(TEXTURE, UV);
```

At the adjacent air texel, `c.rgb` is black, while the new coverage is opaque enough to obscure the scene. This gives a dark border; the small foam/shimmer terms do not reconstruct liquid color.

There is also the ordinary straight-alpha filtering problem: linear interpolation between colored liquid and transparent black darkens edge RGB before blending.

**Lowest-risk first fix:** remove the dilation, establish correct filtered color/alpha behavior, then assess whether extra smoothing is still needed.

If retaining dilation:

- Propagate a corresponding liquid color with coverage.
- Use a coherent alpha representation and blending strategy.
- Define how different liquid materials meet.
- Prevent coverage expansion over solids unless deliberately desired.

Simply dividing center RGB by alpha will not fix locations where the center sample is fully empty but a neighbor supplies coverage.

## C3. The liquid overlay can cover solids

The overlay is drawn after the base layer, and its expanded mask has no solid exclusion test. Adjacent stone can therefore receive the dark/colored fringe.

Before lighting, decide whether this is a fluid silhouette effect or an actual overlay that may cover terrain. For the former, a material/solid mask or equivalent constraint is needed.

## C4. Movement is an insufficient long-term visual classification

All liquids currently receive:

- The same opacity.
- The same depth-ramp formula.
- Foam.
- Shimmer.
- The same smoothing behavior.

This is convenient for water-like additions, but lava and oil need not look like water.

Do not solve this with new hardcoded material-ID branches. When differentiated visuals land, introduce data-driven **render properties or visual profiles** separate from movement class. That preserves data-only additions for instances of existing visual behavior.

## C5. Scene structure: fine for the sandbox

Good choices:

- Matching aspect-centered base and overlay rectangles.
- Nearest filtering for terrain, linear for liquid.
- Overlay and player marker ignore mouse input.
- No Godot-side mirror of every simulation cell.

The full-window transparent liquid quad still runs its shader over the drawn rectangle; CPU dirty tracking does not reduce that fragment work.

For initial lighting, this structure is usable. It does not require immediate conversion to thousands of chunk nodes. Prototype one light and emissive material treatment first.

For a scrolling world later, separate:

- World rendering, camera, and actors.
- HUD and editor controls.

A world-space `Node2D` presentation with a UI layer is a more natural destination than embedding the entire game in a `TextureRect`. This is a presentation refactor, not an engine-state migration.

Also decide whether lighting uses Godot’s canvas lighting or a custom light field. The current RGBA layers do not encode material identity, normals, or emission explicitly; do not try to recover authoritative material properties from grain-modified RGB.

---

# D. Godot-side authority: cells are clean, gameplay is not

The render buffers and material-color caches are derived state. Those are appropriate.

The controller, however, owns:

- Authoritative player position.
- Frame-delta movement and endpoint collision resolution.
- Dig eligibility: “anything except air or stone.”
- Placement eligibility: “only into air.”
- Player persistence and selected-material persistence.

Those are gameplay rules, not just input forwarding.

**Disposition:** acceptable as an explicitly temporary playground avatar/tool layer. Before this becomes survival gameplay, move player actions and state into the portable engine/session layer. GDScript should send intent and display results.

Two current consequences:

### Endpoint collision can tunnel

Movement checks the destination box only. At 90 cells/s, even an ordinary 60 Hz frame moves 1.5 cells; a frame hitch can cross much thicker obstacles.

Moving this code to `_physics_process()` would improve scheduling consistency but would not solve tunneling. A gameplay avatar needs swept collision or bounded substeps in its authoritative movement implementation.

### Frame-based player behavior is outside the deterministic contract

The same cellular tick/input history can coexist with different player positions depending on rendering deltas. That does not invalidate cellular determinism, but it means the whole game is not yet deterministic.

When player actions affect cells, apply those actions at defined simulation boundaries.

---

# E. Small correctness and persistence items

These are distinct from the prior JSON-parser review.

## Bridge validation

- `set_cell()` and `fill_rect()` narrow arbitrary integers to `uint8_t`. For example, `256` becomes air and `257` becomes stone before engine validation. Reject out-of-range and unknown IDs first.
- `world_width_ * world_height_` is evaluated as signed `int` before conversion to `size_t`. Large positive dimensions can overflow. Add dimension/resource limits and checked arithmetic **before resetting the world or allocating**.
- Validate simulation rate as finite and within a supported range. `NaN` and infinity currently produce nonsensical scheduling behavior.
- Fix the header comment saying default 30 Hz; the field is 60 Hz.

## Host save envelope

The header is **14 bytes**, not 11:

```text
version: 1
player x/y: 8
material: 1
sim length: 4
```

The current minimum-length check is too weak. Further:

- Validate `sim_len` against remaining bytes and a configured limit before `get_buffer()`.
- Define trailing-byte behavior.
- Validate player coordinates as finite and within the supported domain.
- Validate all host metadata before committing the engine load.
- Save the selected material by stable name, not raw ID. The adopted core v3 roster remapping does not automatically fix this outer byte.
- Use temporary-file replacement rather than truncating the only save before the new write succeeds.

This does not reopen the settled core save design; it closes an uncovered host envelope.

## Input/UI

- `S` both moves down and single-steps the simulation. Resolve the conflict.
- Letterbox clicks clamp to border cells and paint them. Reject coordinates outside the actual displayed image before clamping the brush.
- Clear drawing state on focus loss and handle release outside the drawing control.
- `_update_marker()` performs material lookup and label assignment every frame despite the HUD-throttling intent.
- Save/load error status is overwritten by the next HUD refresh; use a separate transient message.

---

# F. Test-suite quality: cannot grade an unseen file

Send `tests/simulation_tests.cpp` for the requested exact-count audit. I would distinguish:

| Test type | What it establishes |
|---|---|
| Fixed seed/state → exact final hash | Regression lock and determinism for that case |
| Exact occupied count in a conservative movement fixture | Conservation, if fixture assumptions justify it |
| Exact count after many probabilistic reactions | Mostly an implementation/schedule lock unless analytically justified |
| Heat balance with explicit sources/sinks | Physical/accounting contract |
| Save round-trip plus canonical byte comparisons | Serialization contracts |
| Different insertion histories → same defined result | Independence from incidental container history |
| Dune/pool screenshots | Visual regression, not automatically desirable gameplay |

Do not delete exact-output tests merely because they are brittle. Keep a small, clearly labeled golden regression layer and complement it with invariant and scenario tests. A desirable physics change may legitimately require updating a golden result; updating the result alone does not justify the change.

For **this packet**, add these tests first:

1. Partial repaint equals forced full repaint, especially at chunk boundaries.
2. Final-cell deletion, reset, and load clear old pixels in both layers.
3. No dirty/visible work means no image mutation or texture submission.
4. Multiple ticks/edits before a frame cause one presentation flush.
5. Texture recreation preserves authoritative world bytes.
6. Liquid shader fixtures: isolated drop, one-cell channel, solid border, two liquid colors.
7. Host save truncation, excessive lengths, invalid floats, and selected-material remapping.

For **this packet**, add these tests first: *(duplicated in source — single list intended)*

1. Partial repaint equals forced full repaint, especially at chunk boundaries.
2. Final-cell deletion, reset, and load clear old pixels in both layers.
3. No dirty/visible work means no image mutation or texture submission.
4. Multiple ticks/edits before a frame cause one presentation flush.
5. Texture recreation preserves authoritative world bytes.
6. Liquid shader fixtures: isolated drop, one-cell channel, solid border, two liquid colors.
7. Host save truncation, excessive lengths, invalid floats, and selected-material remapping.

---

## Recommended order

1. **Correctness:** shading invalidation halo, shader color/coverage mismatch, presentation recovery without world reset.
2. **Instrumentation:** separate simulation, reads, painting, image refresh, submission, and GPU timing.
3. **Low-risk performance:** present once per rendered frame; early-return before writable access; filter offscreen dirtiness.
4. **Contracts:** sole dirty consumer, deletion/load clearing tests, bridge and host-save validation.
5. **Lighting prototype:** retain the current two-layer renderer initially; establish solid masking and material visual properties.
6. **Only if measured necessary:** chunk textures or another region-addressable upload design.

**Bottom line:** fix presentation correctness and cadence before investing in partial GPU uploads. At 512², the avoidable repeated work is more actionable than the raw 2 MiB upload size; at larger windows, the monolithic upload design becomes an explicit scaling decision.

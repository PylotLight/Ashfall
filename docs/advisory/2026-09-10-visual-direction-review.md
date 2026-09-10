# External Visual-Direction Review — raw feedback (2026-09-10)

Source: advisory model review of `docs/ARCH_ADVISORY_BRIEF.md` + project screenshot
(first advisory round; predates the perf/JSON/container/bridge/test rounds).
Reproduced verbatim for the local record. This round produced direction, not
task-level patches — its P3A/P3B proposal and lighting/animation guidance remain
the input to P3 scoping, not a backlog.

---

# Executive assessment

**Keep Godot and the portable C++ simulation. Do not restart the project to solve what the screenshot is showing.**

You have built something valuable: a testable simulation core, a clean integration boundary, and a content pipeline that can add materials without hard-coded material checks. Those are good foundations.

However, I disagree with the other AI’s strongest conclusion:

> “The materials and interactions are genuinely close to Noita’s foundations … what you’re seeing is a rendering deficit, not a simulation one.”

**The screenshot’s immediate problem is largely presentation. The project’s overall distance from Noita is not.** Ten materials, local reactions, density-based movement, and temperature conduction establish a falling-sand prototype. They do not yet establish Noita-level fluid behavior, combustion, destruction, object interaction, world streaming, performance, or gameplay.

Also, neither the screenshot nor your brief establishes that your algorithms mirror Noita’s implementation. I would avoid using that as an architectural premise.

My recommendation is:

1. **Make one small, authored scene look excellent with the existing simulation.**
2. **Profile and harden the simulation before expanding world size.**
3. **Build one playable interaction chain involving the player, terrain, liquids, heat, and an object.**
4. **Then prove chunked world persistence and survival gameplay.**
5. Add material breadth only after those foundations work.

Your next milestone should not be “more sophisticated sand.” It should be:

> **A player-scale underground room where a burning object illuminates textured rock, produces convincing flame and smoke, interacts with water, and leaves a persistent result.**

That is a much better benchmark than either a material palette or a large empty simulation box.

---

# 1. What the screenshot actually tells us

There are several distinct issues, and lighting is only one.

## 1.1 It reads as a debugging tool, not a place

The dominant visual elements are:

- A huge rectangular container.
- Uniform rectangular platforms.
- Large areas of almost-black empty space.
- A large debug panel.
- A very small player marker.
- Materials displayed as test piles and tanks.

Even an excellent renderer would struggle to make that composition resemble a finished game.

**The first visual upgrade should include an authored environment, not just a shader.**

Build a scene containing:

- An irregular cave silhouette.
- A visible, character-sized player.
- A foreground rock layer and a distinct background wall.
- A wooden structure or support.
- One water reservoir.
- One warm light source.
- One combustible area.
- A few deliberate focal points.

The existing material demo should remain as a regression/debug scene. It should not remain the main visual benchmark.

## 1.2 Your water is more polished than its surroundings—but stylistically isolated

The water already has shading and edge treatment. In the screenshot, however, it reads somewhat like a beveled blue insert:

- Bright boundary treatment.
- A visibly patterned interior.
- A comparatively dark outline.
- Little relationship to environmental illumination.

It is receiving more presentation work than the stone, sand, player, and background. That imbalance makes the other materials look even flatter.

I would **reduce blanket edge highlighting** and make the water’s appearance depend more on:

- Actual exposed upper surfaces.
- Depth or optical thickness.
- Environmental lighting.
- Underlying background.
- Local motion or impacts.

A boundary between water and sand should not automatically receive the same treatment as a water–air surface.

## 1.3 Grain is not the same as material structure

The stone and sand do have visible variation in the supplied image. The problem is not simply that the grain is too subtle.

The problem is that the variation is largely **unstructured**.

Rock needs larger forms and directional features. Wood needs grain direction. Sand needs a relatively quiet interior with meaningful surface variation. Randomly varying every pixel produces noise, not necessarily texture.

Increasing noise amplitude alone will likely make the image busier rather than better.

## 1.4 Framing matters more than a particular zoom number

The other assessment puts framing too low.

However, I would not prescribe “2–4× zoom” blindly. In the attached image, the simulation cells already appear enlarged in at least some areas. The important ratios are:

- Character height relative to the viewport.
- Visible terrain relative to interaction range.
- Material-cell size relative to character detail.
- UI area relative to playable area.

A following camera is useful, but the real goal is **player-scale composition**.

---

# 2. Keep the architecture—but extend its meaning

Your three-layer separation is good. Preserve it.

The likely evolution is:

```text
Portable C++ game core
├── Cellular materials
├── Temperature / reactions
├── Gameplay entities
├── Character queries and movement
├── Projectiles / explosions / tools
├── Persistent world chunks
├── Tick-indexed commands
└── Deterministic gameplay events

Godot bridge
├── Snapshot production and upload
├── Visible-region tracking
├── Presentation event delivery
├── Material render lookup tables
└── Resource and lifecycle management

Godot presentation
├── Camera
├── World compositing
├── Lighting and bloom
├── Sprites and animation
├── Cosmetic particles
├── Audio
└── UI
```

The important change is that **“engine owns simulation” eventually needs to include gameplay state beyond cells** if your determinism contract applies to the whole game.

If the cells are deterministic but Godot physics controls a character who pushes sand, breaks terrain, and causes reactions, the resulting world is not automatically deterministic.

That decision needs to be explicit before you build much gameplay.

## Keep these choices

- Portable C++ core.
- Fixed simulation ticks.
- No Godot types in authoritative state.
- Data-driven material properties.
- Deterministic random decisions.
- Automated reaction and save tests.
- Chunk-based presentation updates.
- Compact, cache-friendly cell data.

## Reconsider these interpretations

### “Adding a material must never require engine changes”

Excellent for adding content within existing capabilities. Too restrictive as a universal rule.

Adding oil through existing liquid and combustion behavior should be data-only.

Adding:

- Electricity.
- Pressure-driven gas.
- Rigid-body fragments.
- Magnetic particles.
- Multi-cell organisms.

…may legitimately require new simulation primitives.

The right standard is:

> **New instances of an existing behavior should be data-only. New categories of behavior may extend the engine, with explicit contracts and tests.**

Do not turn the reaction schema into an obscure programming language just to preserve an empty engine diff.

### “No floats means deterministic”

It helps. It does not guarantee determinism.

You also need defined behavior for:

- Signed overflow.
- Intermediate integer widths.
- Negative coordinates in hashes.
- Integer serialization and endianness.
- Container iteration order.
- Reaction conflict resolution.
- Chunk traversal order.
- Multithreaded scheduling.
- Stable rule identities.
- Input assignment to ticks.

These become more important as you optimize.

### “All suites green means the foundation is complete”

It means the tested properties hold.

That is genuinely valuable, but add separate evidence for:

- Physical plausibility.
- Gameplay usefulness.
- Runtime scaling.
- Cross-platform replay identity.
- Visual quality.

Exact-count tests can preserve an algorithm’s behavior without proving that behavior is desirable.

---

# 3. Revised priority order

For **immediate Noita-like feel per unit effort**, I would rank the work this way:

| Priority | Work | Why |
|---|---|---|
| 1 | Authored scene, readable player, camera, background | Establishes scale and makes every later improvement visible |
| 2 | Emission, local illumination, restrained bloom | Makes fire/lava belong to the environment |
| 3 | Coherent material palettes and structured textures | Stops terrain looking like colored debug geometry |
| 4 | Flame animation, smoke, sparks, interaction audio | Gives reactions motion, timing, and consequence |
| 5 | Surface-aware liquids and local contact shading | Improves cohesion and material readability |
| 6 | Better combustion and selected phase changes | Makes the visual events mechanically meaningful |
| 7 | Large material roster | Useful only once combinations support gameplay |

That is not a strict waterfall. The first three belong in the same small visual milestone.

## Smallest worthwhile first delivery

Do not implement general global illumination first.

Deliver:

- One cave room.
- One player sprite or even a well-designed placeholder.
- A following camera with stable pixel scaling.
- A background wall.
- Three-tone stone and wood.
- One clustered warm light associated with a fire.
- A low-resolution emission/bloom effect.
- Tick-driven flame color variation.
- A few cosmetic sparks.
- A flame loop and ignition sound.

**Exit criterion:** a short clip reads as “a game in a burning cave,” not “a cellular automaton with a glow filter.”

For the first room, a conventional Godot light is perfectly acceptable. It is a presentation prototype, not necessarily your final per-cell lighting architecture.

---

# 4. Lighting: separate emission, illumination, and bloom

These are different effects.

### Emission

A material appears bright regardless of incident light.

Examples:

- Flame core.
- Lava.
- Glowing hot metal.
- Magical projectile.

### Illumination

The source changes the appearance of nearby surfaces.

Examples:

- Orange light on stone beside a fire.
- A blue liquid lighting the underside of a bridge.
- A moving projectile revealing a tunnel.

### Bloom

Bright image regions spread into neighboring screen pixels.

Examples:

- A soft halo around flame.
- A bright spark appearing larger than its physical pixel.

**Bloom alone will not make the world feel lit.** It can make orange confetti into blurry orange confetti.

You want all three, with illumination doing most of the environmental work.

---

## 4.1 Recommended lighting path

### Stage A: clustered lights for the first visual slice

Extract a small number of representative sources from fire/lava regions.

For example:

- Group nearby emissive cells by coarse tile or connected region.
- Compute a representative position, color, and bounded intensity.
- Merge or prioritize sources under a fixed light budget.
- Smooth source transitions so cluster changes do not cause popping.

Use those to drive a small set of Godot 2D lights.

Advantages:

- Fast to prototype.
- Easy to art-direct.
- Useful for player-carried lights and isolated objects.
- Does not require a custom light solver.

Limitations:

- Dynamic terrain shadows require additional work.
- Many overlapping lights become expensive.
- Large lava fields do not map naturally to hundreds of individual lights.
- Continuously rebuilding detailed terrain occluders can be costly.

**Do not create one Godot light per flame cell.**

### Stage B: a camera-local, coarse light field

For the material world, my preferred scalable next step is:

> **A low-resolution GPU light field derived from emission and opacity, operating over the camera region plus a margin.**

Keep it entirely in presentation unless illumination becomes authoritative gameplay state.

Start experimentally with something like:

- One lighting texel per 4×4 simulation cells.
- A visible-region field plus the maximum relevant lighting radius.
- RGB emission.
- Light transmission/opacity.
- A fixed, bounded amount of propagation work.
- Upsampling during world shading.

Do not interpret those numbers as an optimum. Thin walls, light radius, and target hardware will determine the right resolution.

### Stage C: selective higher-quality shadows

If needed later:

- Higher-resolution occlusion near the player.
- Special shadows for a few important lights.
- Better directional transport.
- Improved handling of thin walls.
- Separate sunlight or ambient visibility.

You probably do not need all of those to produce an excellent result.

---

## 4.2 What a cheap propagation solver actually does

A simple stylized solver can propagate decaying light through neighboring texels.

Conceptually:

```text
light_next[p] =
    max(
        emission[p],
        transmitted_and_attenuated(light_prev[neighbors], p)
    )
```

This can work well aesthetically, but it has important limitations:

- A nearest-neighbor solver needs enough passes to cover the desired radius.
- Four-neighbor propagation can produce diamond-shaped falloff.
- Component-wise maximum is not physically additive lighting.
- Naively summing neighbors can over-amplify light or become unstable.
- Coarse opacity can leak through thin walls or over-block openings.
- Temporal reuse can leave light trails after a source disappears.

That is acceptable if you knowingly choose a stylized approximation.

It is not a free “global illumination” feature.

A flood-fill-like or max-propagation solution, a diffusion-style solution, and clustered radial lights have different aesthetics and costs. Build a small comparison scene before committing.

## 4.3 Preserve surface lighting

A common mistake is treating solid cells as completely opaque and then making them completely black.

You need to distinguish:

- **Light arriving at a surface.**
- **Light passing through that surface.**

Stone should receive light while preventing most transmission beyond it.

This may mean storing or evaluating surface illumination separately from through-cell transport, or sampling nearby open-space lighting when shading an exposed solid boundary.

## 4.4 Do not use temperature as the light field

Temperature is a possible **source input**, not a replacement for illumination.

A reasonable material presentation model is:

```text
emission =
    intrinsic_material_emission
    + optional_heat_emission_curve(temperature)
```

But:

- Hot water should not glow visibly like lava.
- A luminous magical liquid might emit without being hot.
- Flames need art direction beyond a generic temperature ramp.
- Smoke may obscure light without emitting it.

Keep physical heat and visible emission separate.

## 4.5 Basic compositing model

A useful first approximation is:

```text
lit_material =
    albedo × (ambient_light + local_light)
    + emission
```

Then apply bloom from an HDR brightness or emission buffer.

For transparent liquids and smoke, compositing order matters; do not simply apply the opaque formula to everything and expect correct results.

### Practical art-direction advice

- Keep enough ambient light to read unlit terrain.
- Use warm/cool contrast.
- Preserve a bright, sharp flame core.
- Keep bloom low-resolution and restrained.
- Prevent bloom from washing out material boundaries.
- Do lighting math in linear space, with deliberate conversion for color textures.

A dark room with a warm fire works because of **contrast and readable surfaces**, not because the entire frame becomes orange.

---

# 5. Deterministic animation: keep gameplay identity separate from presentation identity

You have three useful levels of determinism.

| Level | Requirement | Recommended approach |
|---|---|---|
| Simulation | Same inputs produce identical authoritative state | Integer/fixed-point core, stable ordering |
| Presentation intent | Same tick produces the same animation choices | Tick-driven seeds, explicit animation phase |
| Rendered pixels | Identical screenshot across GPUs | Usually not worth requiring |

A shader driven by the same tick can still produce slightly different pixels across GPUs because of floating-point and texture-sampling differences.

That is fine if it never changes simulation state.

## 5.1 My recommended policy

### World material animation

Drive it from:

- Simulation tick.
- World coordinates.
- Material/render-profile ID.
- World visual seed.
- Optional existing state such as temperature or remaining fuel.

### Interpolation

If desired, pass an interpolation fraction from Godot for smooth presentation between simulation ticks.

For visual regression captures, force it to a known value.

### UI and purely decorative effects

Wall-clock animation can be allowed if you want it. It does not violate world-state determinism.

However, a blanket `TIME` dependency makes pause behavior, replay capture, and visual debugging less predictable. I would prefer an explicit presentation clock with a documented policy.

**Use a world animation clock for the world, and a separate UI clock for the interface.**

---

## 5.2 Avoid white-noise flicker

This:

```text
brightness = hash(x, y, tick)
```

changes every pixel independently every tick. It often looks like television noise.

Instead, combine:

- Slowly varying regional flicker.
- Faster but weaker local variation.
- Material-specific animation cadence.
- Spatially coherent motion.

For example:

```text
regional_phase = tick / regional_period
local_phase    = tick / local_period

fire_brightness =
    base
    + strong_low_frequency_region_variation
    + weak_high_frequency_cell_variation
```

Interpolate between phase samples if appropriate.

### Fire

- Yellow-white core.
- Orange body.
- Red/darker fading edges.
- Upward temporal structure.
- Correlated flicker over a small region.

### Lava

- Mostly slow changes.
- Dark crust areas.
- Bright cracks or exposed hot patches.
- Occasional localized activity.

### Stone and sand

Mostly stable. Do not animate every material simply because the shader can.

### Water

- Low-amplitude surface motion.
- Sparse highlights.
- Motion strongest near disturbances.
- Quiet bulk liquid.

## 5.3 Where animation should live

Prefer the shader for continuously animated appearance.

The bridge should upload:

- Stable material identity.
- Stable variation seed if needed.
- Relevant changing fields.
- Tick or presentation phase as a uniform.

The shader evaluates the animation.

This avoids a major trap:

> **If flicker is baked into your CPU RGBA snapshot, “unchanged” fire chunks become dirty every animation tick.**

That defeats part of the dirty-chunk design.

The CPU painter is still fine for static texture generation or an initial prototype. But do not make it responsible for every frame of flame shimmer.

## 5.4 Position-anchored versus material-anchored texture

World-coordinate noise is excellent for terrain.

It is less convincing for moving grains: the appearance changes as a grain enters different coordinates.

Options:

1. Accept that for tiny particles.
2. Use only weak variation for moving materials.
3. Move a small visual-variant value with the cell.
4. Derive appearance from an existing persistent cell attribute.

Do not add a unique persistent particle ID to every cell unless something actually needs it.

---

# 6. Material rendering: evolve flat colors, do not discard the data-driven approach

**One base color plus arbitrary noise is not enough for the final style. But a material lookup table feeding a shader is absolutely a viable architecture.**

You do not need hand-painted textures for every material. You need a better description of how each material should look.

## 6.1 Minimal render schema

I would separate physical definitions from rendering definitions:

```text
materials.json
material_visuals.json
```

The latter references stable material IDs.

Example:

```json
{
  "material": "stone",
  "palette": ["#292b35", "#414450", "#666775"],
  "pattern": "strata",
  "pattern_scale": 4,
  "detail_strength": 0.12,
  "edge_response": 0.25,
  "opacity": 1.0,
  "emission": {
    "color": "#000000",
    "strength": 0.0
  }
}
```

Those numbers are illustrative, not a proposed final art preset.

The minimal useful fields are:

- Two to four palette colors.
- A pattern type.
- Pattern scale.
- Variation strength.
- Opacity or transparency mode.
- Emission.
- Animation profile.

This keeps a palette edit from becoming a simulation schema migration.

If light later affects AI, crops, or other mechanics, create a clearly separate authoritative illumination concept. Do not make gameplay depend on a GPU bloom texture.

## 6.2 Three spatial scales

Good terrain benefits from variation at several scales:

1. **Large:** strata, veins, patches, moisture zones.
2. **Medium:** clusters a few cells across.
3. **Small:** restrained single-cell variation.

Your existing painter mostly addresses the third.

### Rock

- Broad tonal patches.
- Slight directional stratification.
- Sparse cracks or inclusions.
- Meaningful exposed-edge treatment.

### Wood

- Directional grain.
- Darker seams.
- Occasional knots.
- Gradual charring near combustion.

### Sand

- Quiet interior.
- Subtle clustered variation.
- Surface-aware highlights.
- Sparse moving grains.

### Dirt

- Chunkier aggregate pattern.
- Embedded stones or roots.
- Different character from smooth sand.

These are art decisions more than algorithmic ones.

## 6.3 Texture atlases are also reasonable

A small authored texture atlas can outperform elaborate procedural noise in both quality and predictability.

Use:

- World-space sampling for static terrain.
- Material-specific atlas regions.
- Limited palette modulation.
- Careful repetition avoidance.

The renderer can support both procedural and authored patterns through the same profile abstraction.

---

# 7. Liquids: improve the cues before attempting expensive fluid rendering

Your current overlay is a reasonable prototype. I would keep it provisionally, but change the interpretation of its features.

## 7.1 Distinguish free surfaces from all boundaries

Foam or a bright rim should not appear identically at:

- Water–air.
- Water–stone.
- Water–sand.
- Water–oil.
- Bottom of a pool.

A first improvement is neighborhood classification based on material IDs.

Start with:

- Exposed upper surface.
- Side contact.
- Bottom contact.
- Interface with another liquid.

Only some boundaries should get surface highlights.

## 7.2 Depth needs a clear meaning

A five-cell ramp is a local shading effect, not general water depth.

Eventually distinguish:

- Distance below a free surface.
- Optical thickness through the viewed scene.
- Distance from a boundary.

These are not interchangeable.

For a stylized side-view game, a cheap approximation is fine. Just avoid having every edge of a water pocket look equally illuminated.

## 7.3 Edge smoothing is an art choice, not an automatic upgrade

Hard cellular silhouettes can look excellent.

Aggressive smoothing can:

- Hide narrow gaps.
- Make the collision boundary appear inaccurate.
- Make water visually overlap stone.
- Turn pixel water into a glossy blob.

Keep material classification nearest-filtered. If you smooth coverage, use a separate coverage channel and mask it against solids.

## 7.4 Refraction comes later

A small background distortion can help, but:

- Distort background imagery more than collision-relevant foreground edges.
- Avoid sampling a screen texture that already includes the same water pass.
- Keep displacement small.
- Do not let refraction obscure the player or hazards.

You can achieve good water without full refraction, reflections, or fluid surface meshes.

---

# 8. Fire needs both presentation and lifecycle changes

Fire is the most obvious place where rendering and simulation meet.

Currently, your fire is described as orange gas that rises and disappears. That will remain limited even with good bloom.

A convincing combustion system has at least three roles:

1. **Fuel-bearing material.**
2. **A burning state or reaction process.**
3. **Visible flame and combustion products.**

The visible flame does not need to carry every part of combustion semantics.

## 8.1 A practical combustion model

For example:

```text
Wood receives sufficient heat
    ↓
Wood begins burning
    ↓
Burning consumes fuel and releases heat
    ↓
Flame/smoke may be emitted into available neighboring space
    ↓
Wood becomes char/ash or is consumed
```

This prevents the whole system from depending on free-floating fire pixels randomly happening to touch fuel.

You do not need oxygen simulation immediately. A simple ventilation proxy can come much later if gameplay benefits.

## 8.2 Split smoke and embers by consequence

### Cosmetic smoke

- Godot particles or a lightweight presentation system.
- Cannot suffocate actors.
- Cannot affect reactions.
- Need not be saved.

### Simulated smoke

- C++ material or authoritative field.
- Can accumulate or move.
- Can affect breathing or visibility if designed.
- Requires state, tests, and persistence.

### Cosmetic sparks

- Cheap particles.
- No ignition.
- No gameplay collision.

### Gameplay embers

- Deterministic particles/entities or cells.
- Carry heat.
- Can ignite fuel.
- Must be included in authoritative state.

This distinction prevents an attractive visual effect from becoming an accidental source of nondeterministic gameplay.

## 8.3 Add a presentation event stream

The core can emit facts such as:

```text
Ignited
Extinguished
MaterialBroken
Impact
Explosion
LiquidDisplaced
PhaseChanged
```

Events should carry enough stable information for the bridge to generate effects:

- Tick.
- Position.
- Event type.
- Magnitude.
- Material.
- Stable event identity or seed where needed.

Then presentation derives:

- Audio.
- Sparks.
- Camera impulses.
- Cosmetic debris.
- Brief surface splashes.

Do not create one Godot node per changed cell. Aggregate events spatially and impose presentation-only budgets.

Gameplay effects must happen in the core, not by consuming a cosmetic event that might be dropped.

---

# 9. The simulation gaps that matter next

I would not add hundreds of materials. I would improve a few categories of behavior.

## 9.1 State beyond material ID

A `uint8` material grid is efficient, but material identity alone cannot represent everything you will likely want.

Potential auxiliary state includes:

- Temperature.
- Remaining fuel.
- Lifetime.
- Wetness.
- Dissolved content.
- Velocity or motion hints.
- Structural/object membership.

Not every cell needs every field.

Use compact side arrays, per-chunk optional fields, or material-specific state where justified.

Also, `uint8` caps the roster at 256 IDs, including reserved IDs. That is not an immediate crisis, but decide how visual variants, material states, and genuinely different materials consume IDs.

**Do not spend separate material IDs on every cosmetic rock variation.**

## 9.2 Movement quality

A powder/liquid/gas dispatch abstraction is a good start. Assess:

- Does liquid equalize across connected spaces?
- How does it behave in a U-shaped passage?
- How much does movement depend on scan direction?
- Can sand jam convincingly?
- Do liquids separate reliably by density?
- Are viscosity differences meaningful?
- Does gas disperse naturally or form persistent grid streaks?
- What is the maximum propagation or movement distance per tick?

You do not need a Navier–Stokes solver. You do need deliberate behavior rather than whatever a local swap rule happens to produce.

## 9.3 Temperature transport

Conduction alone is insufficient if heat does not travel with moving material.

Test explicitly:

- Does hot water carry its heat when it moves?
- Does an oil/water swap move temperature and auxiliary state correctly?
- Is ambient heat represented consistently?
- Are integer transfer rules biased by update order?
- Does rounding create or destroy energy unintentionally?
- Do phase changes cause rapid boil/condense oscillation?

For stable phase changes, use some combination of:

- Hysteresis.
- Energy cost.
- Latent-heat approximation.
- Controlled reaction ordering.

You can keep the model stylized. The important thing is that it behaves predictably.

## 9.4 Explosions and forces

Noita-like interaction is not only adjacent-cell chemistry.

You will need some concept of:

- Terrain removal.
- Impulse to entities.
- Heat injection.
- Ejected material.
- Projectile collision.
- Material hardness or resistance.

Begin with bounded deterministic radial operations. Do not build a full pressure-wave solver first.

## 9.5 Object coupling

Loose material plus fixed terrain is one problem.

A burning wooden beam that rotates, collides, sheds particles, and displaces water is substantially harder.

For an initial survival slice, use a small set of scripted or simplified authoritative objects:

- Crate.
- Door.
- Barrel.
- Platform.
- Support.

Prove how objects:

- Occupy or block cells.
- Receive heat and damage.
- Release material on destruction.
- Interact with liquids.
- Save and restore.

Defer arbitrary pixel-to-rigid-body fragmentation until this is solid.

---

# 10. Performance: 7 ms is significant, but measure the right thing

At 60 Hz, the frame interval is approximately 16.67 ms.

A 6.8 ms simulation step is about **41% of that interval**, not literally most of it. It is nevertheless a substantial CPU cost before gameplay, AI, world generation, bridge work, and spikes.

Also, the screenshot’s 120 FPS / 8.33 ms frame reading does not establish the full execution model.

Questions to answer:

- Is the simulation stepping once per rendered frame or at a fixed 60 Hz?
- Are some 120 Hz render frames doing no simulation?
- Is the HUD showing an average, latest sample, or smoothed value?
- Are timings from an optimized standalone build?
- Does “sim step” exclude snapshot generation and uploads?

Do not make major performance decisions from a single HUD number.

## 10.1 Instrument these separately

**CPU**

- Reactions.
- Powder movement.
- Liquid movement.
- Gas movement.
- Temperature.
- Active-region maintenance.
- Snapshot extraction.
- Image preparation.
- Texture upload submission.
- Gameplay entities.

**GPU**

- Base world.
- Liquid pass.
- Lighting.
- Bloom.
- Particles.
- Final upscale/composite.

Track median, p95, and worst-case spikes.

## 10.2 Use several benchmark worlds

At minimum:

1. Mostly empty.
2. Mostly static terrain.
3. Large falling sand volume.
4. Large moving liquid volume.
5. Broad combustion.
6. Broad temperature activity.
7. Repeated explosions.
8. Camera traversing chunk boundaries.

A sparse thermal map can look excellent in one test and become expensive when a large region heats up.

## 10.3 First optimization targets

### Compile data into efficient runtime tables

Keep JSON flexibility at load time; eliminate unnecessary interpretation in inner loops.

Examples:

- Tags compiled to bitsets.
- Reactions indexed by material or material pair where useful.
- Movement properties in dense arrays.
- Stable compiled rule ordering.
- Temperature defaults available without hash lookup.

### Active regions and sleeping

If the whole 512² grid is scanned every pass, this is likely the first major scaling opportunity.

Use:

- Active chunks.
- Dirty/active rectangles.
- Neighbor wake-up.
- Separate activity for movement, reactions, and temperature.

A resting sand pile can stop moving while still needing to react to heat. Sleeping cannot mean “ignore this chunk forever.”

### Evaluate sparse temperature representation

If “sparse map” means per-cell hash-table access, profile it carefully.

Possible alternatives:

- Optional dense temperature arrays per chunk.
- Dense temperature with an active thermal mask.
- Sparse active chunks rather than sparse individual cells.

For reference, a dense `int16` temperature array at 512² is only 512 KiB. Memory savings from sparsity can be outweighed by lookup and allocation costs.

### Parallelism later, with a deterministic schedule

Do not start by putting arbitrary chunks on a worker pool.

Cross-boundary movement and reactions need defined ownership and conflict resolution.

Options include:

- Deterministic staged updates.
- Chunk coloring.
- Interior/boundary phases.
- Double buffering where suitable.
- Deterministic command collection and merge.

Test output identity across worker counts and scheduling perturbations.

---

# 11. Rendering architecture: your two-layer approach is not a dead end

**Dirty chunks are a good transport mechanism. Two baked RGBA layers should not become the entire rendering contract.**

You eventually want to separate:

- Material identity.
- Albedo.
- Coverage/transparency.
- Emission.
- Light blocking.
- Optional heat/activity data.
- Dynamic presentation animation.

## 11.1 Recommended conceptual inputs

| Input | Purpose | Update behavior |
|---|---|---|
| Material ID | Classification and lookup | Changed cells/chunks |
| Static albedo or visual variation | Terrain appearance | On creation/edit, or shader-derived |
| Coverage/optical properties | Liquids and gases | Changed regions |
| Emission inputs | Fire/lava/hot objects | Changed state; animation in shader |
| Opacity/blocking | Lighting | Changed terrain/materials |
| Optional thermal/activity field | Heat glow, disturbance | Only where needed |

You do not necessarily need six separate textures. Some can be derived or packed together.

The principle is more important than the packing.

## 11.2 Integer identity must stay categorical

Never linearly filter material IDs and then interpret the result as a material.

If a chosen Godot path requires normalized texture storage rather than a directly sampled integer format:

- Use nearest filtering.
- Disable mipmaps for categorical data.
- Decode deliberately.
- Avoid unintended color-space conversion.

Keep filtered liquid coverage separate from categorical identity.

## 11.3 Dirty-region halos

Neighbor-based shaders and CPU effects introduce dependencies outside the changed cell.

Examples:

- A water change affects nearby surface classification.
- Removing stone changes nearby edge shading.
- A light source affects a much larger area.
- Blur samples across chunk boundaries.

You need explicit invalidation rules and chunk gutters or a shared atlas with correct sampling.

Otherwise, seams will appear at 64-cell boundaries.

## 11.4 Lighting dirtiness is not material dirtiness

A fire animating in one chunk can change illumination in several unchanged chunks.

Therefore:

- Material snapshots can remain dirty-driven.
- Animated shading should be independent.
- Lighting needs its own update region or camera-local refresh strategy.

Do not rebake neighboring chunks’ albedo every time a flame flickers.

## 11.5 Actual GPU upload granularity matters

“Only repaint dirty chunks” does not necessarily mean “only upload dirty chunks.”

Inspect the exact API path you use. Updating a whole `ImageTexture` may still submit a whole image even if you only changed a small CPU rectangle.

Possible approaches include:

- One texture per chunk.
- A visible-region atlas.
- Larger grouped textures.
- A lower-level upload path, if profiling justifies it.

At your current size, raw bandwidth is not alarming:

- 512² × 4 bytes ≈ 1 MiB per RGBA8 layer.
- Two layers ≈ 2 MiB.
- Full updates at 60 Hz ≈ 120 MiB/s before other overhead.

That alone is not a reason to rewrite the renderer. Copies, synchronization, allocations, and driver behavior may matter more.

Conversely, tiny per-chunk textures can create excessive draw and upload overhead at large visible chunk counts.

Measure the trade-off.

## 11.6 Snapshot threading

When simulation and rendering become asynchronous, define:

- Who owns each snapshot.
- When it becomes immutable.
- Which tick it represents.
- When its buffer can be reused.

Use double/triple buffering or another explicit handoff.

Do not let rendering read arrays while simulation mutates them.

---

# 12. Godot implementation strategy and Mac considerations

## 12.1 Stay with Godot for now

Godot already provides:

- Native desktop application infrastructure.
- Input.
- Audio.
- UI.
- Resource management.
- Profiling tools.
- Cameras and sprites.
- Particle systems.
- Shader integration.

Replacing all of that does not improve the cellular simulation automatically.

Your C++ core already preserves the option to migrate later.

**Engine replacement should require evidence of a specific, measured blocker.**

## 12.2 Use a low-resolution world render target

For a pixel game, distinguish:

1. Simulation resolution.
2. World-render resolution.
3. Lighting resolution.
4. Display resolution.
5. UI resolution.

Do not run all world effects at the full Retina backbuffer resolution just because the window is large.

A good arrangement is:

- World rendered near its intended pixel-art resolution.
- Lighting at an equal or lower resolution.
- Bloom at reduced resolution.
- Final world image upscaled.
- UI rendered separately at display resolution.

At 4K, a full-screen pass touches millions of pixels. At a modest logical world resolution, the same effect is dramatically cheaper.

Maintain deliberate nearest filtering for the crisp world image. Bloom can be smoother without blurring the base pixels.

## 12.3 Prefer ordinary render passes first

For P3, use:

- Canvas shaders.
- SubViewports/render targets where needed.
- A small, explicit multipass pipeline.

Use compute or lower-level RenderingDevice work when profiling or an algorithm requires it, not merely because you have C++ available.

Ping-pong propagation and custom HDR compositing need careful resource setup. Validate the exact renderer and Godot build you intend to ship; do not assume every 2D glow or render-target feature behaves identically across backends.

## 12.3b Test on the actual Mac target early

*(Second §12.3 in source — duplicate heading preserved as received.)*

Define the supported baseline:

- Apple Silicon only?
- Intel Macs too?
- Minimum memory?
- Minimum OS?
- Target display resolution?
- Required renderer/backend?

Then test:

- Native release builds.
- GDExtension architecture compatibility.
- Sustained thermals.
- Frame pacing.
- Shader correctness.
- Save/replay identity across ARM64 and x86-64, if both are supported.

Do not optimize only against a high-end development desktop.

---

# 13. Native Noita port versus an original game

These are very different projects.

## A genuine native port

A supported native port normally requires:

- Source access.
- Rights to modify and distribute the game.
- Developer/publisher involvement.
- Platform integration and testing.

Without that, running the existing Windows game through a compatibility layer is not the same as creating a native source port.

Binary-level reimplementation would be a substantial technical and legal undertaking, and it does not provide a sensible shortcut to your custom survival RPG.

## An original Noita-inspired game

This is the practical path given your current work.

Build your own:

- Simulation rules.
- Art.
- Sound.
- World design.
- Player systems.
- Progression.
- Identity.

Use Noita as a quality and interaction reference, not as a specification that every internal mechanism must reproduce.

---

# 14. Determinism and persistence need a second audit before scaling

Your current save tests are promising. Expand them now, while the system is still small.

## 14.1 Stable rule identity

Hashing with `rule-index` means inserting or reordering a rule can alter random outcomes for unrelated rules.

That is not necessarily wrong under “same data → same result,” but it makes content evolution fragile.

Prefer a stable rule ID or explicitly versioned compiled ordering.

Use separate random/hash domains for:

- Movement.
- Reactions.
- Lifetimes.
- World generation.
- Cosmetic variation.

Adding a cosmetic effect should never perturb gameplay randomness.

## 14.2 Save continuation is the important test

Test:

```text
Run A:
    simulate N ticks
    save
    simulate M more ticks

Run B:
    load that save
    simulate M ticks

Compare authoritative state
```

Include:

- Tick.
- World seed.
- Entity state.
- Auxiliary cell fields.
- Pending gameplay actions.
- Any scheduling state that affects future behavior.

Active/sleep state deserves attention. If restoring it differently changes which cells update next, it is not merely an optimization detail.

## 14.3 Byte-identical saves need a defined boundary

Decide whether identity covers:

- Canonical uncompressed world serialization.
- The entire compressed file.
- Metadata such as timestamps.
- Version-specific container details.

Canonical payload identity is often a more useful contract than requiring every possible file wrapper to be identical indefinitely.

## 14.4 Cross-platform CI

For supported targets, compare deterministic replay hashes in:

- ARM64.
- x86-64.
- Debug and optimized builds where practical.
- Different worker counts after parallelization.

These tests are much stronger than repeated runs on one machine.

---

# 15. World scale and survival systems are the next major architectural challenge

A 512² simulation is a good lab. A persistent survival world introduces new semantics.

## 15.1 Do not tie simulation strictly to the camera

If looking away stops a fire, the camera becomes a gameplay control.

Instead, define an authoritative activity region around players and important events.

Possible policies:

- Fully simulate nearby chunks.
- Keep dangerous active chunks alive until they settle.
- Freeze distant chunks under an explicit rule.
- Use coarse distant updates for selected systems.

There is no universally correct choice.

But coarse simulation is not automatically equivalent to detailed cellular simulation. You do not promise exact catch-up without proving it.

Chunk load/unload and activation rules must themselves be deterministic.

## 15.2 Persistent destruction

Decide whether saves store:

- Entire modified chunks.
- Deltas against deterministic generation.
- A hybrid.

Record the world-generation version. Regenerating terrain with a changed generator can invalidate old deltas.

## 15.3 Character and terrain coupling

You do not want one physics object per solid cell.

For a deterministic character, consider direct C++ grid queries using:

- Swept collision tests.
- A simple authoritative character shape.
- Fixed-point position and velocity if required.
- Explicit slope/step handling.
- Liquid occupancy sampling.

Render the character in Godot.

For larger bodies, chunk collision contours or simplified occupancy proxies can work, but contour rebuilding needs a budget.

## 15.4 Survival should exploit the simulation

Do not begin with a long list of generic meters.

A strong initial loop could be:

1. Explore a cold cave.
2. Collect fuel and useful material.
3. Create a heat source.
4. Use heat to process a resource.
5. Deal with water, fire spread, and terrain consequences.
6. Return to a persistent shelter.

The simulation should change decisions.

If hunger, crafting, and inventory do not interact with the material world, you risk building two loosely connected games.

---

# 16. Proposed phased roadmap

## P3A — Presentation foundation

**Scope**

- Authored cave benchmark.
- Readable player placeholder.
- Camera follow and stable scaling.
- Background walls.
- Separate debug UI.
- Material render profiles.
- Simple clustered light.
- Emission and restrained bloom.
- Tick-driven fire animation.

**Do not include**

- Large-world streaming.
- Arbitrary rigid-body destruction.
- Dozens of materials.
- General global illumination.

**Exit criteria**

- The scene reads as a place at normal play scale.
- Fire visibly changes nearby surface appearance.
- Materials remain distinguishable with bloom disabled.
- Presentation toggles do not change simulation hashes.
- Pause/replay animation behavior is documented.
- Performance measured on the target Mac.

---

## P3B — Cohesive materials and reaction feedback

**Scope**

- Structured stone/wood/sand appearance.
- Improved water boundary classification.
- Cosmetic smoke, sparks, and debris.
- Event aggregation.
- Ignition, extinguishing, digging, pouring, and impact audio.
- Optional coarse camera-local light-field prototype.

**Exit criteria**

- No chunk seams during camera motion.
- No entire-world uploads solely for flicker.
- No one-node-per-cell effects.
- Particles remain bounded under large fires.
- UI stays crisp and independent of world post-processing.
- Lighting and upload costs have separate measurements.

---

## P3C — Simulation quality and scaling

*(Received text labels this P3C; the task-backlog convention calls the sim-quality
phase P4A. Same scope — name normalized at planning time.)*

**Scope**

- Detailed profiling.
- Active/sleeping regions.
- Thermal storage evaluation.
- Compiled reaction indexing.
- Movement-bias tests.
- Heat-advection tests.
- Better combustion lifecycle.
- A small number of useful phase changes.

**Exit criteria**

- Settled scenes cost substantially less than active scenes.
- Chunk-boundary behavior is tested.
- Save/load continuation matches.
- Supported architectures agree on replay hashes.
- Heavy scenes meet a defined p95 CPU budget.

---

## P4A — Playable interaction slice

**Scope**

- Authoritative player movement.
- Digging.
- Pouring or a container tool.
- Health and one meaningful environmental status.
- One projectile or explosive tool.
- One destructible object.
- One enemy or hazard.

**Exit criteria**

A player can complete an interaction chain such as:

> Dig toward a resource, expose water, redirect it, extinguish burning wood, then destroy a weakened barrier to reach the next room.

That is a much stronger proof than separately demonstrating sand, water, fire, and explosions.

---

## P5 — Persistent survival region

**Scope**

- Chunked terrain.
- Persistent edits.
- Deterministic world generation.
- Explicit distant-simulation policy.
- Inventory and resource processing.
- Shelter/base persistence.
- A small amount of progression.

**Exit criteria**

- Travel does not cause severe frame spikes.
- Returning preserves meaningful world changes.
- Save/load preserves active hazards correctly.
- One complete survival loop is enjoyable without debug tools.

---

# 17. Performance targets for planning

These are **provisional budgets to validate**, not predicted timings:

| Subsystem | Initial target |
|---|---:|
| Cellular simulation | Roughly 4–7 ms CPU in representative active scenes |
| Gameplay/entities | Roughly 1–2 ms CPU |
| Snapshot preparation/submission | Preferably below 1–2 ms CPU |
| World and material rendering | Roughly 1–2 ms GPU at logical resolution |
| Lighting and bloom | Roughly 1–3 ms GPU combined |
| Particles/composite | Bounded, with graceful quality reduction |

CPU and GPU costs do not simply add because they can overlap. Main-thread dependencies and synchronization determine the actual frame.

The important outcome is:

- Stable 60 Hz.
- Headroom for spikes.
- No unbounded effect cost.
- A defined quality fallback.

Cosmetic particles, bloom resolution, light-field resolution, and extra lighting passes can degrade gracefully. Authoritative simulation must not change according to the machine’s instantaneous frame rate.

---

# 18. What I would not build yet

I would explicitly defer:

- A custom replacement engine.
- GPU-authoritative cellular simulation.
- Full computational fluid dynamics.
- Spectral or physically accurate light transport.
- Fully general chemistry.
- Per-cell Godot nodes or lights.
- Arbitrary rotating pixel-body fragmentation.
- Multiplayer lockstep.
- Hundreds of materials.
- Oxygen, humidity, pressure, and structural mechanics all at once.

These are not bad ideas. They are poor first investments for the current gap.

---

# Final recommendation

Your clean simulation boundary is an asset. **Do not scrap it because the current demo looks flat.**

But also do not accept the reassuring idea that lighting is the only substantial thing between this prototype and Noita.

You have three separate jobs:

1. **Make the world visually coherent:** composition, texture, illumination, motion, sound.
2. **Make interactions robust and expressive:** combustion, heat transport, tools, objects, player coupling.
3. **Make the world scale and persist:** active chunks, deterministic scheduling, streaming, saves.

For the immediate next step, I would approve a tightly scoped P3:

> **Keep the current C++ simulation. Replace the test-box presentation with one player-scale cave. Add structured material profiles, fire emission, a small amount of actual local illumination, restrained bloom, shader-side tick animation, and event-driven cosmetic feedback.**

Then judge it with a short playable sequence, profiling captures, and before/after clips—not a material count.

That gives you the fastest visible improvement while preserving the architecture needed for the harder work ahead.

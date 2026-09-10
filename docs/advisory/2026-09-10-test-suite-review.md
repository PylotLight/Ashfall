# External Test-Suite Review — raw feedback (2026-09-10)

Source: advisory model review of `tests/simulation_tests.cpp` (1690 lines).
Reproduced verbatim for the local record. Follow-ups extracted into
`docs/advisory/PERF_TASK_BACKLOG.md` (test-hardening section). This closes the
last open review item — every layer has now had at least one pass.

---

## Test-suite verdict

**Good regression foundation; not yet a sufficient migration safety net.** The strongest tests check conservation per tick, explicit phase behavior, data-driven dispatch, and transactional save rejection. The weakest ones occasionally pass without establishing the behavior named in their messages.

The principal problem is **not excessive exact counts**. Most exact counts here are justified. It is **weak oracles, fixtures that already satisfy the goal, and simulation tests coupled to save-layout offsets**.

This is source inspection, not a test execution.

---

# 1. Fix these misleading passes first

## T1. Slope drainage assertion is vacuous

In `test_water_trickles_down_sand_slope()`:

```cpp
expect(total_water == 1, ...);
expect(slope_water <= 1, "open slope drains instead of dotting up");
```

`slope_water` is a subset of `total_water`. Once the first assertion passes, the second necessarily passes.

This tests water/sand conservation, **not drainage**.

**Fix:** create separate fixtures for:

- An open slope with a demonstrable escape route: the droplet must reach a defined lower region within a tick budget.
- A fully enclosed one-cell pocket: the droplet must remain.

Do not simply change `<= 1` to `== 0` without checking whether the evolving sand creates a legitimate pocket.

## T2. Shadowing regression can pass if burnout never happens

In `test_reaction_inapplicable_rule_does_not_shadow()`:

```cpp
world.step();
expect(world.cell({0, 0}) == ashfall::kAir, ...);
```

A surviving fire can **rise out of `{0,0}`**, leaving air behind. That means the test can pass with the original “immortal fire” bug.

**Fix:** assert the only remaining occupied cell is the wood:

```cpp
expect(world.occupied_cell_count() == 1,
       "burnout removes fire despite inapplicable higher-priority rule");
expect(world.cell({1, 0}) == ashfall::kWood, "wood survives");
```

Optionally check a complete material histogram as well.

This is the highest-value tiny fix in the file.

## T3. “Steady state” means only one unchanged tick

Both steady-state tests stop at the first equal consecutive payload.

With tick-dependent arbitration, an unchanged configuration at tick `t` can move at `t+1`. Thus:

> One unchanged tick proves a pause, not quiescence.

They also inspect:

```cpp
std::equal(current.begin() + 16, current.end(), previous.begin() + 16)
```

That assumes the save layout and that everything after byte 16 is tick-independent. The adopted v3 CRC/header changes can invalidate that assumption.

**Fix:**

- Compare semantic cell state, not sliced save bytes.
- Require stability over a sustained verification window.
- For the full-water basin, also assert the expected filled bottom row.
- For stronger evidence, assert no legal movement exists under the relevant movement contract.

A finite stability window is still bounded evidence, so name it honestly.

## T4. Surface flattening starts with an already-level surface

`test_water_surface_flattens_in_basin()` initializes:

- One full bottom row.
- A sparse partial row immediately above.

Its column counts already differ by at most one **before stepping**. The test therefore mostly checks preservation of that property.

Furthermore, column water counts do not establish an actual level surface:

- Empty columns are excluded from `min_height`.
- Floating water or internal air gaps are not detected.

**Fix:**

- Rename this fixture to “partial surface remains level,” if retained.
- Use a genuinely uneven initial condition for flattening.
- Measure topmost water position and require bottom-contiguous water columns in the water-only fixture.
- Include empty columns when comparing filled heights.

`test_water_mound_levels_quickly()` is the better existing progress test, though its current condition only requires all water to reach the bottom three rows—not full leveling.

The unused `previous = world.save()` should also be removed.

## T5. Reset dirtiness assertion is masked by existing dirtiness

In `test_player_collision_and_dirty_tracking()`:

1. Load creates dirty chunks `{0,0}` and `{2,0}`.
2. Reset runs without clearing them.
3. The test expects dirty count `2`.

If reset did **nothing** to dirtiness, the assertion could still pass.

**Fix:**

```cpp
world.clear_dirty_chunks();
world.reset();
expect(world.dirty_chunks() ==
           std::vector<ashfall::CellPosition>{{2, 0}},
       "reset dirties the removed populated chunk");
```

Use membership rather than exact equality if extra invalidations are permitted by contract.

Keep a separate test for dirty-set accumulation across operations.

## T6. Oil flash silently omits the temperature-enable flag

```cpp
flash.product_temperature = 150;
```

but not:

```cpp
flash.has_product_temperature = true;
```

The other reaction fixtures set both.

As written, the fixture does not request product temperature under the declared flag-based API, and it never checks heat.

It also says “within a few deterministic rolls,” then demands ignition on **the first tick at 90%**.

**Split the intent:**

- Generic oil-to-fire reaction behavior: `p=100`, enable temperature explicitly, check products and heat.
- Hash-roll regression: deliberately retain a named 90% first-tick golden case.
- Production-content integration: load the actual oil-flash row.

Do not change to a multi-tick loop casually: if ignition fails initially, fire and oil may move apart and cease to be eligible.

---

# 2. What the exact counts actually mean

## Keep these exact counts

These are justified by fixture construction or explicit contracts:

- `66` sand and `60` water conserved every tick.
- `15` sand and `194` water in the sinking basin.
- `45` oil and `36` water in the separation fixture.
- Two fire products from one p=100 pair reaction.
- One ignition per primary per tick.
- Exact temperatures derived from the specified integer stencil.
- Zero stored temperatures after cooling to ambient.
- Exact table presentation order.

These are not gratuitously brittle. Replacing them with loose inequalities would weaken the suite.

For occupancy totals that include walls, a material histogram is clearer and harder to fool. For example, an unchanged global occupied count alone would not detect one liquid disappearing while an erroneous stone appears.

## Label these as content/golden contracts

- Spawn exactly `{256,480}`.
- Pit contains exactly `19` wood and `2` fire.
- First-tick oil ignition at `p=90`.
- Fire fully gone after 200 ticks using a specific probabilistic rule set.
- Exact authored kiln geometry.

These can be valuable, but their scope is different:

> They pin an authored scenario or deterministic schedule; they do not prove that the behavior is universally desirable.

Separate content acceptance from engine correctness so intentional level tuning does not look like a physics regression.

## Thermal exactness is useful—but not physical validation

The `100 → 20 → 4 → 0` sequence correctly specifies the current dissipative stencil. It does not prove realistic conduction or heat conservation.

One documentation mismatch needs an explicit decision:

```cpp
expect(world.temperature({1, 1}) == -32767,
       "cold clamps to int16 min");
```

The `int16_t` minimum is **−32768**.

If the intended range is symmetric `[-32767,32767]`, retain that behavior and correct the message/public contract. If full int16 range is intended, this expectation is wrong. This test alone does not establish which implementation is desirable.

---

# 3. Coverage that is genuinely strong

Several parts are worth preserving almost unchanged.

### Per-tick material conservation

`test_mass_conservation()` checks both materials every tick, not only at the end. That catches transient loss/duplication that a final-state comparison could miss.

Add static-wall preservation and a global occupied-count check to make the fixture more complete.

### Explicit reaction phase contracts

The spread, condensation, ignition-gate, and lava-crust fixtures distinguish:

1. Reactions.
2. Movement of products.
3. Heat carry.
4. Conduction.

That is valuable protection when reorganizing `step()`. The arithmetic expectations are interpretable rather than opaque snapshots.

### Failed-save-load state preservation

`expect_rejected()` compares full saved state after rejection. That is substantially stronger than checking the return value.

Extend it to state not serialized—especially dirty tracking—and to heated worlds.

### Data-driven remapping

Remapped material IDs exercising powder fall, liquid spreading, and sinking are a good defense against hardcoded ID dispatch.

The test name overstates the result slightly: it verifies selected equivalent behaviors, not a full side-by-side trajectory comparison. A paired normalized-state comparison would strengthen it.

### Missing-chunk behavior has an explicit intended contract

This packet resolves part of the prior renderer question:

```cpp
expect(world.read_chunk({1, 1}, empty_chunk), ...);
expect(empty_chunk[0] == ashfall::kAir, ...);
```

The test clearly intends:

> Reading a missing chunk succeeds and fills the output with air.

But it currently checks **only the first element**. Change that to `std::all_of`. Likewise, test `copy_rect()` over populated and missing regions using a non-air-prefilled buffer.

---

# 4. Determinism coverage needs stronger adversarial fixtures

## Same-process replay is necessary but limited

`test_command_replay()` proves identical replay for one scenario in the same executable/process.

It does not establish:

- Cross-platform bitwise identity.
- Independence from capacity/growth history.
- Independence from save/load reconstruction.
- Independence from hash-table iteration under collision-heavy workloads.

Those are central to the upcoming hash/storage work.

## “Scan-order independent” tests insertion order, not arbitrary scan order

Reversing insertion order is useful. But six entries may occupy the same buckets regardless of insertion order, and only the final result after five ticks is compared.

The test does not force a different internal traversal or prove independence from every scan order.

**Strengthen with:**

1. Enough keys to exercise growth and collision chains.
2. Reversed and deterministic permuted insertion orders.
3. A grow-then-delete history producing the same final logical world.
4. Snapshot equality before stepping.
5. Comparison after **every tick**.
6. Reaction fixtures where multiple primaries compete for one secondary.

Use fixed deterministic constructions; no runtime RNG is needed.

Also avoid requiring translated worlds to have identical motion: coordinate-based hash decisions intentionally can differ under translation. Translation tests should check invariants unless equivalence is part of the contract.

## Add save/load continuation

Round-trip equality is excellent, but it can miss omitted transient state that affects later execution.

Add:

```text
Build → run N ticks → save/load into twin
Then step both for M ticks and compare after each tick
```

Include reactions, nonzero temperatures, swaps, and negative chunk boundaries. With current saves, load identical material/reaction configuration into the twin first.

This is one of the best migration tests per line of code.

---

# 5. Transactional and parser assertions sometimes overclaim

## Invalid material-table load does not verify “untouched”

`test_loading_table_resets_world()` attempts an invalid load **after successfully resetting to an empty world**, then checks only rejection.

Populate cells, heat, reactions, and tick first; attempt the invalid load; compare all relevant state afterward.

## Reaction-count equality does not prove old rules survived

`test_reactions_reject_unknown_materials()` checks that one rule remains.

A broken loader could replace the old rule with a different rule and still pass. After rejection, run a fixture that demonstrates the original quench behavior remains intact.

## Parser strictness coverage is only a start

The current material+tag test covers both selectors with nonempty strings. It does not cover the previously identified empty-selector exclusivity cases.

Add those and the already-adopted limits/unknown-key tests as backlog items land. **Nothing here warrants reopening the rejected parser-overflow finding.**

## “Canonical” literals are not the actual files

The material test compares a copied JSON literal against code defaults. It does not compare either with `data/materials.json`.

More concretely, the copied “canonical” reaction fixture contains **eight rows**, while the handoff reports nine production rows. It omits oil-flash.

That may be fine as a frozen fixture, but then name it that way. For a production-content integration test, read the actual file through an explicit test-data path.

---

# 6. Save-format tests should be isolated before v3

These are appropriate **version-specific** checks:

- Hand-built v1 payload.
- Exact empty v2 size.
- Mutation of the last material byte via `size() - 5`.

They should live in a serialization test group, not leak into movement/steady-state oracles.

For v3, distinguish:

1. **Framing/integrity rejection:** corrupt bytes and expect CRC failure.
2. **Semantic rejection:** construct an integrity-valid payload with invalid roster references, ordering, duplicates, or limits.

Otherwise a malformed-material test may pass solely because the CRC is wrong, never exercising material validation.

Keep independent hand-built fixtures. If every test input comes from `save()`, matching encoder/decoder mistakes can round-trip successfully.

---

# 7. Highest-value migration additions

Before chunk-authoritative migration, I would add these—not another dozen long settling scenarios:

| Addition | Protects |
|---|---|
| `cell()` vs `copy_rect()` vs `read_chunk()` agreement after every edit/tick | Authoritative state versus derived chunk representation |
| `occupied_cell_count()` equals globally scanned count | Maintained-counter correctness |
| Last-cell deletion removes material chunk and dirties its location | Renderer clearing |
| Reset/load dirtiness with a freshly cleared dirty set | Old-region invalidation |
| Save/load continuation | Hidden/transient-state dependencies |
| Grow/delete/reinsert histories with identical logical state | Container-history independence |
| Two hot/cold materials swapping, including a chunk boundary | Already-filed heat-advection/duplication regression |
| Multiple reactions competing for the same cell | Claim arbitration and one-reaction-per-cell contracts |
| Exact temperature gate boundary and p=0/p=100 cases | Rule boundary semantics |
| Read-span size and negative-coordinate boundary cases | Public API contracts |

For heat advection, the current falling-sand test only covers a move into air. It is not a substitute for swap, displacement, and refilled-source fixtures. Keep those tied to the existing heat-correctness task rather than filing a duplicate bug.

---

# Recommended disposition

**Accept the suite as a useful regression base, but qualify “all green” accordingly.**

First patch, with no simulation changes:

1. Fix the vacuous slope assertion through better fixtures.
2. Make the burnout-shadowing test detect surviving moved fire.
3. Isolate reset dirtiness.
4. Replace sliced-save quiescence checks.
5. Correct oil temperature setup and separate its 90% golden behavior.
6. Check every missing-chunk output element.
7. Rename stale/copied “canonical” fixtures accurately.

Then add representation consistency, save/load continuation, and adversarial insertion-history tests before the storage migration.

**Bottom line:** keep the justified exact arithmetic and conservation counts. Strengthen tests that currently confuse “the expected cell is empty,” “nothing changed for one tick,” or “one count stayed equal” with proof of the intended behavior.

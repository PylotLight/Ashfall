# Noita Materials — Research & Adoption Plan

Sources (fetched 2026-09-08):
- https://noita.fandom.com/wiki/Materials — category taxonomy, densities, hardness, notes
- https://docs.google.com/spreadsheets/d/1MVcKCzQ9-LL6JNh1sle36MNrKLXsgDdAyKw39NxVl-Q — community-mined reaction table
  (`probability, Reactant1..3 → Product1..3`, mined via
  https://gist.github.com/vexx32/9c51583291b5cf176d97bd8bb2b85ae0)

Scope note: Noita's design is reference and inspiration, not a copy target.
Names for generic substances (water, oil, lava) are fine; internal ids,
exact tuning, and magical-effect semantics stay our own. Everything below
must still pass `docs/AGENT_GUIDE.md`: data + tests + rendering path for
every material/reaction.

---

## 1. How Noita models materials

Six categories, which map cleanly onto our `Movement` enum plus P2.5 work:

| Noita category      | Behavior                                  | Our engine status          |
|---------------------|-------------------------------------------|----------------------------|
| Static solids       | Never move; hardness 0–6 (dig tiers)      | Done (P1); hardness = future dig tiers |
| Liquids             | Pool flat; float/sink by density          | Done motion; NO density layering yet |
| Magical liquids     | Liquids with transmutative effects        | Deferred (needs entities/P4+) |
| Gases               | Rise, pool under ceilings, dissipate      | P2.5 (`gas` movement)      |
| Powders             | Fall, pile at repose                      | Done (P1)                  |
| Fire / organic      | Energy + special cases                    | P2.5 (fire + temperature)  |

Three mechanisms do almost all the work:

1. **Density layering.** Every liquid has a float density (water defined as
   1.0). Lighter floats, heavier sinks — including *through each other*.
   Our `density` is an integer used only for sand-vs-water swaps; scaling it
   (water = 100) absorbs Noita's table with no format change.
2. **Tags.** Reactions match material *tags* (`[burnable]`, `[meltable]`,
   `[evaporable]`…) as well as specific materials. One rule covers a whole
   family — this is the design to copy for `data/reactions.json`.
3. **Probabilistic neighbor reactions.** Each row is
   `probability + ≤3 reactants → ≤3 products`, products mapping positionally
   (R1→P1, R2→P2). "Probability" is per-cell-update chance; stirring speeds
   mixing. Our equivalent must be deterministic: hash-based rolls like the
   existing `prefers_left`, same inputs → same outputs.

## 2. Density table (curated, mapped to our int scale ×100)

From the wiki liquids table (water = 1.0 baseline):

| Material      | Noita density | Ours | Behavior note              |
|---------------|---------------|------|----------------------------|
| Oil           | 0.4           | 40   | Floats on water; burns long |
| Vomit         | 0.4           | 40   | Light floaters             |
| Worm blood    | 0.5           | 50   | Floats                     |
| Acid          | 0.6           | 60   | Floats; corrodes (see §3)  |
| Poison        | 0.7           | 70   | Evaporates solo            |
| Slime         | 1.3           | 130  | Heavy, slow, sticky        |
| Lava          | 1.4           | 140  | Sinks through water        |
| Blood         | 1.1           | 110  | Sinks slowly in water      |
| Brine         | 1.2           | 120  | Sinks                      |
| Water         | 1.0           | 100  | Baseline                   |

Engine gap this exposes: liquid-on-liquid displacement (oil poured on water
must spread *on top*, blood must sink *through*). Today only powder/liquid
swaps exist. Required for roster A — file under P2.5 as "density-ordered
liquid swaps".

## 3. Tag glossary (from the mined sheet; families we adopt)

- Combustion: `[burnable]`, `[burnable_fast]`, `[fire]`, `[fire_strong]`
- Phase change: `[meltable]`, `[meltable_to_water]`, `[meltable_to_blood]`,
  `[meltable_to_slime]`, `[meltable_to_acid]`, `[meltable_to_cold]`,
  `[meltable_to_radioactive]`, `[meltable_to_poison]`,
  `[meltable_to_lava]` (+`_fast`), `[meltable_metal]`,
  `[meltable_metal_generic]`, `[freezable]`, `[frozen]`, `[cold]`
- Evaporation: `[evaporable]`, `[evaporable_fast]`, `[evaporable_by_fire]`,
  `[evaporable_custom]` (→ `<self>_vapour`), `[vapour]`
- Corrosion: `[acid]`, `[corrodible]` (acid persists, emits gas)
- Context: `[static]`, `[liquid]`, `[water]`, `[lava]`, `[ice]`

Schema impact (planned P2.5, backward compatible): material entries gain
`"tags": [...]` and `"hardness": 0-6`. Reactions gain tag-or-id matching.

## 4. Curated starter reaction set (all verified in the sheet)

Format below: `p=probability | R1 + R2 (+ R3) → P1 + P2 (+ P3)`.

Core physics pairs (roster A — implement first):
| # | Reaction | Notes |
|---|----------|-------|
| 1 | p=80 \| lava + water → rock + steam | The iconic pair |
| 2 | p=20 \| fire + water → fire + steam | Quenching; fire persists |
| 3 | p=70 \| lava + blood → volcanic rock + steam | Blood variant of #1 |
| 4 | p=8/80 \| lava + burnable(_fast) → lava + fire | Ignition |
| 5 | p=40 \| fire + burnable_fast → fire + fire | Fire spread |
| 6 | p=20 \| coal + fire → fire + air | Fuel burns out |
| 7 | p=3–10 \| steam + static → water + static | Condensation on ceilings |
| 8 | p=40 \| ice + fire → water | Melting (`meltable_to_water`) |
| 9 | p=50 \| acid + corrodible → acid + flammable gas | Corrosion; acid persists |
| 10 | p=15–45 \| evaporable + air → air | Dissipation (puddles dry) |
| 11 | p=13 \| water + toxic sludge → water + water | Purification |
| 12 | p=50/20 \| molten + water/cold → solid + steam | Casting |
| 13 | p=100 \| cement + water → concrete + air | Signature Noita alchemy, cheap to prove tags |
| 14 | p=60 \| whiskey + fire → fire + fumes | Fast-burning fuel demo |

Second wave (roster B — needs temperature + freezing + metals):
freezing liquid + water → ice; frozen steel + fire → steel + steam;
rust + water → rusted metal; snow/urine + … ; molten metal family;
poison/blood → slime; creeping fungus spread.

Explicitly deferred (needs entities/status systems, P4+): all magical
liquids (polymorphine, teleportatium, berserkium…), drinking/stain effects,
gold collection, O2/suffocation.

## 5. Adoption roadmap (phase hooks)

- **P2.3 (renderer):** base colors for roster A from the wiki appearances
  (oil light-brown, lava glowing orange, steam light-grey, smoke grey,
  acid deep-green, blood red, ice translucent blue, wood browns, coal black).
- **P2.4 (temperature):** enables the freeze/melt rows (ice↔water,
  freezing liquid, molten↔solid + steam).
- **P2.5 (gas/fire/reactions):** reaction schema mirrors the sheet
  (`probability`, tag-or-id reactants/products, positional mapping,
  deterministic rolls); implement roster A + density-ordered liquid swaps;
  exit proof stays "data-only oil" (oil is row #4/#5 fuel — perfect fit).
- **P5 (world structure):** biome deposits from roster B (coal veins in
  Coal-Pits-like biome, oil pools, frozen vault ice).
- **P6 (vertical slice):** environmental puzzle wants fire + oil/wood +
  water ( Triangle: burn obstacle → quench → steam ), all roster A.
- **Out of scope:** magical liquids, ingestion, stains, perks — tracked
  here so nobody half-implements them during P2.

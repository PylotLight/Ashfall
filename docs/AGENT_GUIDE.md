# Agent Session Guide

## Start here

Read `README.md`, `ARCHITECTURE.md`, `docs/DESIGN.md`, and the relevant phase
in `docs/ROADMAP.md` before changing code. Treat these documents as the
current source of truth; do not revive legacy prototype assumptions.

## Ownership rules

- `engine/` owns simulation state and rules.
- `tests/` proves deterministic engine behaviour.
- The `bridge/` GDExtension translates and renders; it does not implement
  material rules.
- Godot scenes and GDScript are presentation/tooling, not world authority.

## Required evidence for a change

Every simulation change needs a deterministic test. Every performance claim
needs a reproducible benchmark. Every save-format change needs backward
compatibility or an explicit version/migration decision. Every new material
or reaction needs data, test coverage, and a defined rendering path.

## Scope discipline

Work on one roadmap milestone or a bounded portion of one milestone. Do not
start world generation, NPCs, or GPU compute while earlier milestones lack
their exit criteria (P0/P1/P2 verified; P3 next). Avoid broad refactors without a specific measured or tested
problem.

## Handoff format

At the end of a session, report: changed files, behaviour added, tests run,
benchmark result if relevant, known limitation, and the exact next smallest
task. Update the authoritative design or roadmap only when the decision or
phase state truly changes.

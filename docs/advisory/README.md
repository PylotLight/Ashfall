# Advisory Shelf — README

External review rounds and the task backlogs extracted from them. Raw reviews are
verbatim records (minor transcription notes are always marked); backlogs are the
implementation guides — each item carries source section, scope, and acceptance.

## Files

| File | Contents |
|---|---|
| `ARCH_ADVISORY_BRIEF.md` | (repo root `docs/`) Our one-page brief sent to advisors: arch, constraints, gap list, questions. Historical — the answers are below. |
| `PERF_REVIEW_PACKET.md` | (repo root `docs/`) Our benchmark packet (4 scenarios, 6 segments, questions). Historical. |
| `2026-09-10-visual-direction-review.md` | Round 1: look/feel, lighting path (Stages A–C), animation policy, P3A/P3B proposal, `material_visuals.json` sketch. Direction source for P3 scoping. |
| `2026-09-10-perf-quality-review.md` | Round 2: sparse-rebuild diagnosis, heat-duplication bug (§3), non-transactional reactions (§4), 11-commit sequence (§16). |
| `2026-09-10-json-parser-review.md` | Round 3: parser hardening (limits, exclusivity, unknown keys, parity), patch sequence §14, fuzz plan §13. |
| `2026-09-10-flatmap-saveload-review.md` | Round 4 (primary): FlatMap proofs, cache-cliff warning, v3 name-remap design, P1–P4 canonical contract. |
| `2026-09-10-hash-api-review.md` | Round 5: hash verdict (F4 open, low-4-bit correlation), reserve bounds, migration-friendly API + contract gaps. |
| `2026-09-10-bridge-host-review.md` | Round 6: no partial GPU uploads, halo/shader fixes, once-per-frame cadence, player-authority migration, §F test list. |
| `2026-09-10-test-suite-review.md` | Round 7 (final): T1–T6 oracle fixes, golden/content labeling, continuation + adversarial fixtures, §7 migration table. |
| `PERF_TASK_BACKLOG.md` | Implementation guide: perf correctness → optimizations → storage migration → FlatMap/save/hash/bridge/test follow-ups. |
| `JSON_PARSER_BACKLOG.md` | Implementation guide: 3 design decisions + patches 1–9 + corpus + fuzzing. |
| `REVIEW_HANDOFF.md` | Fresh-chat starter: state snapshot, round verdicts, settled decisions, execution order. |

## Workflows

- **Starting implementation:** read `REVIEW_HANDOFF.md` → pick the group → expand
  its backlog section into a plan under `~/.claude/plans/survival-rpg-mvp/` (see the
  writing-plans skill) → subagent-driven execution.
- **New advisory round:** add `YYYY-MM-DD-<topic>-review.md` verbatim + backlog
  delta to the matching `*_BACKLOG.md` + one line in `REVIEW_HANDOFF.md` + one row
  above. Never edit a filed raw review.
- **Settled decisions** (in `REVIEW_HANDOFF.md`) are not relitigated without new
  measurements.

# Identity oracle (Group 4, Task O0)

Behavior-identity gate for every Group 4 optimization: per-tick FNV-1a-64
hashes over canonical `save()` bytes for six deterministic scenarios. Any
oracle hash change after an optimization = behavior change = bug (Group 4
changes NO behavior by definition).

## What the oracle covers

| Scenario | Setup (see `tests/oracle_scenarios.hpp`) | Measured ticks |
|---|---|---|
| A | Settled play level (`build_play_level` + 200 settle) | 200 |
| B | Water basin resolve (walls + water, no settle) | 200 |
| C | Wood-fire + spread/burnout rules (no settle) | 200 |
| D | Static stone box (+ 10 settle; exercises the O4 fast path) | 200 |
| E | Hot-sand/cold-water swap pair, no settle (heat advection + conduction micro) | 20 |
| F | Shared-reactant arbitration pair, no settle (conflict-resolution micro) | 10 |

A–D replicate `bench/bench_step.cpp` verbatim (same geometry, same settle
ticks, same C reaction JSON literal). E mirrors the hot-powder/cold-liquid
swap fixture; F mirrors the shared-reactant double-transform fixture.
Builders live in `tests/oracle_scenarios.hpp`, shared by the record tool and
the replay test so the three can never drift apart.

Hash files: `tests/oracle/<name>.hash`, one `tick_index hexhash` line per
measured tick plus a `#` header comment with the recording git SHA.
`test_oracle_replay` (in `tests/simulation_tests.cpp`) rebuilds each
scenario, steps, and compares per-tick FNV-1a-64 hashes; the mismatch
message names scenario + tick + expected/actual.

## Staleness policy

Hashes change ONLY on intentional behavior change. If a hash file goes stale
(replay fails after a deliberate engine change), the owning task must justify
the behavior delta in its return message and the integrator re-records.
Group 5 (chunk-authoritative migration) must keep this oracle green unless
its own plan explicitly re-baselines it.

## How to re-record

```sh
c++ -std=c++20 -O2 -I engine/include -I tests bench/oracle_record.cpp \
    build/libashfall_sim.a -o /tmp/oracle_record
/tmp/oracle_record          # run from the repo root; writes tests/oracle/*.hash
/tmp/oracle_record          # run TWICE; diff the outputs — must be identical
ctest                       # replay test + full suite must be green
```

CMake wiring (integrator, NOT in ctest): add `tests/` to the test
target's include dirs and add a manual `oracle_record` executable target
built from `bench/oracle_record.cpp` linked against `ashfall_sim`.

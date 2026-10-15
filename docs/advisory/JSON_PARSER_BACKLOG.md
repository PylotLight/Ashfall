# JSON Parser Hardening Backlog (from 2026-09-10 advisory review)

Source: `docs/advisory/2026-09-10-json-parser-review.md` (§ references below).
Status for ALL items: **backlog, unscheduled**. Order follows the reviewer's patch
sequence (§14). Key correction carried forward: there is NO integer-overflow bug in
`require_json_int()`/`parse_int()` today (one-billion cap) — do not file or fix one;
fix the grammar + future-proof the accumulation instead (§3).

Design decisions to make before/during (reviewer gives options, we choose):

- [ ] **Encoding/identifier policy (§9).** Choice A (ASCII `[a-z][a-z0-9_]*`
      identifiers, UTF-8 descriptive text — reviewer's preference) or Choice B
      (validated UTF-8, byte-equality, no locale folding). Either way: document the
      JSON subset prominently (unsupported escapes!) and decide on providing a
      compatible writer vs. full JSON string parsing with integer-only values.
- [ ] **Empty temperature object (§6).** Reject `{"temperature": {}}` in strict
      content mode (reviewer's recommendation) or explicitly allow + lint. Omitting
      the key already means "no gate".
- [ ] **Error contract (§10).** On failure leave `out` unchanged, commit with
      `out = std::move(table)` only on success; clear `error` at the start of
      public parse/validate calls; adopt at least `{path, message}` diagnostics
      (`materials[4].density: ...`, `reactions[2].reactants[1]: ...`), token byte
      offsets optional; `JsonParser::run()` one-shot vs. reset + document
      `string_view` lifetime. Note: header's "line L col C, first error wins" promise
      currently holds for syntax errors only, not schema errors.

## Patches (§14 — scope + exit criterion each)

- [x] **1. Resource limits.** `JsonLimits` (max input bytes, depth 32/64, nodes,
      string bytes, array items, object fields; reaction count + 256 material cap).
      Enforce BEFORE allocating/descending, not post-parse. Plus: explicit
      allocation-failure policy at the host boundary (no blanket `catch (...)`).
      Exit: deep/large malformed inputs reject safely.
- [x] **2. Numeric grammar.** Reject leading zeros (`01`, `-01`, `00`; accept `0`,
      `-0`); reject `.`/`e` continuations explicitly; precondition-style
      accumulation (`(kMax - digit)/10`); document −1e9..+1e9 token policy; optional
      `assert(low <= high)` hygiene. Full boundary corpus in §12 table. Exit:
      numeric boundary corpus passes.
- [x] **3. Presence-based reactant exclusivity (§5).** Check field PRESENCE, not
      value validity; reject unknown slot keys. Early isolated patch. Note the
      subtle case: `{"material":"water","tag":"liquid"}` must FAIL (both keys
      present) even though each value is valid. Exit: all malformed dual-key cases
      reject.
- [x] **4. Strict unknown-key validation (§6).** Allowed-key lists per object
      (material/reactant/product/temperature/documents as listed in §6); typo'd
      optionals (`min_temperatue`, `hardnes`, `product_temperatur`) must fail loudly
      — today they silently disable gates/outputs. Explicit `metadata` object if
      extensions needed. Exit: optional-field typos cannot silently remove behavior.
- [x] **5. Material validator parity (§7).** Enforce density range + exhaustive
      `Movement` switch + 256-count bound in `validate_material_table()` (C++ route),
      not just JSON route — today an invalid programmatic enum reaches `step()` and
      the cell silently disappears (not static, no powder/liquid/gas branch places
      it). Remove unreachable solid/gas duplicate branch. No invented restrictions
      (zero-density dynamic, static nonsolid stay legal; lint, don't reject).
      Exit: invalid C++ enums/densities reject.
- [x] **6. Transactional output + error contract** — see Design decisions above.
      Exit: failed reload preserves prior table.
- [x] **7. Path/location diagnostics** — see Design decisions above. Exit: errors
      identify row + field.
- [x] **8. Bounded semantic linking/lint (§8).** Three stages (parse → structure →
      link vs. material table). At link: unknown material names reject w/ context;
      tags matching nothing reject-or-prominent-report (watch `vapor`/`vapour`);
      primary-`air` rules reject-or-separate-mechanism (executor only iterates
      occupied — cannot fire today); document tag-neighbor-requires-occupied (no
      air-creation bypass exists, but prove it with a test); zero-probability rules
      warn as priority-suppressors, never "safe to remove"; narrow the
      no-spontaneous-creation claim to what the validator actually enforces
      (explicit empty-neighbor slot may not become occupied). Exit: dead references
      and unsupported reactions are visible.
- [ ] **9. Fuzzing (§13).** Three targets (raw parser / material loader /
      reaction loader+linker — never full sim trajectories); clang
      `-fsanitize=fuzzer,address,undefined`; seed corpus (canonical files, minimal
      docs, all §12 cases, truncations, boundary ints, nesting, dup keys, typo
      near-misses) + schema-key dictionary; differential parse-acceptance vs. a
      trusted JSON parser WITHIN our subset only; separate structured
      programmatic-validator fuzzing (byte fuzzing can't forge invalid enums).
      Exit: sanitizer-clean corpus + repeatable fuzz runs.

## Regression corpus to add with the patches (from §12)

Parser syntax table (empty/garbage/trailing-comma/dup-key/unterminated/bad escapes/
`truex`/leading-zeros/boundaries/depth±1/limit boundaries/UTF-8-per-policy) +
material list (dup id/name, bad air, bad movement both routes, density both routes,
hardness both routes, dup/empty tags, unknown field, oversize array, transactional
output, error clearing) + reaction list (5 dual-key slots, wrong optional types,
unknown keys at all levels, gate typos, empty temp object, min>max, temp boundaries,
too many products, dup names, unresolved names/tags, primary-air).

## Explicitly not now

Replacing the parser with a mature dependency — harden first (patches 1–9), then
evaluate on full-JSON-interop need; retain duplicate-key rejection + resource limits
either way. No general-purpose-parser accretion.

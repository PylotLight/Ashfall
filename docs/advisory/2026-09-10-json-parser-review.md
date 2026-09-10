# External JSON-Parser Review — raw feedback (2026-09-10)

Source: advisory model review of `engine/src/json_subset.cpp`,
`engine/include/ashfall/json_subset.hpp`, `engine/src/material_data.cpp`,
`engine/include/ashfall/material_data.hpp`, `engine/src/reaction_data.cpp`,
`engine/include/ashfall/reaction_data.hpp`. Reproduced verbatim for the local
record. Actionable items extracted into `docs/advisory/JSON_PARSER_BACKLOG.md`.

---

Good call splitting these reviews. I’ll focus on the JSON parser and the two content validators here.

**Verdict: this is a readable, reasonably careful small parser, but I would not yet treat it as a hardened content boundary.** The most important issues are:

1. **Unbounded recursion and allocation.**
2. **Malformed reaction alternatives can be silently accepted.**
3. **Unknown fields are silently ignored—including typos that remove temperature gates.**
4. **Programmatic material validation is weaker than JSON validation.**
5. **Error reporting and failure-state contracts do not match the documented promises.**

One important correction to the original smell-list:

> **I do not see an integer-overflow bug in the supplied `require_json_int()` or the current `parse_int()` implementation.**

There are numeric grammar and policy issues, but the current one-billion limit prevents the accumulator from approaching `long long` overflow.

Below I distinguish confirmed defects from design decisions and hardening recommendations.

---

# 1. Findings ranked by priority

| Priority | Finding | Consequence |
|---|---|---|
| **P0 before accepting untrusted content** | Unlimited recursive nesting | Stack overflow from a small, deeply nested document |
| **P0 before accepting untrusted content** | No input/node/string/container limits | Excessive memory use or parse/validation time |
| **P1** | Reactant exclusivity checks valid values rather than field presence | Malformed alternatives can be accepted |
| **P1** | Unknown fields ignored | Typos silently change simulation behavior |
| **P1** | Programmatic validator accepts invalid `Movement` values and out-of-policy densities | Invalid content can reach the simulation through its C++ API |
| **P1** | Numeric parser accepts leading zeros | Accepts documents that are not valid JSON |
| **P2** | Raw non-UTF-8 bytes accepted in strings | “JSON” contract and identifier handling are underspecified |
| **P2** | Parsing clears caller output before validation succeeds | Failed reload can destroy the caller’s previous parsed table |
| **P2** | Schema errors lack locations and paths | Poor diagnosis of large content files |
| **P2** | Quadratic duplicate checks without adequate limits | Avoidable worst-case work |
| **P2** | Success leaves old error strings intact | Ambiguous API behavior |
| **P3 / policy** | Dead or suspicious reaction definitions not diagnosed | Content can load successfully but never execute |

“P0” here means a blocker **if mods, imported files, or externally supplied content are accepted**. For small, repository-controlled files, exposure is lower, but these are still cheap risks to remove.

---

# 2. What is already good

Several choices are worth preserving.

## Explicit supported syntax

You deliberately support:

- Objects.
- Arrays.
- Strings.
- Integers.
- Booleans.

Rejecting floats, `null`, and most escapes is not inherently wrong for a documented subset.

## Duplicate keys are rejected

This is a good trust-boundary policy:

```cpp
for (const auto& field : out.fields) {
    if (field.first == key.str)
        return fail("duplicate key \"" + field.first + "\"");
}
```

It avoids the dangerous ambiguity of:

```json
{"probability": 0, "probability": 100}
```

being interpreted differently by different tools.

## Trailing garbage is rejected

`run()` checks that the whole input was consumed. That prevents valid-prefix acceptance such as:

```text
{"version": 1} arbitrary trailing text
```

## Narrowing happens after range checks

For the supplied JSON paths:

- Material IDs are checked before conversion to `MaterialId`.
- Colors are checked before conversion to `uint8_t`.
- Density, probability, hardness, and temperatures are bounded before conversion to `int`.

That is correct.

## Parsing and engine activation are separate

Parsing into a table and then validating/resolving before loading the simulation is a sound design.

It just needs a clearer, consistent contract between those stages.

---

# 3. Integer handling: no current overflow, but fix the grammar

## 3.1 `require_json_int()` is safe as written

It does only:

```cpp
if (field->integer < low || field->integer > high)
```

and then assigns one `long long` to another.

There is no arithmetic here that can overflow.

The caller still needs to provide sensible bounds and narrow only after success, which your shown call sites do.

An optional programmer-error check is:

```cpp
assert(low <= high);
```

That is API hygiene, not a malformed-input vulnerability.

## 3.2 `parse_int()` does not currently overflow

The relevant code is:

```cpp
value = value * 10 + digit;
if (value > 1000000000LL)
    return fail("integer out of range");
```

At the start of every continuing iteration:

```text
0 ≤ value ≤ 1,000,000,000
```

Therefore the largest intermediate value is:

```text
10,000,000,009
```

That fits comfortably in `long long`.

The parser returns immediately when the bound is exceeded. A million-digit nonzero integer will not continue accumulating indefinitely.

**Do not file this as an overflow bug in the present implementation.**

## 3.3 The confirmed numeric bug: leading zeros

These currently parse:

```json
01
```

```json
-01
```

```json
000000
```

They are not valid JSON numbers.

A strict subset of JSON should reject syntax that full JSON rejects.

### Recommended grammar

After an optional minus:

- If the first digit is `0`, consume it and reject another digit.
- Otherwise consume a sequence starting with `1`–`9`.
- Reject unsupported numeric continuations such as `.` or `e` with an explicit message.

`-0` is valid JSON; accepting and normalizing it to integer zero is fine.

## 3.4 Make overflow checking future-proof

Although it is safe now, I would change accumulation to a precondition check so a future limit increase cannot introduce overflow:

```cpp
constexpr long long kMaxMagnitude = 1'000'000'000;

const int digit = text_[pos_] - '0';
if (value > (kMaxMagnitude - digit) / 10)
    return fail("integer outside supported range");

value = value * 10 + digit;
```

Then clearly document:

> This format supports integer tokens in the range −1,000,000,000 through +1,000,000,000.

That is a format policy, not a C++ integer limit.

---

# 4. The largest robustness gap: unbounded nesting and size

## 4.1 Deep nesting can overflow the stack

Every nested array/object recursively calls `parse_value()`.

For example:

```text
[[[[[[[[[[ ... ]]]]]]]]]]
```

There is no depth check.

This does not require a giant file. A relatively compact nested document can exhaust the process stack.

It is especially worth addressing because even irrelevant or unknown fields are fully parsed before schema validation.

## 4.2 Add limits at the parser boundary

I recommend an explicit limits object:

```cpp
struct JsonLimits {
    std::size_t max_input_bytes;
    std::size_t max_depth;
    std::size_t max_nodes;
    std::size_t max_string_bytes;
    std::size_t max_array_items;
    std::size_t max_object_fields;
};
```

For these content files, reasonable initial policies might be:

- Depth: 32 or 64.
- Input: a few MiB.
- Object fields: a few dozen.
- Identifier strings: a few hundred bytes.
- Reaction count: a deliberate project limit.

Those are starting points, not universal recommendations. Material definitions naturally have a maximum of 256 distinct IDs under the current schema.

Apply limits **before** allocating or descending:

```text
Before parsing another nested container:
    check depth

Before appending another node:
    check node count

Before appending a string byte:
    check string size

Before appending a field/item:
    check container count
```

A post-parse size check is too late to protect parsing.

## 4.3 Why both byte and node limits?

A byte limit alone is useful, but `JsonValue` has several strings/vectors regardless of its active type.

A tiny token such as:

```json
0
```

creates a much larger in-memory object.

Large arrays of small tokens can therefore have significant memory amplification.

A node limit makes that predictable.

## 4.4 Duplicate-key detection is quadratic

With `n` fields, this loop does approximately O(n²) comparisons:

```cpp
for (const auto& field : out.fields)
```

Long, similarly prefixed keys make comparisons more expensive.

For your actual schemas, **a small object-field cap is probably sufficient**. You do not need to rush to replace every vector with a hash set.

If you choose to support large generic objects, use a better duplicate-detection structure. But this parser does not need to become a general-purpose document engine.

## 4.5 Allocation failure needs an explicit application policy

The parser returns `bool`, but `std::string` and `std::vector` allocation may throw.

That is normal C++, but it means:

> “Malformed data returns false”

is not the same as:

> “All resource failures return false.”

Use resource limits to prevent predictable oversized requests. Decide at the host boundary how allocation failure is surfaced.

Avoid a blanket `catch (...)` that converts actual programming defects into “bad JSON.”

---

# 5. Confirmed schema defect: reactant alternatives are checked incorrectly

This code is the issue:

```cpp
const bool has_material = material != nullptr &&
                          material->type == JsonValue::Type::String &&
                          !material->str.empty();

const bool has_tag = tag != nullptr &&
                     tag->type == JsonValue::Type::String &&
                     !tag->str.empty();

if (has_material == has_tag) {
    ...
}
```

You are checking:

> Exactly one field has a valid value.

The schema says:

> Exactly one field is present, and that field has a valid value.

Those are not equivalent.

## Currently accepted malformed inputs

```json
{"material": "water", "tag": 42}
```

```json
{"material": "water", "tag": ""}
```

```json
{"material": false, "tag": "burnable"}
```

Each has both keys, but only one valid value, so the code accepts it.

That is exactly the sort of ambiguity a strict loader should reject.

## Correct implementation shape

```cpp
const JsonValue* material = find_json_field(slot, "material");
const JsonValue* tag = find_json_field(slot, "tag");

if ((material != nullptr) == (tag != nullptr)) {
    // Both present or both absent.
    return fail_schema(...);
}

const JsonValue& selected = material ? *material : *tag;

if (selected.type != JsonValue::Type::String || selected.str.empty()) {
    return fail_schema(...);
}

ReactantSpec reactant;
reactant.is_tag = tag != nullptr;
reactant.name = selected.str;
```

Also reject unknown keys in the slot.

This should be an early, isolated patch with direct regression tests.

---

# 6. Unknown fields should not silently disappear

The parser reads all object fields, but the schema loaders fetch only recognized ones and ignore the rest.

For game content, this is risky.

## Example: a temperature typo removes a gate

This loads:

```json
"temperature": {
  "min_temperatue": 500
}
```

Neither recognized gate field is present, so the reaction becomes ungated.

A typo can turn:

> Ignite only when sufficiently hot.

into:

> Ignite regardless of temperature.

That is a more serious content failure than a syntax error.

## Other examples

```json
"hardnes": 5
```

silently leaves hardness at zero.

```json
"product_temperatur": 900
```

silently removes heat output.

A misspelled optional field is particularly dangerous because no required-field check catches it.

## Recommendation: strict allowed-key validation

For each schema object, define allowed keys:

```text
Material document:
    version, materials

Material:
    id, name, movement, solid, density, color, tags, hardness

Reaction document:
    version, reactions

Reaction:
    name, probability, reactants, products,
    temperature, product_temperature

Reactant:
    material, tag

Product:
    material

Temperature:
    min_temperature, max_temperature
```

Then reject anything else.

Example diagnostic:

```text
reactions[3].temperature.min_temperatue:
unknown field; expected min_temperature or max_temperature
```

For a versioned, repository-controlled content format, strict rejection is generally better than silent forward compatibility.

If metadata extensions are needed, make them explicit—for example a separate `metadata` object with documented handling.

## Empty temperature objects

Decide whether this is valid:

```json
"temperature": {}
```

I would reject it in strict content mode because it is usually an authoring mistake. Omitting `temperature` already expresses “no gate.”

At minimum, lint it.

---

# 7. Programmatic material validation is incomplete

Your comment says:

> Structural validation for programmatically built tables.

That means the validator must enforce semantic invariants independently of the JSON parser.

Currently it does not.

## 7.1 Density is not validated

The JSON path enforces:

```text
0 ≤ density ≤ 100000
```

But `validate_material_table()` does not.

A programmatically built material can use:

```cpp
spec.density = -100;
```

and pass validation, unless it is air.

Move that range rule into the authoritative validator as well.

The JSON parser can still check before narrowing, but it must not be the only enforcement point.

## 7.2 `Movement` membership is not validated

A programmatically supplied enum value outside the recognized set can pass when `solid == false`.

In your previously supplied `step()`:

- Anything not static enters `dynamic_cells`.
- Only powder/liquid/gas branches place it into `next`.

An unrecognized movement value can therefore cause a cell to disappear during stepping.

### Add an exhaustive validity check

```cpp
switch (spec.movement) {
case Movement::Static:
case Movement::Powder:
case Movement::Liquid:
case Movement::Gas:
    break;
default:
    error = ...;
    return false;
}
```

That is an important validation-to-runtime invariant.

## 7.3 Add an explicit material-count bound

Reject more than 256 entries immediately.

Duplicate-ID checks eventually reject an oversized table, but an early limit:

- States the schema clearly.
- Avoids unnecessary duplicate-name work.
- Applies to programmatically constructed tables too.

## 7.4 Redundant solid/gas check

This is unreachable after the preceding condition:

```cpp
if (spec.solid && spec.movement != Movement::Static)
    return false;

if (spec.movement == Movement::Gas && spec.solid)
    return false;
```

Remove the second branch or restructure diagnostics.

This is not a vulnerability, just misleading redundancy.

## 7.5 Keep policy separate from validity

A zero-density dynamic material may be odd, but it is not automatically invalid under your current contract.

Likewise, a static nonsolid material may be intentional.

I would not invent new restrictions just to make the validator “stricter.” Enforce the actual declared invariants, and use lint warnings for suspicious but permissible combinations.

---

# 8. Reaction validation needs structural checks plus semantic linking

The reaction validator is generally more consistent than the material validator: it checks programmatic ranges and arities too.

However, some things cannot be validated without the material table.

I recommend three stages:

```text
Parse JSON syntax
    ↓
Validate reaction structure
    ↓
Link and validate against the material table
```

## Structural validation

Already mostly present:

- Version.
- Nonempty names.
- Unique names.
- Probability.
- Reactant/product counts.
- Temperature ranges.
- Min ≤ max.

Add limits on:

- Reaction count.
- Name length.
- Referenced identifier length.

## Linked validation

Your earlier `load_reactions()` resolves concrete material names, but tag references are kept as strings.

At link time, diagnose:

### Unknown material names

Reject with rule and slot context.

### Tags matching no material

For shipped content, I would reject or at least report this prominently:

```text
reactions[7].reactants[1].tag:
"vapourr" matches no loaded material
```

The `vapor`/`vapour` spelling inconsistency in the comments is a small example of why this matters.

### Primary reactant `air`

Your reaction executor iterates occupied cells only.

Therefore a rule whose primary is explicit air cannot fire under the current implementation.

Either:

- Reject it as unsupported.
- Or define a separate mechanism for reactions driven by empty cells.

Do not silently imply support.

### Tag matching air in a neighbor slot

In your earlier executor, a tag neighbor requires an occupied cell:

```cpp
!empty && matches_reactant(...)
```

So a tag does not currently act as an alternative way to match empty air.

That means I **do not see a current air-creation bypass through tags**. But the semantic distinction should be documented and tested.

### Probability zero

A zero-probability rule is not necessarily inert in your executor: a full match can still claim priority and suppress lower rules.

Do not lint it as “safe to remove.” A useful warning would be:

```text
This rule never transforms cells but can suppress lower-priority rules.
```

## “No spontaneous creation” is narrower than the comment implies

The validator enforces one specific restriction on an explicit air neighbor product.

It does not establish general mass conservation:

- Material transformations can change density.
- Your runtime reaction outputs are not transactional.
- Rule overlap can affect consumption semantics.

Document the actual guarantee narrowly:

> An explicit empty neighbor slot may not be directly replaced with occupied material by this rule format.

That is accurate and testable.

---

# 9. String handling: define an encoding and identifier policy

## 9.1 Control characters are correctly rejected

This check is good:

```cpp
if (static_cast<unsigned char>(c) < 0x20)
```

It correctly avoids signed-`char` problems for high bytes.

## 9.2 Invalid UTF-8 is currently accepted

Bytes ≥ `0x20` are appended without validating encoding.

That means malformed UTF-8 byte sequences can appear in:

- Keys.
- Material names.
- Tags.
- Reaction names.
- Diagnostics.

For interoperable JSON, the encoding needs to be defined.

## Two reasonable choices

### Choice A: ASCII identifiers, UTF-8 descriptive text

For simulation identifiers, allow something like:

```text
[a-z][a-z0-9_]*
```

Examples:

```text
burnable
molten_iron
cold_condensation
```

Keep human-readable names/descriptions separate when needed.

This is my preference for deterministic content identifiers.

### Choice B: UTF-8 identifiers

Validate UTF-8, and explicitly define equality as byte equality unless you deliberately implement normalization.

Do not accidentally introduce locale-dependent case folding or normalization into material identity.

## Unsupported escapes are a policy, not a defect

Rejecting:

```json
"\n"
```

or:

```json
"\u0061"
```

is allowed by your documented subset.

But common JSON serializers may emit these escapes. That can surprise tooling authors.

Either:

- Document the subset prominently and provide a compatible writer.
- Or use full JSON string parsing while keeping schema values integer-only.

Full JSON syntax does not force floating-point values into your simulation.

---

# 10. Failure behavior and diagnostics need a consistent contract

## 10.1 Parsing currently destroys the previous output on failure

Both loaders begin with:

```cpp
out = MaterialTable{};
```

or:

```cpp
out = ReactionTable{};
```

A failed parse therefore clears the caller’s existing table.

That may be intentional, but it is not a good default for hot reload.

### Recommended contract

> On success, replace `out`. On failure, leave `out` unchanged and return an error.

You already build a local `table`, so simply stop clearing `out` at entry.

Commit with:

```cpp
out = std::move(table);
```

only after all validation succeeds.

This does not itself change the live simulation’s transaction policy, but it makes intermediate parsing safer.

## 10.2 Error strings remain stale after success

None of these functions consistently clears a caller-provided `error` on success.

So this sequence can return success with an old error still present:

```text
Parse invalid document → error populated
Parse valid document using same error string → success, old text remains
```

Choose and document a policy. I recommend clearing the error at the beginning of public parse/validate calls.

## 10.3 Header promises do not match implementation

The material header promises:

> Failure error is “line L col C: message”; first error wins.

That is true for parser syntax errors, but not schema errors.

For example:

```text
duplicate material id
```

has no location or material index.

There are also branches that overwrite helper errors:

```cpp
if (!require_json_string(...) || name.empty()) {
    error = "... must be a non-empty string";
}
```

That is not necessarily bad, but it is not a universal first-error-preservation contract.

## Recommended diagnostic model

At minimum:

```cpp
struct DataError {
    std::string path;
    std::string message;
    // Optional source location.
};
```

Examples:

```text
materials[4].density: expected integer in range 0..100000
```

```text
reactions[2].reactants[1]: specify exactly one of material or tag
```

For source locations, attach token start offsets to parsed values and object keys.

A byte offset plus a line-start index is enough; you do not need to maintain heavyweight source spans everywhere.

Use `std::size_t` for offsets/counters. With small input limits, the current `int` counters are safe, but wide counters are a cleaner contract.

## 10.4 Parser lifecycle is ambiguous

`JsonParser::run()` neither resets state nor declares itself one-shot.

For your current usage, it is constructed and run once, so this is not causing a demonstrated bug.

Still, choose one:

- Reset cursor/error/line state at the beginning of `run()`.
- Or expose a one-shot parsing function instead of a reusable-looking parser object.

Also document that the input `string_view` must remain valid during parsing.

---

# 11. Keep the parser or replace it?

**I would not insist on a replacement, but this is a place where a mature dependency can be a sensible trade.**

A parser does not need to be custom to keep:

- Godot out of the engine.
- Integer-only simulation values.
- Deterministic content.
- Strict schemas.
- Duplicate-key rejection.

If you replace it, verify those properties deliberately. Some DOM parsers silently retain the first or last duplicate key unless configured or intercepted.

Also retain resource limits: a mature parser does not automatically solve application-level denial of service.

## My recommendation for your current team

For the next patch, **harden the existing parser rather than simultaneously replacing it and changing schema semantics**.

The code is small enough to make the immediate fixes tractable:

1. Resource limits.
2. Numeric grammar.
3. Correct union-field validation.
4. Allowed-key validation.
5. Validator parity.
6. Transactional output.
7. Tests and fuzzing.

After that, evaluate replacement based on whether you need full JSON interoperability. Avoid slowly building a general-purpose parser by accident.

---

# 12. Concrete regression tests

## Parser syntax tests

| Input/property | Expected |
|---|---|
| Empty input | Reject |
| Valid value followed by garbage | Reject |
| Object or array trailing comma | Reject |
| Duplicate object key | Reject |
| Unterminated string | Reject |
| Trailing backslash in string | Reject |
| Unescaped newline/control byte | Reject |
| `truex` / `false0` | Reject whole document |
| `01`, `-01`, `00` | Reject |
| `0`, `-0` | Accept integer zero |
| `1000000000` | Accept under current numeric policy |
| `1000000001` | Reject |
| Very long digit sequence | Reject safely |
| Nested containers at depth limit | Accept if otherwise valid |
| One level beyond limit | Reject without stack failure |
| String/node/item limit boundary | Enforce precisely |
| Invalid UTF-8 | Reject if choosing UTF-8 JSON |
| Valid unsupported escapes | Reject with clear subset diagnostic |

## Material tests

- Valid JSON table parses and validates.
- Duplicate ID.
- Duplicate name.
- Missing air.
- Air with wrong properties.
- Invalid movement string.
- Invalid programmatic `Movement`.
- Negative/oversized programmatic density.
- Invalid hardness from JSON and C++.
- Duplicate or empty tags.
- Unknown material field.
- Oversized material array.
- Failure leaves prior output unchanged.
- Success clears prior error.

## Reaction tests

Specifically include all of these:

```json
{"material": "water", "tag": 42}
```

```json
{"material": false, "tag": "burnable"}
```

```json
{"material": "water", "tag": ""}
```

```json
{"material": "water", "tag": "liquid"}
```

```json
{}
```

All should fail as reactant slots.

Also test:

- Wrong optional-field types.
- Unknown keys at every nesting level.
- Misspelled temperature gates.
- Empty temperature object under the chosen policy.
- Min greater than max.
- Boundary temperatures.
- Too many products.
- Duplicate reaction names.
- Unresolved material names.
- Unknown tags under the chosen link policy.
- Unsupported primary-air reactions.

---

# 13. Fuzzing plan

I would create **three small fuzz targets**, not one giant target that parses content and immediately runs arbitrary simulation workloads.

## Target A: raw subset parser

Feed arbitrary bytes through:

```cpp
JsonParser parser(input);
JsonValue root;
parser.run(root);
```

Use production resource limits.

Objectives:

- No crash.
- No out-of-bounds access.
- No undefined behavior.
- No stack exhaustion within admitted limits.
- No excessive resource use.
- Successful parse consumes the whole input.

## Target B: material loader

Feed arbitrary bytes into `parse_material_data()`.

On success, assert:

- `validate_material_table()` succeeds.
- IDs are unique.
- Air invariants hold.
- Every movement is recognized.
- Numeric constraints hold.

On failure:

- Output sentinel is unchanged under the new transactional contract.
- Error is nonempty.

## Target C: reaction loader and bounded linker

Feed arbitrary bytes into `parse_reaction_data()`.

On success:

- Structural validation succeeds.
- Link against a fixed small material table.
- Linking either succeeds or reports a structured error without changing existing compiled reactions.

Do not let this target execute long simulation trajectories; that mixes parser resource testing with simulation performance and movement behavior.

## Tooling

With an appropriate Clang toolchain:

```text
-fsanitize=fuzzer,address,undefined
-fno-omit-frame-pointer
-g
```

Maintain a separate optimized fuzz build if needed for throughput.

Do not treat all nonzero exits as “expected invalid content.” A clean rejection is a normal return, not a crash.

## Seed corpus

Include:

- Current canonical materials and reactions.
- Minimal valid documents.
- Every regression case above.
- Truncated canonical files.
- Boundary integer values.
- Nested arrays/objects.
- Duplicate keys.
- Near-valid typo documents.

A dictionary containing schema keys and enum strings helps mutation reach meaningful branches.

## Differential testing

A trusted full JSON parser can serve as a syntax reference, with an important restriction:

> Only compare acceptance directly within your intentionally supported subset.

Your parser is allowed to reject valid full JSON containing `null`, floats, or unsupported escapes.

But anything your parser accepts should still be valid JSON under the chosen encoding policy. That property would catch leading-zero acceptance.

## Programmatic-validator fuzzing

Byte fuzzing the JSON parser cannot produce an invalid enum through `movement_from_string()`.

Add a separate bounded structured test that generates `MaterialTable` and `ReactionTable` objects directly.

That is how you find divergence between the JSON route and the C++ route.

---

# 14. Proposed patch sequence

| Patch | Scope | Exit criterion |
|---|---|---|
| **1** | Depth/input/node/string/container limits | Deep/large malformed inputs reject safely |
| **2** | Leading-zero rejection and explicit integer policy | Numeric boundary corpus passes |
| **3** | Presence-based reactant exclusivity | All malformed dual-key cases reject |
| **4** | Unknown-key validation | Optional-field typos cannot silently remove behavior |
| **5** | Material validator parity | Invalid C++ enums/densities reject |
| **6** | Transactional parse output and clear error contract | Failed reload preserves prior table |
| **7** | Path/location diagnostics | Errors identify the actual row and field |
| **8** | Bounded semantic linking/lint | Dead references and unsupported reactions are visible |
| **9** | Fuzz targets and persistent regression corpus | Sanitizer-clean corpus and repeatable fuzz runs |

# Bottom line

**This is not a disastrous parser. Its basic scanning and numeric narrowing are more careful than the initial smell-list suggested.**

The problems are mainly at the boundaries between:

- Syntax and schema.
- Field presence and field validity.
- JSON construction and programmatic construction.
- Invalid input and bounded resource consumption.
- Parsing failure and caller state.

The highest-value fixes are **resource limits, strict unknown-field handling, the reactant exclusivity correction, and validator parity**.

Once those land, you will have a substantially more trustworthy content pipeline—and a much better foundation for adding materials and reactions without silently admitting broken definitions.

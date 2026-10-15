#!/usr/bin/env python3
"""Offline differential sampler: ashfall JsonParser vs Python json.loads.

OFFLINE-ONLY BY DESIGN — NOT CI, NOT A GATE. The Group-6 owner decision is
that the differential is an on-demand drift-hunting tool: our parser accepts
a strict subset of JSON, so "Python accepts / we reject" is EXPECTED for
subset gaps (see ALLOWLIST below). CI asserts no-crash + corpus-clean only.
Do NOT wire this script up as a blocking check later.

Usage:
    python3 tests/fuzz/differential_json.py [--seed N] [--verdict-bin PATH]

The script shells out to the tiny stdin->verdict shim
tests/fuzz/json_verdict.cpp (F6-owned; the integrator wires a `json_verdict`
executable target — until then build it manually, e.g.):
    clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \\
        tests/fuzz/json_verdict.cpp engine/src/json_subset.cpp \\
        -I engine/include -o /tmp/ashfall-json-verdict
    python3 tests/fuzz/differential_json.py --verdict-bin /tmp/ashfall-json-verdict

Inputs: committed corpus files under tests/fuzz/corpus/*/ (if present — they
land via F1/F3 working in parallel, so absence is normal) + built-in base
cases + deterministic-seed truncated/mutated derivatives (--seed, default
fixed below).

Verdict semantics (subset differential, see plan Task F6):
  * our ACCEPT  => Python must accept   (soundness; we claim a strict subset)
  * Python accept + subset-shaped => we must accept (completeness)
    MODULO the KNOWN divergence classes in ALLOWLIST (triage, not failures).

UNKNOWN divergences are printed with repro inputs for human triage.
Exit code is always 0: findings are for triage, never gate failures.
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys

DEFAULT_SEED = 20260910

# ---------------------------------------------------------------------------
# KNOWN divergence allowlist (read from engine/src/json_subset.cpp + the
# header contract — NOT guessed). Each entry: Python accepts, we reject
# BY DESIGN, plus the one soundness-side mirror case.
#
# Completeness side (Python ACCEPTs, we REJECT by design):
#  1. null-literal:    `null` -> None. Subset supports only true/false.
#  2. float-exp:       `1.5`, `1e3` -> float. Subset is integers-only; the
#                      parser names `.`/`e`/`E` explicitly ("continuation").
#  3. nan-infinity:    `NaN`, `Infinity`, `-Infinity` -> float. CPython
#                      json.loads accepts these by default (allow_nan=True);
#                      we reject (`-` then `I`... fails "expected integer";
#                      bare `N`/`I` fail "unexpected character").
#  4. escape-subset:   `\n \t \r \b \f \/ \uXXXX` and any `\X`. CPython
#                      accepts all JSON escapes; we emit only `"` and `\`
#                      ("unsupported escape (only \" and \\ allowed)").
#  5. duplicate-key:   `{"a":1,"a":2}` -> last wins in Python; we reject
#                      ("duplicate key ...").
#  6. int-clamp:       |n| > 1000000000. Python bigints are unbounded; we
#                      clamp as a format policy ("limit: integer magnitude").
#  7. depth-limit:     nesting > 64. Python recurses far deeper; we cap via
#                      JsonLimits::max_depth ("limit: nesting depth").
#  8. size-limits:     input > 1MiB / nodes > 2^17 / string bytes > 2^18 /
#                      array items > 2^16 / object fields > 2^13. Python has
#                      no such caps; we reject ("limit: ...").
#  9. non-utf8-encoding: raw bytes Python decodes as UTF-16/32 (BOM or null
#                      pattern sniffing in json.loads(bytes)) but which are
#                      not UTF-8 text. We are byte-oriented with no codec
#                      detection, so we reject the raw bytes by design.
#
# Soundness side (we ACCEPT, Python REJECTs — must stay empty in UTF-8):
# 10. non-utf8-bytes: we accept raw bytes >= 0x20 with NO UTF-8 validation
#                      (header contract); Python rejects undecodable input.
#                      Expected when the input is not valid UTF-8; a UTF-8
#                      input in this bucket would be a REAL soundness bug.
#
# Deliberately NOT divergences (verified by reading the parser — do NOT add):
#  * trailing commas (`[1,2,]`), leading zeros (`01`), `+1` sign, single
#    quotes, comments, `truex`, trailing garbage, empty input: BOTH reject.
#  * unknown keys / non-identifier keys (`{"zzz":1}`, `{"A":1}`): the PARSER
#    accepts any string key — identifier policy ([a-z][a-z0-9_-]*) and
#    unknown-key strictness live in the schema loaders (material/reaction
#    data), NOT in JsonParser, so there is no parser-level divergence.
# ---------------------------------------------------------------------------

ALLOWLIST = (
    "null-literal",
    "float-exp",
    "nan-infinity",
    "escape-subset",
    "duplicate-key",
    "int-clamp",
    "depth-limit",
    "size-limits",
    "non-utf8-encoding",
    "non-utf8-bytes",
)

# Our JsonLimits (must match kDefaultJsonLimits in json_subset.hpp).
LIM_INPUT_BYTES = 1 << 20
LIM_NODES = 1 << 17
LIM_STRING_BYTES = 1 << 18
LIM_ARRAY_ITEMS = 1 << 16
LIM_OBJECT_FIELDS = 1 << 13
LIM_DEPTH = 64
LIM_INT = 1000000000

# Any backslash escape other than \" and \\ in the raw text.
_NONTRIVIAL_ESCAPE = re.compile(r'\\[^"\\]')

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_SCRIPT_DIR))


def base_cases():
    """Hand-built bases: valid subset, known gaps, and mutual rejects."""
    valid = [
        b'{}', b'[]', b'{"a": 1}', b'{"a":1,"b":[true,false]}',
        b'  { "x" : [0, -5, 1000000000, -1000000000] }  \n',
        b'""', b'"hello"', b'"a\\"b\\\\c"', b'0', b'-0', b'42', b'-7',
        b'true', b'false', b'[[[[1]]]]',
        b'{"nested": {"deep": [{"k": true}]}}',
        b'{"": 1}',  # empty key: parser accepts (no identifier check here)
        b'{"A": 1}',  # non-identifier key: parser still accepts (see header)
        b'{"unknown_key": 1, "zzz": [1,2,3]}',
        b'"caf\xc3\xa9"',  # valid UTF-8 multibyte raw bytes: both accept
    ]
    known_gaps = [  # Python accepts, we reject by design (allowlisted).
        b'null', b'{"a": null}', b'[null]',
        b'1.5', b'-0.5', b'1e3', b'2E-4', b'{"f": 3.14}',
        b'NaN', b'Infinity', b'-Infinity', b'{"v": Infinity}',
        b'"\\n"', b'"\\t"', b'"\\u0041"', b'"a\\/b"', b'{"e": "\\r\\n"}',
        b'{"a": 1, "a": 2}', b'[{"d": 1, "d": 1}]',
        b'1000000001', b'-1000000001', b'{"big": 99999999999999999999}',
        b'[' * 70 + b'1' + b']' * 70,  # depth 70 > 64
        b'{"a": 01}',  # NOTE: mutual reject (Python also rejects) — keep as
        b'[1, 2,]',  # control: both reject, guards the triage logic itself.
        b'{"a": 1,}', b'+1', b"'single'", b'{a: 1}',
    ]
    mutual_rejects = [
        b'', b' ', b'{', b'[', b'{"a": ', b'"unterminated', b'truex',
        b'nul', b'{"a":1} garbage', b'[1 2]', b'{"a" 1}', b'{"a":1;;}',
        b'{"1": 2} garbage-tail', b'\x00', b'{"a": "\x01"}',
        b'"bad \\q escape"', b'--5', b'0x10',
    ]
    return valid, known_gaps, mutual_rejects


def collect_corpus(corpus_root):
    found = []
    if not os.path.isdir(corpus_root):
        return found
    for dirpath, _dirnames, filenames in os.walk(corpus_root):
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            if os.path.isfile(path) and not name.startswith("."):
                with open(path, "rb") as fh:
                    found.append((path, fh.read()))
    return found


def derivatives(bases, seed, per_base):
    """Deterministic truncated/mutated derivatives of base inputs."""
    rng = random.Random(seed)
    pool = b'{}[]",: \t\n-0123456789.truefalsnNulleE+.\\'
    out = []
    for _label, data in bases:
        if not data:
            continue
        for _ in range(per_base):
            choice = rng.randrange(3)
            if choice == 0:  # truncation at a deterministic cut point
                cut = rng.randrange(len(data) + 1)
                out.append((f"trunc@{cut}", data[:cut]))
            elif choice == 1:  # single-byte substitution
                pos = rng.randrange(len(data))
                rep = pool[rng.randrange(len(pool)):rng.randrange(len(pool)) + 1]
                if not rep:
                    rep = b"X"
                out.append((f"subst@{pos}", data[:pos] + rep + data[pos + 1:]))
            else:  # suffix append (often turns ACCEPT into trailing-garbage)
                suffix = bytes(rng.choice(pool) for _ in range(rng.randrange(1, 4)))
                out.append(("suffix", data + suffix))
    return out


def run_verdict(verdict_bin, data):
    proc = subprocess.run([verdict_bin], input=data, stdout=subprocess.PIPE,
                          timeout=10)
    line = proc.stdout.decode("utf-8", errors="replace").strip()
    if line == "ACCEPT":
        return True, "", ""
    parts = line.split(" ", 2)
    assert parts[0] == "REJECT", f"bad verdict line: {line!r}"
    klass = parts[1] if len(parts) > 1 else "?"
    detail = parts[2] if len(parts) > 2 else ""
    return False, klass, detail


def walk(value, seen):
    """Collect Python-side features of an accepted value."""
    seen["nodes"] += 1
    if value is None:
        seen["null"] = True
    elif isinstance(value, bool):
        pass
    elif isinstance(value, float):
        seen["float"] = True
    elif isinstance(value, int):
        if abs(value) > LIM_INT:
            seen["bigint"] = True
    elif isinstance(value, str):
        seen["strbytes"] += len(value.encode("utf-8", errors="ignore"))
    elif isinstance(value, list):
        seen["depth"] = max(seen["depth"], 1)
        if len(value) > LIM_ARRAY_ITEMS:
            seen["oversize"] = True
        for item in value:
            child = dict(seen)
            walk(item, child)
            child["depth"] += 1
            for key in ("null", "float", "bigint", "oversize", "dupkey"):
                seen[key] = seen[key] or child[key]
            seen["nodes"] += child["nodes"]
            seen["strbytes"] += child["strbytes"]
            seen["depth"] = max(seen["depth"], child["depth"])
    elif isinstance(value, dict):
        if len(value) > LIM_OBJECT_FIELDS:
            seen["oversize"] = True
        for item in value.values():
            child = dict(seen)
            walk(item, child)
            child["depth"] += 1
            for key in ("null", "float", "bigint", "oversize", "dupkey"):
                seen[key] = seen[key] or child[key]
            seen["nodes"] += child["nodes"]
            seen["strbytes"] += child["strbytes"]
            seen["depth"] = max(seen["depth"], child["depth"])


def fresh_seen():
    return {"null": False, "float": False, "bigint": False, "oversize": False,
            "dupkey": False, "nodes": 0, "strbytes": 0, "depth": 0}


def has_dup_keys(text):
    """Detect duplicate object keys via a pairs hook (raises nothing)."""
    found = [False]

    def hook(pairs):
        keys = [k for k, _ in pairs]
        if len(set(keys)) != len(keys):
            found[0] = True
        return dict(pairs)

    json.loads(text, object_pairs_hook=hook)
    return found[0]


def triage_our_reject_python_accept(raw, our_class):
    """Our REJECT + Python ACCEPT: map to an allowlist class or None."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        return "non-utf8-encoding"
    value = json.loads(text)
    seen = fresh_seen()
    walk(value, seen)
    if seen["null"]:
        return "null-literal"
    if seen["float"]:
        # NaN/Infinity arrive as float too; split for a precise label.
        if "NaN" in text or "Infinity" in text:
            return "nan-infinity"
        return "float-exp"
    if seen["bigint"]:
        return "int-clamp"
    if has_dup_keys(text):
        return "duplicate-key"
    if _NONTRIVIAL_ESCAPE.search(text):
        return "escape-subset"
    if seen["depth"] > LIM_DEPTH:
        return "depth-limit"
    if (seen["nodes"] > LIM_NODES or seen["strbytes"] > LIM_STRING_BYTES
            or seen["oversize"] or len(raw) > LIM_INPUT_BYTES):
        return "size-limits"
    return None  # UNEXPECTED: subset-shaped but we rejected.


def find_verdict_bin(explicit):
    candidates = []
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("ASHFALL_JSON_VERDICT")
    if env:
        candidates.append(env)
    candidates += [
        os.path.join(_REPO_ROOT, "build-fuzz", "json_verdict"),
        os.path.join(_REPO_ROOT, "build", "json_verdict"),
        "/tmp/ashfall-json-verdict",
    ]
    for path in candidates:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise SystemExit(
        "json_verdict binary not found. Build it manually (the integrator "
        "wires the CMake target later):\n"
        "  clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \\\n"
        "      tests/fuzz/json_verdict.cpp engine/src/json_subset.cpp \\\n"
        "      -I engine/include -o /tmp/ashfall-json-verdict\n"
        "then pass --verdict-bin /tmp/ashfall-json-verdict.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED,
                    help="deterministic RNG seed for derivatives")
    ap.add_argument("--verdict-bin", default=None,
                    help="path to the json_verdict shim binary")
    ap.add_argument("--corpus-root", default=os.path.join(_REPO_ROOT, "tests",
                                                          "fuzz", "corpus"),
                    help="committed seed corpus root (may be absent)")
    ap.add_argument("--derivatives-per-base", type=int, default=3)
    args = ap.parse_args()

    verdict_bin = find_verdict_bin(args.verdict_bin)

    valid, known_gaps, mutual_rejects = base_cases()
    bases = ([("valid", d) for d in valid] + [("gap", d) for d in known_gaps]
             + [("reject", d) for d in mutual_rejects])
    corpus = collect_corpus(args.corpus_root)
    cases = ([(f"base:{label}:{data!r}"[:80], data) for label, data in bases]
             + [(f"corpus:{p}", d) for p, d in corpus]
             + [(f"deriv:{label}", d)
                 for label, d in derivatives(bases, args.seed,
                                             args.derivatives_per_base)])

    total = agree = 0
    allowlisted = {name: 0 for name in ALLOWLIST}
    unexpected = []
    for label, data in cases:
        if len(data) > 64 * 1024:  # keep the offline run fast; limits are
            continue  # covered by dedicated small depth/size probes instead.
        total += 1
        ours, our_class, our_detail = run_verdict(verdict_bin, data)
        try:
            json.loads(data)
            py_ok = True
        except Exception:  # noqa: BLE001 — any Python rejection is a verdict.
            py_ok = False
        if ours and py_ok:
            agree += 1
        elif not ours and not py_ok:
            agree += 1  # mutual reject: agreement.
        elif not ours and py_ok:
            bucket = triage_our_reject_python_accept(data, our_class)
            if bucket is not None:
                allowlisted[bucket] += 1
                agree += 1
            else:
                unexpected.append((label, data, f"we REJECT({our_class}): "
                                   f"{our_detail} | Python ACCEPTS"))
        else:  # ours and not py_ok: soundness side.
            try:
                data.decode("utf-8")
                utf8 = True
            except UnicodeDecodeError:
                utf8 = False
            if not utf8:
                allowlisted["non-utf8-bytes"] += 1
                agree += 1
            else:
                unexpected.append((label, data, "we ACCEPT | Python REJECTS "
                                   "(soundness — investigate!)"))

    rate = 100.0 * agree / total if total else 0.0
    print(f"json differential: {agree}/{total} agree ({rate:.1f}%) "
          f"[seed={args.seed} corpus_files={len(corpus)}]")
    print("allowlisted (KNOWN subset divergences — triage, not failures):")
    for name in ALLOWLIST:
        print(f"  {name}: {allowlisted[name]}")
    if unexpected:
        print(f"UNEXPECTED divergences: {len(unexpected)}")
        for label, data, why in unexpected:
            print(f"  - {label} input={data!r:.200} :: {why}")
    else:
        print("unexpected divergences: none")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# Fuzzing (libFuzzer + ASan/UBSan)

## Toolchain: LLVM Clang required

libFuzzer (`-fsanitize=fuzzer`) ships only with LLVM Clang. Apple Clang
(`/usr/bin/clang++`, Xcode) has NO libFuzzer runtime: linking
`-fsanitize=fuzzer` fails with

```text
ld: library '.../lib/clang/21/lib/darwin/libclang_rt.fuzzer_osx.a' not found
clang++: error: linker command failed with exit code 1
```

(recorded with Apple clang 21.0.0 on arm64 macOS). Install LLVM (keg-only,
do NOT symlink system-wide):

```sh
brew install llvm
```

Verified working: Homebrew clang 23.1.1 at
`/opt/homebrew/opt/llvm/bin/clang++` (Target: arm64-apple-darwin25.6.0).
A minimal `LLVMFuzzerTestOneInput` harness compiles and links cleanly
(exit 0) under `-fsanitize=fuzzer` with this compiler.

For the fuzz build only, put LLVM first on PATH (never system-wide, never
in the default build):

```sh
export PATH=/opt/homebrew/opt/llvm/bin:$PATH
```

## Separate build dir: build-fuzz (FUZZ=ON poisons the dir)

`ASHFALL_BUILD_FUZZ=ON` appends `-fsanitize=fuzzer,address,undefined
-fno-omit-frame-pointer -g` to the `ashfall_sim` library itself (engine and
fuzz targets must match sanitizers at link time), so a FUZZ=ON directory
can never be reused as a normal build. Always use a SEPARATE dir:

```sh
# From the repo root:
PATH=/opt/homebrew/opt/llvm/bin:$PATH \
  cmake -S . -B build-fuzz \
    -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
    -DASHFALL_BUILD_FUZZ=ON
cmake --build build-fuzz
```

The default `build/` dir stays FUZZ=OFF (the default): normal Apple Clang
or LLVM builds, no `-fsanitize=fuzzer` anywhere. Configuring with a
non-libFuzzer compiler plus `-DASHFALL_BUILD_FUZZ=ON` fails fast: the
`try_compile` probe for `-fsanitize=fuzzer` aborts configure with a
FATAL_ERROR pointing back at `brew install llvm`.

Fuzz executables are NOT in ctest. Missing `tests/fuzz/fuzz_*.cpp` sources
do not break configure (each target is guarded by an `EXISTS` check with a
`STATUS` skip message); the seven declared targets are `fuzz_json_parse`,
`fuzz_material_load`, `fuzz_reaction_load`, `fuzz_material_struct`,
`fuzz_reaction_struct`, `fuzz_save_crash`, `fuzz_save_roundtrip`
(sources land in Round 2: F1/F2/F3 own them).

## Run commands

Common flags for every target: `-timeout=5 -rss_limit_mb=512`
(per the Group 6 contract). `-max_len` per target class:

| Target(s) | `-max_len` |
|---|---|
| `fuzz_json_parse`, `fuzz_material_load`, `fuzz_reaction_load` (JSON bytes) | 4096 |
| `fuzz_save_crash`, `fuzz_save_roundtrip` (save bytes) | 8192 |
| `fuzz_material_struct`, `fuzz_reaction_struct` (struct fill) | 512 |

```sh
# Corpus run (once targets + corpus exist in Round 2):
./build-fuzz/fuzz_json_parse tests/fuzz/corpus/json_parse/ \
  -timeout=5 -rss_limit_mb=512 -max_len=4096
./build-fuzz/fuzz_save_crash tests/fuzz/corpus/save/ \
  -timeout=5 -rss_limit_mb=512 -max_len=8192
./build-fuzz/fuzz_material_struct tests/fuzz/corpus/material_struct/ \
  -timeout=5 -rss_limit_mb=512 -max_len=512

# Short fresh-fuzzing run seeded by the corpus (CI budget: 120 s per target):
./build-fuzz/fuzz_json_parse tests/fuzz/corpus/json_parse/ \
  -max_total_time=120 -timeout=5 -rss_limit_mb=512 -max_len=4096
```

## Overnight recipe (local only, not CI)

```sh
mkdir -p /tmp/ashfall-fuzz-work
./build-fuzz/fuzz_json_parse /tmp/ashfall-fuzz-work/json \
  tests/fuzz/corpus/json_parse/ \
  -max_total_time=28800 -workers=N -jobs=N \
  -timeout=5 -rss_limit_mb=512 -max_len=4096
```

Repeat per target with its corpus dir and `-max_len` from the table above.
Pick `N` to match local cores. Triage any artifact in the work dir as an
F4 finding (record, do not silently discard).

## Corpus policy

- Small text files only (JSON seeds, hand-built save vectors, byte seeds).
- No binaries, no archives, no PII (trivially true: all seeds are
  hand-written engine inputs).
- Name seeds `NNN-short-desc.<ext>` (e.g. `001-empty-object.json`).
- Keep each seed minimal and focused (one edge case per file where
  practical); corpus files double as regression tests (every file replays
  through a fresh ASan/UBSan binary with zero tolerance for findings).

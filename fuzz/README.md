# Praia fuzz targets

Coverage-guided fuzzing of Praia's parsers via [libFuzzer]. Catches the bug
class no human reviewer reaches: weird byte sequences that nobody would
think to write, but that still tickle a code path into crashing.

## Targets

| Binary | Surface | Source |
|---|---|---|
| `fuzz_json` | `json.parse` | `src/builtins/json.cpp` |
| `fuzz_yaml` | `yaml.parse` | `src/builtins/yaml.cpp` |
| `fuzz_lex_parse` | Lexer + Parser on Praia source | `src/lexer.cpp` + `src/parser.cpp` |

## Build

```sh
make fuzz
```

Requirements:
- **macOS**: Homebrew LLVM (`brew install llvm`) — Apple's bundled Clang
  doesn't ship the libFuzzer runtime. The Makefile auto-detects
  `$(brew --prefix llvm)/bin/clang++`.
- **Linux**: any recent Clang with `-fsanitize=fuzzer` support (most
  distros' default works). Override with `make fuzz CXX_FUZZ=clang++-17`
  if needed.

Outputs land in `build_fuzz/`.

## Run

libFuzzer takes two kinds of directories: a working corpus (it reads
from it AND writes new "interesting" inputs to it as fuzzing
progresses) and additional read-only seed dirs. We split them:

- `fuzz/seeds/<target>/` — small hand-crafted seeds, **committed**.
- `fuzz/corpus/<target>/` — fuzzer working dir, **gitignored**.

Pass the writable corpus first, then the read-only seeds:

```sh
mkdir -p fuzz/corpus/json fuzz/crashes/json

# One hour, single thread, mutate from seeds + accumulated corpus.
./build_fuzz/fuzz_json fuzz/corpus/json/ fuzz/seeds/json/ \
    -max_total_time=3600 \
    -artifact_prefix=fuzz/crashes/json/

# Parallel — 8 worker processes share corpus
./build_fuzz/fuzz_json fuzz/corpus/json/ fuzz/seeds/json/ \
    -jobs=8 -workers=8

# Quick smoke (60 s, no corpus, no I/O)
./build_fuzz/fuzz_yaml -max_total_time=60
```

Useful flags:
- `-max_total_time=N` — stop after N seconds.
- `-runs=N` — stop after N executions.
- `-jobs=N -workers=N` — run N parallel workers (separate processes).
- `-artifact_prefix=PATH/` — where to write crash inputs (default: cwd).
- `-print_final_stats=1` — execs/sec, coverage, memory at exit.

## Triage a crash

When a crash hits, libFuzzer writes the offending bytes as a file like
`crash-<sha1>` and prints the ASan/UBSan diagnostic + stack trace to
stderr. To reproduce:

```sh
./build_fuzz/fuzz_json fuzz/crashes/json/crash-abc123
```

Runs the binary against just that one input. Deterministic — same
crash, same trace. Fix the bug in the parser, **save the crashing
input as a regression test under `tests/`**, then rerun the fuzzer.

The existing `tests/test_json.praia`, `tests/test_yaml.praia`, etc.
already follow this convention — add new cases the same way.

## Seeds

`fuzz/seeds/<target>/` contains seed inputs. They're tiny and
hand-picked to cover common shapes (valid + edge cases). The fuzzer
mutates from these — without seeds it'd waste hours generating bytes
that look like JSON.

Add new seeds when you discover inputs that exercise interesting code
paths. Keep them small (under a few KB); libFuzzer prefers many small
inputs to few large ones.

`fuzz/corpus/` and `fuzz/crashes/` are `.gitignore`'d. Don't commit
fuzzer artifacts; turn discovered crashes into regression tests in
`tests/` instead.

## GC interaction

Praia's heap (`gcNew<PraiaMap>` etc.) tracks every allocation in a
thread-local `entries_` vector. Without a root marker, the real
collector no-ops, so the vector would grow unboundedly across millions
of fuzz iterations. The fuzz binaries link against `fuzz/gc_heap_fuzz.cpp`
— a minimal stub that tracks for accounting and prunes expired
`weak_ptr`s in `collect()`, but skips the mark phase entirely (it's
unneeded since each fuzz iteration's allocations are freed by refcount
when the resulting `Value` goes out of scope). Fuzz targets call
`collect()` every ~4096 inputs to keep the working set bounded.

## OSS-Fuzz

The natural next step once these targets are stable is to onboard
Praia into [OSS-Fuzz] — Google's free continuous-fuzzing service. That
runs each target 24/7 on Google's infrastructure and mails crash
reports. Onboarding is a `projects/praia/` directory in the oss-fuzz
repo: a Dockerfile, a build script, and a project.yaml.

[libFuzzer]: https://llvm.org/docs/LibFuzzer.html
[OSS-Fuzz]: https://github.com/google/oss-fuzz

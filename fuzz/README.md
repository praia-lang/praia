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
| `fuzz_bytes_unpack` | `bytes.unpack` format-parse + data-walk | `src/builtins/bytes.cpp` |

### Input layout

For `fuzz_bytes_unpack`, the input is a single byte stream split into a
format string and a data buffer:

- byte 0: format length F (0..255)
- bytes 1..F+1: format string
- bytes F+1..end: data buffer

This lets the mutator vary both sides independently. Seeds in
`fuzz/seeds/bytes_unpack/` are pre-encoded in this layout — each seed
covers one shape (single u16, big-endian u32, packed float/double,
mixed counts, pad bytes, etc.).

The other three targets take the input bytes directly as the parser
input.

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
# Convenience: uses the safe layout automatically.
make fuzz-run TARGET=json SECONDS=60

# Or invoke libFuzzer directly:
mkdir -p fuzz/corpus/json fuzz/crashes/json
./build_fuzz/fuzz_json fuzz/corpus/json/ fuzz/seeds/json/ \
    -max_total_time=3600 \
    -artifact_prefix=fuzz/crashes/json/

# Parallel — 8 worker processes share corpus
./build_fuzz/fuzz_json fuzz/corpus/json/ fuzz/seeds/json/ \
    -jobs=8 -workers=8

# Quick smoke (no dirs at all — libFuzzer won't touch the filesystem)
./build_fuzz/fuzz_yaml -max_total_time=60
```

> **Footgun warning.** If you pass a *single* directory to libFuzzer, it
> treats that directory as the writable corpus and *will* write
> hash-named files into it as it discovers new coverage. Never pass
> `fuzz/seeds/<target>/` as the only argument — it'll pollute the
> committed seeds. Either use `make fuzz-run` (always safe) or pass two
> dirs (corpus first, seeds second) — libFuzzer writes only to the
> first.

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

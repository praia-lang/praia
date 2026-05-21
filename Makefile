CXX = g++

# Optimization/debug profile. Default `-g` keeps local builds fast
# (no optimizer cost on incremental rebuilds) with full debug info.
# Release builds override this to `-O2 -g -DNDEBUG`:
#   - `-O2` is the standard sweet spot (≈3-5x faster than -O0 on
#     interpreter loops); `-O3`'s aggressive vectorization buys little
#     here and inflates binary size.
#   - `-g` is kept so users with crash reports can produce
#     symbolicated stack traces (debug info lives in a separate
#     section and has no runtime cost).
#   - `-DNDEBUG` is a future-proofing convention; the tree currently
#     has no `assert()` calls.
OPT ?= -g

CXXFLAGS = -std=c++20 -Wall -Wextra -Wno-deprecated-declarations $(OPT) -MMD -MP -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE
SRC_DIR = src
BUILD_DIR = build

SOURCES = $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/vm/*.cpp) $(wildcard $(SRC_DIR)/builtins/*.cpp)
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS = $(OBJECTS:.o=.d)
TARGET = praia

all: $(TARGET)

# Auto-detect readline/libedit support
HAVE_READLINE := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lreadline -o /dev/null 2>/dev/null && echo 1)
HAVE_EDIT     := $(shell echo 'int main(){}' | $(CXX) -x c++ - -ledit -o /dev/null 2>/dev/null && echo 1)

ifeq ($(HAVE_READLINE),1)
  CXXFLAGS += -DHAVE_READLINE
  LDLIBS = -lreadline
else ifeq ($(HAVE_EDIT),1)
  CXXFLAGS += -DHAVE_READLINE
  LDLIBS = -ledit
else
  LDLIBS =
endif

# Auto-detect SQLite
HAVE_SQLITE := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lsqlite3 -o /dev/null 2>/dev/null && echo 1)

ifeq ($(HAVE_SQLITE),1)
  CXXFLAGS += -DHAVE_SQLITE
  LDLIBS += -lsqlite3
endif

# Auto-detect OpenSSL (check standard path, then Homebrew)
HAVE_OPENSSL := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lssl -lcrypto -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_OPENSSL),1)
  CXXFLAGS += -DHAVE_OPENSSL
  LDLIBS += -lssl -lcrypto
else
  # Try Homebrew OpenSSL paths (macOS)
  OPENSSL_PREFIX := $(shell brew --prefix openssl 2>/dev/null)
  ifneq ($(OPENSSL_PREFIX),)
    HAVE_OPENSSL := 1
    CXXFLAGS += -DHAVE_OPENSSL -I$(OPENSSL_PREFIX)/include
    LDLIBS += -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
  endif
endif

# Auto-detect utf8proc (Unicode grapheme/case support)
HAVE_UTF8PROC := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lutf8proc -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_UTF8PROC),1)
  CXXFLAGS += -DHAVE_UTF8PROC
  LDLIBS += -lutf8proc
else
  UTF8PROC_PREFIX := $(shell brew --prefix utf8proc 2>/dev/null)
  ifneq ($(UTF8PROC_PREFIX),)
    HAVE_UTF8PROC := 1
    CXXFLAGS += -DHAVE_UTF8PROC -I$(UTF8PROC_PREFIX)/include
    LDLIBS += -L$(UTF8PROC_PREFIX)/lib -lutf8proc
  endif
endif

# Auto-detect RE2 (safe regex engine — O(n) guaranteed, no catastrophic backtracking)
HAVE_RE2 := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lre2 -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_RE2),1)
  CXXFLAGS += -DHAVE_RE2
  LDLIBS += -lre2
else
  RE2_PREFIX := $(shell brew --prefix re2 2>/dev/null)
  ifneq ($(RE2_PREFIX),)
    HAVE_RE2 := 1
    CXXFLAGS += -DHAVE_RE2 -I$(RE2_PREFIX)/include
    LDLIBS += -L$(RE2_PREFIX)/lib -lre2
    # RE2 depends on Abseil on some platforms
    ABSEIL_PREFIX := $(shell brew --prefix abseil 2>/dev/null)
    ifneq ($(ABSEIL_PREFIX),)
      CXXFLAGS += -I$(ABSEIL_PREFIX)/include
    endif
  endif
endif

# Auto-detect zlib (gzip / deflate compression). Universally available
# on macOS / Linux distros, so we default to assuming it's there; the
# detection is just a safety net for unusual sysroots.
HAVE_ZLIB := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lz -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_ZLIB),1)
  CXXFLAGS += -DHAVE_ZLIB
  LDLIBS += -lz
endif

# libresolv for DNS queries (net.query)
HAVE_RESOLV := $(shell echo 'int main(){}' | $(CXX) -x c++ - -lresolv -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_RESOLV),1)
  LDLIBS += -lresolv
endif

# Auto-detect -ldl for dlopen (Linux needs it, macOS has it in libSystem)
HAVE_DL := $(shell echo 'int main(){}' | $(CXX) -x c++ - -ldl -o /dev/null 2>/dev/null && echo 1)
ifeq ($(HAVE_DL),1)
  LDLIBS += -ldl
endif

# Export symbols so dlopen'd plugins can resolve types from the main binary
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -rdynamic
endif

# Allow callers (release pipeline, packagers) to append extra flags without
# clobbering the auto-detected CXXFLAGS above. Common use: bake in PRAIA_LIBDIR
# for a release build via:
#   make EXTRA_CXXFLAGS='-DPRAIA_LIBDIR="\"/usr/local/lib/praia\""'
CXXFLAGS += $(EXTRA_CXXFLAGS)

# ── Version stamping ──
# Single source of truth: a git tag. `git describe --tags --always --dirty`
# gives clean tag names on release builds (e.g. `v0.7.0`), commit-suffixed
# dev names between tags (`v0.7.0-3-gabc1234`), and a `-dirty` marker if the
# working tree has uncommitted changes. Source tarballs without a `.git/`
# directory fall back to the contents of VERSION at the repo root.
#
# `src/version.h` is generated, gitignored, and depends on a phony target
# so the rule re-evaluates every build. The `cmp -s` check keeps the
# header's mtime stable when the version hasn't actually changed, so
# main.cpp only recompiles on real version moves.
.PHONY: force-version
$(SRC_DIR)/version.h: force-version
	@VERSION=$$(git describe --tags --always --dirty 2>/dev/null) ; \
	[ -z "$$VERSION" ] && VERSION=$$(cat VERSION 2>/dev/null) ; \
	[ -z "$$VERSION" ] && VERSION="unknown" ; \
	printf '#pragma once\n#define PRAIA_VERSION "%s"\n' "$$VERSION" > $@.tmp ; \
	if ! cmp -s $@.tmp $@ 2>/dev/null ; then mv $@.tmp $@ ; else rm $@.tmp ; fi

# main.cpp embeds the version banner — make the dependency explicit so the
# first build creates version.h before compiling main.o. Subsequent rebuilds
# are picked up via the -MMD-generated .d files.
$(BUILD_DIR)/main.o: $(SRC_DIR)/version.h

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR) $(BUILD_DIR)/vm $(BUILD_DIR)/builtins

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(SRC_DIR)/version.h

# ── Install / Uninstall ──
# Usage: make install PREFIX=/usr/local
#        make install PREFIX=/usr LIBDIR=/usr/share/praia
#
# Paths:
#   Binary:  $(PREFIX)/bin/praia
#   Grains:  $(LIBDIR)/grains/         (LIBDIR defaults to $(PREFIX)/lib/praia)
#   Sand:    $(LIBDIR)/ext_grains/sand/   (a regular global grain)
#
# LIBDIR is baked into the binary at compile time so grains resolve
# without relative path guessing. DESTDIR is supported for staging.
#
# The `$(BINDIR)/sand` wrapper is written inline below so `make install`
# doesn't depend on running sand to install sand. Its byte format MUST
# match `sand/grains/bin.praia::wrapperContent` exactly — a test in
# tests/test_sand_manifest_bin.praia asserts equivalence so `sand
# install -g sand@<ver>` produces a wrapper byte-identical to what
# this rule writes.

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib/praia
BINDIR   = $(PREFIX)/bin
SAND_DIR = sand

install:
	$(MAKE) BUILD_DIR=/tmp/praia-install-build CXXFLAGS='$(CXXFLAGS) -DPRAIA_LIBDIR="\"$(LIBDIR)\""'
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -d $(DESTDIR)$(LIBDIR)/include
	cp $(SRC_DIR)/praia_plugin.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/praia_runtime.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/value.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/builtins.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/interpreter.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/gc_heap.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/environment.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/ast.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/token.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/fiber.h $(DESTDIR)$(LIBDIR)/include/
	cp $(SRC_DIR)/signal_state.h $(DESTDIR)$(LIBDIR)/include/
	install -d $(DESTDIR)$(LIBDIR)/grains
	cp -R grains/* $(DESTDIR)$(LIBDIR)/grains/
	# One-release migration: prior installs put sand at $(LIBDIR)/sand.
	# Sand is now a regular global grain under ext_grains/, so drop the
	# stale tree before laying down the new location. Idempotent.
	rm -rf $(DESTDIR)$(LIBDIR)/sand
	install -d $(DESTDIR)$(LIBDIR)/ext_grains/sand
	cp -R $(SAND_DIR)/main.praia $(DESTDIR)$(LIBDIR)/ext_grains/sand/
	cp -R $(SAND_DIR)/grains $(DESTDIR)$(LIBDIR)/ext_grains/sand/
	cp -R $(SAND_DIR)/grain.yaml $(DESTDIR)$(LIBDIR)/ext_grains/sand/
	@printf "#!/bin/sh\n# sand-managed wrapper for sand\nexec '$(BINDIR)/praia' '$(LIBDIR)/ext_grains/sand/main.praia' \"\$$@\"\n" > $(DESTDIR)$(BINDIR)/sand
	chmod 755 $(DESTDIR)$(BINDIR)/sand
	rm -rf /tmp/praia-install-build
	@echo "Installed praia -> $(DESTDIR)$(BINDIR)/praia"
	@echo "Installed sand  -> $(DESTDIR)$(BINDIR)/sand"
	@echo "Installed grains -> $(DESTDIR)$(LIBDIR)/grains/"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/praia
	rm -f $(DESTDIR)$(BINDIR)/sand
	rm -rf $(DESTDIR)$(LIBDIR)
	@echo "Uninstalled praia from $(DESTDIR)$(PREFIX)"

test: $(TARGET) test-input
	./$(TARGET) test

# Integration test for sys.input — needs piped stdin, so runs outside `praia test`.
test-input: $(TARGET)
	@out=$$(printf "Ada\ny\n" | ./$(TARGET) examples/input_demo.praia) && \
	  echo "$$out" | grep -q "Hello, Ada!" && \
	  echo "$$out" | grep -q "Onwards." && \
	  echo "sys.input: ok"
	@out=$$(./$(TARGET) examples/input_demo.praia < /dev/null) && \
	  echo "$$out" | grep -q "No input" && \
	  echo "sys.input EOF: ok"

# ── Plugin build helper ──
# Usage: make plugin SRC=examples/plugins/mathext.cpp OUT=examples/plugins/mathext.dylib
#
# The defines mirror the main CXXFLAGS — fiber.h pulls in <ucontext.h>,
# which on macOS errors out without _XOPEN_SOURCE, and the
# swapcontext/getcontext family is deprecated on both macOS and recent
# glibc (silence the warnings to keep plugin builds clean).
PLUGIN_LDFLAGS =
ifeq ($(UNAME_S),Darwin)
  PLUGIN_LDFLAGS = -undefined dynamic_lookup
endif
plugin:
	$(CXX) -std=c++20 -Wno-deprecated-declarations \
	    -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE \
	    -shared -fPIC -I$(SRC_DIR) $(PLUGIN_LDFLAGS) -o $(OUT) $(SRC)

# ── Fuzz targets ──
# Coverage-guided fuzzing via libFuzzer + ASan + UBSan. Built separately
# from the main g++ pipeline because libFuzzer is Clang-only and the
# sanitizer instrumentation needs to be applied at compile time. Outputs
# live in build_fuzz/ so they don't collide with the main `build/`.
#
# Usage:
#   make fuzz                    # builds all fuzz binaries
#   ./build_fuzz/fuzz_json fuzz/corpus/json/ -max_total_time=60
#
# See fuzz/README.md for triage workflow.
#
# Apple's bundled Clang doesn't ship the libFuzzer runtime, so on macOS
# we prefer Homebrew LLVM if present (provides libclang_rt.fuzzer_osx.a).
# On Linux distro Clang typically ships the runtime; plain `clang++`
# works. CXX_FUZZ can be overridden on the command line:
#   make fuzz CXX_FUZZ=/usr/bin/clang++-17
LLVM_PREFIX := $(shell brew --prefix llvm 2>/dev/null)
ifneq ($(LLVM_PREFIX),)
  CXX_FUZZ ?= $(LLVM_PREFIX)/bin/clang++
else
  CXX_FUZZ ?= clang++
endif

FUZZ_FLAGS  = -std=c++20 -O1 -g -fno-omit-frame-pointer \
              -Wno-deprecated-declarations \
              -fsanitize=fuzzer,address,undefined \
              -D_XOPEN_SOURCE=600 -D_DARWIN_C_SOURCE
FUZZ_BUILD  = build_fuzz

FUZZ_JSON_SRCS  = fuzz/fuzz_json.cpp \
                  src/builtins/json.cpp \
                  fuzz/gc_heap_fuzz.cpp
FUZZ_YAML_SRCS  = fuzz/fuzz_yaml.cpp \
                  src/builtins/yaml.cpp \
                  fuzz/gc_heap_fuzz.cpp
FUZZ_PARSE_SRCS = fuzz/fuzz_lex_parse.cpp \
                  src/lexer.cpp \
                  src/parser.cpp \
                  src/unicode.cpp \
                  fuzz/gc_heap_fuzz.cpp
FUZZ_BYTES_SRCS = fuzz/fuzz_bytes_unpack.cpp \
                  src/builtins/bytes.cpp \
                  src/encoding.cpp \
                  src/unicode.cpp \
                  fuzz/gc_heap_fuzz.cpp

FUZZ_BINS = $(FUZZ_BUILD)/fuzz_json \
            $(FUZZ_BUILD)/fuzz_yaml \
            $(FUZZ_BUILD)/fuzz_lex_parse \
            $(FUZZ_BUILD)/fuzz_bytes_unpack

.PHONY: fuzz fuzz-clean fuzz-run
fuzz: $(FUZZ_BINS)

$(FUZZ_BUILD):
	mkdir -p $@

$(FUZZ_BUILD)/fuzz_json: $(FUZZ_JSON_SRCS) | $(FUZZ_BUILD)
	$(CXX_FUZZ) $(FUZZ_FLAGS) -I$(SRC_DIR) $^ -o $@

$(FUZZ_BUILD)/fuzz_yaml: $(FUZZ_YAML_SRCS) | $(FUZZ_BUILD)
	$(CXX_FUZZ) $(FUZZ_FLAGS) -I$(SRC_DIR) $^ -o $@

$(FUZZ_BUILD)/fuzz_lex_parse: $(FUZZ_PARSE_SRCS) | $(FUZZ_BUILD)
	$(CXX_FUZZ) $(FUZZ_FLAGS) -I$(SRC_DIR) $^ -o $@

$(FUZZ_BUILD)/fuzz_bytes_unpack: $(FUZZ_BYTES_SRCS) | $(FUZZ_BUILD)
	$(CXX_FUZZ) $(FUZZ_FLAGS) -I$(SRC_DIR) $^ -o $@

fuzz-clean:
	rm -rf $(FUZZ_BUILD)

# Convenience runner: `make fuzz-run TARGET=json [SECONDS=60]`.
# Always uses the safe seeds-read-only + corpus-writable layout so an
# accidental single-dir invocation can't pollute fuzz/seeds/.
fuzz-run:
	@if [ -z "$(TARGET)" ]; then \
		echo "Usage: make fuzz-run TARGET=<json|yaml|lex_parse|bytes_unpack> [SECONDS=60]"; \
		exit 1; \
	fi
	@mkdir -p fuzz/corpus/$(TARGET) fuzz/crashes/$(TARGET)
	./$(FUZZ_BUILD)/fuzz_$(TARGET) fuzz/corpus/$(TARGET)/ fuzz/seeds/$(TARGET)/ \
	    -max_total_time=$(if $(SECONDS),$(SECONDS),60) \
	    -artifact_prefix=fuzz/crashes/$(TARGET)/ \
	    -print_final_stats=1

.PHONY: all clean install uninstall test test-input plugin

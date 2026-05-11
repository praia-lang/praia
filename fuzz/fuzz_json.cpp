// libFuzzer target for Praia's JSON parser.
//
// Compile via `make fuzz`. The fuzzer drives `jsonParse` (defined in
// src/builtins/json.cpp) with arbitrary byte sequences and looks for
// crashes, sanitizer errors, or hangs. Malformed input legitimately
// throws RuntimeError — we swallow those; anything else is a real bug.

#include "../src/value.h"
#include "../src/gc_heap.h"
#include <cstddef>
#include <cstdint>
#include <string>

// Defined in src/builtins/json.cpp.
extern Value jsonParse(const std::string& src);

extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/) {
    // jsonParse uses gcNew<PraiaMap>/<PraiaArray> which always pushes a
    // weak_ptr into GcHeap's entries_ vector. Without a root marker
    // collectIfNeeded() no-ops, so entries_ would grow unbounded over a
    // long fuzz run. Install a no-op marker so periodic collect() can
    // sweep expired entries (the actual Praia heap objects die when each
    // Value goes out of scope between fuzz iterations).
    GcHeap::current().setRootMarker([](GcHeap&){});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // 1 MiB input cap — keeps iterations fast. Inputs above this are
    // very unlikely to find new coverage and just slow the fuzzer.
    if (size > 1u << 20) return 0;

    std::string input(reinterpret_cast<const char*>(data), size);
    try {
        (void) jsonParse(input);
    } catch (const RuntimeError&) {
        // Expected — malformed JSON throws.
    } catch (const std::exception&) {
        // Other std exceptions (bad_alloc on huge inputs etc.) — also
        // benign for fuzzing. Anything that aborts or segfaults will
        // still be caught by ASan/the fuzzer itself.
    }

    // Periodic sweep of expired weak_ptrs in the GC heap. Every 4096
    // inputs is plenty — the actual cost is one pass over entries_.
    static unsigned counter = 0;
    if ((++counter & 0xFFF) == 0) {
        GcHeap::current().collect();
    }
    return 0;
}

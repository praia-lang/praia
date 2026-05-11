// libFuzzer target for Praia's YAML parser.
//
// Same shape as fuzz_json.cpp — see that file for the GC interaction
// explanation. Drives `yamlParse` (defined in src/builtins/yaml.cpp).

#include "../src/value.h"
#include "../src/gc_heap.h"
#include <cstddef>
#include <cstdint>
#include <string>

extern Value yamlParse(const std::string& src);

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    GcHeap::current().setRootMarker([](GcHeap&){});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1u << 20) return 0;

    std::string input(reinterpret_cast<const char*>(data), size);
    try {
        (void) yamlParse(input);
    } catch (const RuntimeError&) {
        // expected
    } catch (const std::exception&) {
        // benign
    }

    // Counter is per-process; libFuzzer's parallel modes use separate
    // processes, so no race. See fuzz_json.cpp for full reasoning.
    static unsigned counter = 0;
    if ((++counter & 0xFFF) == 0) {
        GcHeap::current().collect();
    }
    return 0;
}

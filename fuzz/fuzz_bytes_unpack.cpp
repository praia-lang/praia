// libFuzzer target for Praia's bytes.unpack.
//
// Drives the full unpack path (format-string parsing + data buffer
// walking) via bytesUnpack(fmt, data), extracted to file scope in
// src/builtins/bytes.cpp for fuzz access.
//
// Input layout: the fuzzer hands us a single byte stream. We split it
// into a format string and a data buffer using a length-prefix scheme:
//   byte 0          — format length F (0..255)
//   bytes 1..F+1    — format string
//   bytes F+1..end  — data buffer
// This lets the mutator vary both sides independently while keeping the
// split deterministic.

#include "../src/value.h"
#include "../src/gc_heap.h"
#include <cstddef>
#include <cstdint>
#include <string>

extern Value bytesUnpack(const std::string& fmt, const std::string& data);

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    GcHeap::current().setRootMarker([](GcHeap&){});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > 1u << 20) return 0;

    size_t fmtLen = data[0];
    if (1 + fmtLen > size) fmtLen = size - 1;
    std::string fmt(reinterpret_cast<const char*>(data + 1), fmtLen);
    std::string buf(reinterpret_cast<const char*>(data + 1 + fmtLen),
                    size - 1 - fmtLen);

    try {
        (void) bytesUnpack(fmt, buf);
    } catch (const RuntimeError&) {
        // expected — malformed format or short data throws
    } catch (const std::exception&) {
        // benign
    }

    static unsigned counter = 0;
    if ((++counter & 0xFFF) == 0) {
        GcHeap::current().collect();
    }
    return 0;
}

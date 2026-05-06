#pragma once

#include "../value.h"
#include "opcode.h"
#include <cstdint>
#include <string>
#include <vector>

struct Chunk {
    std::vector<uint8_t> code;
    std::vector<Value> constants;

    // Inline cache for OP_GET_GLOBAL / OP_SET_GLOBAL: parallel to `constants`,
    // mapping a constant pool index (the global's name) to the VM's
    // resolved slot index. -1 means "not yet resolved" — first access does
    // the name → slot lookup and writes the slot here; subsequent accesses
    // skip the string hash entirely. Slot values are stable for the
    // lifetime of a VM (slots are append-only), so a single-cell cache is
    // sufficient — no invalidation needed.
    //
    // Concurrency: shared chunks (async tasks reuse the parent's
    // CompiledFunction) may race on the first resolution. The result is
    // identical regardless of who wins, and aligned int reads/writes are
    // atomic on every supported architecture (x86, ARM); the worst case
    // is duplicate resolution work.
    std::vector<int> globalSlotCache;

    // Run-length encoded line info
    struct LineEntry { int line; int count; };
    std::vector<LineEntry> lines;

    // Run-length encoded column info
    struct ColumnEntry { int column; int count; };
    std::vector<ColumnEntry> columns;

    void write(uint8_t byte, int line, int column = 0);
    void write(OpCode op, int line, int column = 0);
    void writeU16(uint16_t value, int line, int column = 0);
    uint16_t addConstant(Value value);
    int getLine(int offset) const;
    int getColumn(int offset) const;
    void patchU16(int offset, uint16_t value);
    int size() const { return static_cast<int>(code.size()); }
};

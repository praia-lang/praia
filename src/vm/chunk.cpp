#include "chunk.h"

void Chunk::write(uint8_t byte, int line, int column) {
    code.push_back(byte);
    if (!lines.empty() && lines.back().line == line) {
        lines.back().count++;
    } else {
        lines.push_back({line, 1});
    }
    if (!columns.empty() && columns.back().column == column) {
        columns.back().count++;
    } else {
        columns.push_back({column, 1});
    }
}

void Chunk::write(OpCode op, int line, int column) {
    write(static_cast<uint8_t>(op), line, column);
}

void Chunk::writeU16(uint16_t value, int line, int column) {
    write(static_cast<uint8_t>((value >> 8) & 0xFF), line, column);
    write(static_cast<uint8_t>(value & 0xFF), line, column);
}

uint16_t Chunk::addConstant(Value value) {
    if (constants.size() >= 65535)
        throw RuntimeError("Too many constants in one chunk (max 65535)", 0);
    constants.push_back(std::move(value));
    globalSlotCache.push_back(-1); // parallel slot cache stays in sync with constants
    return static_cast<uint16_t>(constants.size() - 1);
}

int Chunk::getLine(int offset) const {
    int current = 0;
    for (auto& entry : lines) {
        current += entry.count;
        if (offset < current) return entry.line;
    }
    return lines.empty() ? 0 : lines.back().line;
}

int Chunk::getColumn(int offset) const {
    int current = 0;
    for (auto& entry : columns) {
        current += entry.count;
        if (offset < current) return entry.column;
    }
    return columns.empty() ? 0 : columns.back().column;
}

void Chunk::patchU16(int offset, uint16_t value) {
    code[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    code[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

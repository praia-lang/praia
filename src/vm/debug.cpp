#include "debug.h"
#include "opcode.h"
#include "vm.h"
#include <iomanip>
#include <iostream>

static int simpleInstruction(const char* name, int offset) {
    std::cout << name << "\n";
    return offset + 1;
}

static int constantInstruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t idx = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << idx
              << " '" << chunk.constants[idx].toString() << "'\n";
    return offset + 3;
}

static int byteInstruction(const char* name, const Chunk& chunk, int offset) {
    uint8_t val = chunk.code[offset + 1];
    std::cout << std::left << std::setw(20) << name << " " << (int)val << "\n";
    return offset + 2;
}

static int u16Instruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t val = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << val << "\n";
    return offset + 3;
}

static int jumpInstruction(const char* name, int sign, const Chunk& chunk, int offset) {
    uint16_t jump = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    std::cout << std::left << std::setw(20) << name << " " << offset
              << " -> " << (offset + 3 + sign * jump) << "\n";
    return offset + 3;
}

// [argc:8] [names_idx:16]
static int callNamedInstruction(const char* name, const Chunk& chunk, int offset) {
    uint8_t argc = chunk.code[offset + 1];
    uint16_t namesIdx = (chunk.code[offset + 2] << 8) | chunk.code[offset + 3];
    std::cout << std::left << std::setw(20) << name << " argc=" << (int)argc
              << " names=" << namesIdx << "\n";
    return offset + 4;
}

// [name_idx:16] [argc:8]
static int invokeInstruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t nameIdx = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    uint8_t argc = chunk.code[offset + 3];
    std::cout << std::left << std::setw(20) << name << " " << nameIdx
              << " '" << chunk.constants[nameIdx].toString() << "' argc=" << (int)argc << "\n";
    return offset + 4;
}

// [path_idx:16] [alias_idx:16]
static int importInstruction(const char* name, const Chunk& chunk, int offset) {
    uint16_t pathIdx = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
    uint16_t aliasIdx = (chunk.code[offset + 3] << 8) | chunk.code[offset + 4];
    std::cout << std::left << std::setw(20) << name
              << " path=" << pathIdx << " '" << chunk.constants[pathIdx].toString() << "'"
              << " alias=" << aliasIdx << " '" << chunk.constants[aliasIdx].toString() << "'\n";
    return offset + 5;
}

// [count:8] followed by count [name_idx:16]
static int exportInstruction(const char* name, const Chunk& chunk, int offset) {
    uint8_t count = chunk.code[offset + 1];
    std::cout << std::left << std::setw(20) << name << " count=" << (int)count << " [";
    int pos = offset + 2;
    for (int i = 0; i < count; i++) {
        uint16_t idx = (chunk.code[pos] << 8) | chunk.code[pos + 1];
        if (i > 0) std::cout << ", ";
        std::cout << chunk.constants[idx].toString();
        pos += 2;
    }
    std::cout << "]\n";
    return pos;
}

void disassembleChunk(const Chunk& chunk, const std::string& name) {
    std::cout << "== " << name << " ==\n";
    for (int offset = 0; offset < chunk.size();) {
        offset = disassembleInstruction(chunk, offset);
    }
}

int disassembleInstruction(const Chunk& chunk, int offset) {
    std::cout << std::setfill('0') << std::setw(4) << offset << " ";

    int line = chunk.getLine(offset);
    if (offset > 0 && line == chunk.getLine(offset - 1))
        std::cout << "   | ";
    else
        std::cout << std::setfill(' ') << std::setw(4) << line << " ";

    uint8_t instruction = chunk.code[offset];
    switch (static_cast<OpCode>(instruction)) {
        // Literals
        case OpCode::OP_CONSTANT:       return constantInstruction("OP_CONSTANT", chunk, offset);
        case OpCode::OP_NIL:            return simpleInstruction("OP_NIL", offset);
        case OpCode::OP_TRUE:           return simpleInstruction("OP_TRUE", offset);
        case OpCode::OP_FALSE:          return simpleInstruction("OP_FALSE", offset);

        // Stack
        case OpCode::OP_POP:            return simpleInstruction("OP_POP", offset);
        case OpCode::OP_POPN:           return byteInstruction("OP_POPN", chunk, offset);
        case OpCode::OP_DUP:            return simpleInstruction("OP_DUP", offset);
        case OpCode::OP_SWAP:           return simpleInstruction("OP_SWAP", offset);

        // Arithmetic
        case OpCode::OP_ADD:            return simpleInstruction("OP_ADD", offset);
        case OpCode::OP_SUBTRACT:       return simpleInstruction("OP_SUBTRACT", offset);
        case OpCode::OP_MULTIPLY:       return simpleInstruction("OP_MULTIPLY", offset);
        case OpCode::OP_DIVIDE:         return simpleInstruction("OP_DIVIDE", offset);
        case OpCode::OP_MODULO:         return simpleInstruction("OP_MODULO", offset);
        case OpCode::OP_NEGATE:         return simpleInstruction("OP_NEGATE", offset);

        // Bitwise
        case OpCode::OP_BIT_AND:        return simpleInstruction("OP_BIT_AND", offset);
        case OpCode::OP_BIT_OR:         return simpleInstruction("OP_BIT_OR", offset);
        case OpCode::OP_BIT_XOR:        return simpleInstruction("OP_BIT_XOR", offset);
        case OpCode::OP_BIT_NOT:        return simpleInstruction("OP_BIT_NOT", offset);
        case OpCode::OP_SHL:            return simpleInstruction("OP_SHL", offset);
        case OpCode::OP_SHR:            return simpleInstruction("OP_SHR", offset);

        // Comparison
        case OpCode::OP_EQUAL:          return simpleInstruction("OP_EQUAL", offset);
        case OpCode::OP_NOT_EQUAL:      return simpleInstruction("OP_NOT_EQUAL", offset);
        case OpCode::OP_IS:             return simpleInstruction("OP_IS", offset);
        case OpCode::OP_LESS:           return simpleInstruction("OP_LESS", offset);
        case OpCode::OP_GREATER:        return simpleInstruction("OP_GREATER", offset);
        case OpCode::OP_LESS_EQUAL:     return simpleInstruction("OP_LESS_EQUAL", offset);
        case OpCode::OP_GREATER_EQUAL:  return simpleInstruction("OP_GREATER_EQUAL", offset);
        case OpCode::OP_NOT:            return simpleInstruction("OP_NOT", offset);

        // Variables
        case OpCode::OP_DEFINE_GLOBAL:  return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
        case OpCode::OP_GET_GLOBAL:     return constantInstruction("OP_GET_GLOBAL", chunk, offset);
        case OpCode::OP_SET_GLOBAL:     return constantInstruction("OP_SET_GLOBAL", chunk, offset);
        case OpCode::OP_GET_LOCAL:      return u16Instruction("OP_GET_LOCAL", chunk, offset);
        case OpCode::OP_SET_LOCAL:      return u16Instruction("OP_SET_LOCAL", chunk, offset);

        // Postfix
        case OpCode::OP_POST_INC_LOCAL:   return u16Instruction("OP_POST_INC_LOCAL", chunk, offset);
        case OpCode::OP_POST_DEC_LOCAL:   return u16Instruction("OP_POST_DEC_LOCAL", chunk, offset);
        case OpCode::OP_POST_INC_GLOBAL:  return constantInstruction("OP_POST_INC_GLOBAL", chunk, offset);
        case OpCode::OP_POST_DEC_GLOBAL:  return constantInstruction("OP_POST_DEC_GLOBAL", chunk, offset);
        case OpCode::OP_POST_INC_UPVALUE: return u16Instruction("OP_POST_INC_UPVALUE", chunk, offset);
        case OpCode::OP_POST_DEC_UPVALUE: return u16Instruction("OP_POST_DEC_UPVALUE", chunk, offset);

        // Control flow
        case OpCode::OP_JUMP:           return jumpInstruction("OP_JUMP", 1, chunk, offset);
        case OpCode::OP_JUMP_BACK:      return jumpInstruction("OP_JUMP_BACK", -1, chunk, offset);
        case OpCode::OP_JUMP_IF_FALSE:  return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
        case OpCode::OP_JUMP_IF_TRUE:   return jumpInstruction("OP_JUMP_IF_TRUE", 1, chunk, offset);
        case OpCode::OP_JUMP_IF_NOT_NIL:return jumpInstruction("OP_JUMP_IF_NOT_NIL", 1, chunk, offset);
        case OpCode::OP_POP_JUMP_IF_FALSE: return jumpInstruction("OP_POP_JUMP_IF_FALSE", 1, chunk, offset);

        // Functions
        case OpCode::OP_CALL:           return byteInstruction("OP_CALL", chunk, offset);
        case OpCode::OP_CALL_SPREAD:    return simpleInstruction("OP_CALL_SPREAD", offset);
        case OpCode::OP_CALL_NAMED:     return callNamedInstruction("OP_CALL_NAMED", chunk, offset);
        case OpCode::OP_RETURN:         return simpleInstruction("OP_RETURN", offset);
        case OpCode::OP_CLOSURE: {
            uint16_t idx = (chunk.code[offset + 1] << 8) | chunk.code[offset + 2];
            std::cout << std::left << std::setw(20) << "OP_CLOSURE" << " " << idx
                      << " '" << chunk.constants[idx].toString() << "'\n";
            int pos = offset + 3;
            // Read upvalue descriptors from the closure's function
            auto& val = chunk.constants[idx];
            int uvCount = 0;
            if (val.isCallable()) {
                auto* vmcc = dynamic_cast<VMClosureCallable*>(val.asCallable().get());
                if (vmcc) uvCount = vmcc->closure->upvalueCount;
            }
            for (int i = 0; i < uvCount; i++) {
                uint8_t isLocal = chunk.code[pos++];
                uint16_t index = (chunk.code[pos] << 8) | chunk.code[pos + 1];
                pos += 2;
                std::cout << std::setfill('0') << std::setw(4) << (pos - 3) << "    |   "
                          << (isLocal ? "local" : "upvalue") << " " << index << "\n";
            }
            return pos;
        }
        case OpCode::OP_GET_UPVALUE:    return u16Instruction("OP_GET_UPVALUE", chunk, offset);
        case OpCode::OP_SET_UPVALUE:    return u16Instruction("OP_SET_UPVALUE", chunk, offset);
        case OpCode::OP_CLOSE_UPVALUE:  return simpleInstruction("OP_CLOSE_UPVALUE", offset);

        // Classes
        case OpCode::OP_CLASS:          return constantInstruction("OP_CLASS", chunk, offset);
        case OpCode::OP_METHOD:         return constantInstruction("OP_METHOD", chunk, offset);
        case OpCode::OP_STATIC_METHOD:  return constantInstruction("OP_STATIC_METHOD", chunk, offset);
        case OpCode::OP_INHERIT:        return simpleInstruction("OP_INHERIT", offset);
        case OpCode::OP_GET_PROPERTY:   return constantInstruction("OP_GET_PROPERTY", chunk, offset);
        case OpCode::OP_GET_PROPERTY_OPT: return constantInstruction("OP_GET_PROPERTY_OPT", chunk, offset);
        case OpCode::OP_SET_PROPERTY:   return constantInstruction("OP_SET_PROPERTY", chunk, offset);
        case OpCode::OP_GET_THIS:       return simpleInstruction("OP_GET_THIS", offset);
        case OpCode::OP_GET_SUPER:      return constantInstruction("OP_GET_SUPER", chunk, offset);
        case OpCode::OP_INVOKE:         return invokeInstruction("OP_INVOKE", chunk, offset);
        case OpCode::OP_SUPER_INVOKE:   return invokeInstruction("OP_SUPER_INVOKE", chunk, offset);

        // Collections
        case OpCode::OP_BUILD_ARRAY:    return u16Instruction("OP_BUILD_ARRAY", chunk, offset);
        case OpCode::OP_BUILD_MAP:      return u16Instruction("OP_BUILD_MAP", chunk, offset);
        case OpCode::OP_INDEX_GET:      return simpleInstruction("OP_INDEX_GET", offset);
        case OpCode::OP_INDEX_GET_OPT:  return simpleInstruction("OP_INDEX_GET_OPT", offset);
        case OpCode::OP_INDEX_SET:      return simpleInstruction("OP_INDEX_SET", offset);
        case OpCode::OP_UNPACK_SPREAD:  return simpleInstruction("OP_UNPACK_SPREAD", offset);
        case OpCode::OP_BUILD_STRING:   return u16Instruction("OP_BUILD_STRING", chunk, offset);

        // Error handling
        case OpCode::OP_TRY_BEGIN:      return jumpInstruction("OP_TRY_BEGIN", 1, chunk, offset);
        case OpCode::OP_TRY_END:        return simpleInstruction("OP_TRY_END", offset);
        case OpCode::OP_THROW:          return simpleInstruction("OP_THROW", offset);
        case OpCode::OP_DEFER:          return simpleInstruction("OP_DEFER", offset);
        case OpCode::OP_BUILD_TAGGED:   return invokeInstruction("OP_BUILD_TAGGED", chunk, offset);
        case OpCode::OP_TAG_OR_CALL:    return invokeInstruction("OP_TAG_OR_CALL", chunk, offset);
        case OpCode::OP_RUN_DEFERS:     return simpleInstruction("OP_RUN_DEFERS", offset);

        // Generators
        case OpCode::OP_YIELD:          return simpleInstruction("OP_YIELD", offset);

        // Advanced
        case OpCode::OP_ASYNC:          return byteInstruction("OP_ASYNC", chunk, offset);
        case OpCode::OP_ASYNC_NAMED:    return callNamedInstruction("OP_ASYNC_NAMED", chunk, offset);
        case OpCode::OP_AWAIT:          return simpleInstruction("OP_AWAIT", offset);
        case OpCode::OP_IMPORT:         return importInstruction("OP_IMPORT", chunk, offset);
        case OpCode::OP_EXPORT:         return exportInstruction("OP_EXPORT", chunk, offset);

        default:
            std::cout << "Unknown opcode " << (int)instruction << "\n";
            return offset + 1;
    }
}

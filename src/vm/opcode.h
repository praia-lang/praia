#pragma once

#include <cstdint>

enum class OpCode : uint8_t {
    // ── Literals ──
    OP_CONSTANT,        // [idx:16] Push constants[idx]
    OP_NIL,
    OP_TRUE,
    OP_FALSE,

    // ── Stack ──
    OP_POP,
    OP_POPN,            // [n:8]
    OP_DUP,             // duplicate top of stack
    OP_SWAP,            // swap top two stack values

    // ── Arithmetic ──
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_NEGATE,

    // ── Bitwise ──
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_SHL,
    OP_SHR,

    // ── Comparison ──
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_IS,
    OP_LESS,
    OP_GREATER,
    OP_LESS_EQUAL,
    OP_GREATER_EQUAL,
    OP_NOT,

    // ── Variables ──
    OP_DEFINE_GLOBAL,   // [name_idx:16]
    OP_GET_GLOBAL,      // [name_idx:16]
    OP_SET_GLOBAL,      // [name_idx:16]
    OP_GET_LOCAL,       // [slot:16]
    OP_SET_LOCAL,       // [slot:16]

    // ── Function prologue ──
    OP_IS_MISSING_ARG,    // [slot:16] — pushes true if param at slot was not provided by caller

    // ── Postfix ──
    OP_POST_INC_LOCAL,    // [slot:16]
    OP_POST_DEC_LOCAL,    // [slot:16]
    OP_POST_INC_GLOBAL,   // [name_idx:16]
    OP_POST_DEC_GLOBAL,   // [name_idx:16]
    OP_POST_INC_UPVALUE,  // [slot:16]
    OP_POST_DEC_UPVALUE,  // [slot:16]

    // ── Control flow ──
    OP_JUMP,            // [offset:16]
    OP_JUMP_BACK,       // [offset:16]
    OP_JUMP_IF_FALSE,   // [offset:16] peek, don't pop
    OP_JUMP_IF_TRUE,    // [offset:16] peek, don't pop
    OP_JUMP_IF_NOT_NIL, // [offset:16] peek, don't pop
    OP_POP_JUMP_IF_FALSE, // [offset:16] pop then jump

    // ── Functions ──
    OP_CALL,            // [argc:8]
    OP_CALL_SPREAD,     // stack: [callee, argsArray] — call with spread args
    OP_CALL_NAMED,      // [argc:8] [names_idx:16] — call with named arguments
    OP_RETURN,
    OP_CLOSURE,         // [fn_idx:16] followed by upvalue descriptors
    OP_GET_UPVALUE,     // [slot:16]
    OP_SET_UPVALUE,     // [slot:16]
    OP_CLOSE_UPVALUE,

    // ── Classes ──
    OP_CLASS,           // [name_idx:16]
    OP_METHOD,          // [name_idx:16]
    OP_STATIC_METHOD,   // [name_idx:16]
    OP_METHOD_DECORATOR,// [name_idx:16] — store decorator callable for method
    OP_INHERIT,
    OP_GET_PROPERTY,    // [name_idx:16]
    OP_GET_PROPERTY_OPT,// [name_idx:16] — nil if object is nil
    OP_SET_PROPERTY,    // [name_idx:16]
    OP_GET_THIS,
    OP_GET_SUPER,       // [name_idx:16]
    OP_INVOKE,          // [name_idx:16] [argc:8]
    OP_SUPER_INVOKE,    // [name_idx:16] [argc:8]

    // ── Collections ──
    OP_BUILD_ARRAY,     // [count:16]
    OP_BUILD_MAP,       // [count:16] (count key-value pairs)
    OP_INDEX_GET,
    OP_INDEX_GET_OPT,   // nil if object is nil
    OP_INDEX_SET,
    OP_UNPACK_SPREAD,
    OP_BUILD_STRING,    // [count:16]

    // ── Error handling ──
    OP_TRY_BEGIN,       // [catch_offset:16]
    OP_TRY_END,
    OP_THROW,
    OP_DEFER,           // push top-of-stack onto current frame's defer list
    OP_RUN_DEFERS,      // execute all defers for current frame in LIFO order

    // ── Tagged values ──
    OP_BUILD_TAGGED,    // [name_idx:16] [argc:8] — build tagged value from stack args
    OP_TAG_OR_CALL,     // [name_idx:16] [argc:8] — if callee is callable, call; else build tagged

    // ── Advanced ──
    OP_YIELD,           // yield value from generator
    OP_ASYNC,           // [argc:8]
    OP_ASYNC_NAMED,     // [argc:8] [names_idx:16] — async call with named arguments
    OP_AWAIT,
    OP_IMPORT,          // [path_idx:16] [alias_idx:16]
    OP_EXPORT,          // [count:8] followed by count name indices
};

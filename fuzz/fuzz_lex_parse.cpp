// libFuzzer target for Praia's lexer + parser pipeline.
//
// Drives Lexer + Parser on arbitrary byte sequences treated as Praia
// source. Catches crashes/sanitizer errors in the language-frontend
// code paths — token scanning, parser recovery, AST construction.
// Mirrors the compile() chain in src/main.cpp.

#include "../src/lexer.h"
#include "../src/parser.h"
#include "../src/value.h"  // for RuntimeError
#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 1u << 20) return 0;

    std::string source(reinterpret_cast<const char*>(data), size);
    try {
        Lexer lex(source);
        auto toks = lex.tokenize();
        if (lex.hasError()) return 0;
        Parser parser(toks);
        (void) parser.parse();
    } catch (const RuntimeError&) {
        // expected — parse errors throw
    } catch (const std::exception&) {
        // benign — parser ParseError, bad_alloc, etc.
    }
    return 0;
}

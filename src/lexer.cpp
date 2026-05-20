#include "lexer.h"
#include "unicode.h"
#include <iostream>
#include <unordered_map>

static const std::unordered_map<std::string, TokenType> keywords = {
    {"let",    TokenType::LET},
    {"func",   TokenType::FUNC},
    {"if",     TokenType::IF},
    {"else",   TokenType::ELSE},
    {"elif",   TokenType::ELIF},
    {"match",  TokenType::MATCH},
    {"static", TokenType::STATIC},
    {"while",  TokenType::WHILE},
    {"for",    TokenType::FOR},
    {"in",     TokenType::IN},
    {"is",     TokenType::IS},
    {"return",   TokenType::RETURN},
    {"break",    TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"try",    TokenType::TRY},
    {"catch",  TokenType::CATCH},
    {"throw",  TokenType::THROW},
    {"finally",TokenType::FINALLY},
    {"ensure", TokenType::ENSURE},
    {"defer",  TokenType::DEFER},
    {"async",   TokenType::ASYNC},
    {"await",   TokenType::AWAIT},
    {"yield",   TokenType::YIELD},
    {"enum",    TokenType::ENUM},
    {"class",   TokenType::CLASS},
    {"extends", TokenType::EXTENDS},
    {"this",    TokenType::THIS},
    {"super",   TokenType::SUPER},
    {"lam",     TokenType::LAM},
    {"use",    TokenType::USE},
    {"export", TokenType::EXPORT},
    {"true",   TokenType::TRUE},
    {"false",  TokenType::FALSE},
    {"nil",    TokenType::NIL},
};

Lexer::Lexer(const std::string& source) : source(source) {}

std::vector<Token> Lexer::tokenize() {
    // Skip shebang line (#!/usr/bin/env praia)
    if (current + 1 < static_cast<int>(source.size()) &&
        source[0] == '#' && source[1] == '!') {
        while (!isAtEnd() && source[current] != '\n') current++;
        if (!isAtEnd()) { current++; line++; lineStart = current; }
    }

    while (!isAtEnd()) {
        start = current;
        scanToken();
    }
    tokens.push_back({TokenType::EOF_TOKEN, "", line, current - lineStart + 1});
    return tokens;
}

void Lexer::scanToken() {
    char c = advance();
    switch (c) {
        case '(': addToken(TokenType::LPAREN); break;
        case ')': addToken(TokenType::RPAREN); break;
        case '{': addToken(TokenType::LBRACE); break;
        case '}': addToken(TokenType::RBRACE); break;
        case '[': addToken(TokenType::LBRACKET); break;
        case ']': addToken(TokenType::RBRACKET); break;
        case ',': addToken(TokenType::COMMA); break;
        case '*': addToken(match('=') ? TokenType::STAR_ASSIGN : TokenType::STAR); break;
        case '%': addToken(match('=') ? TokenType::PERCENT_ASSIGN : TokenType::PERCENT); break;
        case '.':
            if (match('.')) {
                addToken(match('.') ? TokenType::SPREAD : TokenType::DOT_DOT);
            } else {
                addToken(TokenType::DOT);
            }
            break;
        case ':': addToken(TokenType::COLON); break;
        case '?':
            if (match('.')) addToken(TokenType::QUESTION_DOT);
            else if (match('?')) addToken(TokenType::NIL_COALESCE);
            else addToken(TokenType::QUESTION);
            break;

        case '+':
            if (match('+')) addToken(TokenType::INCREMENT);
            else if (match('=')) addToken(TokenType::PLUS_ASSIGN);
            else addToken(TokenType::PLUS);
            break;
        case '-':
            if (match('-')) addToken(TokenType::DECREMENT);
            else if (match('=')) addToken(TokenType::MINUS_ASSIGN);
            else addToken(TokenType::MINUS);
            break;
        case '!':
            addToken(match('=') ? TokenType::NEQ : TokenType::NOT);
            break;
        case '=':
            addToken(match('=') ? TokenType::EQ : TokenType::ASSIGN);
            break;
        case '<':
            if (match('<')) addToken(TokenType::SHL);
            else addToken(match('=') ? TokenType::LTE : TokenType::LT);
            break;
        case '>':
            if (match('>')) addToken(TokenType::SHR);
            else addToken(match('=') ? TokenType::GTE : TokenType::GT);
            break;

        case '&':
            addToken(match('&') ? TokenType::AND : TokenType::BIT_AND);
            break;
        case '|':
            if (match('|')) addToken(TokenType::OR);
            else if (peek() == '?' && peekNext() == '>') {
                advance(); advance(); // consume ? and >
                addToken(TokenType::PIPE_TRY);
            }
            else if (match('>')) addToken(TokenType::PIPE);
            else addToken(TokenType::BIT_OR);
            break;
        case '^': addToken(TokenType::BIT_XOR); break;
        case '~': addToken(TokenType::BIT_NOT); break;

        case '/':
            if (match('/')) {
                while (peek() != '\n' && !isAtEnd()) advance();
            } else if (match('*')) {
                blockComment();
            } else if (match('=')) {
                addToken(TokenType::SLASH_ASSIGN);
            } else {
                addToken(TokenType::SLASH);
            }
            break;

        case ' ':
        case '\r':
        case '\t':
            break;
        case ';':
            addToken(TokenType::SEMICOLON);
            break;
        case '\n':
            line++;
            lineStart = current;
            break;

        case '"':
            if (peek() == '"' && peekNext() == '"') {
                advance(); advance(); // consume the other two quotes
                tripleString('"');
            } else {
                string('"');
            }
            break;
        case '\'':
            if (peek() == '\'' && peekNext() == '\'') {
                advance(); advance();
                tripleString('\'');
            } else {
                string('\'');
            }
            break;

        case '@': addToken(TokenType::AT); break;

        // `#{` opens a set literal (#{1, 2, 3}). A bare `#` is not a
        // valid token — fall through to the default error so typos
        // like `# comment` (Praia uses `//`) don't silently parse.
        case '#':
            if (match('{')) { addToken(TokenType::HASH_LBRACE); break; }
            error("Unexpected character '#' (did you mean '#{' for a set literal, or '//' for a comment?)");
            break;

        default:
            if (std::isdigit(c)) {
                number();
            } else if (std::isalpha(c) || c == '_') {
                identifier();
            } else {
                error(std::string("Unexpected character '") + c + "'");
            }
            break;
    }
}

// --- Character helpers ---

char Lexer::advance() {
    return source[current++];
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source[current];
}

char Lexer::peekNext() const {
    if (current + 1 >= static_cast<int>(source.size())) return '\0';
    return source[current + 1];
}

bool Lexer::match(char expected) {
    if (isAtEnd() || source[current] != expected) return false;
    current++;
    return true;
}

bool Lexer::isAtEnd() const {
    return current >= static_cast<int>(source.size());
}

// --- Token scanners ---

void Lexer::number() {
    // Hex literal: 0x or 0X
    if (source[start] == '0' && (peek() == 'x' || peek() == 'X')) {
        advance(); // consume 'x'
        if (!std::isxdigit(peek())) {
            error("Expected hex digit after '0x'");
            return;
        }
        while (std::isxdigit(peek()) || peek() == '_') advance();
        addToken(TokenType::NUMBER);
        return;
    }

    // Binary literal: 0b or 0B
    if (source[start] == '0' && (peek() == 'b' || peek() == 'B')) {
        advance(); // consume 'b'
        if (peek() != '0' && peek() != '1') {
            error("Expected binary digit after '0b'");
            return;
        }
        while (peek() == '0' || peek() == '1' || peek() == '_') advance();
        addToken(TokenType::NUMBER);
        return;
    }

    // Octal literal: 0o or 0O
    if (source[start] == '0' && (peek() == 'o' || peek() == 'O')) {
        advance(); // consume 'o'
        if (peek() < '0' || peek() > '7') {
            error("Expected octal digit after '0o'");
            return;
        }
        while ((peek() >= '0' && peek() <= '7') || peek() == '_') advance();
        addToken(TokenType::NUMBER);
        return;
    }

    // Decimal integer or float
    while (std::isdigit(peek()) || peek() == '_') advance();

    if (peek() == '.' && std::isdigit(peekNext())) {
        advance(); // consume '.'
        while (std::isdigit(peek()) || peek() == '_') advance();
    }

    // Scientific notation: e or E followed by optional +/- and digits
    if (peek() == 'e' || peek() == 'E') {
        advance(); // consume 'e'
        if (peek() == '+' || peek() == '-') advance();
        if (!std::isdigit(peek())) {
            error("Expected digit in exponent");
            return;
        }
        while (std::isdigit(peek()) || peek() == '_') advance();
    }

    // Validate underscore placement: not at start/end of digit runs, not doubled
    std::string lex = source.substr(start, current - start);
    for (size_t i = 0; i < lex.size(); i++) {
        if (lex[i] == '_') {
            if (i == 0 || i == lex.size() - 1 ||
                lex[i - 1] == '_' || lex[i - 1] == '.' || lex[i - 1] == 'x' ||
                lex[i - 1] == 'X' || lex[i - 1] == 'b' || lex[i - 1] == 'B' ||
                lex[i - 1] == 'o' || lex[i - 1] == 'O' || lex[i - 1] == 'e' ||
                lex[i - 1] == 'E' || lex[i - 1] == '+' || lex[i - 1] == '-') {
                error("Invalid underscore placement in number literal");
                return;
            }
        }
    }

    addToken(TokenType::NUMBER);
}

void Lexer::string(char quote) {
    std::string value;
    bool hasInterp = false;
    bool firstInterp = true;

    while (peek() != quote && !isAtEnd()) {
        if (peek() == '\n') { line++; lineStart = current + 1; }

        // String interpolation: %{expr} (works in both quote styles)
        if (peek() == '%' && peekNext() == '{') {
            hasInterp = true;
            advance(); // consume %
            advance(); // consume {

            TokenType partType = firstInterp ? TokenType::INTERP_START : TokenType::INTERP_MID;
            tokens.push_back({partType, value, line, start - lineStart + 1});
            value.clear();
            firstInterp = false;

            // Collect the expression source between %{ and the matching }
            // Track strings and comments so braces inside them don't confuse the count
            std::string expr;
            int depth = 1;
            char stringQuote = 0;
            bool inLineComment = false;
            bool inBlockComment = false;
            while (depth > 0 && !isAtEnd()) {
                char ch = peek();
                char next = peekNext();

                if (inLineComment) {
                    if (ch == '\n') { inLineComment = false; line++; lineStart = current + 1; }
                    expr += advance();
                    continue;
                }
                if (inBlockComment) {
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    if (ch == '*' && next == '/') { inBlockComment = false; expr += advance(); }
                    expr += advance();
                    continue;
                }
                if (stringQuote) {
                    if (ch == '\\') { expr += advance(); if (!isAtEnd()) expr += advance(); continue; }
                    if (ch == stringQuote) stringQuote = 0;
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    expr += advance();
                    continue;
                }

                if (ch == '/' && next == '/') { inLineComment = true; expr += advance(); expr += advance(); continue; }
                if (ch == '/' && next == '*') { inBlockComment = true; expr += advance(); expr += advance(); continue; }
                // Catch the most common user error: writing `\"` inside
                // `%{...}` thinking the outer string needs the escape.
                // The interpolation collector is brace-balanced, so
                // quotes inside are tracked correctly without escaping.
                // The user's `\"` would either (a) silently change the
                // expression semantics or (b) blow up further down with
                // an unhelpful "Unterminated string" because the inner
                // pair-handling interacts badly with the outer string's
                // closing quote — point them at the simple fix loudly
                // and early.
                if (ch == '\\' && (next == '"' || next == '\'')) {
                    error("Don't escape quotes inside %{...} — the interpolation "
                          "is brace-balanced, so quotes work unescaped. "
                          "Write `\\\"text\\\"` as just `\"text\"` (drop the backslashes).");
                    return;
                }
                if (ch == '"' || ch == '\'') { stringQuote = ch; expr += advance(); continue; }

                if (ch == '{') depth++;
                if (ch == '}') depth--;
                if (depth > 0) {
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    expr += advance();
                } else {
                    advance(); // consume closing }
                }
            }

            // Lex the expression with a sub-lexer
            Lexer subLexer(expr);
            auto exprTokens = subLexer.tokenize();
            for (auto& t : exprTokens) {
                if (t.type != TokenType::EOF_TOKEN) {
                    t.line = line;
                    tokens.push_back(t);
                }
            }
            if (subLexer.hasError()) hadError = true;

            continue;
        }

        // Escape sequences
        if (peek() == '\\') {
            advance(); // consume backslash
            if (isAtEnd()) {
                error("Unterminated escape sequence");
                return;
            }
            char escaped = advance();
            switch (escaped) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '0':  value += '\0'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '%':  value += '%';  break;
                case 'x': {
                    // \xHH — hex escape
                    if (current + 2 > static_cast<int>(source.size())) {
                        error("Incomplete \\x escape"); break;
                    }
                    auto hexVal = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = hexVal(peek()), lo = hexVal(peekNext());
                    if (hi < 0 || lo < 0) { error("Invalid \\x escape"); break; }
                    value += static_cast<char>((hi << 4) | lo);
                    advance(); advance();
                    break;
                }
                case 'u': {
                    // \u{HHHH} — Unicode escape (1-6 hex digits)
                    if (isAtEnd() || peek() != '{') {
                        error("Expected '{' after \\u"); break;
                    }
                    advance(); // consume '{'
                    auto hexVal = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int32_t cp = 0;
                    int digits = 0;
                    bool bad = false;
                    while (!isAtEnd() && peek() != '}') {
                        int d = hexVal(peek());
                        if (d < 0) { error("Invalid hex digit in \\u{} escape"); bad = true; break; }
                        cp = (cp << 4) | d;
                        digits++;
                        if (digits > 6) { error("Too many digits in \\u{} escape"); bad = true; break; }
                        advance();
                    }
                    if (bad) break;
                    if (isAtEnd() || peek() != '}') { error("Unterminated \\u{} escape"); break; }
                    advance(); // consume '}'
                    if (digits == 0) { error("Empty \\u{} escape"); break; }
                    if (cp > 0x10FFFF) { error("Unicode codepoint out of range"); break; }
                    value += utf8_encode(cp);
                    break;
                }
                default:
                    error(std::string("Unknown escape sequence '\\") + escaped + "'");
                    value += escaped;
                    break;
            }
            continue;
        }

        value += advance();
    }

    if (isAtEnd()) {
        error("Unterminated string");
        return;
    }

    advance(); // consume closing quote

    if (hasInterp) {
        tokens.push_back({TokenType::INTERP_END, value, line, start - lineStart + 1});
    } else {
        tokens.push_back({TokenType::STRING, value, line, start - lineStart + 1});
    }
}

void Lexer::tripleString(char quote) {
    // Skip the first newline after opening triple-quote
    if (peek() == '\n') { advance(); line++; lineStart = current; }

    std::string value;
    bool hasInterp = false;
    bool firstInterp = true;

    while (!isAtEnd()) {
        // Check for closing triple-quote
        if (peek() == quote && peekNext() == quote &&
            current + 2 < static_cast<int>(source.size()) && source[current + 2] == quote) {
            advance(); advance(); advance(); // consume closing triple
            break;
        }

        if (peek() == '\n') { line++; lineStart = current + 1; }

        // String interpolation
        if (peek() == '%' && peekNext() == '{') {
            hasInterp = true;
            advance(); advance();
            TokenType partType = firstInterp ? TokenType::INTERP_START : TokenType::INTERP_MID;
            tokens.push_back({partType, value, line, start - lineStart + 1});
            value.clear();
            firstInterp = false;

            // Collect the expression between %{ and matching }.
            // Track strings and comments so braces inside them don't confuse the count.
            std::string expr;
            int depth = 1;
            char stringQuote = 0;
            bool inLineComment = false;
            bool inBlockComment = false;
            while (depth > 0 && !isAtEnd()) {
                char ch = peek();
                char next = peekNext();

                if (inLineComment) {
                    if (ch == '\n') { inLineComment = false; line++; lineStart = current + 1; }
                    expr += advance();
                    continue;
                }
                if (inBlockComment) {
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    if (ch == '*' && next == '/') { inBlockComment = false; expr += advance(); }
                    expr += advance();
                    continue;
                }
                if (stringQuote) {
                    if (ch == '\\') { expr += advance(); if (!isAtEnd()) expr += advance(); continue; }
                    if (ch == stringQuote) stringQuote = 0;
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    expr += advance();
                    continue;
                }

                if (ch == '/' && next == '/') { inLineComment = true; expr += advance(); expr += advance(); continue; }
                if (ch == '/' && next == '*') { inBlockComment = true; expr += advance(); expr += advance(); continue; }
                // Same friendly error as the regular-string collector
                // — see the parallel block above for the rationale.
                if (ch == '\\' && (next == '"' || next == '\'')) {
                    error("Don't escape quotes inside %{...} — the interpolation "
                          "is brace-balanced, so quotes work unescaped. "
                          "Write `\\\"text\\\"` as just `\"text\"` (drop the backslashes).");
                    return;
                }
                if (ch == '"' || ch == '\'') { stringQuote = ch; expr += advance(); continue; }

                if (ch == '{') depth++;
                if (ch == '}') depth--;
                if (depth > 0) {
                    if (ch == '\n') { line++; lineStart = current + 1; }
                    expr += advance();
                } else {
                    advance(); // consume closing }
                }
            }

            Lexer subLexer(expr);
            auto exprTokens = subLexer.tokenize();
            for (auto& t : exprTokens) {
                if (t.type != TokenType::EOF_TOKEN) {
                    t.line = line;
                    tokens.push_back(t);
                }
            }
            if (subLexer.hasError()) hadError = true;
            continue;
        }

        // Escape sequences
        if (peek() == '\\') {
            advance();
            if (isAtEnd()) { error("Unterminated escape sequence"); return; }
            char escaped = advance();
            switch (escaped) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '0':  value += '\0'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '%':  value += '%';  break;
                case 'x': {
                    if (current + 2 > static_cast<int>(source.size())) {
                        error("Incomplete \\x escape"); break;
                    }
                    auto hexVal = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int hi = hexVal(peek()), lo = hexVal(peekNext());
                    if (hi < 0 || lo < 0) { error("Invalid \\x escape"); break; }
                    value += static_cast<char>((hi << 4) | lo);
                    advance(); advance();
                    break;
                }
                case 'u': {
                    if (isAtEnd() || peek() != '{') {
                        error("Expected '{' after \\u"); break;
                    }
                    advance();
                    auto hexVal = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    int32_t cp = 0;
                    int digits = 0;
                    bool bad = false;
                    while (!isAtEnd() && peek() != '}') {
                        int d = hexVal(peek());
                        if (d < 0) { error("Invalid hex digit in \\u{} escape"); bad = true; break; }
                        cp = (cp << 4) | d;
                        digits++;
                        if (digits > 6) { error("Too many digits in \\u{} escape"); bad = true; break; }
                        advance();
                    }
                    if (bad) break;
                    if (isAtEnd() || peek() != '}') { error("Unterminated \\u{} escape"); break; }
                    advance();
                    if (digits == 0) { error("Empty \\u{} escape"); break; }
                    if (cp > 0x10FFFF) { error("Unicode codepoint out of range"); break; }
                    value += utf8_encode(cp);
                    break;
                }
                default: value += escaped; break;
            }
            continue;
        }

        value += advance();
    }

    if (hasInterp) {
        tokens.push_back({TokenType::INTERP_END, value, line, start - lineStart + 1});
    } else {
        tokens.push_back({TokenType::STRING, value, line, start - lineStart + 1});
    }
}

void Lexer::identifier() {
    while (std::isalnum(peek()) || peek() == '_') advance();

    std::string text = source.substr(start, current - start);
    auto it = keywords.find(text);
    TokenType type = (it != keywords.end()) ? it->second : TokenType::IDENTIFIER;
    addToken(type);
}

void Lexer::blockComment() {
    int depth = 1;
    while (depth > 0 && !isAtEnd()) {
        if (peek() == '/' && peekNext() == '*') {
            advance(); advance();
            depth++;
        } else if (peek() == '*' && peekNext() == '/') {
            advance(); advance();
            depth--;
        } else {
            if (peek() == '\n') { line++; lineStart = current + 1; }
            advance();
        }
    }

    if (depth > 0) {
        error("Unterminated block comment");
    }
}

// --- Helpers ---

void Lexer::addToken(TokenType type) {
    int col = start - lineStart + 1;
    tokens.push_back({type, source.substr(start, current - start), line, col});
}

void Lexer::addToken(TokenType type, const std::string& lexeme) {
    int col = start - lineStart + 1;
    tokens.push_back({type, lexeme, line, col});
}

void Lexer::error(const std::string& message) {
    int col = current - lineStart;
    if (col > 0)
        std::cerr << "[line " << line << ":col " << col << "] Error: " << message << std::endl;
    else
        std::cerr << "[line " << line << "] Error: " << message << std::endl;
    hadError = true;
}

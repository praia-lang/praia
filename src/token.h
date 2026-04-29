#pragma once

#include <iostream>
#include <string>

enum class TokenType {
    // Literals
    NUMBER,
    STRING,

    // String interpolation: "%{expr} text %{expr} text"
    // becomes: INTERP_START, <expr tokens>, INTERP_MID, <expr tokens>, INTERP_END
    INTERP_START,
    INTERP_MID,
    INTERP_END,

    // Identifier
    IDENTIFIER,

    // Keywords
    LET,
    FUNC,
    IF,
    ELSE,
    ELIF,
    MATCH,
    STATIC,
    WHILE,
    FOR,
    IN,
    IS,
    RETURN,
    BREAK,
    CONTINUE,
    TRY,
    CATCH,
    THROW,
    FINALLY,
    ENSURE,
    DEFER,
    ASYNC,
    AWAIT,
    YIELD,
    ENUM,
    CLASS,
    EXTENDS,
    THIS,
    SUPER,
    LAM,
    USE,
    EXPORT,
    TRUE,
    FALSE,
    NIL,

    // Operators
    PLUS,       // +
    MINUS,      // -
    STAR,       // *
    SLASH,      // /
    PERCENT,    // %
    ASSIGN,     // =
    PLUS_ASSIGN,  // +=
    MINUS_ASSIGN, // -=
    STAR_ASSIGN,  // *=
    SLASH_ASSIGN, // /=
    PERCENT_ASSIGN, // %=
    EQ,         // ==
    NEQ,        // !=
    LT,         // <
    GT,         // >
    LTE,        // <=
    GTE,        // >=
    NOT,        // !
    INCREMENT,  // ++
    DECREMENT,  // --
    AND,        // &&
    OR,         // ||
    BIT_AND,    // &
    BIT_OR,     // | (single pipe, distinct from |>)
    BIT_XOR,    // ^
    BIT_NOT,    // ~
    SHL,        // <<
    SHR,        // >>
    DOT,        // .
    DOT_DOT,    // ..
    SPREAD,     // ...
    PIPE,       // |>
    PIPE_TRY,   // |?>
    COLON,      // :
    QUESTION,       // ?
    QUESTION_DOT,   // ?.
    NIL_COALESCE,   // ??
    AT,         // @

    // Delimiters
    LPAREN,     // (
    RPAREN,     // )
    LBRACE,     // {
    RBRACE,     // }
    LBRACKET,   // [
    RBRACKET,   // ]
    COMMA,      // ,

    // Special
    EOF_TOKEN,
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column = 0;
};

inline std::string tokenTypeToString(TokenType type) {
    switch (type) {
        case TokenType::NUMBER:      return "NUMBER";
        case TokenType::STRING:      return "STRING";
        case TokenType::INTERP_START: return "INTERP_START";
        case TokenType::INTERP_MID:  return "INTERP_MID";
        case TokenType::INTERP_END:  return "INTERP_END";
        case TokenType::IDENTIFIER:  return "IDENTIFIER";
        case TokenType::LET:         return "LET";
        case TokenType::FUNC:        return "FUNC";
        case TokenType::IF:          return "IF";
        case TokenType::ELSE:        return "ELSE";
        case TokenType::ELIF:        return "ELIF";
        case TokenType::MATCH:       return "MATCH";
        case TokenType::STATIC:      return "STATIC";
        case TokenType::WHILE:       return "WHILE";
        case TokenType::FOR:         return "FOR";
        case TokenType::IN:          return "IN";
        case TokenType::RETURN:      return "RETURN";
        case TokenType::BREAK:       return "BREAK";
        case TokenType::CONTINUE:    return "CONTINUE";
        case TokenType::TRY:         return "TRY";
        case TokenType::CATCH:       return "CATCH";
        case TokenType::THROW:       return "THROW";
        case TokenType::IS:          return "IS";
        case TokenType::FINALLY:     return "FINALLY";
        case TokenType::ENSURE:      return "ENSURE";
        case TokenType::DEFER:       return "DEFER";
        case TokenType::ASYNC:       return "ASYNC";
        case TokenType::AWAIT:       return "AWAIT";
        case TokenType::YIELD:       return "YIELD";
        case TokenType::ENUM:        return "ENUM";
        case TokenType::CLASS:       return "CLASS";
        case TokenType::EXTENDS:     return "EXTENDS";
        case TokenType::THIS:        return "THIS";
        case TokenType::SUPER:       return "SUPER";
        case TokenType::LAM:         return "LAM";
        case TokenType::USE:         return "USE";
        case TokenType::EXPORT:      return "EXPORT";
        case TokenType::TRUE:        return "TRUE";
        case TokenType::FALSE:       return "FALSE";
        case TokenType::NIL:         return "NIL";
        case TokenType::PLUS:        return "PLUS";
        case TokenType::MINUS:       return "MINUS";
        case TokenType::STAR:        return "STAR";
        case TokenType::SLASH:       return "SLASH";
        case TokenType::PERCENT:     return "PERCENT";
        case TokenType::ASSIGN:      return "ASSIGN";
        case TokenType::PLUS_ASSIGN: return "PLUS_ASSIGN";
        case TokenType::MINUS_ASSIGN:return "MINUS_ASSIGN";
        case TokenType::STAR_ASSIGN: return "STAR_ASSIGN";
        case TokenType::SLASH_ASSIGN:return "SLASH_ASSIGN";
        case TokenType::PERCENT_ASSIGN:return "PERCENT_ASSIGN";
        case TokenType::EQ:          return "EQ";
        case TokenType::NEQ:         return "NEQ";
        case TokenType::LT:          return "LT";
        case TokenType::GT:          return "GT";
        case TokenType::LTE:         return "LTE";
        case TokenType::GTE:         return "GTE";
        case TokenType::NOT:         return "NOT";
        case TokenType::INCREMENT:   return "INCREMENT";
        case TokenType::DECREMENT:   return "DECREMENT";
        case TokenType::AND:         return "AND";
        case TokenType::OR:          return "OR";
        case TokenType::BIT_AND:     return "BIT_AND";
        case TokenType::BIT_OR:      return "BIT_OR";
        case TokenType::BIT_XOR:     return "BIT_XOR";
        case TokenType::BIT_NOT:     return "BIT_NOT";
        case TokenType::SHL:         return "SHL";
        case TokenType::SHR:         return "SHR";
        case TokenType::DOT:         return "DOT";
        case TokenType::DOT_DOT:     return "DOT_DOT";
        case TokenType::SPREAD:      return "SPREAD";
        case TokenType::PIPE:        return "PIPE";
        case TokenType::PIPE_TRY:    return "PIPE_TRY";
        case TokenType::COLON:       return "COLON";
        case TokenType::QUESTION:    return "QUESTION";
        case TokenType::QUESTION_DOT:return "QUESTION_DOT";
        case TokenType::NIL_COALESCE:return "NIL_COALESCE";
        case TokenType::AT:          return "AT";
        case TokenType::LPAREN:      return "LPAREN";
        case TokenType::RPAREN:      return "RPAREN";
        case TokenType::LBRACE:      return "LBRACE";
        case TokenType::RBRACE:      return "RBRACE";
        case TokenType::LBRACKET:    return "LBRACKET";
        case TokenType::RBRACKET:    return "RBRACKET";
        case TokenType::COMMA:       return "COMMA";
        case TokenType::EOF_TOKEN:   return "EOF";
    }
    return "UNKNOWN";
}

inline std::ostream& operator<<(std::ostream& os, const Token& token) {
    os << "[" << tokenTypeToString(token.type);
    if (!token.lexeme.empty()) {
        os << " \"" << token.lexeme << "\"";
    }
    os << " line:" << token.line << "]";
    return os;
}

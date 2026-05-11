#pragma once

#include "ast.h"
#include "token.h"
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::vector<StmtPtr> parse();
    bool hasError() const { return hadError; }

private:
    struct ParseError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Statements
    StmtPtr statement();
    StmtPtr letStatement();
    StmtPtr funcStatement();
    StmtPtr classStatement();
    StmtPtr enumStatement();
    StmtPtr ifStatement();
    StmtPtr matchStatement();
    StmtPtr whileStatement();
    StmtPtr forStatement();
    StmtPtr returnStatement();
    StmtPtr breakStatement();
    StmtPtr continueStatement();
    StmtPtr throwStatement();
    StmtPtr tryCatchStatement();
    StmtPtr ensureStatement();
    StmtPtr deferStatement();
    StmtPtr useStatement();
    StmtPtr exportStatement();
    StmtPtr expressionStatement();
    StmtPtr block();

    // Expressions (ordered low → high precedence)
    ExprPtr expression();
    ExprPtr assignment();
    ExprPtr pipe();
    ExprPtr ternary();
    ExprPtr nilCoalesce();
    ExprPtr logicOr();
    ExprPtr logicAnd();
    ExprPtr bitOr();
    ExprPtr bitXor();
    ExprPtr bitAnd();
    ExprPtr equality();
    ExprPtr comparison();
    ExprPtr shift();
    ExprPtr addition();
    ExprPtr multiplication();
    ExprPtr unary();
    ExprPtr postfix();
    ExprPtr call();
    ExprPtr primary();
    ExprPtr interpolatedString();

    // Helpers
    Token advance();
    Token peek() const;
    Token previous() const;
    bool check(TokenType type) const;
    bool match(TokenType type);
    Token consume(TokenType type, const std::string& message);
    bool isAtEnd() const;
    bool isNameToken(TokenType type) const;
    ParseError error(const Token& token, const std::string& message);
    void synchronize();
    void consumeStatementSeparator();

    std::vector<Token> tokens;
    int current = 0;
    bool hadError = false;
    int loopDepth = 0;
    int functionDepth = 0;
    int yieldCount = 0; // tracks yield expressions in current function body
    std::deque<StmtPtr> pending_; // for decorator desugaring (extra stmts to emit)
};

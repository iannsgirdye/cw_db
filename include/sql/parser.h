#pragma once

#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

#include "sql/ast.h"

namespace cw_db {

// Exceptions raised by Parser when the input does not pass lexical or
// syntactic validation. Caught uniformly by executor::run_query and turned
// into ExecutionResult::error.
class LexError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thin wrapper around the bison-generated parser (yyparse) and the
// flex-generated lexer (yylex), exposing the same one-statement-at-a-time
// API the rest of the codebase already depends on.
//
// One Parser instance corresponds to exactly one SQL statement -- the upper
// layers (StreamSplitter in main.cpp, line-framed server protocol in
// server.cpp, /query handler in HTTP server) take care of slicing the input
// at the top-level ';' boundary before handing each fragment to the parser.
class Parser {
public:
    explicit Parser(std::string source) : source_(std::move(source)) {}

    Statement parse_statement();

    // True when source is empty / whitespace-only or the single statement
    // has already been consumed.
    bool eof() const noexcept {
        if (done_) return true;
        for (char c : source_) {
            if (!std::isspace(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

private:
    std::string source_;
    bool        done_ = false;
};

}  // namespace cw_db

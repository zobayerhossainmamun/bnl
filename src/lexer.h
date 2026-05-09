#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "diagnostic.h"
#include "token.h"

namespace bnl {

class Lexer {
public:
    explicit Lexer(std::string_view source);

    std::vector<Token> tokenize();

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    bool has_errors() const { return !diagnostics_.empty(); }

private:
    void skip_trivia();
    void scan_token();

    void scan_identifier_or_keyword();
    void scan_number();
    void scan_string();

    bool at_end() const { return cursor_ >= source_.size(); }

    // Single-byte advance / peek (for ASCII-fast-path tokens).
    char peek(std::size_t offset = 0) const;
    char advance();
    bool match(char expected);

    // UTF-8 codepoint decode, advances cursor + column on success.
    std::uint32_t decode_codepoint();

    void emit(TokenType type);
    void emit(TokenType type, std::size_t start_offset);
    void error(const std::string& message);

    std::string_view source_;
    std::size_t cursor_     = 0;
    std::size_t token_start_ = 0;

    std::size_t line_         = 1;
    std::size_t column_       = 1;
    std::size_t token_line_   = 1;
    std::size_t token_column_ = 1;

    std::vector<Token>      tokens_;
    std::vector<Diagnostic> diagnostics_;
};

}  // namespace bnl

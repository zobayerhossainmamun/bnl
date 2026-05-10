#pragma once

#include <cstddef>
#include <string_view>

namespace bnl {

enum class TokenType {
    // Literals
    Number,
    String,
    Identifier,
    True,
    False,
    Null,

    // Keywords
    If,
    Else,
    While,
    For,
    Function,
    Return,
    Var,
    Class,
    Extends,
    Super,
    Import,
    As,
    And,
    Or,
    Not,
    Try,
    Catch,
    Throw,
    Finally,
    Of,

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Dot,
    Semicolon,
    Colon,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Assign,
    Bang,
    Lt,
    Gt,
    EqEq,
    BangEq,
    LtEq,
    GtEq,
    AmpAmp,
    PipePipe,

    // Special
    EndOfFile,
    Error,
};

const char* token_type_name(TokenType type);

struct Token {
    TokenType type;
    std::string_view lexeme;
    std::size_t line;
    std::size_t column;
};

}  // namespace bnl

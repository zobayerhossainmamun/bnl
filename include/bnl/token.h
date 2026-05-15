#pragma once

#include <cstddef>
#include <string_view>

namespace bnl {

enum class TokenType {
    // Literals
    Number,
    String,
    TemplateString,  // "...${expr}...": lexeme keeps the full source slice
                     // (open quote .. close quote) and the parser rebuilds
                     // it into a `+` chain of segments + interpolated exprs.
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
    Switch,
    Case,
    Default,
    Break,
    Continue,
    Wait,

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

    // Compound assignment: x += y desugars to x = x + y at parse time
    // (single-eval handled in the interpreter via CompoundAssignExpr).
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,

    // Postfix / prefix increment / decrement. Both forms desugar to
    // CompoundAssignExpr(target, Plus/Minus, 1) and yield the new value.
    PlusPlus,
    MinusMinus,

    // Null-coalescing: a ?? b returns a if non-null, else b.
    QuestionQuestion,

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

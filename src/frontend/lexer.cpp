#include "frontend/lexer.h"

#include <cctype>
#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "bnl/token.h"

namespace bnl {

namespace {

// ---------- keyword table -----------------------------------------------------
// Both Bangla and English forms map to the same TokenType so that the parser
// treats them identically. Add or rename entries here as the language evolves.

const std::unordered_map<std::string_view, TokenType>& keyword_table() {
    static const std::unordered_map<std::string_view, TokenType> table = {
        // Control flow
        {"if",        TokenType::If},      {"\xe0\xa6\xaf\xe0\xa6\xa6\xe0\xa6\xbf",                                                         TokenType::If},        // যদি
        {"else",      TokenType::Else},    {"\xe0\xa6\xa8\xe0\xa6\xbe\xe0\xa6\xb9\xe0\xa6\xb2\xe0\xa7\x87",                                 TokenType::Else},      // নাহলে
        {"while",     TokenType::While},   {"\xe0\xa6\xaf\xe0\xa6\xa4\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7\xe0\xa6\xa3",                     TokenType::While},     // যতক্ষণ
        {"for",       TokenType::For},     {"\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa6\xa4\xe0\xa6\xbf",                                 TokenType::For},       // প্রতি

        // Definitions
        {"function",  TokenType::Function},{"\xe0\xa6\xab\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb6\xe0\xa6\xa8",                                 TokenType::Function},  // ফাংশন
        {"return",    TokenType::Return},  {"\xe0\xa6\xab\xe0\xa7\x87\xe0\xa6\xb0\xe0\xa6\xa4",                     TokenType::Return},    // ফেরত
        {"var",       TokenType::Var},     {"\xe0\xa6\x9a\xe0\xa6\xb2\xe0\xa6\x95",                                 TokenType::Var},       // চলক
        {"\xe0\xa6\xa7\xe0\xa6\xb0\xe0\xa6\xbf",                                 TokenType::Var},                                                                       // ধরি
        {"class",     TokenType::Class},   {"\xe0\xa6\xb6\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa7\x87\xe0\xa6\xa3\xe0\xa7\x80", TokenType::Class}, // শ্রেণী
        {"extends",   TokenType::Extends}, {"\xe0\xa6\xaa\xe0\xa7\x8d\xe0\xa6\xb0\xe0\xa6\xb8\xe0\xa6\xbe\xe0\xa6\xb0\xe0\xa6\xbf\xe0\xa6\xa4", TokenType::Extends}, // প্রসারিত
        {"super",     TokenType::Super},   {"\xe0\xa6\x89\xe0\xa6\xaa\xe0\xa6\xb0\xe0\xa7\x87\xe0\xa6\xb0",                                       TokenType::Super},   // উপরের
        {"import",    TokenType::Import},  {"\xe0\xa6\x86\xe0\xa6\xae\xe0\xa6\xa6\xe0\xa6\xbe\xe0\xa6\xa8\xe0\xa6\xbf",                           TokenType::Import},  // আমদানি
        {"as",        TokenType::As},      {"\xe0\xa6\xaf\xe0\xa7\x87\xe0\xa6\xae\xe0\xa6\xa8",                                                   TokenType::As},      // যেমন

        // Logical / boolean
        {"and",       TokenType::And},     {"\xe0\xa6\x8f\xe0\xa6\xac\xe0\xa6\x82",                                 TokenType::And},       // এবং
        {"or",        TokenType::Or},      {"\xe0\xa6\x85\xe0\xa6\xa5\xe0\xa6\xac\xe0\xa6\xbe",                     TokenType::Or},        // অথবা
        {"not",       TokenType::Not},     {"\xe0\xa6\xa8\xe0\xa6\xbe",                                             TokenType::Not},       // না
        {"true",      TokenType::True},    {"\xe0\xa6\xb8\xe0\xa6\xa4\xe0\xa7\x8d\xe0\xa6\xaf",                     TokenType::True},      // সত্য
        {"false",     TokenType::False},   {"\xe0\xa6\xae\xe0\xa6\xbf\xe0\xa6\xa5\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa6\xbe", TokenType::False}, // মিথ্যা
        {"null",      TokenType::Null},    {"\xe0\xa6\xa8\xe0\xa6\xbe\xe0\xa6\x87",                                                         TokenType::Null},      // নাই
        {"\xe0\xa6\xa8\xe0\xa6\xbe\xe0\xa6\xb2",                                 TokenType::Null},                                                                      // নাল

        // for-of iteration
        {"of",        TokenType::Of},      {"\xe0\xa6\x8f\xe0\xa6\xb0",                                                                     TokenType::Of},        // এর

        // Error handling
        {"try",       TokenType::Try},     {"\xe0\xa6\x9a\xe0\xa7\x87\xe0\xa6\xb7\xe0\xa7\x8d\xe0\xa6\x9f\xe0\xa6\xbe",                     TokenType::Try},       // চেষ্টা
        {"catch",     TokenType::Catch},   {"\xe0\xa6\xa7\xe0\xa6\xb0\xe0\xa7\x81\xe0\xa6\xa8",                                             TokenType::Catch},     // ধরুন
        {"throw",     TokenType::Throw},   {"\xe0\xa6\xa8\xe0\xa6\xbf\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7\xe0\xa7\x87\xe0\xa6\xaa",         TokenType::Throw},     // নিক্ষেপ
        {"finally",   TokenType::Finally}, {"\xe0\xa6\x85\xe0\xa6\xac\xe0\xa6\xb6\xe0\xa7\x87\xe0\xa6\xb7\xe0\xa7\x87",                     TokenType::Finally},   // অবশেষে

        // Switch / multi-way branching
        {"switch",    TokenType::Switch},  {"\xe0\xa6\xac\xe0\xa6\xbf\xe0\xa6\x95\xe0\xa6\xb2\xe0\xa7\x8d\xe0\xa6\xaa",                     TokenType::Switch},    // বিকল্প
        {"case",      TokenType::Case},    {"\xe0\xa6\x85\xe0\xa6\xac\xe0\xa6\xb8\xe0\xa7\x8d\xe0\xa6\xa5\xe0\xa6\xbe",                     TokenType::Case},      // অবস্থা
        {"default",   TokenType::Default}, {"\xe0\xa6\x85\xe0\xa6\xa8\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa6\xa5\xe0\xa6\xbe\xe0\xa7\x9f",                 TokenType::Default},   // অন্যথায় (NFC: য় = U+09DF)
        {"\xe0\xa6\x85\xe0\xa6\xa8\xe0\xa7\x8d\xe0\xa6\xaf\xe0\xa6\xa5\xe0\xa6\xbe\xe0\xa6\xaf\xe0\xa6\xbc", TokenType::Default},   // অন্যথায় (NFD: য + ◌়)

        // Loop / switch flow
        {"break",     TokenType::Break},   {"\xe0\xa6\xa5\xe0\xa6\xbe\xe0\xa6\xae\xe0\xa7\x81\xe0\xa6\xa8",                                 TokenType::Break},     // থামুন
        {"continue",  TokenType::Continue},{"\xe0\xa6\x9a\xe0\xa6\xb2\xe0\xa7\x81\xe0\xa6\xa8",                                             TokenType::Continue},  // চলুন

        // Async — suspends current frame until the Future resolves
        {"wait",      TokenType::Wait},    {"\xe0\xa6\x85\xe0\xa6\xaa\xe0\xa7\x87\xe0\xa6\x95\xe0\xa7\x8d\xe0\xa6\xb7\xe0\xa6\xbe",         TokenType::Wait},      // অপেক্ষা

        // Note: `print` / `লিখুন` are deliberately NOT keywords. They are ordinary
        // identifiers that the runtime registers as builtin functions in the
        // global scope (with both Bangla and English names aliased to one impl).
    };
    return table;
}

// ---------- utf-8 / identifier helpers ----------------------------------------

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Permissive identifier rules for MVP: ASCII letters/underscore are
// identifier-start; ASCII digits join only after the first character; any
// non-ASCII codepoint is treated as both identifier-start and continue. Real
// Unicode XID_Start / XID_Continue can replace this once we pull in ICU.

bool is_identifier_start(std::uint32_t cp) {
    if (cp < 0x80) {
        return is_ascii_alpha(static_cast<char>(cp)) || cp == '_';
    }
    return true;
}

bool is_identifier_continue(std::uint32_t cp) {
    if (cp < 0x80) {
        return is_ascii_alpha(static_cast<char>(cp)) || is_ascii_digit(static_cast<char>(cp)) || cp == '_';
    }
    return true;
}

// Returns the byte-length of the UTF-8 sequence whose lead byte is `b`,
// or 0 if `b` is not a valid lead byte.
std::size_t utf8_lead_length(unsigned char b) {
    if (b < 0x80) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0;
}

}  // namespace

// ---------- Lexer -------------------------------------------------------------

Lexer::Lexer(std::string_view source) : source_(source) {
    // Skip a UTF-8 BOM if present.
    if (source_.size() >= 3 &&
        static_cast<unsigned char>(source_[0]) == 0xEF &&
        static_cast<unsigned char>(source_[1]) == 0xBB &&
        static_cast<unsigned char>(source_[2]) == 0xBF) {
        cursor_ = 3;
    }
}

std::vector<Token> Lexer::tokenize() {
    while (!at_end()) {
        skip_trivia();
        if (at_end()) break;

        token_start_  = cursor_;
        token_line_   = line_;
        token_column_ = column_;

        scan_token();
    }

    token_start_  = cursor_;
    token_line_   = line_;
    token_column_ = column_;
    emit(TokenType::EndOfFile);
    return std::move(tokens_);
}

void Lexer::skip_trivia() {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            advance();
        } else if (c == '\n') {
            advance();
            line_++;
            column_ = 1;
        } else if (c == '/' && peek(1) == '/') {
            while (!at_end() && peek() != '\n') advance();
        } else if (c == '/' && peek(1) == '*') {
            advance(); advance();
            while (!at_end() && !(peek() == '*' && peek(1) == '/')) {
                if (peek() == '\n') { line_++; column_ = 0; }
                advance();
            }
            if (at_end()) {
                error("unterminated block comment");
                return;
            }
            advance(); advance();  // consume "*/"
        } else {
            break;
        }
    }
}

void Lexer::scan_token() {
    unsigned char lead = static_cast<unsigned char>(peek());

    // ASCII fast path for punctuation and operators.
    if (lead < 0x80) {
        char c = advance();
        switch (c) {
            case '(': emit(TokenType::LParen); return;
            case ')': emit(TokenType::RParen); return;
            case '{': emit(TokenType::LBrace); return;
            case '}': emit(TokenType::RBrace); return;
            case '[': emit(TokenType::LBracket); return;
            case ']': emit(TokenType::RBracket); return;
            case ',': emit(TokenType::Comma); return;
            case '.': emit(TokenType::Dot); return;
            case ';': emit(TokenType::Semicolon); return;
            case ':': emit(TokenType::Colon); return;
            case '+': emit(TokenType::Plus); return;
            case '-': emit(TokenType::Minus); return;
            case '*': emit(TokenType::Star); return;
            case '/': emit(TokenType::Slash); return;
            case '%': emit(TokenType::Percent); return;

            case '=': emit(match('=') ? TokenType::EqEq   : TokenType::Assign); return;
            case '!': emit(match('=') ? TokenType::BangEq : TokenType::Bang);   return;
            case '<': emit(match('=') ? TokenType::LtEq   : TokenType::Lt);     return;
            case '>': emit(match('=') ? TokenType::GtEq   : TokenType::Gt);     return;

            case '&':
                if (match('&')) { emit(TokenType::AmpAmp); return; }
                error("unexpected '&' (did you mean '&&'?)");
                emit(TokenType::Error);
                return;
            case '|':
                if (match('|')) { emit(TokenType::PipePipe); return; }
                error("unexpected '|' (did you mean '||'?)");
                emit(TokenType::Error);
                return;

            case '"':
            case '\'':
                scan_string(c);
                return;

            default:
                if (is_ascii_digit(c)) {
                    scan_number();
                    return;
                }
                if (is_identifier_start(static_cast<std::uint32_t>(c))) {
                    scan_identifier_or_keyword();
                    return;
                }
                error(std::string("unexpected character '") + c + "'");
                emit(TokenType::Error);
                return;
        }
    }

    // Non-ASCII: Bangla digit ০-৯ (U+09E6..U+09EF, UTF-8 E0 A7 A6..AF) starts
    // a numeric literal; anything else must be the start of a UTF-8 identifier.
    if (lead == 0xE0 && static_cast<unsigned char>(peek(1)) == 0xA7) {
        unsigned char b2 = static_cast<unsigned char>(peek(2));
        if (b2 >= 0xA6 && b2 <= 0xAF) {
            scan_number();
            return;
        }
    }

    std::size_t saved_cursor = cursor_;
    std::size_t saved_column = column_;
    std::uint32_t cp = decode_codepoint();
    if (cp == 0xFFFD) {
        error("invalid UTF-8 byte sequence");
        emit(TokenType::Error);
        return;
    }
    if (is_identifier_start(cp)) {
        // Roll back so scan_identifier_or_keyword re-consumes this codepoint
        // (and counts its column exactly once).
        cursor_ = saved_cursor;
        column_ = saved_column;
        scan_identifier_or_keyword();
        return;
    }

    error("unexpected character (codepoint U+" + std::to_string(cp) + ")");
    emit(TokenType::Error);
}

void Lexer::scan_identifier_or_keyword() {
    while (!at_end()) {
        unsigned char lead = static_cast<unsigned char>(peek());
        if (lead < 0x80) {
            if (!is_identifier_continue(static_cast<std::uint32_t>(lead))) break;
            advance();
        } else {
            std::size_t saved_cursor = cursor_;
            std::size_t saved_column = column_;
            std::uint32_t cp = decode_codepoint();
            if (cp == 0xFFFD || !is_identifier_continue(cp)) {
                cursor_ = saved_cursor;
                column_ = saved_column;
                break;
            }
        }
    }

    auto lexeme = source_.substr(token_start_, cursor_ - token_start_);
    const auto& table = keyword_table();
    auto it = table.find(lexeme);
    emit(it != table.end() ? it->second : TokenType::Identifier);
}

void Lexer::scan_number() {
    // Digit predicate that accepts either ASCII (0-9) or Bangla (০-৯ at
    // U+09E6..U+09EF, UTF-8 E0 A7 A6..AF) digits at a byte offset from cursor_.
    auto digit_at = [this](std::size_t off) -> bool {
        if (cursor_ + off >= source_.size()) return false;
        unsigned char b0 = static_cast<unsigned char>(source_[cursor_ + off]);
        if (b0 < 0x80) return is_ascii_digit(static_cast<char>(b0));
        if (b0 == 0xE0 && cursor_ + off + 2 < source_.size()
            && static_cast<unsigned char>(source_[cursor_ + off + 1]) == 0xA7) {
            unsigned char b2 = static_cast<unsigned char>(source_[cursor_ + off + 2]);
            return b2 >= 0xA6 && b2 <= 0xAF;
        }
        return false;
    };
    // Consumes one digit at cursor_; counts a Bangla digit as a single column.
    auto consume_digit = [this]() {
        unsigned char b0 = static_cast<unsigned char>(peek());
        if (b0 < 0x80) {
            advance();
        } else {
            cursor_ += 3;
            column_++;
        }
    };

    while (digit_at(0)) consume_digit();
    if (!at_end() && peek() == '.' && digit_at(1)) {
        advance();  // consume '.'
        while (digit_at(0)) consume_digit();
    }
    emit(TokenType::Number);
}

void Lexer::scan_string(char quote) {
    // The opening quote was already consumed by scan_token(). The token's
    // lexeme keeps that quote so decode_string_literal can verify the pair
    // and pick the matching escape ('\'' inside '...' vs. '\"' inside "...").
    while (!at_end() && peek() != quote) {
        char c = peek();
        if (c == '\n') {
            line_++;
            column_ = 0;
        }
        if (c == '\\' && !at_end()) {
            advance();           // consume backslash
            if (at_end()) break; // error caught below
        }
        advance();
    }

    if (at_end()) {
        error("unterminated string literal");
        emit(TokenType::Error);
        return;
    }

    advance();  // closing quote
    emit(TokenType::String);
}

// ---------- low-level cursor primitives --------------------------------------

char Lexer::peek(std::size_t offset) const {
    std::size_t i = cursor_ + offset;
    return i < source_.size() ? source_[i] : '\0';
}

char Lexer::advance() {
    char c = source_[cursor_++];
    column_++;
    return c;
}

bool Lexer::match(char expected) {
    if (at_end() || source_[cursor_] != expected) return false;
    advance();
    return true;
}

std::uint32_t Lexer::decode_codepoint() {
    if (at_end()) return 0xFFFD;
    unsigned char b0 = static_cast<unsigned char>(source_[cursor_]);
    std::size_t len = utf8_lead_length(b0);
    if (len == 0 || cursor_ + len > source_.size()) {
        cursor_++;
        column_++;
        return 0xFFFD;
    }

    std::uint32_t cp = 0;
    switch (len) {
        case 1: cp = b0; break;
        case 2: cp = b0 & 0x1F; break;
        case 3: cp = b0 & 0x0F; break;
        case 4: cp = b0 & 0x07; break;
    }
    for (std::size_t i = 1; i < len; ++i) {
        unsigned char b = static_cast<unsigned char>(source_[cursor_ + i]);
        if ((b & 0xC0) != 0x80) {
            cursor_++;
            column_++;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    cursor_ += len;
    column_++;
    return cp;
}

void Lexer::emit(TokenType type) {
    emit(type, token_start_);
}

void Lexer::emit(TokenType type, std::size_t start_offset) {
    Token tok{
        type,
        source_.substr(start_offset, cursor_ - start_offset),
        token_line_,
        token_column_,
    };
    tokens_.push_back(tok);
}

void Lexer::error(const std::string& message) {
    diagnostics_.push_back({token_line_, token_column_, message});
}

// ---------- token type names --------------------------------------------------

const char* token_type_name(TokenType type) {
    switch (type) {
        case TokenType::Number:     return "Number";
        case TokenType::String:     return "String";
        case TokenType::Identifier: return "Identifier";
        case TokenType::True:       return "True";
        case TokenType::False:      return "False";
        case TokenType::Null:       return "Null";
        case TokenType::If:         return "If";
        case TokenType::Else:       return "Else";
        case TokenType::While:      return "While";
        case TokenType::For:        return "For";
        case TokenType::Function:   return "Function";
        case TokenType::Return:     return "Return";
        case TokenType::Var:        return "Var";
        case TokenType::Class:      return "Class";
        case TokenType::Extends:    return "Extends";
        case TokenType::Super:      return "Super";
        case TokenType::Import:     return "Import";
        case TokenType::As:         return "As";
        case TokenType::And:        return "And";
        case TokenType::Or:         return "Or";
        case TokenType::Not:        return "Not";
        case TokenType::Try:        return "Try";
        case TokenType::Catch:      return "Catch";
        case TokenType::Throw:      return "Throw";
        case TokenType::Finally:    return "Finally";
        case TokenType::Of:         return "Of";
        case TokenType::Switch:     return "Switch";
        case TokenType::Case:       return "Case";
        case TokenType::Default:    return "Default";
        case TokenType::Break:      return "Break";
        case TokenType::Continue:   return "Continue";
        case TokenType::Wait:       return "Wait";
        case TokenType::LParen:     return "LParen";
        case TokenType::RParen:     return "RParen";
        case TokenType::LBrace:     return "LBrace";
        case TokenType::RBrace:     return "RBrace";
        case TokenType::LBracket:   return "LBracket";
        case TokenType::RBracket:   return "RBracket";
        case TokenType::Comma:      return "Comma";
        case TokenType::Dot:        return "Dot";
        case TokenType::Semicolon:  return "Semicolon";
        case TokenType::Colon:      return "Colon";
        case TokenType::Plus:       return "Plus";
        case TokenType::Minus:      return "Minus";
        case TokenType::Star:       return "Star";
        case TokenType::Slash:      return "Slash";
        case TokenType::Percent:    return "Percent";
        case TokenType::Assign:     return "Assign";
        case TokenType::Bang:       return "Bang";
        case TokenType::Lt:         return "Lt";
        case TokenType::Gt:         return "Gt";
        case TokenType::EqEq:       return "EqEq";
        case TokenType::BangEq:     return "BangEq";
        case TokenType::LtEq:       return "LtEq";
        case TokenType::GtEq:       return "GtEq";
        case TokenType::AmpAmp:     return "AmpAmp";
        case TokenType::PipePipe:   return "PipePipe";
        case TokenType::EndOfFile:  return "EndOfFile";
        case TokenType::Error:      return "Error";
    }
    return "?";
}

}  // namespace bnl

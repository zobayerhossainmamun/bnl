#include <fmt/core.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "bnl/interpreter.h"
#include "bnl/token.h"
#include "bnl/version.h"
#include "frontend/ast_printer.h"
#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "runtime/module_loader.h"   // bnl::ModuleError

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace {

void enable_utf8_console() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

void dump_tokens(const std::vector<bnl::Token>& tokens) {
    fmt::print("--- tokens ({}) ---\n", tokens.size());
    for (const auto& t : tokens) {
        fmt::print("{:>4}:{:<3}  {:<12}  '{}'\n",
                   t.line, t.column, bnl::token_type_name(t.type), t.lexeme);
    }
}

// Extract the n-th 1-based line from a source buffer, without the trailing newline.
std::string extract_line(const std::string& source, std::size_t line_no) {
    if (line_no == 0) return "";
    std::size_t start = 0;
    std::size_t cur   = 1;
    while (start < source.size() && cur < line_no) {
        if (source[start] == '\n') ++cur;
        ++start;
    }
    if (cur != line_no) return "";
    std::size_t end = source.find('\n', start);
    if (end == std::string::npos) end = source.size();
    if (end > 0 && source[end - 1] == '\r') --end;       // strip CR for CRLF files
    return source.substr(start, end - start);
}

// Count UTF-8 codepoints (1 per multibyte sequence). Used to position the
// caret at the END of a line for "unexpected end of input" errors.
std::size_t utf8_count(const std::string& s) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c < 0x80)         i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else                         i += 1;
        ++n;
    }
    return n;
}

// Build the spaces (with tabs preserved) so a caret aligns under target_col.
// Counts codepoints, not bytes — tracks UTF-8 multibyte sequences as 1 column.
std::string caret_padding(const std::string& line, std::size_t target_col) {
    std::string out;
    std::size_t col = 1;
    std::size_t i   = 0;
    while (i < line.size() && col < target_col) {
        unsigned char c = static_cast<unsigned char>(line[i]);
        if (c == '\t') {
            out += '\t';
            ++i;
        } else if (c < 0x80) {
            out += ' ';
            ++i;
        } else {
            std::size_t len = 1;
            if      ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            out += ' ';
            i   += len;
        }
        ++col;
    }
    return out;
}

void print_diagnostics(const char*                          phase,
                       const std::string&                   source,
                       const std::string&                   display_path,
                       const std::vector<bnl::Diagnostic>&  diags) {
    if (diags.empty()) return;
    const std::string where = display_path.empty() ? std::string("<eval>") : display_path;

    for (const auto& d : diags) {
        // header: <file>:<line>:<col>: <phase> error: <message>
        fmt::print(stderr, "{}:{}:{}: {} error: {}\n",
                   where, d.line, d.column, phase, d.message);

        // source line. If the diagnostic points at a line that doesn't exist
        // (typical for "end of input" errors that report the next line), fall
        // back to the last non-empty line and place the caret at its end.
        std::size_t  show_line = d.line;
        std::size_t  show_col  = d.column;
        std::string  line      = extract_line(source, show_line);
        if (line.empty() && show_line > 1) {
            show_line = show_line - 1;
            line      = extract_line(source, show_line);
            while (line.empty() && show_line > 1) {
                --show_line;
                line = extract_line(source, show_line);
            }
            show_col = utf8_count(line) + 1;
        }
        if (line.empty()) continue;

        std::string gutter = fmt::format("{:>4}", show_line);
        std::string blank (gutter.size(), ' ');

        fmt::print(stderr, "{} | {}\n", gutter, line);
        fmt::print(stderr, "{} | {}^\n", blank, caret_padding(line, show_col));
    }
}

// Convenience: format a single error using the same clang-style layout as
// the multi-diagnostic path.
void print_single_error(const char* phase,
                        const std::string& source,
                        const std::string& display_path,
                        std::size_t line, std::size_t column,
                        const std::string& message) {
    std::vector<bnl::Diagnostic> diags;
    diags.push_back({line, column, message});
    print_diagnostics(phase, source, display_path, diags);
}

void print_version() {
    fmt::print("Bnlang (bnl) {} ({})\n", bnl::kVersion, bnl::kPlatform);
}

// ---------- REPL -----------------------------------------------------------

// Returns true when `buf` looks like a complete top-level statement: every
// opener has a matching closer and the buffer ends with `;` or `}`. Inside
// strings + `//` comments, brackets are ignored.
bool is_complete_input(const std::string& buf) {
    int depth = 0;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        char c = buf[i];
        if (c == '"' || c == '\'') {
            char quote = c;
            ++i;
            while (i < buf.size() && buf[i] != quote) {
                if (buf[i] == '\\' && i + 1 < buf.size()) ++i;
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1 < buf.size() && buf[i + 1] == '/') {
            while (i < buf.size() && buf[i] != '\n') ++i;
            continue;
        }
        if      (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') --depth;
    }
    if (depth > 0) return false;

    // Last meaningful char (skip trailing whitespace) must be ; or }
    std::size_t i = buf.size();
    while (i > 0) {
        char c = buf[i - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { --i; continue; }
        return c == ';' || c == '}';
    }
    return false;  // empty buffer → wait for input
}

int run_repl() {
    fmt::print("Bnlang (bnl) {} ({})\n", bnl::kVersion, bnl::kPlatform);
    fmt::print("Type statements ending with `;` or `}}`. Ctrl-Z (Win) / Ctrl-D (Unix) to exit.\n\n");

    bnl::Interpreter interp;
    std::string      buffer;
    bool             continuation = false;

    // Token::lexeme is a string_view into the original source. AST nodes
    // (notably FunctionStmt's param list) reference these views; if we let
    // the buffer free while a function defined from it is still callable,
    // the views dangle. Stash every source forever for the REPL session.
    std::vector<std::string> kept_sources;

    while (true) {
        // fmt::print needs a compile-time format string — pick one branch.
        if (continuation) fmt::print("... ");
        else              fmt::print(">>> ");
        std::cout.flush();

        std::string line;
        if (!std::getline(std::cin, line)) {
            fmt::print("\n");
            break;                              // EOF
        }
        // An empty line at a continuation prompt force-submits whatever's
        // buffered (even if incomplete) so the user can recover from a bad
        // start without typing more.
        if (line.empty() && continuation && !buffer.empty()) {
            // Fall through and try to run as-is.
        } else {
            if (!buffer.empty()) buffer += '\n';
            buffer += line;
            if (!is_complete_input(buffer)) {
                continuation = true;
                continue;
            }
        }

        // Move the buffer into permanent storage and lex from there so
        // Token::lexeme string_views stay valid for the rest of the session.
        kept_sources.push_back(std::move(buffer));
        buffer.clear();              // moved-from is unspecified; reset for next iter
        const std::string& source = kept_sources.back();

        const std::string display_path = "<repl>";
        bnl::Lexer  lexer(source);
        auto        tokens = lexer.tokenize();
        if (lexer.has_errors()) {
            print_diagnostics("lex", source, display_path, lexer.diagnostics());
        } else {
            bnl::Parser parser(std::move(tokens));
            auto        program = parser.parse();
            if (parser.has_errors()) {
                print_diagnostics("parse", source, display_path, parser.diagnostics());
            } else {
                try {
                    interp.run(std::move(program));
                } catch (const bnl::RuntimeError& e) {
                    print_single_error("runtime", source, display_path,
                                       e.token.line, e.token.column, e.what());
                } catch (const bnl::ModuleError& e) {
                    print_single_error("module", source, display_path,
                                       e.token.line, e.token.column, e.what());
                } catch (const std::exception& e) {
                    fmt::print(stderr, "error: {}\n", e.what());
                }
            }
        }

        continuation = false;
    }
    return 0;
}

void print_usage() {
    fmt::print(
        "Bnlang (bnl) {} ({})\n"
        "\n"
        "Usage:\n"
        "  bnl                         start the interactive REPL\n"
        "  bnl <file.bnl> [args...]    run a script file\n"
        "  bnl -e '<code>' [args...]   run inline source\n"
        "\n"
        "Flags:\n"
        "  -v, --version               print version and exit\n"
        "  -h, --help                  print this help and exit\n"
        "      --tokens                dump the lexer's token stream\n"
        "      --ast                   dump the parsed AST\n",
        bnl::kVersion, bnl::kPlatform);
}

}  // namespace

int main(int argc, char** argv) {
    enable_utf8_console();

    bool        show_tokens     = false;
    bool        show_ast        = false;
    bool        has_inline_code = false;
    std::string path;
    std::string inline_code;
    std::vector<std::string> program_args;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (path.empty() && !has_inline_code) {
            if      (a == "-v" || a == "--version") { print_version(); return 0; }
            else if (a == "-h" || a == "--help")    { print_usage();   return 0; }
            else if (a == "--tokens")               show_tokens = true;
            else if (a == "--ast")                  show_ast    = true;
            else if (a == "--quiet")                /* legacy no-op — banner is gone */ ;
            else if (a == "-e" || a == "--eval") {
                if (++i >= argc) {
                    fmt::print(stderr, "error: {} requires a code argument\n", a);
                    return 1;
                }
                inline_code     = argv[i];
                has_inline_code = true;
            }
            else                                    path = std::string(a);
        } else {
            // Anything after the script path / -e code is forwarded as a script arg.
            program_args.emplace_back(a);
        }
    }

    if (path.empty() && !has_inline_code) {
        // No file, no -e — drop into the REPL.
        return run_repl();
    }

    try {
        std::string source = has_inline_code ? std::move(inline_code) : read_file(path);
        const std::string display_path = has_inline_code ? std::string("<eval>") : path;

        bnl::Lexer lexer(source);
        auto       tokens = lexer.tokenize();
        if (show_tokens) dump_tokens(tokens);
        print_diagnostics("lex", source, display_path, lexer.diagnostics());
        if (lexer.has_errors()) return 2;

        bnl::Parser parser(std::move(tokens));
        auto        program = parser.parse();
        print_diagnostics("parse", source, display_path, parser.diagnostics());
        if (parser.has_errors()) return 3;

        if (show_ast) {
            fmt::print("--- ast ({} top-level stmts) ---\n", program.size());
            fmt::print("{}\n", bnl::ast_to_string(program));
        }

        bnl::Interpreter interp;
        interp.set_program_args(std::move(program_args));
        std::filesystem::path entry = path.empty()
            ? std::filesystem::path{}
            : std::filesystem::weakly_canonical(std::filesystem::path(path));
        try {
            if (!interp.run(std::move(program), entry)) return 4;
        } catch (const bnl::RuntimeError& e) {
            print_single_error("runtime", source, display_path,
                               e.token.line, e.token.column, e.what());
            return 4;
        } catch (const bnl::ModuleError& e) {
            print_single_error("module", source, display_path,
                               e.token.line, e.token.column, e.what());
            return 4;
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "error: {}\n", e.what());
        return 1;
    }
    return 0;
}

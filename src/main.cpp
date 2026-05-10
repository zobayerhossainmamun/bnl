#include <fmt/core.h>

#include <filesystem>
#include <fstream>
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

constexpr std::string_view kDemoSource = R"(// bnl demo: Bangla and English keywords interchangeably.
চলক greeting = "Hello";
var name = "bnl";

কাজ shout(s) {
    ফেরত s + "!";
}

যদি (1 < 2 এবং name != null) {
    print(shout(greeting + ", " + name));
} নয়তো {
    লিখুন("unreachable");
}

// Closures and recursion both work.
function make_counter() {
    var n = 0;
    function step() {
        n = n + 1;
        return n;
    }
    return step;
}

var c = make_counter();
print("counter:", c(), c(), c());

কাজ fib(n) {
    যদি (n < 2) { ফেরত n; }
    ফেরত fib(n - 1) + fib(n - 2);
}

print("fib(10) =", fib(10));

// While loop counting down with the Bangla keyword.
var i = 3;
যতক্ষণ (i > 0) {
    print("ticking", i);
    i = i - 1;
}
)";

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

void print_diagnostics(const char* phase, const std::vector<bnl::Diagnostic>& diags) {
    if (diags.empty()) return;
    fmt::print(stderr, "\n--- {} diagnostics ({}) ---\n", phase, diags.size());
    for (const auto& d : diags) {
        fmt::print(stderr, "  {}:{}  {}\n", d.line, d.column, d.message);
    }
}

}  // namespace

int main(int argc, char** argv) {
    enable_utf8_console();

    bool        show_tokens     = false;
    bool        show_ast        = false;
    bool        show_banner     = true;
    bool        has_inline_code = false;
    std::string path;
    std::string inline_code;
    std::vector<std::string> program_args;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (path.empty() && !has_inline_code) {
            if      (a == "--tokens")             show_tokens = true;
            else if (a == "--ast")                show_ast    = true;
            else if (a == "--quiet")              show_banner = false;
            else if (a == "-e" || a == "--eval") {
                if (++i >= argc) {
                    fmt::print(stderr, "error: {} requires a code argument\n", a);
                    return 1;
                }
                inline_code     = argv[i];
                has_inline_code = true;
                show_banner     = false;   // -e implies --quiet (matches python/node/ruby)
            }
            else                                  path = std::string(a);
        } else {
            // Anything after the script path / -e code is forwarded as a script arg.
            program_args.emplace_back(a);
        }
    }

    if (show_banner) fmt::print("bnl {} ({})\n\n", bnl::kVersion, bnl::kPlatform);

    try {
        std::string source;
        if (has_inline_code)    source = std::move(inline_code);
        else if (path.empty())  source = std::string(kDemoSource);
        else                    source = read_file(path);

        if (path.empty() && !has_inline_code && show_banner) {
            fmt::print("--- source (built-in demo) ---\n{}\n", source);
        }

        bnl::Lexer lexer(source);
        auto       tokens = lexer.tokenize();
        if (show_tokens) dump_tokens(tokens);
        print_diagnostics("lex", lexer.diagnostics());
        if (lexer.has_errors()) return 2;

        bnl::Parser parser(std::move(tokens));
        auto        program = parser.parse();
        print_diagnostics("parse", parser.diagnostics());
        if (parser.has_errors()) return 3;

        if (show_ast) {
            fmt::print("--- ast ({} top-level stmts) ---\n", program.size());
            fmt::print("{}\n", bnl::ast_to_string(program));
        }

        if (show_banner) fmt::print("--- output ---\n");
        bnl::Interpreter interp;
        interp.set_program_args(std::move(program_args));
        std::filesystem::path entry = path.empty()
            ? std::filesystem::path{}
            : std::filesystem::weakly_canonical(std::filesystem::path(path));
        if (!interp.run(program, entry)) return 4;
    } catch (const std::exception& e) {
        fmt::print(stderr, "error: {}\n", e.what());
        return 1;
    }
    return 0;
}

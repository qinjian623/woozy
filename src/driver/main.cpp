#include "lang/version.hpp"
#include "lang/support/input.hpp"
#include "lang/syntax/parser.hpp"
#include <iostream>
#include <string_view>

namespace {
int syntax_check(const char* path, bool json) {
    lang::SourceManager sources;
    auto finish = [&](const auto& result) {
        if (json) {
            std::cout << lang::diagnostic_json(
                sources, std::string(lang::version()), "cli-request",
                result.status(), result.diagnostics) << '\n';
        } else if (result.success()) {
            std::cout << "syntax ok: " << path << '\n';
        } else {
            for (const auto& diagnostic : result.diagnostics) {
                if (diagnostic.primary) {
                    const auto& span = *diagnostic.primary;
                    std::cerr << sources.get(span.snapshot)->path << ":bytes["
                              << span.begin_byte << ',' << span.end_byte << "): ";
                }
                std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
            }
        }
        return static_cast<int>(result.exit_code());
    };
    try {
        auto input = lang::read_source(path, sources);
        if (!input.success()) return finish(input);
        auto tokens = lang::lex(*input.value);
        if (!tokens.success()) return finish(tokens);
        return finish(lang::parse(*tokens.value));
    } catch (const std::exception& error) {
        lang::PhaseResult<int> failure;
        failure.diagnostics.push_back({"E_INTERNAL_ERROR", "toolchain", "error",
                                      error.what(), {}, {}, {}, {}, {}, {}});
        return finish(failure);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: langc --help | --version\n"
                     "       langc check --syntax-only [--format json] FILE\n"
                     "Full check, build, lsp and capabilities are not implemented\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "langc " << lang::version() << '\n';
        return 0;
    }
    if (argc >= 2) {
        const std::string_view command(argv[1]);
        bool syntax = false;
        for (int i = 2; i < argc; ++i)
            syntax = syntax || std::string_view(argv[i]) == "--syntax-only";
        if (command == "check" && syntax) {
            bool json = false;
            const char* path = nullptr;
            for (int i = 2; i < argc; ++i) {
                const std::string_view argument(argv[i]);
                if (argument == "--syntax-only") continue;
                if (argument == "--format" && i + 1 < argc &&
                    std::string_view(argv[i + 1]) == "json") {
                    json = true;
                    ++i;
                } else if (argument.empty() || argument.front() == '-' || path) {
                    std::cerr << "E_DRIVER_ARGUMENT: expected one source file and optional --format json\n";
                    return 2;
                } else {
                    path = argv[i];
                }
            }
            if (!path) {
                std::cerr << "E_DRIVER_ARGUMENT: source file is required\n";
                return 2;
            }
            return syntax_check(path, json);
        }
        if (command == "check" || command == "build" || command == "lsp" ||
            command == "capabilities") {
            std::cerr << "E_CAPABILITY_UNAVAILABLE: " << command
                      << " is not implemented in T00 bootstrap\n";
            return 1;
        }
    }
    std::cerr << "E_DRIVER_ARGUMENT: expected --help, --version, check, build, lsp or capabilities\n";
    return 2;
}

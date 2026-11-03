#include "lang/version.hpp"
#include "lang/support/input.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <string_view>

namespace {
int check(const char* path, bool syntax, bool json, std::string_view backend) {
    lang::SourceManager sources;
    auto finish = [&](const auto& result) {
        if (json) {
            std::cout << lang::diagnostic_json(
                sources, std::string(lang::version()), "cli-request",
                result.status(), result.diagnostics) << '\n';
        } else if (result.success()) {
            std::cout << (syntax ? "syntax ok: " : "check ok: ") << path << '\n';
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
        if (!syntax) {
            auto project = lang::modules::load_project({path, {}, {}}, sources);
            if (!project.success()) return finish(project);
            if (!backend.empty()) project.value->backend = backend;
            auto resolved = lang::modules::resolve_project(
                std::make_shared<const lang::modules::Project>(std::move(*project.value)));
            return finish(lang::sema::check_project(resolved));
        }
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
                     "       langc check --project MANIFEST [--backend native|python] [--format json]\n"
                     "Build, lsp and capabilities are not implemented\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "langc " << lang::version() << '\n';
        return 0;
    }
    if (argc >= 2) {
        const std::string_view command(argv[1]);
        if (command == "check") {
            bool syntax = false, json = false;
            const char* path = nullptr;
            const char* project = nullptr;
            std::string_view backend;
            for (int i = 2; i < argc; ++i) {
                const std::string_view argument(argv[i]);
                if (argument == "--syntax-only") { syntax = true; continue; }
                if (argument == "--format" && i + 1 < argc &&
                    std::string_view(argv[i + 1]) == "json") {
                    json = true;
                    ++i;
                } else if (argument == "--project" && i + 1 < argc && !project &&
                           !std::string_view(argv[i + 1]).empty() &&
                           !std::string_view(argv[i + 1]).starts_with('-')) {
                    project = argv[++i];
                } else if (argument == "--backend" && i + 1 < argc && backend.empty() &&
                           (std::string_view(argv[i + 1]) == "native" ||
                            std::string_view(argv[i + 1]) == "python")) {
                    backend = argv[++i];
                } else if (argument.empty() || argument.front() == '-' || path) {
                    std::cerr << "E_DRIVER_ARGUMENT: invalid check arguments\n";
                    return 2;
                } else {
                    path = argv[i];
                }
            }
            if ((syntax && (!path || project || !backend.empty())) ||
                (!syntax && (!project || path))) {
                std::cerr << "E_DRIVER_ARGUMENT: use --syntax-only FILE or --project MANIFEST\n";
                return 2;
            }
            return check(syntax ? path : project, syntax, json, backend);
        }
        if (command == "build" || command == "lsp" ||
            command == "capabilities") {
            std::cerr << "E_CAPABILITY_UNAVAILABLE: " << command
                      << " is not implemented\n";
            return 1;
        }
    }
    std::cerr << "E_DRIVER_ARGUMENT: expected --help, --version, check, build, lsp or capabilities\n";
    return 2;
}

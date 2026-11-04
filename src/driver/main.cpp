#include "lang/version.hpp"
#include "lang/support/input.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/sema/check.hpp"
#include "lang/backend/python.hpp"
#include "lang/support/process.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <json-c/json.h>
#include <limits>
#include <iostream>
#include <string_view>

namespace {
namespace fs = std::filesystem;
#ifdef LANG_NATIVE_HELPER
bool diagnostic_document(const std::string& text, int exit) {
    if (text.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    auto* parser = json_tokener_new();
    if (!parser) throw std::bad_alloc();
    auto* value = json_tokener_parse_ex(parser, text.data(), static_cast<int>(text.size()));
    auto end = json_tokener_get_parse_end(parser);
    bool valid = value && json_tokener_get_error(parser) == json_tokener_success &&
        std::all_of(text.begin() + end, text.end(), [](unsigned char c) { return std::isspace(c); });
    if (valid) {
        auto* status = json_object_object_get(value, "status");
        auto* diagnostics = json_object_object_get(value, "diagnostics");
        valid = status && json_object_is_type(status, json_type_string) &&
            diagnostics && json_object_is_type(diagnostics, json_type_array);
        if (valid) {
            std::string_view s = json_object_get_string(status);
            valid = (exit == 0 && s == "ok") || ((exit == 1 || exit == 2) && s == "error") ||
                (exit == 3 && s == "internal_error") || (exit == 130 && s == "cancelled");
        }
    }
    if (value) json_object_put(value);
    json_tokener_free(parser);
    return valid;
}
#endif
void write_python(const fs::path& out, const lang::backend::python::Output& output) {
    fs::create_directories(out);
    auto pattern = (out / ".langc-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
        throw fs::filesystem_error("create build staging directory", out,
                                   std::error_code(errno, std::generic_category()));
    const fs::path stage(pattern);
    bool preserve = false;
    try {
        auto write = [&](const char* name, const std::string& text) {
            std::ofstream file;
            file.exceptions(std::ios::failbit | std::ios::badbit);
            file.open(stage / name, std::ios::binary);
            file << text;
            file.close();
        };
        write("program.py", output.code);
        write("langrt.py", output.runtime);
        for (const char* name : {"langrt.py", "program.py"}) {
            const auto status = fs::symlink_status(out / name);
            if (status.type() != fs::file_type::not_found && !fs::is_regular_file(status))
                throw fs::filesystem_error("output must be a regular file", out / name,
                                          std::make_error_code(std::errc::invalid_argument));
        }
        // shortcut: rollback covers reported errors; process interruption needs separate recovery.
        const auto backup = stage / "previous-first";
        const bool had_first = fs::exists(out / "langrt.py");
        if (had_first) fs::copy_file(out / "langrt.py", backup);
        bool published = false;
        try {
            fs::rename(stage / "langrt.py", out / "langrt.py");
            published = true;
            fs::rename(stage / "program.py", out / "program.py");
        } catch (...) {
            if (published) {
                std::error_code error;
                if (had_first) fs::rename(backup, out / "langrt.py", error);
                else fs::remove(out / "langrt.py", error);
                if (error) {
                    preserve = true;
                    throw fs::filesystem_error((had_first ? "restore first output failed; previous output retained at " + backup.string()
                                              : "remove new output failed; retained at " + (out / "langrt.py").string()),
                                              out / "langrt.py", error);
                }
            }
            throw;
        }
        fs::remove(backup);
        fs::remove(stage);
    } catch (...) {
        std::error_code ignored;
        if (!preserve) fs::remove_all(stage, ignored);
        throw;
    }
}
int compile(const char* path, bool syntax, bool json, std::string_view backend,
            const char* out = nullptr, const char* opt = nullptr, const char* target = nullptr) {
    lang::SourceManager sources;
    auto finish = [&](const auto& result) {
        if (json) {
            std::cout << lang::diagnostic_json(
                sources, std::string(lang::version()), "cli-request",
                result.status(), result.diagnostics) << '\n';
        } else if (result.success()) {
            std::cout << (out ? "build ok: " : syntax ? "syntax ok: " : "check ok: ")
                      << (out ? out : path) << '\n';
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
    auto fail = [&](const char* code, const std::string& message,
                    lang::FailureKind kind, const char* phase = "toolchain") {
        lang::PhaseResult<int> result;
        result.failure = kind;
        result.diagnostics.push_back({code, phase, "error", message});
        return finish(result);
    };
    try {
        if (!syntax) {
            auto project = lang::modules::load_project({path, {}, {}}, sources);
            if (!project.success()) return finish(project);
            if (!backend.empty()) project.value->backend = backend;
            if (out && project.value->backend == "native") {
#ifdef LANG_NATIVE_HELPER
                std::vector<std::string> args{LANG_NATIVE_HELPER, "--project", path,
                                              "--out-dir", out, "--opt", opt ? opt : "0"};
                if (target) args.insert(args.end(), {"--target", target});
                if (json) args.insert(args.end(), {"--format", "json"});
                auto result = lang::run_process(args);
                if (result.signalled)
                    return fail("E_INTERNAL_ERROR", "native helper terminated with status " +
                                std::to_string(result.exit_code) + ": " + result.output,
                                lang::FailureKind::internal);
                if (result.exit_code != 0 && result.exit_code != 1 &&
                    result.exit_code != 2 && result.exit_code != 3 && result.exit_code != 130)
                    return fail("E_TOOLCHAIN_MISSING", "native helper failed with status " +
                                std::to_string(result.exit_code) + ": " + result.output,
                                lang::FailureKind::environment);
                if (json && !diagnostic_document(result.output, result.exit_code))
                    return fail("E_INTERNAL_ERROR", "invalid native helper diagnostics: " + result.output,
                                lang::FailureKind::internal);
                (json || result.exit_code == 0 ? std::cout : std::cerr) << result.output;
                return result.exit_code;
#else
                return fail("E_CAPABILITY_UNAVAILABLE", "native build requires LANG_ENABLE_LLVM=ON",
                            lang::FailureKind::unsupported, "codegen");
#endif
            }
            if (out && (opt || target))
                return fail("E_DRIVER_ARGUMENT", "--opt and --target require native build",
                            lang::FailureKind::environment);
            auto resolved = lang::modules::resolve_project(
                std::make_shared<const lang::modules::Project>(std::move(*project.value)));
            auto checked = lang::sema::check_project(resolved);
            if (!checked.success() || !out) return finish(checked);
            if (checked.value->config().backend != lang::hir::Backend::python)
                return fail("E_CAPABILITY_UNAVAILABLE", "native build is not implemented",
                            lang::FailureKind::unsupported, "codegen");
            const auto& module = checked.value->module();
            std::optional<lang::SymbolId> entry;
            for (const auto& function : module.functions) {
                const auto& symbol = module.resolved->symbols.at(function.symbol.value);
                if (symbol.module == module.identity && symbol.name == "main")
                    entry = symbol.id;
            }
            if (!entry)
                return fail("E_CAPABILITY_UNAVAILABLE", "executable build requires main in the entry module",
                            lang::FailureKind::unsupported, "codegen");
            auto emitted = lang::backend::python::emit(*checked.value, *entry);
            if (!emitted.success()) return finish(emitted);
            write_python(out, *emitted.value);
            return finish(emitted);
        }
        auto input = lang::read_source(path, sources);
        if (!input.success()) return finish(input);
        auto tokens = lang::lex(*input.value);
        if (!tokens.success()) return finish(tokens);
        return finish(lang::parse(*tokens.value));
    } catch (const fs::filesystem_error& error) {
        return fail("E_INPUT_IO", error.what(), lang::FailureKind::environment);
    } catch (const std::ios_base::failure& error) {
        return fail("E_INPUT_IO", error.what(), lang::FailureKind::environment);
    } catch (const std::system_error& error) {
        return fail("E_TOOLCHAIN_MISSING", error.what(), lang::FailureKind::environment);
    } catch (const std::exception& error) {
        return fail("E_INTERNAL_ERROR", error.what(), lang::FailureKind::internal);
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: langc --help | --version\n"
                     "       langc check --syntax-only [--format json] FILE\n"
                     "       langc check --project MANIFEST [--backend native|python] [--format json]\n"
                     "       langc build --project MANIFEST --backend python --out-dir DIR [--format json]\n"
                     "       langc build --project MANIFEST --backend native --out-dir DIR [--opt 0|2] [--target TRIPLE] [--format json]\n"
                     "Native build requires LANG_ENABLE_LLVM=ON; lsp and capabilities are not implemented\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "langc " << lang::version() << '\n';
        return 0;
    }
    if (argc >= 2) {
        const std::string_view command(argv[1]);
        if (command == "check" || command == "build") {
            bool syntax = false, json = false;
            const char* path = nullptr;
            const char* project = nullptr;
            const char* out = nullptr;
            const char* opt = nullptr;
            const char* target = nullptr;
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
                } else if (argument == "--out-dir" && command == "build" &&
                           i + 1 < argc && !out && !std::string_view(argv[i + 1]).empty() &&
                           !std::string_view(argv[i + 1]).starts_with('-')) {
                    out = argv[++i];
                } else if (argument == "--backend" && i + 1 < argc && backend.empty() &&
                           (std::string_view(argv[i + 1]) == "native" ||
                            std::string_view(argv[i + 1]) == "python")) {
                    backend = argv[++i];
                } else if (argument == "--opt" && command == "build" && i + 1 < argc && !opt &&
                           (std::string_view(argv[i + 1]) == "0" || std::string_view(argv[i + 1]) == "2")) {
                    opt = argv[++i];
                } else if (argument == "--target" && command == "build" && i + 1 < argc && !target &&
                           !std::string_view(argv[i + 1]).empty() && !std::string_view(argv[i + 1]).starts_with('-')) {
                    target = argv[++i];
                } else if (argument.empty() || argument.front() == '-' || path) {
                    std::cerr << "E_DRIVER_ARGUMENT: invalid check/build arguments\n";
                    return 2;
                } else {
                    path = argv[i];
                }
            }
            if ((syntax && (!path || project || !backend.empty() || command == "build")) ||
                (!syntax && (!project || path)) || (command == "build" && !out)) {
                std::cerr << "E_DRIVER_ARGUMENT: use check --syntax-only FILE, check --project MANIFEST, or build --project MANIFEST --out-dir DIR\n";
                return 2;
            }
            return compile(syntax ? path : project, syntax, json, backend, out, opt, target);
        }
        if (command == "lsp" ||
            command == "capabilities") {
            std::cerr << "E_CAPABILITY_UNAVAILABLE: " << command
                      << " is not implemented\n";
            return 1;
        }
    }
    std::cerr << "E_DRIVER_ARGUMENT: expected --help, --version, check, build, lsp or capabilities\n";
    return 2;
}

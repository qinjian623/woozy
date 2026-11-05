#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include "lang/support/process.hpp"
#include "lang/version.hpp"
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <map>

using namespace lang;
namespace {
namespace fs = std::filesystem;
struct Stage {
  fs::path path;
  bool preserve = false;
  explicit Stage(const fs::path &out) {
    fs::create_directories(out);
    auto pattern = (out / ".langc-native-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
      throw fs::filesystem_error("create build staging directory", out,
                                std::error_code(errno, std::generic_category()));
    path = pattern;
  }
  ~Stage() {
    std::error_code ignored;
    if (!preserve) fs::remove_all(path, ignored);
  }
};
struct WriteError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void write(const std::string &path, const void *data, size_t size) {
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char *>(data),
            static_cast<std::streamsize>(size));
  out.flush();
  if (!out)
    throw WriteError("cannot write " + path);
}
void write(const std::string &path, const std::string &text) {
  write(path, text.data(), text.size());
}
uint64_t number(const std::string &s) {
  uint64_t n;
  auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (error != std::errc{} || end != s.data() + s.size())
    throw std::runtime_error("invalid integer argument: " + s);
  return n;
}
} // namespace
int main(int argc, char **argv) {
  SourceManager sources;
  bool json = false;
  bool executable = false;
  auto report = [&](const auto &r) {
    if (json)
      (executable ? std::cout : std::cerr) << diagnostic_json(sources, std::string(version()), "cli-request",
                                   r.status(), r.diagnostics)
                << '\n';
    else
      for (const auto &d : r.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    return static_cast<int>(r.exit_code());
  };
  try {
    std::map<std::string, std::string> args;
    bool check_only = false;
    for (int a = 1; a < argc; ++a) {
      std::string key = argv[a];
      if (key == "--check-only") {
        check_only = true;
        continue;
      }
      if (key == "--help") {
        std::cout << "lang_native_helper --project MANIFEST [--std-binding BUNDLE] "
                     "[--entry NAME | --entry-symbol ID]\n"
                     "  [--check-only] [--opt 0|2] [--target "
                     "x86_64-unknown-linux-gnu]\n"
                     "  --out-dir DIR | --out-object FILE [--out-llvm FILE] [--out-llvm-before "
                     "FILE]\n"
                     "  [--out-native FILE] [--format json]\n";
        return 0;
      }
      if (key != "--project" && key != "--std-binding" && key != "--entry" &&
          key != "--entry-symbol" && key != "--opt" && key != "--target" &&
          key != "--out-object" && key != "--out-llvm" &&
          key != "--out-llvm-before" && key != "--out-native" &&
          key != "--out-dir" && key != "--format")
        throw std::runtime_error("unknown option: " + key);
      if (key == "--std-binding" &&
          (a + 1 == argc || !argv[a + 1][0] || argv[a + 1][0] == '-'))
        throw std::runtime_error("missing or invalid value for --std-binding");
      if (++a == argc || !args.emplace(key, argv[a]).second)
        throw std::runtime_error("missing or duplicate option: " + key);
    }
    if (args.contains("--format")) {
      if (args.at("--format") != "json")
        throw std::runtime_error("--format must be json");
      json = true;
    }
    if (args.contains("--std-binding") && args.at("--std-binding").empty())
      throw std::runtime_error("empty option: --std-binding");
    executable = args.contains("--out-dir");
    if (!args.contains("--project") ||
        (!check_only && !executable && !args.contains("--out-object")))
      throw std::runtime_error(
          "--project and (unless --check-only) --out-dir or --out-object are required");
    if (args.contains("--entry") && args.contains("--entry-symbol"))
      throw std::runtime_error("choose one entry selector");
    if (executable && (check_only || args.contains("--out-object") ||
                       args.contains("--out-llvm") || args.contains("--out-llvm-before") ||
                       args.contains("--out-native")))
      throw std::runtime_error("--out-dir cannot be combined with explicit output files or --check-only");
    auto project = modules::load_project(
        {args.at("--project"), {},
         args.contains("--std-binding")
             ? std::optional<fs::path>(args.at("--std-binding"))
             : std::optional<fs::path>{}},
        sources);
    if (!project.success())
      return report(project);
    // Explicit native helper selection; word_bits remains the manifest's value.
    project.value->backend = "native";
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    if (!resolved.success())
      return report(resolved);
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      return report(checked);
    if (check_only) {
      std::cout << "{\"status\":\"passed\",\"phase\":\"check\",\"backend\":"
                   "\"native\",\"word_bits\":"
                << checked.value->config().word_bits << "}\n";
      return 0;
    }
    SymbolId entry;
    if (args.contains("--entry-symbol"))
      entry = SymbolId{number(args.at("--entry-symbol"))};
    else {
      std::string name = args.contains("--entry") ? args.at("--entry") : "main";
      std::optional<SymbolId> selected;
      for (auto &s : resolved.value->symbols)
        if (s.module == resolved.value->project->entry &&
            s.kind == modules::SymbolKind::function && s.name == name) {
          if (selected)
            throw std::runtime_error("ambiguous entry name");
          selected = s.id;
        }
      if (!selected) {
        if (executable) {
          PhaseResult<int> r;
          r.failure = FailureKind::unsupported;
          r.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE", "codegen", "error",
                                   "executable build requires main in the entry module"});
          return report(r);
        }
        throw std::runtime_error("entry function not found in entry module");
      }
      entry = *selected;
    }
    backend::native::Options options;
    if (args.contains("--opt")) {
      auto n = number(args.at("--opt"));
      if (n != 0 && n != 2)
        throw std::runtime_error("--opt must be 0 or 2");
      options.optimization = static_cast<unsigned>(n);
    }
    if (args.contains("--target"))
      options.triple = args.at("--target");
    auto emitted = backend::native::emit(*checked.value, entry, options);
    if (!emitted.success())
      return report(emitted);
    const auto &out = *emitted.value;
    if (executable) {
      const fs::path directory(args.at("--out-dir"));
      Stage stage(directory);
      const auto object = stage.path / "program.o";
      const auto program = stage.path / "program";
      for (const char *dependency : {LANG_NATIVE_CLANG, LANG_NATIVE_LLD, LANG_NATIVE_RUNTIME})
        if (!fs::is_regular_file(dependency))
          throw std::system_error(ENOENT, std::generic_category(), dependency);
      write(object.string(), out.object.data(), out.object.size());
      if (out.triple != "x86_64-unknown-linux-gnu" &&
          (out.triple.rfind("x86_64-", 0) != 0 || out.triple.find("linux") == std::string::npos)) {
        PhaseResult<int> failed;
        failed.failure = FailureKind::unsupported;
        failed.diagnostics.push_back({"E_TARGET_UNSUPPORTED", "codegen", "error",
                                     "native executable target only supports Linux x86_64: " + out.triple});
        return report(failed);
      }
      auto link = run_process({LANG_NATIVE_CLANG, "--target=" + out.triple,
                               "-nostdlib", "-static",
                               "--ld-path=" LANG_NATIVE_LLD, object.string(),
                               LANG_NATIVE_RUNTIME, "-o", program.string()});
      if (link.exit_code != 0) {
        PhaseResult<int> failed;
        const bool environment = link.signalled || link.exit_code == 127 || link.exit_code == 126;
        failed.failure = environment ? FailureKind::environment : FailureKind::source;
        Diagnostic diagnostic{environment ? "E_TOOLCHAIN_MISSING" : "E_LINK_FAILED",
                              "link", "error", "native linker exited with status " + std::to_string(link.exit_code)};
        diagnostic.notes.push_back(link.output);
        failed.diagnostics.push_back(std::move(diagnostic));
        return report(failed);
      }
      for (const char* name : {"program.o", "program"}) {
          const auto status = fs::symlink_status(directory / name);
          if (status.type() != fs::file_type::not_found && !fs::is_regular_file(status))
              throw fs::filesystem_error("output must be a regular file", directory / name,
                                        std::make_error_code(std::errc::invalid_argument));
      }
      // shortcut: rollback covers reported errors; process interruption needs separate recovery.
      const auto backup = stage.path / "previous-first";
      const bool had_first = fs::exists(directory / "program.o");
      if (had_first) fs::copy_file(directory / "program.o", backup);
      bool published = false;
      try {
          fs::rename(stage.path / "program.o", directory / "program.o");
          published = true;
          fs::rename(stage.path / "program", directory / "program");
      } catch (...) {
          if (published) {
              std::error_code error;
              if (had_first) fs::rename(backup, directory / "program.o", error);
              else fs::remove(directory / "program.o", error);
              if (error) {
                  stage.preserve = true;
                  throw fs::filesystem_error((had_first ? "restore first output failed; previous output retained at " + backup.string()
                                              : "remove new output failed; retained at " + (directory / "program.o").string()),
                                            directory / "program.o", error);
              }
          }
          throw;
      }
      if (json)
        std::cout << diagnostic_json(sources, std::string(version()), "cli-request",
                                     emitted.status(), emitted.diagnostics) << '\n';
      else
        std::cout << "build ok: " << (directory / "program").string() << '\n';
      return 0;
    }
    write(args.at("--out-object"), out.object.data(), out.object.size());
    if (args.contains("--out-llvm"))
      write(args.at("--out-llvm"), out.llvm_ir);
    if (args.contains("--out-llvm-before"))
      write(args.at("--out-llvm-before"), out.llvm_before_optimization);
    if (args.contains("--out-native"))
      write(args.at("--out-native"), out.native_ir);
    json_object *metadata = json_object_new_object();
    auto string = [&](const char *k, const std::string &v) {
      json_object_object_add(metadata, k, json_object_new_string(v.c_str()));
    };
    string("status", "passed");
    string("phase", "object");
    string("llvm", "22.1.8");
    string("contract", "compiler-dev/1");
    string("runtime_abi", std::string(backend::native::runtime_abi));
    string("triple", out.triple);
    string("data_layout", out.data_layout);
    json_object_object_add(metadata, "entry_symbol",
                           json_object_new_uint64(entry.value));
    for (auto key :
         {"cfg_verified", "llvm_verified_before", "llvm_verified_after"})
      json_object_object_add(metadata, key, json_object_new_boolean(true));
    std::cout << json_object_to_json_string_ext(metadata,
                                                JSON_C_TO_STRING_PLAIN)
              << '\n';
    json_object_put(metadata);
    return 0;
  } catch (const fs::filesystem_error &e) {
    PhaseResult<int> r;
    r.failure = FailureKind::environment;
    r.diagnostics.push_back({"E_INPUT_IO", "codegen", "error", e.what()});
    return report(r);
  } catch (const std::system_error &e) {
    PhaseResult<int> r;
    r.failure = FailureKind::environment;
    r.diagnostics.push_back({"E_TOOLCHAIN_MISSING", "link", "error", e.what()});
    return report(r);
  } catch (const WriteError &e) {
    PhaseResult<int> r;
    r.failure = FailureKind::environment;
    r.diagnostics.push_back(
        {"E_INPUT_IO", "codegen", "error", e.what(), {}, {}, {}, {}, {}, {}});
    return report(r);
  } catch (const std::exception &e) {
    PhaseResult<int> r;
    r.failure = FailureKind::environment;
    r.diagnostics.push_back({"E_DRIVER_ARGUMENT",
                             "toolchain",
                             "error",
                             e.what(),
                             {},
                             {},
                             {},
                             {},
                             {},
                             {}});
    return report(r);
  }
}

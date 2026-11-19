#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include "lang/support/process.hpp"
#include "lang/support/publish.hpp"
#include "lang/version.hpp"
#include "lang/hir/identity.hpp"
#include "lang/modules/parse_cache.hpp"
#include "lang/sema/front_cache.hpp"
#include "lang/support/cache_io.hpp"
#include "lang/support/file_digest.hpp"
#include <charconv>
#include <chrono>
#include <algorithm>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Support/MemoryBufferRef.h>
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
std::string shell_quote(std::string_view value) {
  std::string out = "'";
  for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
}
std::string ninja_escape(std::string_view value) {
  std::string out;
  for (char c : value) {
    if (c == '$') out += "$$";
    else if (c == ' ') out += "$ ";
    else if (c == ':') out += "$:";
    else if (c == '\n' || c == '\r') throw WriteError("newline in Ninja path");
    else out += c;
  }
  return out;
}
bool valid_object(std::string_view bytes) {
  auto object = llvm::object::ObjectFile::createObjectFile(
      llvm::MemoryBufferRef(llvm::StringRef(bytes.data(), bytes.size()), "package-cache"));
  if (!object) { llvm::consumeError(object.takeError()); return false; }
  if (!llvm::isa<llvm::object::ELF64LEObjectFile>(object->get()) ||
      (*object)->getArch() != llvm::Triple::x86_64 || !(*object)->isRelocatableObject()) return false;
  for (const auto &section : (*object)->sections()) {
    auto contents = section.getContents();
    if (!contents) { llvm::consumeError(contents.takeError()); return false; }
    for (const auto &relocation : section.relocations()) {
      auto symbol = relocation.getSymbol();
      if (symbol != (*object)->symbol_end()) {
        auto name = symbol->getName();
        if (!name) { llvm::consumeError(name.takeError()); return false; }
      }
    }
  }
  return true;
}
PhaseResult<std::monostate> package_build(const hir::VerifiedHirModule &checked,
    SymbolId entry, backend::native::Options options, const fs::path &directory,
    sema::FrontCache *front_cache, const modules::ParseCache *parse_cache, const FileDigests &tool_digests) {
  PhaseResult<std::monostate> result;
  auto cfg = ir::lower(checked);
  if (!cfg.success()) {
    result.state = cfg.state; result.failure = cfg.failure;
    result.diagnostics = std::move(cfg.diagnostics); return result;
  }
  auto identities = std::make_shared<hir::IdentityKeys>(checked);
  options.identities = identities;
  std::vector<std::pair<std::string, size_t>> named_functions;
  for (size_t i = 0; i < cfg.value->module().functions.size(); ++i)
    named_functions.emplace_back(identities->function_name(cfg.value->module().functions[i].symbol), i);
  std::sort(named_functions.begin(), named_functions.end());
  auto source_order = std::make_shared<std::vector<size_t>>();
  for (const auto &[name, index] : named_functions) source_order->push_back(index);
  options.source_order = source_order;
  const auto &project = *checked.module().resolved->project;
  if (checked.config().word_bits != 64 || options.triple != "x86_64-unknown-linux-gnu") {
    result.failure = FailureKind::unsupported;
    result.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE", "codegen", "error", "package objects require native/64 Linux x86_64"});
    return result;
  }
  if (!project.c_bindings.empty()) {
    result.failure = FailureKind::unsupported;
    result.diagnostics.push_back({"E_CAPABILITY_UNAVAILABLE", "codegen", "error", "package objects do not yet support C binding adapters"});
    return result;
  }
  StagingDir stage(directory, ".woozyc-packages-XXXXXX");
  std::string plan = "rule opt\n  command = " + ninja_escape(shell_quote(LANG_NATIVE_OPT)) +
      " -S -passes='verify,default<O" + std::to_string(options.optimization) +
      ">,verify' -verify-each -mtriple=" + options.triple +
      " -mcpu=generic $in -o $out\nrule object\n  command = " + ninja_escape(shell_quote(LANG_NATIVE_LLC)) +
      " -filetype=obj -O=" + std::to_string(options.optimization) +
      " -mtriple=" + options.triple +
      " -mcpu=generic -relocation-model=pic -code-model=small $in -o $out\n";
  std::unique_ptr<json_object, decltype(&json_object_put)> package_metadata(json_object_new_array(), json_object_put);
  std::string objects;
  size_t object_hits = 0, object_misses = 0, compile_jobs = 0;
  uint64_t llvm_emit_nanoseconds = 0;
  std::vector<std::pair<std::string, std::string>> new_objects;
  options.llvm_only = true;
  for (const auto &package : project.modules) {
    bool used = false;
    for (const auto &function : cfg.value->module().functions)
      if (identities->function_owner(function.symbol) == package.id) used = true;
    if (!used) continue;
    const auto name = "package-" + std::to_string(package.id.value);
    auto *metadata = json_object_new_object();
    json_object_object_add(metadata, "logical_path", json_object_new_string(package.logical_path.c_str()));
    json_object_object_add(metadata, "identity", json_object_new_string(package.identity.c_str()));
    json_object_object_add(metadata, "object", json_object_new_string((name + ".o").c_str()));
    json_object_object_add(metadata, "object_cache_hit", json_object_new_boolean(false));
    json_object_object_add(metadata, "object_key", json_object_new_string(""));
    json_object_object_add(metadata, "frontend_key", json_object_new_string(""));
    json_object_array_add(package_metadata.get(), metadata);
    options.package = package.id;
    const auto emit_started = std::chrono::steady_clock::now();
    auto output = backend::native::emit(*cfg.value, entry, options);
    llvm_emit_nanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - emit_started).count();
    if (!output.success()) {
      result.state = output.state; result.failure = output.failure;
      result.diagnostics = std::move(output.diagnostics); return result;
    }
    const auto llvm_digest = sha256(output.value->llvm_before_optimization);
    json_object_object_add(metadata, "llvm_ir_sha256", json_object_new_string(llvm_digest.c_str()));
    std::string object_key;
    if (front_cache) {
      // The actual verified optimizer input is the complete code-generation
      // dependency. This covers nominal ABI, instantiated bodies, transitive
      // constant values and embedded runtime/source numbers without a second
      // dependency model. Warm builds deliberately emit LLVM again.
      cache::Writer key; key.string("wz-native-llvm-object/1");
      key.string(front_cache->fingerprint); key.string(package.identity); key.string(llvm_digest);
      object_key = sha256(key.bytes) + ".obj";
      json_object_object_add(metadata, "object_key", json_object_new_string(object_key.c_str()));
      json_object_object_add(metadata, "frontend_key", json_object_new_string(front_cache->package_key(checked.module(), package.id).c_str()));
      auto record = cache::load(front_cache->directory / object_key);
      if (record) {
        try {
          cache::Reader input{*record};
          if (input.string() == "wz-native-object/1" && input.string() == object_key) {
            const auto object = input.string(); input.done();
            if (valid_object(object)) {
              write((stage.path() / (name + ".o")).string(), object);
              json_object_object_add(metadata, "object_cache_hit", json_object_new_boolean(true));
              objects += " " + name + ".o"; ++object_hits; continue;
            }
          }
        } catch (const std::exception &) {}
      }
    }
    if (front_cache) ++object_misses;
    ++compile_jobs;
    if (front_cache) new_objects.emplace_back(object_key, name + ".o");
    write((stage.path() / (name + ".ll")).string(), output.value->llvm_before_optimization);
    plan += "build " + name + ".verified.ll: opt " + name + ".ll\nbuild " + name +
        ".o: object " + name + ".verified.ll\n";
    objects += " " + name + ".o";
  }
  plan += "rule combine\n  command = " + ninja_escape(shell_quote(LANG_NATIVE_LLD)) +
      " -r $in -o $out\nbuild program.o: combine" + objects + "\n";
  const auto runtime = project.hosted ? LANG_HOSTED_RUNTIME : LANG_NATIVE_RUNTIME;
  std::string link = shell_quote(LANG_NATIVE_CLANG) + " --target=" + options.triple +
      " --ld-path=" + shell_quote(LANG_NATIVE_LLD);
  link += project.hosted ? " -Wl,-u,lang_hosted_startup_v1 -pthread" : " -nostdlib -static";
  std::string link_flags;
  for (const auto &dir : project.c_library_dirs) link_flags += " -L" + shell_quote(dir.string());
  for (const auto &lib : project.c_libraries) link_flags += " -l" + shell_quote(lib);
  plan += "rule link\n  command = " + ninja_escape(link) + " $in" + ninja_escape(link_flags) + " -o $out\nbuild program: link" + objects +
      " " + ninja_escape(runtime) + "\ndefault program program.o\n";
  write((stage.path() / "build.ninja").string(), plan);
  const auto execution = run_process({LANG_NATIVE_NINJA, "-C", stage.path().string(), "-v"});
  write((stage.path() / "package-build.log").string(), execution.output);
  if (execution.exit_code != 0 || execution.signalled) {
    result.failure = execution.signalled || execution.exit_code == 126 || execution.exit_code == 127
        ? FailureKind::environment : FailureKind::internal;
    result.diagnostics.push_back({"E_NATIVE_BUILD_FAILED", "codegen", "error",
        "Ninja package build failed with status " + std::to_string(execution.exit_code),
        {}, {}, {}, {execution.output}});
    return result;
  }
  if (front_cache) {
    for (const auto &[key, name] : new_objects) {
      std::ifstream input(stage.path() / name, std::ios::binary);
      const std::string object((std::istreambuf_iterator<char>(input)), {});
      if (!input.eof() && !input) throw WriteError("cannot read generated package object");
      cache::Writer record; record.string("wz-native-object/1"); record.string(key); record.string(object);
      cache::store(front_cache->directory, key, record.bytes);
    }
  }
  write((stage.path() / "package-records.json").string(), json_object_to_json_string_ext(package_metadata.get(), JSON_C_TO_STRING_PRETTY));
  std::string statistics = "{\"schema_version\":1,\"build_fingerprint\":\"" + (front_cache ? front_cache->fingerprint : "") + "\",\"parse_hits\":" + std::to_string(parse_cache ? parse_cache->hits : 0) +
      ",\"string_tokens_checked\":" + std::to_string(parse_cache ? parse_cache->string_tokens_checked : 0) +
      ",\"parse_misses\":" + std::to_string(parse_cache ? parse_cache->misses : 0) +
      ",\"frontend_package_hits\":" + std::to_string(front_cache ? front_cache->hits : 0) +
      ",\"frontend_package_misses\":" + std::to_string(front_cache ? front_cache->misses : 0) +
      ",\"checked_bodies\":" + std::to_string(front_cache ? front_cache->checked_bodies : checked.module().functions.size()) +
      ",\"rejected_frontend_cache\":" + std::to_string(front_cache ? front_cache->rejected : 0) +
      ",\"tool_digest_hits\":" + std::to_string(tool_digests.hits) +
      ",\"tool_digest_misses\":" + std::to_string(tool_digests.misses) +
      ",\"tool_digest_bytes_read\":" + std::to_string(tool_digests.bytes_read) +
      ",\"llvm_emit_jobs\":" + std::to_string(json_object_array_length(package_metadata.get())) +
      ",\"llvm_emit_nanoseconds\":" + std::to_string(llvm_emit_nanoseconds) +
      ",\"object_hits\":" + std::to_string(object_hits) + ",\"object_misses\":" + std::to_string(object_misses) +
      ",\"opt_jobs\":" + std::to_string(compile_jobs) +
      ",\"llc_jobs\":" + std::to_string(compile_jobs) + ",\"link_jobs\":1}\n";
  write((stage.path() / "cache-statistics.json").string(), statistics);
  publish_staged_files(stage, directory, {"package-build.log", "package-records.json", "cache-statistics.json", "program.o", "program"});
  result.value = std::monostate{}; result.state = PhaseState::complete;
  return result;
}
int run(int argc, char **argv) {
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
    bool package_objects = false;
    for (int a = 1; a < argc; ++a) {
      std::string key = argv[a];
      if (key == "--package-objects") { package_objects = true; continue; }
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
                     "  [--out-native FILE] [--format json]\n"
                     "  --package-objects [--cache-dir DIR] (with --out-dir)\n"
                     "  --package LOGICAL_PATH (with --out-object)\n";
        return 0;
      }
      if (key != "--project" && key != "--std-binding" && key != "--entry" &&
          key != "--entry-symbol" && key != "--opt" && key != "--target" &&
          key != "--out-object" && key != "--out-llvm" &&
          key != "--out-llvm-before" && key != "--out-native" &&
          key != "--out-dir" && key != "--format" && key != "--package" && key != "--cache-dir" && key != "--driver-fingerprint" && key != "--driver-binary")
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
    if (args.contains("--package") && executable)
      throw std::runtime_error("--package requires --out-object");
    if (package_objects && !executable)
      throw std::runtime_error("--package-objects requires --out-dir");
    if (args.contains("--entry") && args.contains("--entry-symbol"))
      throw std::runtime_error("choose one entry selector");
    if (executable && (check_only || args.contains("--out-object") ||
                       args.contains("--out-llvm") || args.contains("--out-llvm-before") ||
                       args.contains("--out-native")))
      throw std::runtime_error("--out-dir cannot be combined with explicit output files or --check-only");
    std::shared_ptr<modules::ParseCache> parse_cache;
    std::optional<sema::FrontCache> front_cache;
    FileDigests tool_digests;
    if (args.contains("--cache-dir")) {
      tool_digests.directory = fs::path(args.at("--cache-dir")) / "file-digests";
      if (!package_objects) throw std::runtime_error("--cache-dir requires --package-objects");
      cache::Writer fingerprint;
      fingerprint.string("wz-package-build/1");
      fingerprint.string(args.contains("--driver-binary") ? tool_digests.get(args.at("--driver-binary")) :
          (args.contains("--driver-fingerprint") ? args.at("--driver-fingerprint") : "standalone-helper"));
      fingerprint.string(args.contains("--opt") ? args.at("--opt") : "0");
      fingerprint.string(args.contains("--target") ? args.at("--target") : "x86_64-unknown-linux-gnu");
      for (const auto &path : {fs::path("/proc/self/exe"), fs::path(LANG_NATIVE_RUNTIME), fs::path(LANG_HOSTED_RUNTIME),
           fs::path(LANG_NATIVE_OPT), fs::path(LANG_NATIVE_LLC), fs::path(LANG_NATIVE_CLANG), fs::path(LANG_NATIVE_LLD), fs::path(LANG_NATIVE_NINJA)}) {
        fingerprint.string(tool_digests.get(path));
      }
      const auto digest = sha256(fingerprint.bytes);
      parse_cache = std::make_shared<modules::ParseCache>(fs::path(args.at("--cache-dir")) / "parse", digest);
      front_cache.emplace(fs::path(args.at("--cache-dir")) / "frontend", digest);
    }
    auto project = modules::load_project(
        {args.at("--project"), {},
         args.contains("--std-binding")
             ? std::optional<fs::path>(args.at("--std-binding"))
             : std::optional<fs::path>{}, parse_cache},
        sources);
    if (!project.success())
      return report(project);
    // Explicit native helper selection; word_bits remains the manifest's value.
    project.value->backend = "native";
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    if (!resolved.success())
      return report(resolved);
    auto checked = front_cache ? sema::check_project_cached(resolved, *front_cache) : sema::check_project(resolved);
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
    if (args.contains("--package")) {
      for (const auto &package : checked.value->module().resolved->project->modules)
        if (package.logical_path == args.at("--package")) options.package = package.id;
      if (!options.package) throw std::runtime_error("unknown logical package");
    }
    if (package_objects) {
      auto built = package_build(*checked.value, entry, options, args.at("--out-dir"),
          front_cache ? &*front_cache : nullptr, parse_cache.get(), tool_digests);
      if (!built.success() || json) return report(built);
      std::cout << "build ok: " << (fs::path(args.at("--out-dir")) / "program").string() << '\n';
      return 0;
    }
    auto emitted = backend::native::emit(*checked.value, entry, options);
    if (!emitted.success())
      return report(emitted);
    const auto &out = *emitted.value;
    const auto &frozen_project = *checked.value->module().resolved->project;
    if (executable) {
      const fs::path directory(args.at("--out-dir"));
      StagingDir stage(directory, ".woozyc-native-XXXXXX");
      const auto object = stage.path() / "program.o";
      const auto program = stage.path() / "program";
      const auto adapter_c = stage.path() / "c_adapter.c";
      const auto adapter_o = stage.path() / "c_adapter.o";
      std::vector<const char *> dependencies{LANG_NATIVE_CLANG, LANG_NATIVE_LLD,
                                             LANG_NATIVE_RUNTIME};
      if (frozen_project.hosted) dependencies.push_back(LANG_HOSTED_RUNTIME);
      for (const char *dependency : dependencies)
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
      bool has_adapter = !out.c_adapter_source.empty();
      if (has_adapter) {
        write(adapter_c.string(), out.c_adapter_source);
        auto compile = run_process({LANG_NATIVE_CLANG, "--target=" + out.triple,
                                    "-std=c11", "-O2", "-fPIC", "-c",
                                    adapter_c.string(), "-o", adapter_o.string()});
        if (compile.exit_code != 0) {
          PhaseResult<int> failed;
          const bool environment = compile.signalled || compile.exit_code == 127 ||
                                   compile.exit_code == 126;
          failed.failure = environment ? FailureKind::environment : FailureKind::source;
          Diagnostic diagnostic{environment ? "E_TOOLCHAIN_MISSING" : "E_LINK_FAILED",
                                "link", "error",
                                "C adapter compilation failed with status " +
                                    std::to_string(compile.exit_code)};
          diagnostic.notes.push_back(compile.output);
          failed.diagnostics.push_back(std::move(diagnostic));
          return report(failed);
        }
      }
      std::vector<std::string> link{LANG_NATIVE_CLANG, "--target=" + out.triple};
      if (frozen_project.hosted) {
        link.push_back("-Wl,-u,lang_hosted_startup_v1");
        link.push_back("-pthread");
      } else {
        link.push_back("-nostdlib");
        link.push_back("-static");
      }
      link.push_back("--ld-path=" LANG_NATIVE_LLD);
      link.push_back(object.string());
      if (has_adapter) link.push_back(adapter_o.string());
      link.push_back(frozen_project.hosted ? LANG_HOSTED_RUNTIME : LANG_NATIVE_RUNTIME);
      for (const auto &dir : frozen_project.c_library_dirs)
        link.push_back("-L" + dir.string());
      for (const auto &library : frozen_project.c_libraries)
        link.push_back("-l" + library);
      link.push_back("-o");
      link.push_back(program.string());
      auto linked = run_process(link);
      if (linked.exit_code != 0) {
        PhaseResult<int> failed;
        const bool environment = linked.signalled || linked.exit_code == 127 || linked.exit_code == 126;
        failed.failure = environment ? FailureKind::environment : FailureKind::source;
        Diagnostic diagnostic{environment ? "E_TOOLCHAIN_MISSING" : "E_LINK_FAILED",
                              "link", "error", "native linker exited with status " + std::to_string(linked.exit_code)};
        diagnostic.notes.push_back(linked.output);
        failed.diagnostics.push_back(std::move(diagnostic));
        return report(failed);
      }
      publish_staged_files(stage, directory, {"program.o", "program"});
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
    string("runtime", frozen_project.hosted ? "hosted" : "freestanding");
    json_object_object_add(metadata, "c_adapter",
                           json_object_new_boolean(!out.c_adapter_source.empty()));
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
bool flushed(std::ostream &stream) {
  stream.flush();
  return stream.good();
}
} // namespace
int main(int argc, char **argv) {
  const int status = run(argc, argv);
  const bool stdout_ok = flushed(std::cout);
  const bool stderr_ok = flushed(std::cerr);
  return stdout_ok && stderr_ok ? status : 2;
}

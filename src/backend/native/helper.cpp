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
#include "lang/support/resource.hpp"
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
#include <system_error>

using namespace lang;
namespace {
namespace fs = std::filesystem;
bool version_token(std::string_view text) {
  constexpr std::string_view version = "22.1.8";
  auto part = [](char c) { return (c >= '0' && c <= '9') || c == '.'; };
  for (auto at = text.find(version); at != std::string_view::npos;
       at = text.find(version, at + 1)) {
    const bool before = at == 0 || !part(text[at - 1]);
    const auto end = at + version.size();
    const bool after = end == text.size() || !part(text[end]);
    if (before && after) return true;
  }
  return false;
}
[[noreturn]] void tool_error(const std::string &message) {
  throw std::system_error(EINVAL, std::generic_category(), message);
}
struct LlvmTools {
  fs::path opt, llc, clang, lld;
};
void verify_tool(const fs::path &path, std::string_view name) {
  if (!is_executable_file(path))
    throw std::system_error(ENOENT, std::generic_category(),
                            "not an executable LLVM tool: " + path.string());
  auto result = run_process({path.string(), "--version"});
  if (result.signalled || result.exit_code != 0 || !version_token(result.output))
    tool_error(std::string(name) + " is not pinned LLVM 22.1.8: " +
               path.string() + "\n" + result.output);
}
LlvmTools verified_tools(const fs::path &directory) {
  LlvmTools tools{directory / "opt", directory / "llc",
                  directory / "clang", directory / "ld.lld"};
  verify_tool(tools.opt, "opt");
  verify_tool(tools.llc, "llc");
  verify_tool(tools.clang, "clang");
  verify_tool(tools.lld, "ld.lld");
  return tools;
}
std::optional<fs::path> environment_tools() {
  const char *value = std::getenv("WOOZY_LLVM_BIN");
  if (!value || !*value) return std::nullopt;
  fs::path directory(value);
  if (!directory.is_absolute()) {
    auto root = resource_root();
    if (!root)
      throw std::system_error(ENOENT, std::generic_category(),
                              "relative WOOZY_LLVM_BIN needs a package root");
    directory = *root / directory;
  }
  if (!fs::is_directory(directory))
    throw std::system_error(ENOENT, std::generic_category(),
                            "WOOZY_LLVM_BIN is not a directory: " +
                                directory.string());
  return directory;
}
const LlvmTools &llvm_tools() {
  static const LlvmTools tools = [] {
    if (auto directory = environment_tools())
      return verified_tools(*directory);
    for (const auto &root : resource_roots()) {
      if (auto directory = find_resource_at(root, "toolchain")) {
        if (!fs::is_directory(*directory))
          tool_error("LLVM resource is not a directory: " +
                     directory->string());
        return verified_tools(*directory);
      }
    }
    std::string last_error;
    for (const auto &directory : path_directories()) {
      if (!is_executable_file(directory / "opt") ||
          !is_executable_file(directory / "llc") ||
          !is_executable_file(directory / "clang") ||
          !is_executable_file(directory / "ld.lld"))
        continue;
      try {
        return verified_tools(directory);
      } catch (const std::system_error &error) {
        last_error = error.what();
      }
    }
    if (!last_error.empty()) tool_error(last_error);
    throw std::system_error(ENOENT, std::generic_category(),
                            "pinned LLVM 22.1.8 tool directory not found");
  }();
  return tools;
}
fs::path llvm_tool(std::string_view name) {
  const auto &tools = llvm_tools();
  if (name == "opt") return tools.opt;
  if (name == "llc") return tools.llc;
  if (name == "clang") return tools.clang;
  if (name == "ld.lld") return tools.lld;
  throw std::invalid_argument("unknown LLVM tool: " + std::string(name));
}
fs::path ninja_tool() {
  if (auto resource = find_resource("toolchain/ninja")) {
    if (!is_executable_file(*resource))
      throw std::system_error(ENOENT, std::generic_category(),
                              "ninja resource is not executable: " +
                                  resource->string());
    return *resource;
  }
  if (auto path = find_program("ninja")) return *path;
  throw std::system_error(ENOENT, std::generic_category(), "ninja not found");
}
fs::path runtime_archive(bool hosted) {
  const std::string name =
      hosted ? "liblang_hosted_runtime.a" : "liblang_native_runtime.a";
  for (const auto &root : resource_roots()) {
    for (const auto &relative : {std::string("runtime/") + name, name}) {
      if (auto candidate = find_resource_at(root, relative)) {
        if (!fs::is_regular_file(*candidate))
          throw std::system_error(EINVAL, std::generic_category(),
                                  "runtime archive is not a regular file: " +
                                      candidate->string());
        return *candidate;
      }
    }
  }
  throw std::system_error(ENOENT, std::generic_category(),
                          "native runtime not found: " + name);
}
int report_resource_paths() {
  try {
    const auto &tools = llvm_tools();
    auto *root = json_object_new_object();
    json_object_object_add(root, "schema_version", json_object_new_int(1));
    if (auto stdlib = find_resource("stdlib"))
      json_object_object_add(root, "stdlib",
                             json_object_new_string(stdlib->string().c_str()));
    auto *runtime = json_object_new_object();
    json_object_object_add(runtime, "native",
                           json_object_new_string(runtime_archive(false).string().c_str()));
    json_object_object_add(runtime, "hosted",
                           json_object_new_string(runtime_archive(true).string().c_str()));
    json_object_object_add(root, "runtime", runtime);
    auto *toolchain = json_object_new_object();
    json_object_object_add(toolchain, "opt",
                           json_object_new_string(tools.opt.string().c_str()));
    json_object_object_add(toolchain, "llc",
                           json_object_new_string(tools.llc.string().c_str()));
    json_object_object_add(toolchain, "clang",
                           json_object_new_string(tools.clang.string().c_str()));
    json_object_object_add(toolchain, "ld.lld",
                           json_object_new_string(tools.lld.string().c_str()));
    json_object_object_add(root, "toolchain", toolchain);
    json_object_object_add(root, "ninja",
                           json_object_new_string(ninja_tool().string().c_str()));
    std::cout << json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN)
              << '\n';
    json_object_put(root);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "E_TOOLCHAIN_MISSING: " << error.what() << '\n';
    return 2;
  }
}
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
  const auto opt = llvm_tool("opt");
  const auto llc = llvm_tool("llc");
  const auto lld = llvm_tool("ld.lld");
  const auto clang = llvm_tool("clang");
  const auto ninja = ninja_tool();
  StagingDir stage(directory, ".woozyc-packages-XXXXXX");
  std::string plan = "rule opt\n  command = " + ninja_escape(shell_quote(opt.string())) +
      " -S -passes='verify,default<O" + std::to_string(options.optimization) +
      ">,verify' -verify-each -mtriple=" + options.triple +
      " -mcpu=generic $in -o $out\nrule object\n  command = " + ninja_escape(shell_quote(llc.string())) +
      " -filetype=obj -O=" + std::to_string(options.optimization) +
      " -mtriple=" + options.triple +
      " -mcpu=generic -relocation-model=pic -code-model=small $in -o $out\n";
  // CoroSplit debug locations can reference distinct metadata that LLVM's
  // textual round-trip cannot preserve. Keep optimized async debug input as bitcode.
  plan += "rule opt_bitcode\n  command = " + ninja_escape(shell_quote(opt.string())) +
      " -passes='verify,default<O" + std::to_string(options.optimization) +
      ">,verify' -verify-each -mtriple=" + options.triple +
      " -mcpu=generic $in -o $out\n";
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
      if (options.debug_info) key.string("-g");
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
    bool async_package = false;
    for (const auto &function : cfg.value->module().functions)
      if (options.debug_info && function.async &&
          identities->function_owner(function.symbol) == package.id)
        async_package = true;
    const auto verified = name + (async_package ? ".verified.bc" : ".verified.ll");
    plan += "build " + verified + (async_package ? ": opt_bitcode " : ": opt ") +
        name + ".ll\nbuild " + name + ".o: object " + verified + "\n";
    objects += " " + name + ".o";
  }
  plan += "rule combine\n  command = " + ninja_escape(shell_quote(lld.string())) +
      " -r $in -o $out\nbuild program.o: combine" + objects + "\n";
  const auto runtime = runtime_archive(project.hosted);
  std::string link = shell_quote(clang.string()) + " --target=" + options.triple +
      " --ld-path=" + shell_quote(lld.string());
  link += project.hosted ? " -Wl,-u,lang_hosted_startup_v1 -pthread" : " -nostdlib -static";
  std::string link_flags;
  for (const auto &dir : project.c_library_dirs) link_flags += " -L" + shell_quote(dir.string());
  for (const auto &lib : project.c_libraries) link_flags += " -l" + shell_quote(lib);
  plan += "rule link\n  command = " + ninja_escape(link) + " $in" + ninja_escape(link_flags) + " -o $out\nbuild program: link" + objects +
      " " + ninja_escape(runtime.string()) + "\ndefault program program.o\n";
  write((stage.path() / "build.ninja").string(), plan);
  const auto execution = run_process({ninja.string(), "-C", stage.path().string(), "-v"});
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
    bool debug_info = false;
    for (int a = 1; a < argc; ++a) {
      std::string key = argv[a];
      if (key == "-g") {
        if (debug_info) throw std::runtime_error("duplicate option: -g");
        debug_info = true;
        continue;
      }
      if (key == "--package-objects") { package_objects = true; continue; }
      if (key == "--check-only") {
        check_only = true;
        continue;
      }
      if (key == "--help") {
        std::cout << "lang_native_helper --project MANIFEST [--std-binding BUNDLE] "
                     "[--entry NAME | --entry-symbol ID]\n"
                     "  [--check-only] [-g] [--opt 0|2] [--target "
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
    if (debug_info && check_only)
      throw std::runtime_error("-g requires native build; cannot combine with --check-only");
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
      if (debug_info) fingerprint.string("-g");
      const auto native_runtime = runtime_archive(false);
      const auto hosted_runtime = runtime_archive(true);
      const auto opt = llvm_tool("opt");
      const auto llc = llvm_tool("llc");
      const auto clang = llvm_tool("clang");
      const auto lld = llvm_tool("ld.lld");
      const auto ninja = ninja_tool();
      for (const auto &path : {fs::path("/proc/self/exe"), native_runtime,
           hosted_runtime, opt, llc, clang, lld, ninja}) {
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
    options.debug_info = debug_info;
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
      const auto clang = llvm_tool("clang");
      const auto lld = llvm_tool("ld.lld");
      const auto native_runtime = runtime_archive(false);
      fs::path hosted_runtime;
      std::vector<fs::path> dependencies{clang, lld, native_runtime};
      if (frozen_project.hosted) {
        hosted_runtime = runtime_archive(true);
        dependencies.push_back(hosted_runtime);
      }
      for (const auto &dependency : dependencies)
        if (!fs::is_regular_file(dependency))
          throw std::system_error(ENOENT, std::generic_category(),
                                  dependency.string());
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
        auto compile = run_process({clang.string(), "--target=" + out.triple,
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
      std::vector<std::string> link{clang.string(), "--target=" + out.triple};
      if (frozen_project.hosted) {
        link.push_back("-Wl,-u,lang_hosted_startup_v1");
        link.push_back("-pthread");
      } else {
        link.push_back("-nostdlib");
        link.push_back("-static");
      }
      link.push_back("--ld-path=" + lld.string());
      link.push_back(object.string());
      if (has_adapter) link.push_back(adapter_o.string());
      link.push_back(frozen_project.hosted ? hosted_runtime.string()
                                           : native_runtime.string());
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
  if (argc == 2 &&
      (std::string_view(argv[1]) == "--__resource-paths" ||
       std::string_view(argv[1]) == "--__resource_paths"))
    return report_resource_paths();
  const int status = run(argc, argv);
  const bool stdout_ok = flushed(std::cout);
  const bool stderr_ok = flushed(std::cerr);
  return stdout_ok && stderr_ok ? status : 2;
}

#include "lang/backend/python.hpp"
#include "lang/sema/check.hpp"
#include "lang/version.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json-c/json.h>

using namespace lang;
namespace fs = std::filesystem;
namespace {
struct BadArguments : std::runtime_error {
  using std::runtime_error::runtime_error;
};
void write(const fs::path &path, const std::string &content) {
  std::ofstream file;
  file.exceptions(std::ios::failbit | std::ios::badbit);
  file.open(path, std::ios::binary);
  file << content;
  file.close();
}
int report(SourceManager &sources, const auto &result) {
  std::cout << diagnostic_json(sources, std::string(version()), "T06",
                               result.status(), result.diagnostics)
            << '\n';
  return static_cast<int>(result.exit_code());
}
int failure(SourceManager &sources, const char *code, const std::exception &e,
            ExitCode exit) {
  Diagnostic d;
  d.code = code;
  d.phase = "toolchain";
  d.severity = "error";
  d.message = e.what();
  std::cout << diagnostic_json(sources, std::string(version()), "T06",
                               exit == ExitCode::internal_error
                                   ? Status::internal_error
                                   : Status::error,
                               {d})
            << '\n';
  return static_cast<int>(exit);
}
} // namespace
int main(int argc, char **argv) {
  SourceManager sources;
  try {
    fs::path manifest, out, std_binding;
    bool runner = true;
    for (int i = 1; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (arg == "--project" && i + 1 < argc)
        manifest = argv[++i];
      else if (arg == "--out-dir" && i + 1 < argc)
        out = argv[++i];
      else if (arg == "--std-binding" && i + 1 < argc)
        std_binding = argv[++i];
      else if (arg == "--no-runner")
        runner = false;
      else
        throw BadArguments("usage: lang_python_emit --project MANIFEST "
                           "--out-dir DIR [--std-binding BUNDLE] "
                           "[--no-runner]");
    }
    if (manifest.empty() || out.empty())
      throw BadArguments("--project and --out-dir are required");
    auto loaded = modules::load_project(
        {manifest, {},
         std_binding.empty() ? std::optional<fs::path>{}
                             : std::optional<fs::path>{std_binding}},
        sources);
    if (!loaded.success())
      return report(sources, loaded);
    // The test helper selects Python before resolution/checking, as the driver
    // does for --backend. It never relabels an already verified native module.
    loaded.value->backend = "python";
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*loaded.value)));
    if (!resolved.success())
      return report(sources, resolved);
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      return report(sources, checked);
    const auto &module = checked.value->module();
    std::optional<SymbolId> entry;
    for (const auto &f : module.functions) {
      const auto &s = module.resolved->symbols.at(f.symbol.value);
      if (s.module == module.identity && s.name == "main")
        entry = s.id;
    }
    if (!entry)
      throw BadArguments("entry module has no main function");
    auto emitted = backend::python::emit(*checked.value, *entry, {runner});
    if (!emitted.success())
      return report(sources, emitted);
    fs::create_directories(out);
    write(out / "program.py", emitted.value->code);
    write(out / "langrt.py", emitted.value->runtime);
    write(out / "verified.hir", hir::dump(*checked.value));
    auto *raw = json_object_new_object();
    std::unique_ptr<json_object, decltype(&json_object_put)> metadata(
        raw, json_object_put);
    json_object_object_add(raw, "contract",
                           json_object_new_string("compiler-dev/1"));
    json_object_object_add(raw, "backend", json_object_new_string("python"));
    json_object_object_add(raw, "word_bits", json_object_new_int(64));
    json_object_object_add(raw, "entry_symbol",
                           json_object_new_uint64(entry->value));
    auto *symbols = json_object_new_array();
    for (const auto &s : emitted.value->symbols) {
      auto *item = json_object_new_object();
      json_object_object_add(item, "symbol",
                             json_object_new_uint64(s.symbol.value));
      json_object_object_add(item, "generated",
                             json_object_new_string(s.name.c_str()));
      json_object_object_add(
          item, "source_name",
          json_object_new_string(
              module.resolved->symbols.at(s.symbol.value).name.c_str()));
      json_object_array_add(symbols, item);
    }
    json_object_object_add(raw, "symbols", symbols);
    // Test observers check the concrete HIR signature before calling a probe.
    auto scalar = [&](TypeId id) {
      const auto &t = *module.types.find(id);
      switch (t.kind) {
      case hir::TypeKind::Float: return "f" + std::to_string(t.bits);
      case hir::TypeKind::SignedInt: return "i" + std::to_string(t.bits);
      case hir::TypeKind::UnsignedInt: return "u" + std::to_string(t.bits);
      case hir::TypeKind::Bool: return std::string("bool");
      case hir::TypeKind::Unit: return std::string("Unit");
      default: return std::string("non_scalar");
      }
    };
    auto *functions = json_object_new_array();
    for (const auto &f : module.functions) {
      const auto &signature = *module.types.find(f.signature);
      auto *item = json_object_new_object();
      auto generated = "_f" + std::to_string(f.symbol.value);
      json_object_object_add(item, "generated", json_object_new_string(generated.c_str()));
      json_object_object_add(item, "source_name", json_object_new_string(
          module.resolved->symbols.at(f.symbol.value).name.c_str()));
      json_object_object_add(item, "result", json_object_new_string(scalar(signature.result).c_str()));
      auto *parameters = json_object_new_array();
      for (const auto &p : signature.parameters)
        json_object_array_add(parameters, json_object_new_string(scalar(p.type).c_str()));
      json_object_object_add(item, "parameters", parameters);
      json_object_array_add(functions, item);
    }
    json_object_object_add(raw, "functions", functions);
    auto *lines = json_object_new_array();
    for (const auto &s : emitted.value->source_map) {
      auto *item = json_object_new_object();
      json_object_object_add(item, "line", json_object_new_uint64(s.line));
      json_object_object_add(item, "snapshot",
                             json_object_new_uint64(s.source.snapshot.value));
      json_object_object_add(item, "start_byte",
                             json_object_new_uint64(s.source.begin_byte));
      json_object_object_add(item, "end_byte",
                             json_object_new_uint64(s.source.end_byte));
      json_object_object_add(
          item, "path",
          json_object_new_string(sources.get(s.source.snapshot)->path.c_str()));
      json_object_array_add(lines, item);
    }
    json_object_object_add(raw, "source_map", lines);
    write(out / "metadata.json",
          json_object_to_json_string_ext(raw, JSON_C_TO_STRING_PRETTY));
    return report(sources, emitted);
  } catch (const BadArguments &e) {
    return failure(sources, "E_DRIVER_ARGUMENT", e,
                   ExitCode::environment_error);
  } catch (const std::filesystem::filesystem_error &e) {
    return failure(sources, "E_INPUT_IO", e, ExitCode::environment_error);
  } catch (const std::ios_base::failure &e) {
    return failure(sources, "E_INPUT_IO", e, ExitCode::environment_error);
  } catch (const std::exception &e) {
    return failure(sources, "E_INTERNAL_ERROR", e, ExitCode::internal_error);
  }
}

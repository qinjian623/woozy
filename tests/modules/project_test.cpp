#include "lang/modules/project.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace lang;
namespace fs = std::filesystem;
void require(bool b, const char *m) {
  if (!b)
    throw std::runtime_error(m);
}
void write(const fs::path &p, const std::string &s) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << s;
  require(bool(f), "write");
}
bool code(const auto &r, const std::string &c) {
  for (auto &d : r.diagnostics)
    if (d.code == c)
      return true;
  return false;
}
int main() {
  auto root = fs::temp_directory_path() /
              ("lang-project-test-" + std::to_string(getpid()));
  try {
    write(root / "lang.toml", R"(schema_version=1
[package]
name="demo"
source_root="src"
entry="app"
[dependencies.geo]
kind="source"
path="vendor/geo"
[dependencies.alias]
kind="source"
path="vendor/geo"
[dependencies.py]
kind="python"
binding="binding.json"
host_package="host"
)");
    write(root / "src/a.lang",
          "package demo; import \"geo\"; import \"alias\"; import \"py\" as p; "
          "pub func main() {}\n");
    write(root / "src/b.lang", "package demo; pub const N = 1;\n");
    write(root / "vendor/geo/lang.toml",
          "schema_version=1\n[package]\nname=\"geometry\"\nsource_root="
          "\"lib\"\nentry=\"app\"\n");
    write(root / "vendor/geo/lib/g.lang",
          "package geometry; pub func area() {}\n");
    write(
        root / "binding.json",
        R"({"schema_version":1,"contract":"compiler-dev/1","package_requirements":[{"package":"host"}],"modules":[{"logical_path":"py","host_module":"host"},{"logical_path":"secret","host_module":"secret"}],"types":[{"module":"py","name":"Thing","id":"host.Thing"}],"values":[{"module":"py","name":"v"}],"callables":[{"module":"py","name":"Thing","call_kind":"constructor"},{"module":"py","name":"method","call_kind":"method","owner_type":"host.Thing"},{"module":"py","name":"run","call_kind":"function","id":"host.run"}]})");
    modules::ProjectOptions opts{root / "lang.toml", {}, {}};
    SourceManager sm;
    auto r = modules::load_project(opts, sm);
    if (!r.success()) {
      for (auto &d : r.diagnostics)
        std::cerr << d.message << '\n';
    }
    require(r.success(), "valid project");
    require(r.value->modules.size() == 3, "canonical dedup / bundle isolation");
    auto &app = r.value->module(r.value->entry);
    require(app.files.size() == 2 && app.imports.size() == 3, "files/imports");
    require(app.imports[0].target == app.imports[1].target, "alias dedup");
    auto &py = r.value->module(app.imports[2].target);
    require(py.foreign_exports.size() == 3,
            "foreign exports / constructor merge");
    require(r.value->bindings.size() == 1 && !r.value->bindings[0].json.empty(),
            "raw binding");
    require(sm.get(app.imports[0].span.snapshot)->path.ends_with("a.lang"),
            "source snapshot");
    require(app.files[1]->tokens[3].spelling == "pub", "pub retained in AST");
    auto run = [&](const std::string &source) {
      write(root / "src/a.lang", source);
      return modules::load_project(opts, sm);
    };
    auto bad = run("package demo; import \"app//bad\"; func main() {}");
    require(!bad.success() && code(bad, "E_IMPORT_PATH") &&
                bad.exit_code() == ExitCode::source_error,
            "bad logical path");
    auto mismatch = run("package other; func main() {}");
    require(!mismatch.success() && code(mismatch, "E_PACKAGE_MISMATCH"),
            "package mismatch");
    write(root / "src/sub/s.lang", "package sub; import \"app\"; func f() {}");
    auto loop = run("package demo; import \"app/sub\"; func main() {}");
    require(!loop.success() && code(loop, "E_IMPORT_CYCLE"), "loop");
    bool related = false;
    for (auto &d : loop.diagnostics)
      if (d.code == "E_IMPORT_CYCLE") {
        related = !d.related.empty();
        require(d.message.find("app -> app/sub -> app") != std::string::npos,
                "full cycle");
      }
    require(related, "loop related locations");
    auto missing = run("package demo; import \"app/missing\"; func main() {}");
    require(!missing.success() &&
                missing.exit_code() == ExitCode::environment_error,
            "missing file classification");
    auto undeclared = run("package demo; import \"secret\"; func main() {}");
    require(!undeclared.success() && code(undeclared, "E_IMPORT_NOT_FOUND"),
            "bundle must not open undeclared modules");
    auto no_std = run("package demo; import \"std/io\"; func main() {}");
    require(!no_std.success() && code(no_std, "E_IMPORT_NOT_FOUND"),
            "explicit std required");
    write(root / "stdlib/io/io.lang", "package io; pub func print() {}");
    opts.std_source_root = root / "stdlib";
    auto with_std = modules::load_project(opts, sm);
    require(with_std.success() && with_std.value->modules.size() == 2,
            "std source");
    opts.std_source_root.reset();
    write(root / "vendor/geo/lib/g.lang",
          "package geometry; import \"app\"; pub func area() {}");
    auto dep_loop = run("package demo; import \"geo\"; func main() {}");
    require(!dep_loop.success() && code(dep_loop, "E_IMPORT_CYCLE"),
            "dependency app uses dependency source_root");
    write(root / "vendor/geo/lib/g.lang",
          "package geometry; pub func area() {}");
    auto bundle_text = r.value->bindings.front().json;
    opts.std_binding_bundle = root / "binding.json";
    auto absent = run("package demo; import \"std/missing\"; func main() {}");
    require(!absent.success() && code(absent, "E_BINDING_INVALID"),
            "matching module required");
    opts.std_binding_bundle.reset();
    fs::remove(root / "binding.json");
    auto absent_file = run("package demo; import \"py\"; func main() {}");
    require(!absent_file.success() &&
                absent_file.exit_code() == ExitCode::environment_error,
            "missing bundle file");
    write(root / "binding.json", "{bad");
    auto malformed = run("package demo; import \"py\"; func main() {}");
    require(!malformed.success() && code(malformed, "E_BINDING_INVALID") &&
                malformed.exit_code() == ExitCode::source_error,
            "bad JSON classification");
    write(root / "binding.json", bundle_text);
    auto nul_bundle = bundle_text;
    auto logical_name = nul_bundle.find("\"logical_path\":\"py\"");
    require(logical_name != std::string::npos, "fixture module name");
    nul_bundle.replace(logical_name, std::string("\"logical_path\":\"py\"").size(),
                       R"("logical_path":"py\u0000hidden")");
    write(root / "binding.json", nul_bundle);
    auto nul = run("package demo; import \"py\"; func main() {}");
    require(!nul.success() && code(nul, "E_BINDING_INVALID"), "embedded NUL must not truncate module name");
    write(root / "binding.json", bundle_text);
    auto again = run("package demo; import \"geo\"; import \"alias\"; import "
                     "\"py\" as p; pub func main() {}\n");
    require(again.success(), "reload");
    for (size_t i = 0; i < r.value->modules.size(); ++i)
      require(r.value->modules[i].identity ==
                      again.value->modules[i].identity &&
                  again.value->modules[i].id.value == i,
              "deterministic module ids");
    auto rendered = diagnostic_json(sm, "test", "test", missing.status(),
                                    missing.diagnostics);
    require(rendered.find("a.lang") != std::string::npos,
            "format original location");
    auto syntax = run("package demo; func (");
    require(!syntax.success() && syntax.exit_code() == ExitCode::source_error,
            "syntax");
    std::stop_source cancel;
    cancel.request_stop();
    auto stopped = modules::load_project(opts, sm, cancel.get_token());
    require(stopped.exit_code() == ExitCode::cancelled, "cancel");
    write(root / "lang.toml", R"(schema_version=1
[package]
name="demo"
source_root="src\u0000hidden"
entry="app"
)");
    auto nul_path = modules::load_project(opts, sm);
    require(!nul_path.success() && code(nul_path, "E_MANIFEST_INVALID"), "NUL path accepted");
    std::cout << "passed: temporary projects, sorted files, source manifest, "
                 "dedup, pub AST, JSON exports, graph cycle, invalid path, "
                 "package mismatch, missing file, undeclared dependency, "
                 "JSON/syntax errors, cancel\n";
    fs::remove_all(root);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << " (fixture " << root << ")\n";
    return 1;
  }
}

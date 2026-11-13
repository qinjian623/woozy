// C++ reference for the same fixture sources used by the Wz component driver.
// It constructs the real Project/ResolvedProject/HirModule through the public
// lex/parse/resolve/sema/verify APIs and prints the capability result and the
// typed system-call span.  It is a test oracle, not a second implementation of
// the production capability rule.
#include "lang/hir/hir.hpp"
#include "lang/modules/resolve.hpp"
#include "lang/sema/check.hpp"
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace lang;

struct CallSite {
  bool found = false;
  uint32_t operation = 0;
  uint64_t begin = 0, end = 0;
};

struct Report {
  bool ok = false;
  std::string code, phase, message;
  bool primary = false;
  uint64_t begin = 0, end = 0;
  CallSite call;
};

static std::string read_file(const std::string &path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot read " + path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

static std::shared_ptr<const AstFile> parse_file(uint64_t id,
                                                 const std::string &path,
                                                 const std::string &text) {
  auto snapshot = std::make_shared<SourceSnapshot>(
      FileId{id}, RevisionId{id}, SourceSnapshotId{id}, path, text);
  auto lexed = lex(snapshot);
  if (!lexed.success()) throw std::runtime_error("fixture did not lex: " + path);
  auto parsed = parse(*lexed.value);
  if (!parsed.success() || parsed.value->recovered)
    throw std::runtime_error("fixture did not parse: " + path);
  return std::make_shared<const AstFile>(*parsed.value);
}

static AstNodeId first_import(const AstFile &file) {
  for (auto id : file.nodes.at(file.root.value).children)
    if (file.node(id).kind == AstKind::Import) return id;
  throw std::runtime_error("fixture has no import");
}

static std::shared_ptr<modules::Project>
build_project(const std::string &first_path, const std::string &second_path,
              const std::string &logical, const std::string &package_name,
              const std::string &alias, const std::string &backend) {
  auto first = parse_file(1, "app/main.wz", read_file(first_path));
  auto second = parse_file(2, logical + "/lib.wz", read_file(second_path));
  auto project = std::make_shared<modules::Project>();
  project->package_name = "app";
  project->backend = backend;
  project->word_bits = 64;
  project->entry = ModuleId{0};
  modules::Module app;
  app.id = ModuleId{0};
  app.logical_path = app.identity = app.package_name = "app";
  app.files.push_back(first);
  auto import = first_import(*first);
  app.imports.push_back(modules::Import{
      0, import, ModuleId{1}, alias, first->node(import).span});
  modules::Module library;
  library.id = ModuleId{1};
  library.logical_path = library.identity = logical;
  library.package_name = package_name;
  library.files.push_back(second);
  project->modules.push_back(std::move(app));
  project->modules.push_back(std::move(library));
  return project;
}

static std::shared_ptr<modules::Project> build_one(const std::string &path,
                                                    const std::string &backend) {
  auto file = parse_file(1, "app/main.wz", read_file(path));
  auto project = std::make_shared<modules::Project>();
  project->package_name = "app";
  project->backend = backend;
  project->word_bits = 64;
  project->entry = ModuleId{0};
  modules::Module app;
  app.id = ModuleId{0};
  app.logical_path = app.identity = app.package_name = "app";
  app.files.push_back(file);
  project->modules.push_back(std::move(app));
  return project;
}

static CallSite find_system_call(const hir::HirModule &module,
                                 uint32_t operation) {
  for (const auto &node : module.nodes) {
    const auto *call = std::get_if<hir::Call>(&node.data);
    if (!call || call->kind != hir::CalleeKind::system || !call->system)
      continue;
    auto value = static_cast<uint32_t>(*call->system);
    if (value == operation)
      return CallSite{true, value, node.source.span.begin_byte,
                      node.source.span.end_byte};
  }
  return {};
}

static Report run(const std::shared_ptr<modules::Project> &project,
                  uint32_t operation) {
  Report report;
  auto resolved = modules::resolve_project(project);
  if (!resolved.success())
    throw std::runtime_error("fixture did not resolve");
  auto typed = sema::check_types(resolved);
  if (!typed.success()) throw std::runtime_error("fixture did not type-check");
  report.call = find_system_call(*typed.value, operation);
  auto verified = hir::verify(*typed.value, {}, hir::VerifyMode::backend);
  report.ok = verified.success();
  if (!verified.diagnostics.empty()) {
    const auto &diagnostic = verified.diagnostics.front();
    report.code = diagnostic.code;
    report.phase = diagnostic.phase;
    report.message = diagnostic.message;
    if (diagnostic.primary) {
      report.primary = true;
      report.begin = diagnostic.primary->begin_byte;
      report.end = diagnostic.primary->end_byte;
    }
  }
  return report;
}

static std::string text_or_none(const std::string &value) {
  return value.empty() ? "none" : value;
}

static void print(const char *label, const Report &report) {
  std::cout << "CPP " << label << " result=" << (report.ok ? "accept" : "reject")
            << " code=" << text_or_none(report.code)
            << " phase=" << text_or_none(report.phase)
            << " primary=" << (report.primary ? 1 : 0)
            << " begin=" << report.begin << " end=" << report.end
            << " call=" << (report.call.found ? 1 : 0)
            << " operation=" << report.call.operation
            << " call-begin=" << report.call.begin
            << " call-end=" << report.call.end
            << " message=" << text_or_none(report.message) << "\n";
}

int main(int argc, char **argv) {
  try {
    if (argc != 2) return 2;
    std::string root = argv[1];
    uint32_t os_operation =
        static_cast<uint32_t>(hir::SystemOperation::text_length);
    uint32_t text_operation = static_cast<uint32_t>(hir::SystemOperation::text_len);
    auto native_only_native = run(build_project(
        root + "/native_only/main.wz", root + "/native_only/os.wz",
        "std/os", "os", "os", "native"), os_operation);
    auto native_only_python = run(build_project(
        root + "/native_only/main.wz", root + "/native_only/os.wz",
        "std/os", "os", "os", "python"), os_operation);
    auto value_python = run(build_project(
        root + "/value/main.wz", root + "/value/text.wz",
        "std/text", "text", "text", "python"), text_operation);
    auto ordinary_python = run(build_one(
        root + "/ordinary/main.wz", "python"), text_operation);
    print("native-only-native", native_only_native);
    print("native-only-python", native_only_python);
    print("value-python", value_python);
    print("ordinary-python", ordinary_python);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "CPP oracle failed: " << error.what() << "\n";
    return 1;
  }
}

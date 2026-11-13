// Test-only C++ oracle for the pure-Wz resolver.  It builds the same fixture
// Project records from the real C++ source/lexer/parser packages and prints the
// exact field order consumed by compare.py.  It is never linked into or called
// by the self-host compiler.
#include "lang/modules/resolve.hpp"
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/syntax/token.hpp"

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace lang;
using namespace lang::modules;

std::string read_text(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

std::string join_path(const std::string &base, const std::string &name) {
  if (!base.empty() && base.back() == '/') return base + name;
  return base + "/" + name;
}

std::vector<std::string> split(const std::string &text, char separator) {
  std::vector<std::string> out;
  std::string part;
  for (char c : text) {
    if (c == separator) {
      out.push_back(part);
      part.clear();
    } else {
      part += c;
    }
  }
  out.push_back(part);
  return out;
}

std::string trim(const std::string &value) {
  size_t begin = 0, end = value.size();
  while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                         value[begin] == '\r' || value[begin] == '\n'))
    ++begin;
  while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                         value[end - 1] == '\r' || value[end - 1] == '\n'))
    --end;
  return value.substr(begin, end - begin);
}

size_t number(const std::string &value) {
  try {
    return static_cast<size_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

AstNodeId import_node(const AstFile &file, size_t ordinal) {
  size_t found = 0;
  for (AstNodeId id : file.node(file.root).children) {
    if (file.node(id).kind != AstKind::Import) continue;
    if (found == ordinal) return id;
    ++found;
  }
  return AstNodeId(0);
}

std::string span_text(const SourceSpan &span) {
  return std::to_string(span.snapshot.value) + ":" +
         std::to_string(span.begin_byte) + ":" +
         std::to_string(span.end_byte);
}

std::string node_text(const NodeRef &value) {
  return std::to_string(value.module.value) + ":" +
         std::to_string(value.file) + ":" + std::to_string(value.node.value);
}

std::string option_span(const std::optional<SourceSpan> &value) {
  return value ? span_text(*value) : "-";
}

std::string option_node(const std::optional<NodeRef> &value) {
  return value ? node_text(*value) : "-";
}

std::string option_usize(const std::optional<ModuleId> &value) {
  return value ? std::to_string(value->value) : "-";
}

std::string option_foreign(const std::optional<ForeignExportRef> &value) {
  if (!value) return "-";
  return std::to_string(value->module.value) + ":" +
         std::to_string(value->index);
}

Project build_project(const std::string &case_dir) {
  Project project;
  project.entry = ModuleId(0);
  SourceManager sources;
  const std::string manifest = read_text(join_path(case_dir, "project.txt"));
  const auto lines = split(manifest, '\n');
  size_t module_count = 0;
  for (const auto &line : lines) {
    const auto entry = trim(line);
    if (entry.empty() || entry[0] == '#') continue;
    const auto fields = split(entry, ' ');
    if (fields.size() < 2 || fields[0] != "module") continue;
    Module module{ModuleId(module_count), fields[1], fields[1], "p"};
    for (size_t index = 2; index < fields.size(); ++index) {
      const std::string path = join_path(case_dir, fields[index]);
      auto source = sources.update(sources.add_file(path), read_text(path));
      auto lexed = lex(source);
      if (!lexed.success())
        throw std::runtime_error("fixture lex failed: " + path);
      auto parsed = parse(*lexed.value);
      if (!parsed.success())
        throw std::runtime_error("fixture parse failed: " + path);
      module.files.push_back(
          std::make_shared<const AstFile>(std::move(*parsed.value)));
    }
    project.modules.push_back(std::move(module));
    ++module_count;
  }
  for (const auto &line : lines) {
    const auto entry = trim(line);
    if (entry.empty() || entry[0] == '#') continue;
    const auto fields = split(entry, ' ');
    if (fields.size() >= 6 && fields[0] == "import") {
      const size_t module_index = number(fields[1]);
      const size_t file_index = number(fields[2]);
      const size_t target = number(fields[3]);
      auto &module = project.modules.at(module_index);
      const auto &file = *module.files.at(file_index);
      const AstNodeId id = import_node(file, number(fields[5]));
      module.imports.push_back(Import{file_index, id, ModuleId(target),
                                      fields[4], file.node(id).span});
    }
  }
  for (const auto &line : lines) {
    const auto entry = trim(line);
    if (entry.empty() || entry[0] == '#') continue;
    const auto fields = split(entry, ' ');
    if (fields.size() >= 5 && fields[0] == "foreign") {
      const size_t module_index = number(fields[1]);
      const size_t file_index = number(fields[4]);
      const auto &file =
          *project.modules.at(module_index).files.at(file_index);
      const size_t declaration_index = project.foreign_bindings.size();
      ForeignDeclaration declaration;
      declaration.id = ForeignBindingId(declaration_index);
      declaration.bundle = 0;
      declaration.source = file.node(file.root).span;
      declaration.identity = fields[2];
      declaration.module = "host";
      declaration.name = fields[2];
      declaration.detail = BindingValue{"i32"};
      declaration.stable_identity = fields[2];
      project.foreign_bindings.push_back(std::move(declaration));
      ForeignExport export_value;
      export_value.name = fields[2];
      export_value.identity = fields[2];
      export_value.kind = number(fields[3]) == 0
                              ? ForeignExportKind::type
                              : number(fields[3]) == 2
                                    ? ForeignExportKind::function
                                    : ForeignExportKind::value;
      export_value.bundle = 0;
      export_value.declaration = ForeignBindingId(declaration_index);
      project.modules.at(module_index).foreign_exports.push_back(
          std::move(export_value));
    }
  }
  return project;
}

void dump(const PhaseResult<ResolvedProject> &result) {
  std::cout << "H|" << static_cast<int>(result.state) << "|"
            << static_cast<int>(result.failure) << "|"
            << (result.value ? 1 : 0) << "|" << result.diagnostics.size()
            << "\n";
  for (const auto &diagnostic : result.diagnostics) {
    std::cout << "D|" << diagnostic.code << "|" << diagnostic.phase << "|"
              << diagnostic.severity << "|" << diagnostic.message << "|"
              << (diagnostic.primary ? 1 : 0) << "|"
              << (diagnostic.primary ? span_text(*diagnostic.primary) : "-")
              << "|" << diagnostic.related.size() << "\n";
    for (const auto &related : diagnostic.related)
      std::cout << "R|" << span_text(related.span) << "|" << related.message
                << "\n";
  }
  if (!result.value) return;
  const auto &value = *result.value;
  std::cout << "N|" << value.symbols.size() << "\n";
  for (const auto &symbol : value.symbols) {
    std::cout << "S|" << symbol.id.value << "|" << symbol.name << "|"
              << static_cast<int>(symbol.kind) << "|" << symbol.module.value
              << "|" << option_node(symbol.definition) << "|"
              << option_span(symbol.declaration) << "|"
              << (symbol.exported ? 1 : 0) << "|" << option_usize(symbol.target)
              << "|" << option_foreign(symbol.foreign) << "\n";
  }
  std::cout << "O|" << value.occurrences.size() << "\n";
  for (const auto &occurrence : value.occurrences)
    std::cout << "O|" << node_text(occurrence.source) << "|"
              << occurrence.symbol.value << "|" << span_text(occurrence.span)
              << "\n";
  std::cout << "M|" << value.deferred_members.size() << "\n";
  for (const auto &member : value.deferred_members)
    std::cout << "M|" << node_text(member) << "\n";
  std::cout << "P|" << value.self_parameters.size() << "\n";
  for (const auto &parameter : value.self_parameters)
    std::cout << "P|" << node_text(parameter.first) << "|"
              << parameter.second.value << "\n";
  std::cout << "E\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: oracle CASE_DIR\n";
    return 2;
  }
  try {
    auto project = build_project(argv[1]);
    auto result = resolve_project(
        std::make_shared<Project>(std::move(project)));
    dump(result);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "oracle: " << error.what() << "\n";
    return 1;
  }
}

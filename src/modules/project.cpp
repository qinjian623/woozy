#include "lang/modules/project.hpp"
#include "lang/support/input.hpp"
#include <algorithm>
#include <functional>
#include <json-c/json.h>
#include <map>
#include <set>
#include <toml++/toml.hpp>
namespace lang::modules {
namespace {
namespace fs = std::filesystem;
struct Abort {};
bool ident(std::string_view s) {
  auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  if (s.empty() || !alpha(s[0]))
    return false;
  for (char c : s)
    if (!alpha(c) && !(c >= '0' && c <= '9'))
      return false;
  return true;
}
bool logical(std::string_view s) {
  size_t p = 0;
  for (;;) {
    auto e = s.find('/', p);
    if (!ident(s.substr(p, e == s.npos ? s.size() - p : e - p)))
      return false;
    if (e == s.npos)
      return true;
    p = e + 1;
  }
}
struct Dependency {
  std::string kind, host;
  fs::path source, binding;
};
struct Config {
  fs::path root;
  std::string name, entry;
  std::map<std::string, Dependency> deps;
};
struct Loader {
  SourceManager &sm;
  std::stop_token stop;
  ProjectOptions options;
  PhaseResult<Project> result;
  Project project;
  std::map<std::string, Config> configs;
  std::map<std::string, ModuleId> identities;
  std::map<std::string, std::string> logical_sources;
  std::map<std::string, size_t> bundle_ids;
  std::vector<std::shared_ptr<const SourceSnapshot>> bundle_sources;
  std::vector<std::shared_ptr<json_object>> bundle_objects;
  Loader(SourceManager &manager, std::stop_token token,
         const ProjectOptions &opts)
      : sm(manager), stop(token), options(opts) {}
  void error(std::string code, std::string msg,
             std::optional<SourceSpan> span = {},
             FailureKind kind = FailureKind::source) {
    if (code == "E_MANIFEST_INVALID") kind = FailureKind::environment;
    Diagnostic d;
    d.code = std::move(code);
    d.phase = "resolve";
    d.severity = "error";
    d.message = std::move(msg);
    d.primary = span;
    result.diagnostics.push_back(std::move(d));
    if (kind == FailureKind::environment)
      result.failure = kind;
  }
  [[noreturn]] void fail(std::string code, std::string msg,
                         std::optional<SourceSpan> span = {},
                         FailureKind kind = FailureKind::source) {
    error(std::move(code), std::move(msg), span, kind);
    throw Abort{};
  }
  std::shared_ptr<const SourceSnapshot> read(const fs::path &p) {
    check_cancelled(stop);
    auto r = read_source(p, sm, stop);
    if (r.state == PhaseState::cancelled)
      throw Cancelled{};
    if (!r.success()) {
      result.failure = r.failure;
      result.diagnostics.insert(result.diagnostics.end(), r.diagnostics.begin(),
                                r.diagnostics.end());
      throw Abort{};
    }
    return *r.value;
  }
  fs::path canonical(const fs::path &p) {
    check_cancelled(stop);
    return fs::canonical(p);
  }
  Config config(const fs::path &manifest) {
    auto path = canonical(manifest);
    auto key = path.string();
    if (configs.contains(key))
      return configs.at(key);
    auto src = read(path);
    toml::table table;
    try {
      table = toml::parse(src->normalized(), path.string());
    } catch (const toml::parse_error &e) {
      fail("E_MANIFEST_INVALID", std::string(e.description()),
           src->raw_span(0, src->original.size()));
    }
    auto span = src->raw_span(0, src->original.size());
    bool main_manifest = path == canonical(options.manifest);
    if (table["schema_version"].value<int64_t>() != 1)
      fail("E_MANIFEST_INVALID", "schema_version must be 1", span);
    auto required = [&](auto view, const char *key) {
      auto s = view[key].template value<std::string>();
      if (!s || s->empty() || s->find('\0') != std::string::npos)
        fail("E_MANIFEST_INVALID", std::string("missing or invalid ") + key, span);
      return *s;
    };
    Config c;
    c.name = required(table["package"], "name");
    if (!ident(c.name))
      fail("E_MANIFEST_INVALID", "invalid package name", span);
    c.root = canonical(path.parent_path() /
                       required(table["package"], "source_root"));
    if (table["package"]["entry"] && !table["package"]["entry"].is_string())
      fail("E_MANIFEST_INVALID", "entry must be a string", span);
    c.entry = main_manifest ? required(table["package"], "entry")
                           : table["package"]["entry"].value_or(std::string("app"));
    if (!logical(c.entry))
      fail("E_MANIFEST_INVALID", "entry must be a logical module", span);
    if (auto deps = table["dependencies"].as_table())
      for (auto &&[name, value] : *deps) {
        std::string n(name.str());
        if (!ident(n) || n == "std" || n == "app")
          fail("E_MANIFEST_INVALID", "reserved or invalid dependency: " + n, span);
        auto t = value.as_table();
        if (!t)
          fail("E_MANIFEST_INVALID", "dependency must be a table", span);
        Dependency d;
        d.kind = required((*t), "kind");
        if (d.kind == "source")
          d.source = path.parent_path() / required((*t), "path");
        else if (d.kind == "python") {
          d.binding = path.parent_path() / required((*t), "binding");
          d.host = required((*t), "host_package");
        } else if (d.kind == "native")
          fail("E_CAPABILITY_UNAVAILABLE",
               "native ABI module loading requires T11", span);
        else
          fail("E_MANIFEST_INVALID", "unknown dependency kind", span);
        c.deps.emplace(n, std::move(d));
      }
    else if (table.contains("dependencies"))
      fail("E_MANIFEST_INVALID", "dependencies must be a table", span);
    if (table["build"] && !table["build"].is_table())
      fail("E_MANIFEST_INVALID", "build must be a table", span);
    if (main_manifest) {
      if (!(c.entry == "app" || c.entry.starts_with("app/")))
        fail("E_MANIFEST_INVALID", "project entry must be an app logical module", span);
      project.package_name = c.name;
      project.backend =
          table["build"]["backend"].value_or(std::string("native"));
      if (project.backend != "native" && project.backend != "python")
        fail("E_MANIFEST_INVALID", "invalid backend", span);
      if (table["build"]["word_bits"] &&
          !table["build"]["word_bits"].is_integer())
        fail("E_MANIFEST_INVALID", "word_bits must be an integer", span);
      if (table["build"]["backend"] && !table["build"]["backend"].is_string())
        fail("E_MANIFEST_INVALID", "backend must be a string", span);
      auto bits = table["build"]["word_bits"].value_or(int64_t(64));
      if (bits != 32 && bits != 64)
        fail("E_MANIFEST_INVALID", "word_bits must be 32 or 64", span);
      project.word_bits = static_cast<unsigned>(bits);
    }
    configs.emplace(key, c);
    return c;
  }
  json_object *field(json_object *o, const char *key) {
    json_object *v = nullptr;
    json_object_object_get_ex(o, key, &v);
    return v;
  }
  std::string str(json_object *o, const char *key) {
    auto v = field(o, key);
    return v && json_object_is_type(v, json_type_string)
               ? std::string(json_object_get_string(v), json_object_get_string_len(v))
               : "";
  }
  size_t bundle(const fs::path &p) {
    auto path = canonical(p);
    auto key = path.string();
    if (bundle_ids.contains(key))
      return bundle_ids.at(key);
    auto src = read(path);
    auto span = src->raw_span(0, src->original.size());
    auto tok = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(
        json_tokener_new(), json_tokener_free);
    json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT);
    auto raw = json_tokener_parse_ex(tok.get(), src->original.data(),
                                     static_cast<int>(src->original.size()));
    auto obj = std::shared_ptr<json_object>(raw, json_object_put);
    auto end = json_tokener_get_parse_end(tok.get());
    if (json_tokener_get_error(tok.get()) != json_tokener_success || !raw ||
        !json_object_is_type(raw, json_type_object))
      fail("E_BINDING_INVALID", "invalid JSON binding bundle", span);
    for (size_t i = end; i < src->original.size(); ++i)
      if (src->original[i] != ' ' && src->original[i] != '\n' &&
          src->original[i] != '\r' && src->original[i] != '\t')
        fail("E_BINDING_INVALID", "trailing JSON content", span);
    if (!json_object_is_type(field(raw, "schema_version"), json_type_int) ||
        json_object_get_int(field(raw, "schema_version")) != 1 ||
        str(raw, "contract") != "compiler-dev/1")
      fail("E_BINDING_INVALID", "invalid binding version", span);
    for (auto key2 : {"modules", "types", "values", "callables"})
      if (!json_object_is_type(field(raw, key2), json_type_array))
        fail("E_BINDING_INVALID", std::string("missing binding array: ") + key2, span);
    std::set<std::string> mods;
    auto ms = field(raw, "modules");
    for (size_t i = 0; i < json_object_array_length(ms); ++i) {
      auto m = json_object_array_get_idx(ms, i);
      auto n = str(m, "logical_path");
      if (!logical(n) || str(m, "host_module").empty() ||
          !mods.insert(n).second)
        fail("E_BINDING_INVALID", "invalid or duplicate binding module", span);
    }
    size_t id = project.bindings.size();
    project.bindings.push_back({path, src->original});
    bundle_ids.emplace(key, id);
    bundle_sources.push_back(src);
    bundle_objects.push_back(std::move(obj));
    return id;
  }
  ModuleId foreign(std::string name, const fs::path &path,
                   const std::string &host, std::optional<SourceSpan> where) {
    auto b = bundle(path);
    auto obj = bundle_objects[b].get();
    auto span =
        bundle_sources[b]->raw_span(0, bundle_sources[b]->original.size());
    bool found = false;
    auto ms = field(obj, "modules");
    for (size_t i = 0; i < json_object_array_length(ms); ++i)
      if (str(json_object_array_get_idx(ms, i), "logical_path") == name)
        found = true;
    if (!found)
      fail("E_BINDING_INVALID", "bundle does not declare module " + name,
           where ? where : std::optional<SourceSpan>(span));
    if (!host.empty()) {
      bool matched = false;
      auto req = field(obj, "package_requirements");
      if (json_object_is_type(req, json_type_array))
        for (size_t i = 0; i < json_object_array_length(req); ++i)
          if (str(json_object_array_get_idx(req, i), "package") == host)
            matched = true;
      if (!matched)
        fail("E_BINDING_INVALID", "bundle does not declare host package " + host, span);
    }
    std::string identity =
        "binding:" + project.bindings[b].path.string() + ":" + name;
    claim(name, identity, where);
    if (identities.contains(identity))
      return identities.at(identity);
    ModuleId id(project.modules.size());
    identities.emplace(identity, id);
    project.modules.push_back({id, name, identity, name, {}, {}, {}});
    std::map<std::string, ForeignExport> exports;
    for (auto key : {"types", "values", "callables"}) {
      auto arr = field(obj, key);
      for (size_t i = 0; i < json_object_array_length(arr); ++i) {
        check_cancelled(stop);
        auto e = json_object_array_get_idx(arr, i);
        if (str(e, "module") != name)
          continue;
        auto n = str(e, "name");
        if (!ident(n))
          fail("E_BINDING_INVALID", "invalid export name", span);
        auto owner = field(e, "owner_type");
        auto kind = str(e, "call_kind");
        if (std::string_view(key) == "callables" &&
            (kind == "method" ||
             (owner && !json_object_is_type(owner, json_type_null))))
          continue;
        bool type = std::string_view(key) == "types",
             ctor = kind == "constructor";
        if (ctor) {
          if (!exports.contains(n) ||
              exports.at(n).kind != ForeignExportKind::type)
            fail("E_BINDING_INVALID", "constructor requires same-name type", span);
          continue;
        }
        auto identity2 = str(e, "id");
        if (identity2.empty())
          identity2 = identity + ":" + n;
        ForeignExport ex{n, identity2,
                         type ? ForeignExportKind::type
                         : std::string_view(key) == "values"
                             ? ForeignExportKind::value
                             : ForeignExportKind::function,
                         b};
        if (!exports.emplace(n, std::move(ex)).second)
          fail("E_BINDING_INVALID", "duplicate export " + n, span);
      }
    }
    for (auto &[n, e] : exports)
      project.modules[id.value].foreign_exports.push_back(std::move(e));
    return id;
  }
  void claim(const std::string &logical_name, const std::string &identity,
             std::optional<SourceSpan> where) {
    auto [it, ok] = logical_sources.emplace(logical_name, identity);
    if (!ok && it->second != identity)
      fail("E_MODULE_CONFLICT",
           "different sources for logical module " + logical_name, where);
  }
  ModuleId resolve(const std::string &name, const Config &context,
                   std::optional<SourceSpan> where = {}) {
    check_cancelled(stop);
    if (!logical(name))
      fail("E_IMPORT_PATH", "invalid logical import: " + name, where);
    auto slash = name.find('/');
    auto head = name.substr(0, slash);
    auto suffix = slash == name.npos ? std::string{} : name.substr(slash + 1);
    Config c = context;
    fs::path root;
    if (head == "app")
      root = c.root;
    else if (head == "std") {
      if (options.std_source_root && options.std_binding_bundle)
        fail("E_MODULE_CONFLICT", "two explicit std sources", where);
      if (options.std_binding_bundle)
        return foreign(name, *options.std_binding_bundle, "", where);
      if (!options.std_source_root)
        fail("E_IMPORT_NOT_FOUND", "std source is not configured", where);
      root = *options.std_source_root;
    } else {
      auto it = c.deps.find(head);
      if (it == c.deps.end())
        fail("E_IMPORT_NOT_FOUND", "undeclared dependency " + head, where);
      auto dep = it->second;
      if (dep.kind == "python")
        return foreign(name, dep.binding, dep.host, where);
      root = canonical(dep.source);
      if (fs::exists(root / "lang.toml")) {
        c = config(root / "lang.toml");
        root = c.root;
      } else {
        c.root = root;
        c.deps.clear();
      }
    }
    auto dir = canonical(suffix.empty() ? root : root / suffix);
    // Canonical directory is the source module identity, including symlink
    // aliases.
    std::string identity = "source:" + dir.string();
    claim(head == "app" ? "app@" + c.root.string() + ":" + name : name,
          identity, where);
    if (identities.contains(identity))
      return identities.at(identity);
    ModuleId id(project.modules.size());
    identities.emplace(identity, id);
    project.modules.push_back({id, name, identity, "", {}, {}, {}});
    std::vector<fs::path> paths;
    for (auto &e : fs::directory_iterator(dir)) {
      check_cancelled(stop);
      if (e.is_regular_file() && e.path().extension() == ".lang")
        paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty())
      fail("E_MODULE_EMPTY", "no .lang files in " + dir.string(), where,
           FailureKind::environment);
    for (auto &p : paths) {
      auto src = read(p);
      auto lexed = lex(src, stop);
      if (lexed.state == PhaseState::cancelled)
        throw Cancelled{};
      result.diagnostics.insert(result.diagnostics.end(),
                                lexed.diagnostics.begin(),
                                lexed.diagnostics.end());
      if (!lexed.success())
        throw Abort{};
      auto parsed = parse(*lexed.value, stop);
      if (parsed.state == PhaseState::cancelled)
        throw Cancelled{};
      result.diagnostics.insert(result.diagnostics.end(),
                                parsed.diagnostics.begin(),
                                parsed.diagnostics.end());
      if (!parsed.success())
        throw Abort{};
      auto ast = std::make_shared<const AstFile>(std::move(*parsed.value));
      auto pkg = ast->node(ast->node(ast->root).children.front());
      auto package = std::string(ast->tokens.at(pkg.token_begin + 1).spelling);
      if (project.modules[id.value].files.empty())
        project.modules[id.value].package_name = package;
      else if (project.modules[id.value].package_name != package) {
        error("E_PACKAGE_MISMATCH", "inconsistent package name", pkg.span);
        const auto &first = *project.modules[id.value].files.front();
        result.diagnostics.back().related.push_back(
            {first.node(first.node(first.root).children.front()).span,
             "first package declaration"});
        throw Abort{};
      }
      project.modules[id.value].files.push_back(std::move(ast));
    }
    auto count = project.modules[id.value].files.size();
    for (size_t f = 0; f < count; ++f) {
      auto ast = project.modules[id.value].files[f];
      for (auto node : ast->node(ast->root).children) {
        const auto &n = ast->node(node);
        if (n.kind != AstKind::Import)
          continue;
        auto path = ast->tokens.at(n.token_begin + 1).string_value;
        if (!path || !logical(*path)) {
          error("E_IMPORT_PATH", "invalid logical import", n.span);
          continue;
        }
        std::string alias = path->substr(path->find_last_of('/') == path->npos
                                             ? 0
                                             : path->find_last_of('/') + 1);
        if (n.token_end - n.token_begin > 3)
          alias = std::string(ast->tokens.at(n.token_begin + 3).spelling);
        try {
          auto target = resolve(*path, c, n.span);
          project.modules[id.value].imports.push_back(
              {f, node, target, alias, n.span});
        } catch (const Abort &) { /* retain other import diagnostics */
        } catch (const fs::filesystem_error &e) {
          error("E_INPUT_IO", e.what(), n.span, FailureKind::environment);
        }
      }
    }
    return id;
  }
  void cycles() {
    std::vector<int> color(project.modules.size());
    std::vector<ModuleId> stack;
    std::vector<SourceSpan> edges;
    std::function<void(ModuleId)> visit = [&](ModuleId id) {
      check_cancelled(stop);
      color[id.value] = 1;
      stack.push_back(id);
      for (const auto &edge : project.modules[id.value].imports) {
        auto target = edge.target;
        if (color[target.value] == 0) {
          edges.push_back(edge.span);
          visit(target);
          edges.pop_back();
        } else if (color[target.value] == 1) {
          auto start =
              std::find(stack.begin(), stack.end(), target) - stack.begin();
          std::string msg = "import cycle: ";
          for (size_t i = start; i < stack.size(); ++i)
            msg += project.module(stack[i]).logical_path + " -> ";
          msg += project.module(target).logical_path;
          error("E_IMPORT_CYCLE", msg, edge.span);
          for (size_t i = start; i < edges.size(); ++i)
            result.diagnostics.back().related.push_back(
                {edges[i], "cycle import"});
        }
      }
      stack.pop_back();
      color[id.value] = 2;
    };
    for (size_t i = 0; i < project.modules.size(); ++i)
      if (!color[i])
        visit(ModuleId(i));
  }
  PhaseResult<Project> run() {
    result.failure = FailureKind::source;
    try {
      check_cancelled(stop);
      project.root = canonical(options.manifest).parent_path();
      auto c = config(options.manifest);
      project.entry = resolve(c.entry, c);
      cycles();
      check_cancelled(stop);
      result.state = result.diagnostics.empty() ? PhaseState::complete
                                                : PhaseState::recovered;
      result.value = std::move(project);
    } catch (const Cancelled &) {
      result.state = PhaseState::cancelled;
    } catch (const Abort &) {
      result.state = PhaseState::failed;
    } catch (const fs::filesystem_error &e) {
      error("E_INPUT_IO", e.what(), {}, FailureKind::environment);
    }
    return std::move(result);
  }
};
} // namespace
PhaseResult<Project> load_project(const ProjectOptions &options,
                                  SourceManager &sm, std::stop_token stop) {
  return Loader{sm, stop, options}.run();
}
} // namespace lang::modules

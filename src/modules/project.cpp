#include "lang/modules/project.hpp"
#include "lang/support/input.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <json-c/json.h>
#include <map>
#include <limits>
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
bool dotted(std::string_view s) {
  std::string path(s);
  std::replace(path.begin(), path.end(), '.', '/');
  return logical(path) && s.find('/') == s.npos;
}
// SHA-256 of the original bytes, independent of paths and process-local IDs.
std::string sha256(std::string_view bytes) {
  constexpr std::array<uint32_t, 64> k = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  std::array<uint32_t, 8> h = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                               0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  std::vector<unsigned char> data(bytes.begin(), bytes.end());
  uint64_t bits = uint64_t(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<unsigned char>(bits >> (i * 8)));
  for (size_t offset = 0; offset < data.size(); offset += 64) {
    std::array<uint32_t, 64> w{};
    for (size_t i = 0; i < 16; ++i)
      for (size_t j = 0; j < 4; ++j) w[i] = (w[i] << 8) | data[offset + i * 4 + j];
    for (size_t i = 16; i < 64; ++i) {
      auto x = w[i - 15], y = w[i - 2];
      w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) +
             w[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto [a,b,c,d,e,f,g,t] = h;
    for (size_t i = 0; i < 64; ++i) {
      auto s = t + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
               ((e & f) ^ (~e & g)) + k[i] + w[i];
      auto u = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) +
               ((a & b) ^ (a & c) ^ (b & c));
      t=g; g=f; f=e; e=d+s; d=c; c=b; b=a; a=s+u;
    }
    std::array<uint32_t, 8> v = {a,b,c,d,e,f,g,t};
    for (size_t i = 0; i < 8; ++i) h[i] += v[i];
  }
  std::string out;
  for (auto word : h)
    for (int i = 7; i >= 0; --i) out += "0123456789abcdef"[(word >> (i * 4)) & 15];
  return out;
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
  std::map<std::string, SourceSpan> binding_identities;
  std::shared_ptr<const SourceSnapshot> binding_source;
  std::map<std::string, SourceSpan> binding_spans;
  Loader(SourceManager &manager, std::stop_token token,
         const ProjectOptions &opts)
      : sm(manager), stop(token), options(opts) {
#ifdef LANG_STD_SOURCE_ROOT
    if (!options.std_source_root && !options.std_binding_bundle)
      options.std_source_root = fs::path(LANG_STD_SOURCE_ROOT);
#endif
  }
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
  struct Json {
    json_object *value;
    std::string pointer;
  };
  static std::string pointer_part(std::string_view s) {
    std::string out;
    for (char c : s) out += c == '~' ? "~0" : c == '/' ? "~1" : std::string(1, c);
    return out;
  }
  SourceSpan location(const Json &j) const {
    auto it = binding_spans.find(j.pointer);
    return it == binding_spans.end() ? binding_spans.at("") : it->second;
  }
  [[noreturn]] void invalid(const Json &j, std::string message) {
    fail("E_BINDING_INVALID", j.pointer + ": " + message, location(j));
  }
  // json-c validates the syntax. This pass records original byte ranges and
  // rejects duplicate object keys, which json-c otherwise silently replaces.
  void index_json() {
    binding_spans.clear();
    const auto &text = binding_source->original;
    size_t p = 0;
    auto space = [&] {
      while (p < text.size() && (text[p] == ' ' || text[p] == '\t' ||
                                 text[p] == '\r' || text[p] == '\n')) ++p;
    };
    auto string_end = [&] {
      ++p;
      while (text[p] != '"') {
        if (text[p] == '\\') ++p;
        ++p;
      }
      ++p;
    };
    std::function<void(std::string)> visit = [&](std::string path) {
      check_cancelled(stop);
      space();
      size_t begin = p;
      if (text[p] == '{') {
        ++p; space();
        std::set<std::string> keys;
        while (text[p] != '}') {
          size_t key_begin = p;
          string_end();
          auto key_text = text.substr(key_begin, p - key_begin);
          auto key = std::unique_ptr<json_object, decltype(&json_object_put)>(
              json_tokener_parse(key_text.c_str()), json_object_put);
          std::string name(json_object_get_string(key.get()), json_object_get_string_len(key.get()));
          // json-c truncates object keys at NUL; inspect the full decoded key
          // before the object table can hide missing or unknown fields.
          if (name.find('\0') != name.npos)
            fail("E_BINDING_INVALID", "JSON field name contains NUL",
                 binding_source->raw_span(key_begin, p));
          if (!keys.insert(name).second)
            fail("E_BINDING_INVALID", "duplicate JSON field: " + name,
                 binding_source->raw_span(key_begin, p));
          space(); ++p;
          visit(path + "/" + pointer_part(name));
          space();
          if (text[p] != ',') break;
          ++p; space();
        }
        ++p;
      } else if (text[p] == '[') {
        ++p; space();
        size_t i = 0;
        while (text[p] != ']') {
          visit(path + "/" + std::to_string(i++));
          space();
          if (text[p] != ',') break;
          ++p; space();
        }
        ++p;
      } else if (text[p] == '"') string_end();
      else while (p < text.size() && text[p] != ',' && text[p] != ']' &&
                  text[p] != '}' && text[p] != ' ' && text[p] != '\n' &&
                  text[p] != '\r' && text[p] != '\t') ++p;
      binding_spans.emplace(std::move(path), binding_source->raw_span(begin, p));
    };
    visit("");
  }
  void object(Json j, std::initializer_list<const char *> fields) {
    if (!json_object_is_type(j.value, json_type_object)) invalid(j, "expected object");
    for (auto key : fields) {
      json_object *value = nullptr;
      if (!json_object_object_get_ex(j.value, key, &value))
        invalid(j, std::string("missing field: ") + key);
    }
    json_object_object_foreach(j.value, key, value) {
      (void)value;
      if (std::none_of(fields.begin(), fields.end(), [&](auto f) { return std::string_view(f) == key; }))
        invalid({nullptr, j.pointer + "/" + pointer_part(key)}, "unknown field");
    }
  }
  Json field(Json j, const char *key) {
    json_object *v = nullptr;
    if (j.value) json_object_object_get_ex(j.value, key, &v);
    return {v, j.pointer + "/" + key};
  }
  std::vector<Json> array(Json j) {
    if (!json_object_is_type(j.value, json_type_array)) invalid(j, "expected array");
    std::vector<Json> out;
    for (size_t i = 0; i < json_object_array_length(j.value); ++i)
      out.push_back({json_object_array_get_idx(j.value, i), j.pointer + "/" + std::to_string(i)});
    return out;
  }
  std::string string(Json j) {
    if (!json_object_is_type(j.value, json_type_string)) invalid(j, "expected string");
    return {json_object_get_string(j.value), size_t(json_object_get_string_len(j.value))};
  }
  std::string identity(Json j) {
    auto s = string(j);
    if (s.empty() || s.find('\0') != s.npos) invalid(j, "empty identity or embedded NUL");
    return s;
  }
  std::string identifier(Json j) {
    auto s = string(j);
    if (!ident(s)) invalid(j, "expected identifier");
    return s;
  }
  std::string choice(Json j, std::initializer_list<const char *> choices) {
    auto s = string(j);
    if (std::none_of(choices.begin(), choices.end(), [&](auto c) { return s == c; }))
      invalid(j, "invalid enum value");
    return s;
  }
  std::optional<std::string> nullable(Json j) {
    if (json_object_is_type(j.value, json_type_null)) return {};
    return identity(j);
  }
  std::vector<std::string> strings(Json j, bool attributes = false) {
    std::vector<std::string> out;
    for (auto v : array(j)) out.push_back(attributes ? identifier(v) : string(v));
    return out;
  }
  bool boolean(Json j) {
    if (!json_object_is_type(j.value, json_type_boolean)) invalid(j, "expected boolean");
    return json_object_get_boolean(j.value);
  }
  static bool builtin(std::string_view name) {
    for (auto s : {"Unit", "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32",
                   "u64", "isize", "usize", "f32", "f64", "string"})
      if (name == s) return true;
    return false;
  }
  size_t bundle(const fs::path &p) {
    auto path = canonical(p);
    auto key = path.string();
    if (bundle_ids.contains(key)) return bundle_ids.at(key);
    binding_source = read(path);
    auto span = binding_source->raw_span(0, binding_source->original.size());
    const auto &text = binding_source->original;
    auto tok = std::unique_ptr<json_tokener, decltype(&json_tokener_free)>(json_tokener_new(), json_tokener_free);
    json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    if (text.size() >= size_t(std::numeric_limits<int>::max()))
      fail("E_BINDING_INVALID", "binding JSON is too large", span);
    auto raw = json_tokener_parse_ex(tok.get(), text.data(), static_cast<int>(text.size()));
    auto obj = std::unique_ptr<json_object, decltype(&json_object_put)>(raw, json_object_put);
    if (json_tokener_get_error(tok.get()) != json_tokener_success || !raw)
      fail("E_BINDING_INVALID", "invalid JSON binding bundle", span);
    for (size_t i = json_tokener_get_parse_end(tok.get()); i < text.size(); ++i)
      if (text[i] != ' ' && text[i] != '\n' && text[i] != '\r' && text[i] != '\t')
        fail("E_BINDING_INVALID", "trailing JSON content", binding_source->raw_span(i, i + 1));
    index_json();
    Json root{raw, ""};
    object(root, {"schema_version", "contract", "review_status", "allowed_backends",
                  "package_requirements", "modules", "types", "values", "callables", "conformance_required"});
    BindingBundle b;
    b.path = path; b.json = text; b.sha256 = sha256(text); b.source = span;
    auto version = field(root, "schema_version");
    if (!json_object_is_type(version.value, json_type_int) || json_object_get_int64(version.value) != 1)
      invalid(version, "schema_version must be 1");
    b.schema_version = 1;
    b.contract = choice(field(root, "contract"), {"compiler-dev/1"});
    b.review_status = choice(field(root, "review_status"), {"draft_requires_conformance", "verified_against_lock"});
    b.allowed_backends = strings(field(root, "allowed_backends"));
    if (b.allowed_backends != std::vector<std::string>{"python"})
      invalid(field(root, "allowed_backends"), "allowed_backends must be [python]");
    b.conformance_required = strings(field(root, "conformance_required"));
    std::set<std::string> packages;
    for (auto j : array(field(root, "package_requirements"))) {
      object(j, {"package", "version_policy"});
      PackageRequirement req{identity(field(j, "package")), string(field(j, "version_policy")), location(j)};
      if (!packages.insert(req.package).second) invalid(j, "duplicate package requirement");
      b.package_requirements.push_back(std::move(req));
    }
    std::map<std::string, size_t> mods;
    for (auto j : array(field(root, "modules"))) {
      object(j, {"logical_path", "host_module", "provider"});
      BindingModule m{string(field(j, "logical_path")), string(field(j, "host_module")),
                      choice(field(j, "provider"), {"python_package", "language_runtime"}), location(j)};
      if (!logical(m.logical_path)) invalid(field(j, "logical_path"), "invalid logical module");
      if (!dotted(m.host_module)) invalid(field(j, "host_module"), "invalid host module");
      if ((m.logical_path == "std" || m.logical_path.starts_with("std/")) && m.provider != "language_runtime")
        invalid(field(j, "provider"), "std requires language_runtime provider");
      if (m.logical_path == "app" || m.logical_path.starts_with("app/"))
        invalid(field(j, "logical_path"), "app is reserved for source modules");
      if (!mods.emplace(m.logical_path, b.modules.size()).second) invalid(j, "duplicate binding module");
      b.modules.push_back(std::move(m));
    }
    size_t bundle_id = project.bindings.size();
    std::vector<ForeignDeclaration> declarations;
    std::map<std::string, SourceSpan> ids;
    for (auto category : {"types", "values", "callables"}) {
      for (auto j : array(field(root, category))) {
        check_cancelled(stop);
        bool type = std::string_view(category) == "types", value = std::string_view(category) == "values";
        if (type) object(j, {"id", "module", "name", "kind", "host_attribute", "copy"});
        else if (value) object(j, {"module", "name", "type", "host_attribute"});
        else object(j, {"id", "module", "name", "call_kind", "owner_type", "host_attribute", "parameters",
                         "return_type", "error_type", "ownership", "effects", "receiver_escapes"});
        ForeignDeclaration d;
        d.id = ForeignBindingId(project.foreign_bindings.size() + declarations.size());
        d.bundle = bundle_id; d.source = location(j); d.json_pointer = j.pointer;
        d.module = string(field(j, "module")); d.name = identifier(field(j, "name"));
        if (!mods.contains(d.module)) invalid(field(j, "module"), "undeclared module");
        if (d.name == "_") invalid(field(j, "name"), "export cannot be _");
        d.host_attribute = strings(field(j, "host_attribute"), true);
        d.identity = value ? d.module + "/" + d.name : identity(field(j, "id"));
        if (!value) {
          if (builtin(d.identity) || !ids.emplace(d.identity, d.source).second || binding_identities.contains(d.identity))
            invalid(field(j, "id"), "duplicate or reserved declaration id");
        }
        for (const auto &part : {b.contract, b.sha256, d.module, std::string(category), d.identity})
          d.stable_identity += std::to_string(part.size()) + ":" + part;
        if (type) {
          d.detail = BindingType{choice(field(j, "kind"), {"host_reference", "opaque_protocol", "runtime_error"}),
                                 choice(field(j, "copy"), {"shared_reference", "error_payload_copy"})};
        } else if (value) d.detail = BindingValue{identity(field(j, "type"))};
        else {
          BindingCallable c;
          c.call_kind = choice(field(j, "call_kind"), {"function", "constructor", "method", "callable"});
          c.owner_type = nullable(field(j, "owner_type"));
          c.return_type = identity(field(j, "return_type"));
          c.error_type = nullable(field(j, "error_type"));
          if (c.error_type && *c.error_type != "lang.py.Error") invalid(field(j, "error_type"), "invalid error type");
          c.receiver_escapes = choice(field(j, "receiver_escapes"), {"no", "may_retain", "not_applicable"});
          auto own = field(j, "ownership");
          object(own, {"return", "storage"});
          c.return_ownership = choice(field(own, "return"), {"python_reference", "unit", "value"});
          c.storage = choice(field(own, "storage"), {"host_defined", "copy", "view_or_alias", "no_tensor_storage"});
          for (auto e : array(field(j, "effects")))
            c.effects.push_back(choice(e, {"reads_arguments", "mutates_receiver", "retains_arguments", "mutates_host_rng",
                                           "autograd", "consumes_iterator", "retains_receiver"}));
          for (auto p2 : array(field(j, "parameters"))) {
            object(p2, {"name", "type", "kind", "optional", "mode", "escapes"});
            c.parameters.push_back({identifier(field(p2, "name")), identity(field(p2, "type")),
                choice(field(p2, "kind"), {"positional_only", "positional_or_keyword", "keyword_only"}),
                boolean(field(p2, "optional")), choice(field(p2, "mode"), {"ordinary", "move", "inout"}),
                choice(field(p2, "escapes"), {"no", "may_retain"}), location(p2)});
          }
          d.detail = std::move(c);
        }
        declarations.push_back(std::move(d));
      }
    }
    // All fields have passed format checks before references and combinations
    // are inspected. A failed bundle is never added to Project's public tables.
    std::map<std::string, ForeignDeclaration *> types;
    for (auto &d : declarations)
      if (std::holds_alternative<BindingType>(d.detail)) types.emplace(d.identity, &d);
    auto type_ref = [&](const std::string &name, Json at) -> ForeignDeclaration * {
      if (builtin(name)) return nullptr;
      auto it = types.find(name);
      if (it == types.end()) invalid(at, "unknown type: " + name);
      return it->second;
    };
    std::map<std::pair<std::string, std::string>, const ForeignDeclaration *> names;
    for (const auto &d : declarations) {
      Json j{nullptr, d.json_pointer};
      if (auto t = std::get_if<BindingType>(&d.detail)) {
        if ((t->kind == "runtime_error") != (t->copy == "error_payload_copy")) invalid(j, "type kind/copy mismatch");
        if ((t->kind == "opaque_protocol") != d.host_attribute.empty()) invalid(j, "type host_attribute mismatch");
        if (t->kind == "runtime_error" && (d.identity != "lang.py.Error" || d.module != "std/py" ||
            b.modules.at(mods.at(d.module)).provider != "language_runtime")) invalid(j, "invalid runtime error identity/provider");
      }
      if (std::holds_alternative<BindingType>(d.detail) || std::holds_alternative<BindingValue>(d.detail)) {
        if (!names.emplace(std::pair{d.module, d.name}, &d).second) invalid(j, "duplicate export name");
      }
      if (auto v = std::get_if<BindingValue>(&d.detail)) {
        type_ref(v->type, field(j, "type"));
        if (d.host_attribute.empty()) invalid(j, "value requires a host attribute");
      }
    }
    std::set<std::string> signatures;
    for (const auto &d : declarations) {
      auto c = std::get_if<BindingCallable>(&d.detail);
      if (!c) continue;
      Json j{nullptr, d.json_pointer};
      bool receiver = c->call_kind == "method" || c->call_kind == "callable";
      if (receiver != c->owner_type.has_value()) invalid(field(j, "owner_type"), "call kind/owner mismatch");
      if (receiver == (c->receiver_escapes == "not_applicable")) invalid(field(j, "receiver_escapes"), "call kind/receiver escape mismatch");
      if ((c->call_kind == "callable") != d.host_attribute.empty()) invalid(field(j, "host_attribute"), "call kind/host attribute mismatch");
      if (receiver) {
        auto owner = type_ref(*c->owner_type, field(j, "owner_type"));
        if (!owner || owner->module != d.module || std::get<BindingType>(owner->detail).kind == "runtime_error")
          invalid(field(j, "owner_type"), "owner must be an object type in the same module");
        std::get<BindingType>(owner->detail).members.push_back(d.id);
      } else if (c->call_kind == "constructor") {
        auto it = names.find({d.module, d.name});
        if (it == names.end() || !std::holds_alternative<BindingType>(it->second->detail)) invalid(j, "constructor requires same-name type");
        auto target = it->second;
        if (target->identity != c->return_type || std::get<BindingType>(target->detail).kind != "host_reference" ||
            target->host_attribute != d.host_attribute) invalid(j, "constructor target/return mismatch");
      } else {
        if (names.contains({d.module, d.name})) invalid(j, "function conflicts with type/value export");
      }
      auto ret = type_ref(c->return_type, field(j, "return_type"));
      auto expected = ret ? "python_reference" : c->return_type == "Unit" ? "unit" : "value";
      if (c->return_ownership != expected || (!ret && c->storage != "no_tensor_storage"))
        invalid(field(j, "ownership"), "return type/ownership/storage mismatch");
      if (c->error_type) {
        auto error = type_ref(*c->error_type, field(j, "error_type"));
        if (!error || std::get<BindingType>(error->detail).kind != "runtime_error") invalid(field(j, "error_type"), "error must reference runtime_error");
      }
      std::set<std::string> parameters;
      int previous_kind = 0;
      bool optional_positional = false, retains = false;
      // Parameter names/kinds/modes/types/omission define a candidate. Return,
      // effects and host spelling cannot distinguish duplicate signatures.
      std::string signature;
      auto append = [&](const std::string &s) { signature += std::to_string(s.size()) + ":" + s; };
      append(d.module); append(c->owner_type.value_or("")); append(c->call_kind); append(d.name);
      for (size_t i = 0; i < c->parameters.size(); ++i) {
        const auto &p2 = c->parameters[i];
        Json at{nullptr, j.pointer + "/parameters/" + std::to_string(i)};
        if (!parameters.insert(p2.name).second || p2.name == "_") invalid(at, "duplicate or discard parameter name");
        type_ref(p2.type, field(at, "type"));
        int kind = p2.kind == "positional_only" ? 0 : p2.kind == "positional_or_keyword" ? 1 : 2;
        if (kind < previous_kind) invalid(at, "invalid parameter kind order");
        previous_kind = kind;
        if (kind < 2) {
          if (optional_positional && !p2.optional) invalid(at, "required positional parameter after optional parameter");
          optional_positional |= p2.optional;
        }
        if (p2.mode == "inout" && p2.escapes != "no") invalid(at, "inout parameter cannot escape");
        retains |= p2.escapes == "may_retain";
        append(p2.name); append(p2.type); append(p2.kind); append(p2.mode); append(p2.optional ? "optional" : "required");
      }
      if (!signatures.insert(signature).second) invalid(j, "duplicate callable signature");
      std::set<std::string> effects(c->effects.begin(), c->effects.end());
      if (effects.size() != c->effects.size()) invalid(field(j, "effects"), "duplicate effect");
      if (retains != effects.contains("retains_arguments")) invalid(field(j, "effects"), "parameter escape/retains_arguments mismatch");
      if ((c->receiver_escapes == "may_retain") != effects.contains("retains_receiver")) invalid(field(j, "effects"), "receiver escape/retains_receiver mismatch");
      if (!receiver && effects.contains("mutates_receiver")) invalid(field(j, "effects"), "mutates_receiver requires an owner");
    }
    project.bindings.push_back(std::move(b));
    for (auto &d : declarations) {
      project.bindings.back().declarations.push_back(d.id);
      project.foreign_bindings.push_back(std::move(d));
    }
    binding_identities.insert(ids.begin(), ids.end());
    bundle_ids.emplace(key, bundle_id);
    return bundle_id;
  }
  ModuleId foreign(std::string name, const fs::path &path,
                   const std::string &host, std::optional<SourceSpan> where) {
    auto b = bundle(path);
    const auto &bundle = project.bindings.at(b);
    auto module = std::find_if(bundle.modules.begin(), bundle.modules.end(), [&](const auto &m) { return m.logical_path == name; });
    if (module == bundle.modules.end())
      fail("E_BINDING_INVALID", "bundle does not declare module " + name, where ? where : std::optional<SourceSpan>(bundle.source));
    if (!host.empty() && std::none_of(bundle.package_requirements.begin(), bundle.package_requirements.end(),
                                     [&](const auto &r) { return r.package == host; }))
      fail("E_BINDING_INVALID", "bundle does not declare host package " + host, bundle.source);
    std::string identity = "binding:" + bundle.path.string() + ":" + name;
    claim(name, identity, where);
    if (identities.contains(identity)) return identities.at(identity);
    ModuleId id(project.modules.size());
    identities.emplace(identity, id);
    project.modules.push_back({id, name, identity, name, {}, {}, {}, BindingModuleRef{b, size_t(module - bundle.modules.begin())}});
    std::map<std::string, ForeignExport> exports;
    for (auto binding : bundle.declarations) {
      const auto &d = project.foreign_binding(binding);
      if (d.module != name) continue;
      auto c = std::get_if<BindingCallable>(&d.detail);
      if (c) continue;
      bool type = std::holds_alternative<BindingType>(d.detail);
      exports.emplace(d.name, ForeignExport{d.name, d.stable_identity,
          type ? ForeignExportKind::type : ForeignExportKind::value, b, binding, {}});
    }
    for (auto binding : bundle.declarations) {
      const auto &d = project.foreign_binding(binding);
      if (d.module != name) continue;
      auto c = std::get_if<BindingCallable>(&d.detail);
      if (!c || c->owner_type) continue;
      if (c->call_kind == "constructor") exports.at(d.name).candidates.push_back(binding);
      else {
        auto [it, inserted] = exports.try_emplace(d.name, ForeignExport{d.name, d.stable_identity,
            ForeignExportKind::function, b, {}, {}});
        (void)inserted;
        it->second.candidates.push_back(binding);
      }
    }
    for (auto &[n, e] : exports) project.modules[id.value].foreign_exports.push_back(std::move(e));
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
      if (!fs::is_directory(root / suffix))
        fail("E_IMPORT_NOT_FOUND", "standard module is not provided: " + name, where);
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
      if (e.is_regular_file() && e.path().extension() == ".wz")
        paths.push_back(e.path());
    }
    std::sort(paths.begin(), paths.end());
    if (paths.empty())
      fail("E_MODULE_EMPTY", "no .wz files in " + dir.string(), where,
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

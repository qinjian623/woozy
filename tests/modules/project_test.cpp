#include "lang/modules/resolve.hpp"
#include <algorithm>
#include <cstdlib>
#include <set>
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
std::string read(const fs::path &p) {
  std::ifstream f(p, std::ios::binary);
  require(bool(f), "read fixture");
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
std::string replaced(std::string text, const std::string &from, const std::string &to) {
  auto pos = text.find(from);
  require(pos != text.npos, ("missing mutation target: " + from).c_str());
  text.replace(pos, from.size(), to);
  return text;
}
void json_key_tests(const fs::path &root) {
  using namespace modules;
  const std::string original = R"({
"schema_version": 1, "contract": "compiler-dev/1",
"review_status": "draft_requires_conformance", "allowed_backends": ["python"],
"package_requirements": [{"package": "host", "version_policy": "test"}],
"modules": [{"logical_path": "py", "host_module": "host", "provider": "python_package"}],
"types": [{"id": "host.Thing", "module": "py", "name": "Thing", "kind": "host_reference", "host_attribute": ["Thing"], "copy": "shared_reference"}],
"values": [{"module": "py", "name": "v", "type": "i64", "host_attribute": ["v"]}],
"callables": [{"id": "host.run", "module": "py", "name": "run", "call_kind": "function", "owner_type": null,
"host_attribute": ["run"], "parameters": [{"name": "x", "type": "i64", "kind": "positional_or_keyword", "optional": false, "mode": "ordinary", "escapes": "no"}],
"return_type": "Unit", "error_type": null, "ownership": {"return": "unit", "storage": "no_tensor_storage"}, "effects": [], "receiver_escapes": "not_applicable"}],
"conformance_required": ["check"]})";
  write(root / "lang.toml", "schema_version=1\n[package]\nname=\"main\"\nsource_root=\"src\"\nentry=\"app\"\n"
        "[dependencies.py]\nkind=\"python\"\nbinding=\"binding.json\"\nhost_package=\"host\"\n");
  write(root / "src/main.wz", "package main; import \"py\" as p; func main() { p.run(); }");
  SourceManager sm;
  auto load = [&](const std::string &text) {
    write(root / "binding.json", text);
    return load_project({root / "lang.toml", {}, {}}, sm);
  };
  size_t positive = 0, negative = 0;
  auto good = [&](const std::string &text) {
    auto p = load(text);
    require(p.success(), "legal JSON key/value rejected");
    require(resolve_project(std::make_shared<const Project>(*p.value)).success(), "legal JSON key/value names rejected");
    ++positive;
    return *p.value;
  };
  auto bad = [&](const std::string &text, const std::string &key) {
    auto p = load(text);
    require(!p.success() && p.exit_code() == ExitCode::source_error && code(p, "E_BINDING_INVALID"), "invalid JSON key accepted");
    require(!p.value || (p.value->bindings.empty() && p.value->foreign_bindings.empty()), "invalid key published binding declarations");
    auto begin = text.find(key);
    require(begin != text.npos, "bad key test range");
    bool exact = false;
    for (const auto &d : p.diagnostics)
      if (d.code == "E_BINDING_INVALID" && d.primary && d.phase == "resolve") {
        const auto &s = *d.primary;
        exact |= sm.get(s.snapshot)->path == fs::canonical(root / "binding.json").string() &&
                 s.begin_byte == begin && s.end_byte == begin + key.size();
      }
    require(exact, "JSON key diagnostic must cover the original encoded key");
    ++negative;
  };
  good(original);
  good(replaced(original, R"("contract":)", R"("contr\u0061ct":)"));
  good(replaced(original, R"("optional":)", R"("optio\u006eal":)"));
  good(replaced(original, R"("kind":)", R"("ki\u006ed":)"));
  auto values = good(replaced(replaced(original, R"("version_policy": "test")", R"("version_policy": "safe\u0000policy")"),
                              R"("conformance_required": ["check"])", R"("conformance_required": ["check\u0000later"])"));
  require(values.bindings[0].package_requirements[0].version_policy == std::string("safe\0policy", 11) &&
          values.bindings[0].conformance_required == std::vector<std::string>{std::string("check\0later", 11)}, "legal string NUL was truncated");
  for (auto field : {"schema_version", "contract", "version_policy", "provider", "kind", "type", "call_kind", "return", "optional"}) {
    auto key = std::string("\"") + field + "\\u0000hidden\"";
    bad(replaced(original, std::string("\"") + field + "\":", key + ":"), key);
  }
  bad(replaced(original, R"("contract": "compiler-dev/1")", R"("contract": "compiler-dev/1", "contract\u0000hidden": "compiler-dev/1")"), R"("contract\u0000hidden")");
  bad(replaced(original, R"("optional": false)", R"("optional": false, "optional\u0000hidden": false)"), R"("optional\u0000hidden")");
  bad(replaced(original, "{", R"({"\u0000extra": 0,)"), R"("\u0000extra")");
  bad(replaced(original, R"("contract": "compiler-dev/1")", R"("contract": "compiler-dev/1", "contr\u0061ct": "compiler-dev/1")"), R"("contr\u0061ct")");
  std::cout << "JSON keys: " << positive << " legal controls, " << negative << " invalid keys with exact spans\n";
}
void binding_tests(const fs::path &root) {
  using namespace modules;
  const auto repo = fs::path(__FILE__).parent_path().parent_path().parent_path();
  const auto original = read(repo / "contracts/python_bindings.example.json");
  write(root / "lang.toml", R"(schema_version=1
[package]
name="main"
source_root="src"
entry="app"
[build]
backend="python"
[dependencies.numpy]
kind="python"
binding="binding.json"
host_package="numpy"
[dependencies.torch]
kind="python"
binding="binding.json"
host_package="torch"
)");
  write(root / "src/main.wz", read(repo / "baseline/samples/04_numpy_torch.wz"));
  write(root / "binding.json", original);
  SourceManager sm;
  ProjectOptions opts{root / "lang.toml", {}, root / "binding.json"};
  auto load = [&] { return load_project(opts, sm); };
  auto loaded = load();
  for (auto &d : loaded.diagnostics) std::cerr << d.message << '\n';
  require(loaded.success(), "original example load");
  auto project = std::make_shared<const Project>(*loaded.value);
  auto resolved = resolve_project(project);
  require(resolved.success(), "original 04 names");
  require(project->modules.size() == 7 && project->bindings.size() == 1,
          "original six imported modules");
  const auto &bundle = project->bindings.front();
  require(bundle.modules.size() == 6 && bundle.declarations.size() == 26 &&
          bundle.sha256 == "c118c3e4912234dc752355fd8f41b6274433c86a3448fdc53dcdb63d7aa2cd71" &&
          bundle.json == original && bundle.path == fs::canonical(root / "binding.json") &&
          bundle.schema_version == 1 && bundle.contract == "compiler-dev/1" &&
          bundle.review_status == "draft_requires_conformance" &&
          bundle.allowed_backends == std::vector<std::string>{"python"} &&
          bundle.conformance_required.size() == 8 && bundle.package_requirements.size() == 2 &&
          bundle.package_requirements[0].package == "numpy" &&
          bundle.package_requirements[1].package == "torch" &&
          bundle.package_requirements[0].version_policy == "T10按实际通过conformance的CPU环境写入lock" &&
          bundle.conformance_required.back() == "所有依赖导入只发生在显式执行/conformance阶段",
          "bundle fields, original source and SHA-256");
  std::set<uint64_t> ids;
  std::set<std::string> stable;
  size_t types = 0, values = 0, calls = 0;
  auto declaration = [&](const std::string &identity) -> const ForeignDeclaration & {
    for (auto id : bundle.declarations)
      if (project->foreign_binding(id).identity == identity) return project->foreign_binding(id);
    throw std::runtime_error("missing declaration " + identity);
  };
  auto exported = [&](const std::string &module, const std::string &name) -> const ForeignExport & {
    for (const auto &symbol : resolved.value->symbols)
      if (project->module(symbol.module).logical_path == module && symbol.name == name && symbol.foreign) {
        auto e = resolved.value->foreign_export(symbol.id);
        require(e && e == &project->foreign_export(*symbol.foreign), "SymbolId export reference");
        auto id = e->declaration ? *e->declaration : e->candidates.at(0);
        require(symbol.declaration && symbol.declaration->snapshot == project->foreign_binding(id).source.snapshot &&
                symbol.declaration->begin_byte == project->foreign_binding(id).source.begin_byte,
                "foreign symbol definition source");
        return *e;
      }
    throw std::runtime_error("missing export " + module + "/" + name);
  };
  for (auto id : bundle.declarations) {
    const auto &d = project->foreign_binding(id);
    require(d.id == id && ids.insert(id.value).second && stable.insert(d.stable_identity).second &&
            d.stable_identity.find(bundle.sha256) != std::string::npos && d.bundle == 0,
            "unique binding IDs and persistent identities");
    auto source = sm.get(d.source.snapshot);
    require(source->path == bundle.path.string() && d.source.begin_byte < d.source.end_byte &&
            source->original.at(d.source.begin_byte) == '{' &&
            source->original.at(d.source.end_byte - 1) == '}' && !d.json_pointer.empty(), "declaration source");
    if (std::holds_alternative<BindingType>(d.detail)) {
      ++types;
      require(exported(d.module, d.name).declaration == id, "type export ID");
    } else if (std::holds_alternative<BindingValue>(d.detail)) {
      ++values;
      require(exported(d.module, d.name).declaration == id, "value export ID");
    } else {
      ++calls;
      const auto &c = std::get<BindingCallable>(d.detail);
      if (c.call_kind == "function" || c.call_kind == "constructor") {
        const auto &e = exported(d.module, d.name);
        require(std::find(e.candidates.begin(), e.candidates.end(), id) != e.candidates.end(), "callable candidate ID");
        require((c.call_kind == "constructor") == (e.kind == ForeignExportKind::type), "constructor attaches to type");
      } else {
        const auto &owner = std::get<BindingType>(declaration(*c.owner_type).detail);
        require(std::find(owner.members.begin(), owner.members.end(), id) != owner.members.end(), "owner member index");
        for (const auto &s : resolved.value->symbols)
          require(!s.foreign || s.name != d.name || project->module(s.module).logical_path != d.module,
                  "method/callable leaked into module names");
      }
    }
  }
  require(types == 9 && values == 2 && calls == 15, "original declaration counts");
  const std::vector<std::string> host_modules = {"langrt", "numpy", "torch", "torch.nn", "torch.optim", "torch.nn.functional"};
  for (size_t i = 0; i < bundle.modules.size(); ++i) {
    const auto &desc = bundle.modules[i];
    require(desc.host_module == host_modules[i] && desc.provider == (i == 0 ? "language_runtime" : "python_package"), "module host/provider");
    bool found = false;
    for (const auto &m : project->modules)
      if (m.logical_path == desc.logical_path) {
        found = project->binding_module(m.id) == &desc;
        require(m.binding && m.binding->bundle == 0 && m.binding->index == i, "module description index");
      }
    require(found, "module descriptor lookup");
  }
  for (auto name : {"lang.py.Error", "py.numpy.ndarray", "py.numpy.DTypeSpec", "py.torch.Tensor", "py.torch.dtype",
                    "py.torch.Generator", "py.torch.ParameterIterator", "py.torch.nn.Linear", "py.torch.optim.SGD"}) {
    const auto &d = declaration(name);
    const auto &t = std::get<BindingType>(d.detail);
    bool protocol = d.identity == "py.numpy.DTypeSpec" || d.identity == "py.torch.ParameterIterator";
    bool error = d.identity == "lang.py.Error";
    require(t.kind == (protocol ? "opaque_protocol" : error ? "runtime_error" : "host_reference") &&
            t.copy == (error ? "error_payload_copy" : "shared_reference") &&
            (protocol ? d.host_attribute.empty() : d.host_attribute == std::vector<std::string>{d.name}), "type kind/copy/attribute");
  }
  require(std::get<BindingValue>(declaration("numpy/float32").detail).type == "py.numpy.DTypeSpec" &&
          std::get<BindingValue>(declaration("torch/float32").detail).type == "py.torch.dtype" &&
          declaration("numpy/float32").host_attribute == std::vector<std::string>{"float32"}, "dtype value fields");
  struct ExpectedCall { const char *id, *kind, *owner, *result, *storage, *effects, *parameters; };
  const ExpectedCall expected[] = {
    {"numpy.linspace.scalar","function","","py.numpy.ndarray","copy","reads_arguments,","start:f64:positional_or_keyword:required:ordinary:no;stop:f64:positional_or_keyword:required:ordinary:no;num:i64:positional_or_keyword:optional:ordinary:no;dtype:py.numpy.DTypeSpec:keyword_only:optional:ordinary:no;"},
    {"numpy.expand_dims.array_axis","function","","py.numpy.ndarray","view_or_alias","reads_arguments,retains_arguments,","a:py.numpy.ndarray:positional_or_keyword:required:ordinary:may_retain;axis:i64:positional_or_keyword:required:ordinary:no;"},
    {"numpy.multiply.array_scalar","function","","py.numpy.ndarray","copy","reads_arguments,","x1:py.numpy.ndarray:positional_only:required:ordinary:no;x2:f64:positional_only:required:ordinary:no;"},
    {"numpy.add.array_scalar","function","","py.numpy.ndarray","copy","reads_arguments,","x1:py.numpy.ndarray:positional_only:required:ordinary:no;x2:f64:positional_only:required:ordinary:no;"},
    {"numpy.asarray.array_dtype","function","","py.numpy.ndarray","view_or_alias","reads_arguments,retains_arguments,","a:py.numpy.ndarray:positional_or_keyword:required:ordinary:may_retain;dtype:py.numpy.DTypeSpec:positional_or_keyword:optional:ordinary:no;"},
    {"torch.manual_seed","function","","py.torch.Generator","no_tensor_storage","mutates_host_rng,","seed:i64:positional_or_keyword:required:ordinary:no;"},
    {"torch.tensor.ndarray","function","","py.torch.Tensor","copy","reads_arguments,","data:py.numpy.ndarray:positional_or_keyword:required:ordinary:no;dtype:py.torch.dtype:keyword_only:optional:ordinary:no;device:string:keyword_only:optional:ordinary:no;"},
    {"torch.nn.Linear.constructor","constructor","","py.torch.nn.Linear","no_tensor_storage","mutates_host_rng,","in_features:i64:positional_or_keyword:required:ordinary:no;out_features:i64:positional_or_keyword:required:ordinary:no;device:string:keyword_only:optional:ordinary:no;dtype:py.torch.dtype:keyword_only:optional:ordinary:no;"},
    {"torch.nn.Linear.parameters","method","py.torch.nn.Linear","py.torch.ParameterIterator","view_or_alias","reads_arguments,retains_receiver,",""},
    {"torch.nn.Linear.call","callable","py.torch.nn.Linear","py.torch.Tensor","host_defined","reads_arguments,autograd,retains_arguments,retains_receiver,","input:py.torch.Tensor:positional_or_keyword:required:ordinary:may_retain;"},
    {"torch.optim.SGD.constructor","constructor","","py.torch.optim.SGD","no_tensor_storage","retains_arguments,consumes_iterator,","params:py.torch.ParameterIterator:positional_or_keyword:required:ordinary:may_retain;lr:f64:positional_or_keyword:optional:ordinary:no;"},
    {"torch.optim.SGD.zero_grad","method","py.torch.optim.SGD","Unit","no_tensor_storage","mutates_receiver,retains_receiver,","set_to_none:bool:positional_or_keyword:optional:ordinary:no;"},
    {"torch.nn.functional.mse_loss","function","","py.torch.Tensor","host_defined","reads_arguments,autograd,retains_arguments,","input:py.torch.Tensor:positional_or_keyword:required:ordinary:may_retain;target:py.torch.Tensor:positional_or_keyword:required:ordinary:may_retain;"},
    {"torch.Tensor.backward","method","py.torch.Tensor","Unit","no_tensor_storage","mutates_receiver,autograd,retains_receiver,",""},
    {"torch.optim.SGD.step.no_closure","method","py.torch.optim.SGD","Unit","no_tensor_storage","mutates_receiver,retains_receiver,",""}
  };
  for (const auto &e : expected) {
    const auto &d = declaration(e.id);
    const auto &c = std::get<BindingCallable>(d.detail);
    std::string params, effects;
    for (const auto &p : c.parameters) {
      params += p.name + ":" + p.type + ":" + p.kind + ":" + (p.optional ? "optional" : "required") + ":" + p.mode + ":" + p.escapes + ";";
      require(p.source.snapshot == d.source.snapshot && p.source.begin_byte > d.source.begin_byte && p.source.end_byte < d.source.end_byte, "parameter source");
    }
    for (const auto &effect : c.effects) effects += effect + ",";
    require(c.call_kind == e.kind && c.owner_type.value_or("") == e.owner && c.return_type == e.result &&
            c.error_type == std::optional<std::string>("lang.py.Error") && c.storage == e.storage &&
            c.return_ownership == (c.return_type == "Unit" ? "unit" : "python_reference") &&
            effects == e.effects && params == e.parameters &&
            c.receiver_escapes == (c.owner_type ? "may_retain" : "not_applicable") &&
            (c.call_kind == "callable" ? d.host_attribute.empty() : d.host_attribute == std::vector<std::string>{d.name}), "all callable fields");
  }
  // Change an individual declaration without depending on JSON member order.
  auto edit = [&](const std::string &id, const std::string &from, const std::string &to) {
    const auto &d = declaration(id);
    auto snippet = original.substr(d.source.begin_byte, d.source.end_byte - d.source.begin_byte);
    auto changed = replaced(snippet, from, to);
    auto text = original;
    text.replace(d.source.begin_byte, snippet.size(), changed);
    return text;
  };
  size_t positive = 1, negative = 0;
  auto good = [&](const std::string &text) {
    write(root / "binding.json", text);
    auto p = load();
    if (!p.success()) for (auto &d : p.diagnostics) std::cerr << d.message << '\n';
    require(p.success(), "valid binding mutation rejected");
    auto names = resolve_project(std::make_shared<const Project>(*p.value));
    require(names.success(), "valid binding names rejected");
    ++positive;
    return *p.value;
  };
  auto bad = [&](const std::string &text, const std::string &at = "") {
    write(root / "binding.json", text);
    auto p = load();
    require(!p.success() && p.exit_code() == ExitCode::source_error && code(p, "E_BINDING_INVALID"), "invalid binding accepted/classification");
    for (auto &d : p.diagnostics) if (d.code == "E_BINDING_INVALID") {
      require(d.primary.has_value(), "binding diagnostic has no location");
      auto s = sm.get(d.primary->snapshot);
      require(s->path == fs::canonical(root / "binding.json").string() &&
              d.primary->begin_byte < d.primary->end_byte && d.primary->end_byte <= text.size(), "binding diagnostic source/range");
      if (!at.empty()) require(text.substr(d.primary->begin_byte, d.primary->end_byte - d.primary->begin_byte).find(at) != std::string::npos, "diagnostic does not point to bad field");
    }
    ++negative;
  };
  // Required fields and unknown fields at every object shape.
  for (auto field : {"schema_version", "contract", "review_status", "allowed_backends", "package_requirements", "modules", "types", "values", "callables", "conformance_required"})
    bad(replaced(original, std::string("\"") + field + "\":", std::string("\"missing_") + field + "\":"));
  const std::vector<std::pair<std::string, std::vector<std::string>>> shapes = {
    {"lang.py.Error", {"id","module","name","kind","host_attribute","copy"}},
    {"numpy/float32", {"module","name","type","host_attribute"}},
    {"numpy.linspace.scalar", {"id","module","name","call_kind","owner_type","host_attribute","parameters","return_type","error_type","ownership","effects","receiver_escapes"}}
  };
  for (const auto &[id, fields] : shapes) {
    for (const auto &f : fields) bad(edit(id, "\"" + f + "\":", "\"missing_" + f + "\":"));
    bad(edit(id, "{", "{\"unknown\": true,"), "true");
  }
  for (auto field : {"name","type","kind","optional","mode","escapes"}) {
    const auto &d = declaration("numpy.linspace.scalar");
    const auto &p = std::get<BindingCallable>(d.detail).parameters.front();
    auto text = original;
    auto snippet = text.substr(p.source.begin_byte, p.source.end_byte - p.source.begin_byte);
    text.replace(p.source.begin_byte, snippet.size(), replaced(snippet, std::string("\"") + field + "\":", std::string("\"missing_") + field + "\":"));
    bad(text);
  }
  for (const auto &change : std::vector<std::pair<std::string, std::string>>{
      {"\"schema_version\": 1", "\"schema_version\": 4294967297"},
      {"\"schema_version\": 1", "\"schema_version\": 1.0"},
      {"\"contract\": \"compiler-dev/1\"", "\"contract\": 1"},
      {"\"review_status\": \"draft_requires_conformance\"", "\"review_status\": \"unchecked\""},
      {"\"python\"", "\"native\""}, {"\"numpy\",", "null,"},
      {"\"host_module\": \"numpy\"", "\"host_module\": \"numpy; import os\""},
      {"\"host_module\": \"numpy\"", "\"host_module\": \"numpy/other\""},
      {"\"provider\": \"python_package\"", "\"provider\": \"other\""},
      {"\"provider\": \"language_runtime\"", "\"provider\": \"python_package\""},
      {"\"logical_path\": \"numpy\"", "\"logical_path\": \"app/x\""},
      {"\"version_policy\":", "\"missing_policy\":"},
      {"\"package\": \"numpy\"", "\"unknown_package\": \"numpy\""},
      {"\"host_module\": \"numpy\"", "\"host_module\": 2"},
      {"\"logical_path\": \"numpy\"", "\"logical_path\": \"torch\""},
      {"\"logical_path\":", "\"missing_logical\":"}, {"\"provider\":", "\"unknown_provider\":"}})
    bad(replaced(original, change.first, change.second));
  bad(replaced(original, "{", "{\"unknown\":0,"), "0");
  bad(replaced(original, "\"schema_version\": 1", "\"schema_version\": 1, \"schema_version\": 1"), "schema_version");
  bad(replaced(original, "\"contract\": \"compiler-dev/1\"", "\"contract\": \"compiler-dev/1\", \"contr\\u0061ct\": \"compiler-dev/1\""));
  bad(edit("py.numpy.ndarray", "\"shared_reference\"", "\"error_payload_copy\""));
  bad(edit("py.numpy.ndarray", "\"host_reference\"", "\"dynamic\""), "dynamic");
  bad(edit("py.numpy.ndarray", "\"ndarray\"\n", "\"ndarray()\"\n"), "ndarray()");
  bad(edit("py.numpy.ndarray", "\"host_attribute\": [\n        \"ndarray\"\n      ]", "\"host_attribute\": []"));
  bad(edit("py.numpy.DTypeSpec", "\"host_attribute\": []", "\"host_attribute\": [\"DTypeSpec\"]"));
  bad(edit("py.numpy.ndarray", "\"py.numpy.ndarray\"", "\"py.numpy.DTypeSpec\""), "py.numpy.DTypeSpec");
  bad(edit("py.numpy.ndarray", "\"py.numpy.ndarray\"", "\"\""));
  bad(edit("py.numpy.ndarray", "\"py.numpy.ndarray\"", "\"bad\\u0000id\""));
  bad(edit("numpy/float32", "\"py.numpy.DTypeSpec\"", "\"Any\""), "Any");
  bad(edit("numpy/float32", "\"float32\"\n", "12\n"), "12");
  bad(edit("numpy/float32", "\"numpy\"", "\"missing/module\""), "missing/module");
  bad(edit("numpy.linspace.scalar", "\"numpy.linspace.scalar\"", "\"py.numpy.ndarray\""));
  bad(edit("numpy.linspace.scalar", "\"call_kind\": \"function\"", "\"call_kind\": \"dynamic\""));
  bad(edit("numpy.linspace.scalar", "\"owner_type\": null", "\"owner_type\": 42"), "42");
  bad(edit("numpy.linspace.scalar", "\"owner_type\": null", "\"owner_type\": \"py.numpy.ndarray\""));
  bad(edit("torch.nn.Linear.parameters", "\"py.torch.nn.Linear\"", "\"missing.Type\""), "missing.Type");
  bad(edit("torch.nn.Linear.parameters", "\"py.torch.nn.Linear\"", "\"py.torch.Tensor\""));
  bad(edit("torch.nn.Linear.call", "\"host_attribute\": []", "\"host_attribute\": [\"forward\"]"));
  bad(edit("torch.nn.Linear.constructor", "\"py.torch.nn.Linear\"", "\"py.torch.Tensor\""));
  bad(edit("torch.nn.Linear.constructor", "\"call_kind\": \"constructor\"", "\"call_kind\": \"function\""));
  bad(edit("numpy.linspace.scalar", "\"return_type\": \"py.numpy.ndarray\"", "\"return_type\": \"unknown.Type\""));
  bad(edit("numpy.linspace.scalar", "\"python_reference\"", "\"value\""));
  bad(edit("torch.optim.SGD.step.no_closure", "\"unit\"", "\"python_reference\""));
  bad(edit("torch.optim.SGD.step.no_closure", "\"no_tensor_storage\"", "\"copy\""));
  bad(edit("numpy.linspace.scalar", "\"error_type\": \"lang.py.Error\"", "\"error_type\": \"py.numpy.ndarray\""));
  bad(edit("lang.py.Error", "\"runtime_error\"", "\"host_reference\""));
  bad(edit("numpy.linspace.scalar", "\"ownership\": {", "\"ownership\": {\"unknown\":0,"));
  bad(edit("numpy.linspace.scalar", "\"storage\":", "\"missing_storage\":"));
  bad(edit("numpy.linspace.scalar", "\"return\":", "\"missing_return\":"));
  bad(edit("numpy.linspace.scalar", "\"effects\": [", "\"effects\": [42,"));
  bad(edit("numpy.linspace.scalar", "\"reads_arguments\"", "\"unknown_effect\""));
  bad(edit("numpy.linspace.scalar", "\"not_applicable\"", "\"no\""));
  bad(edit("numpy.linspace.scalar", "\"parameters\": [", "\"parameters\": [{\"unknown\":0},"));
  bad(edit("numpy.linspace.scalar", "\"optional\": false", "\"optional\": 0"), "0");
  bad(edit("numpy.linspace.scalar", "\"name\": \"stop\"", "\"name\": \"start\""));
  bad(edit("numpy.linspace.scalar", "\"type\": \"f64\"", "\"type\": \"Any\""));
  bad(edit("numpy.linspace.scalar", "\"kind\": \"positional_or_keyword\"", "\"kind\": \"varargs\""));
  bad(edit("numpy.linspace.scalar", "\"kind\": \"positional_or_keyword\"", "\"kind\": \"keyword_only\""));
  bad(edit("numpy.linspace.scalar", "\"optional\": false", "\"optional\": true"));
  bad(edit("numpy.linspace.scalar", "\"mode\": \"ordinary\"", "\"mode\": \"borrow\""));
  bad(edit("numpy.linspace.scalar", "\"escapes\": \"no\"", "\"escapes\": \"always\""));
  bad(edit("numpy.expand_dims.array_axis", "\"mode\": \"ordinary\"", "\"mode\": \"inout\""));
  bad(edit("numpy.expand_dims.array_axis", "\"may_retain\"", "\"no\""));
  bad(edit("torch.nn.Linear.parameters", "\"receiver_escapes\": \"may_retain\"", "\"receiver_escapes\": \"no\""));
  bad(edit("numpy/float32", "\"name\": \"float32\"", "\"name\": \"ndarray\""));
  bad(edit("numpy.linspace.scalar", "\"name\": \"linspace\"", "\"name\": \"float32\""));
  bad(edit("numpy.linspace.scalar", "\"numpy.linspace.scalar\"", "42"), "42");
  bad(edit("numpy.linspace.scalar", "\"linspace\"\n", "\"linspace[0]\"\n"));
  bad(edit("torch.nn.Linear.parameters", "\"owner_type\": \"py.torch.nn.Linear\"", "\"owner_type\": null"));
  bad(edit("numpy.linspace.scalar", "\"effects\": [", "\"effects\": [\"mutates_receiver\","));
  bad(edit("numpy.linspace.scalar", "\"reads_arguments\"", "\"reads_arguments\", \"reads_arguments\""));
  bad(edit("numpy.linspace.scalar", "\"copy\"", "\"borrowed\""));
  bad(replaced(original, "\"conformance_required\": [", "\"conformance_required\": [0,"));
  for (auto field : {"modules", "types", "values", "callables", "package_requirements", "conformance_required"})
    bad(replaced(original, std::string("\"") + field + "\": [", std::string("\"") + field + "\": [null,"));
  // Positive counterparts: omission, protocol declarations, non-ordinary modes
  // are retained for a future checker; this reader never enables their use.
  good(edit("numpy.linspace.scalar", "\"mode\": \"ordinary\"", "\"mode\": \"move\""));
  good(edit("numpy.linspace.scalar", "\"mode\": \"ordinary\"", "\"mode\": \"inout\""));
  good(edit("numpy.linspace.scalar", "\"error_type\": \"lang.py.Error\"", "\"error_type\": null"));
  good(replaced(original, "\"draft_requires_conformance\"", "\"verified_against_lock\""));
  // Two distinct finite overloads of a normal function and a constructor.
  auto duplicate = [&](const std::string &id, const std::string &new_id, const std::string &from, const std::string &to) {
    const auto &d = declaration(id);
    auto snippet = original.substr(d.source.begin_byte, d.source.end_byte - d.source.begin_byte);
    snippet = replaced(snippet, id, new_id);
    if (!from.empty()) snippet = replaced(snippet, from, to);
    auto text = original;
    text.insert(d.source.end_byte, ",\n" + snippet);
    return text;
  };
  bad(duplicate("numpy.linspace.scalar", "numpy.linspace.duplicate", "", ""));
  auto overload = good(duplicate("numpy.linspace.scalar", "numpy.linspace.integer", "\"type\": \"f64\"", "\"type\": \"i64\""));
  auto names = resolve_project(std::make_shared<const Project>(overload));
  size_t symbols = 0;
  for (const auto &s : names.value->symbols) if (s.foreign && s.name == "linspace") {
    ++symbols;
    const auto &e = *names.value->foreign_export(s.id);
    require(e.candidates.size() == 2 && !e.declaration &&
            overload.foreign_binding(e.candidates[0]).identity != overload.foreign_binding(e.candidates[1]).identity,
            "finite ordinary overloads from SymbolId");
  }
  require(symbols == 1, "overload created duplicate symbols");
  auto constructors = good(duplicate("torch.nn.Linear.constructor", "torch.nn.Linear.other", "\"type\": \"i64\"", "\"type\": \"i32\""));
  names = resolve_project(std::make_shared<const Project>(constructors));
  symbols = 0;
  for (const auto &s : names.value->symbols) if (s.foreign && s.name == "Linear") {
    ++symbols;
    require(s.kind == SymbolKind::type && names.value->foreign_export(s.id)->candidates.size() == 2 &&
            names.value->foreign_export(s.id)->declaration.has_value(), "constructor overload attaches to type symbol");
  }
  require(symbols == 1, "constructor created ordinary function symbol");
  auto scalar = edit("torch.manual_seed", "\"return_type\": \"py.torch.Generator\"", "\"return_type\": \"f64\"");
  auto scalar_own = scalar.find("\"return\": \"python_reference\"", declaration("torch.manual_seed").source.begin_byte);
  require(scalar_own != scalar.npos, "scalar ownership mutation target");
  scalar.replace(scalar_own, std::string("\"return\": \"python_reference\"").size(), "\"return\": \"value\"");
  good(scalar);
  good(edit("py.numpy.ndarray", "\"ndarray\"\n", "\"core\", \"ndarray\"\n"));
  // Required keyword-only parameters can follow optional keyword-only ones.
  good(edit("torch.tensor.ndarray", "\"name\": \"device\",\n          \"type\": \"string\",\n          \"kind\": \"keyword_only\",\n          \"optional\": true",
            "\"name\": \"device\",\n          \"type\": \"string\",\n          \"kind\": \"keyword_only\",\n          \"optional\": false"));
  auto same = good(original);
  for (size_t i = 0; i < project->foreign_bindings.size(); ++i)
    require(same.foreign_bindings[i].stable_identity == project->foreign_bindings[i].stable_identity, "identity changed on reload");
  // A different source file with identical bytes has the same content identity.
  write(root / "other.json", original);
  opts.std_binding_bundle = root / "other.json";
  write(root / "src/main.wz", "package main; import \"std/py\"; func main() {}");
  auto relocated = load();
  require(relocated.success(), "relocated bundle");
  for (size_t i = 0; i < project->foreign_bindings.size(); ++i)
    require(relocated.value->foreign_bindings[i].stable_identity == project->foreign_bindings[i].stable_identity, "identity depends on file location");
  // Distinct, valid bundles allocate one Project-wide ID range. Loading the
  // original as the second bundle changes its local IDs, not stable identities.
  const std::string extra = R"({"schema_version":1,"contract":"compiler-dev/1","review_status":"draft_requires_conformance","allowed_backends":["python"],"package_requirements":[{"package":"extra","version_policy":"test"}],"modules":[{"logical_path":"extra","host_module":"extra","provider":"python_package"}],"types":[{"id":"extra.Thing","module":"extra","name":"Thing","kind":"host_reference","host_attribute":["Thing"],"copy":"shared_reference"}],"values":[],"callables":[],"conformance_required":[]})";
  auto original_manifest = read(root / "lang.toml");
  write(root / "extra.json", extra);
  write(root / "lang.toml", original_manifest + "\n[dependencies.extra]\nkind=\"python\"\nbinding=\"extra.json\"\nhost_package=\"extra\"\n");
  write(root / "src/main.wz", "package main; import \"extra\"; import \"numpy\"; func main() {}");
  auto separate = load();
  require(separate.success() && separate.value->bindings.size() == 2 && separate.value->foreign_bindings.size() == 27, "multiple valid bundles");
  for (size_t i = 0; i < separate.value->foreign_bindings.size(); ++i)
    require(separate.value->foreign_bindings[i].id == ForeignBindingId(i), "cross-bundle ID collision");
  require(separate.value->foreign_bindings[1].stable_identity == project->foreign_bindings[0].stable_identity &&
          separate.value->foreign_bindings[1].id != project->foreign_bindings[0].id, "stable identity depends on local ID");
  write(root / "lang.toml", original_manifest);
  // Distinct bundles must not silently alias an explicit declaration identity.
  write(root / "src/main.wz", "package main; import \"std/py\"; import \"numpy\"; func main() {}");
  auto collision = load();
  require(!collision.success() && code(collision, "E_BINDING_INVALID"), "duplicate IDs across bundles");
  opts.std_binding_bundle = root / "binding.json";
  opts.std_source_root = root / "src";
  auto std_conflict = load();
  require(!std_conflict.success() && code(std_conflict, "E_MODULE_CONFLICT"), "two std sources");
  opts.std_source_root.reset();
  auto manifest = read(root / "lang.toml");
  write(root / "lang.toml", manifest + "\n[dependencies.std]\nkind=\"python\"\nbinding=\"binding.json\"\nhost_package=\"numpy\"\n");
  auto shadow = load();
  require(!shadow.success() && code(shadow, "E_MANIFEST_INVALID"), "project std shadow");
  write(root / "lang.toml", manifest);
  // Keep a usable foreign project for the separately executed CLI refusal test.
  write(root / "src/main.wz", "package main; import \"numpy\" as np; func main() { np.asarray; }");
  require(load().success(), "foreign refusal fixture is valid");
  std::cout << "T10 declarations: " << positive << " positive variants, " << negative
            << " invalid variants; original 6/9/2/15 fields and names verified\n";
}

int main() {
  auto pattern = (fs::temp_directory_path() / "lang-project-test-XXXXXX").string();
  auto unique = mkdtemp(pattern.data());
  require(unique != nullptr, "create unique temporary project");
  fs::path root(unique);
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
    write(root / "src/a.wz",
          "package demo; import \"geo\"; import \"alias\"; import \"py\" as p; "
          "pub func main() {}\n");
    write(root / "src/b.wz", "package demo; pub const N = 1;\n");
    write(root / "vendor/geo/lang.toml",
          "schema_version=1\n[package]\nname=\"geometry\"\nsource_root="
          "\"lib\"\nentry=\"app\"\n");
    write(root / "vendor/geo/lib/g.wz",
          "package geometry; pub func area() {}\n");
    write(
        root / "binding.json",
        R"({"schema_version":1,"contract":"compiler-dev/1","review_status":"draft_requires_conformance","allowed_backends":["python"],"package_requirements":[{"package":"host","version_policy":"test only"}],"modules":[{"logical_path":"py","host_module":"host","provider":"python_package"},{"logical_path":"secret","host_module":"secret","provider":"python_package"}],"types":[{"module":"py","name":"Thing","id":"host.Thing","kind":"host_reference","host_attribute":["Thing"],"copy":"shared_reference"}],"values":[{"module":"py","name":"v","type":"i64","host_attribute":["v"]}],"callables":[{"id":"host.Thing.constructor","module":"py","name":"Thing","call_kind":"constructor","owner_type":null,"host_attribute":["Thing"],"parameters":[],"return_type":"host.Thing","error_type":null,"ownership":{"return":"python_reference","storage":"no_tensor_storage"},"effects":[],"receiver_escapes":"not_applicable"},{"id":"host.Thing.method","module":"py","name":"method","call_kind":"method","owner_type":"host.Thing","host_attribute":["method"],"parameters":[],"return_type":"Unit","error_type":null,"ownership":{"return":"unit","storage":"no_tensor_storage"},"effects":[],"receiver_escapes":"no"},{"module":"py","name":"run","call_kind":"function","id":"host.run","owner_type":null,"host_attribute":["run"],"parameters":[],"return_type":"Unit","error_type":null,"ownership":{"return":"unit","storage":"no_tensor_storage"},"effects":[],"receiver_escapes":"not_applicable"}],"conformance_required":["test fixture only"]})");
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
    require(sm.get(app.imports[0].span.snapshot)->path.ends_with("a.wz"),
            "source snapshot");
    require(app.files[1]->tokens[3].spelling == "pub", "pub retained in AST");
    auto run = [&](const std::string &source) {
      write(root / "src/a.wz", source);
      return modules::load_project(opts, sm);
    };
    auto bad = run("package demo; import \"app//bad\"; func main() {}");
    require(!bad.success() && code(bad, "E_IMPORT_PATH") &&
                bad.exit_code() == ExitCode::source_error,
            "bad logical path");
    auto mismatch = run("package other; func main() {}");
    require(!mismatch.success() && code(mismatch, "E_PACKAGE_MISMATCH"),
            "package mismatch");
    write(root / "src/sub/s.wz", "package sub; import \"app\"; func f() {}");
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
    auto bundled_std = run("package demo; import \"std/os\"; func main() {}");
    require(bundled_std.success() && std::any_of(bundled_std.value->modules.begin(), bundled_std.value->modules.end(),
                [](const auto &module) { return module.logical_path == "std/os" && !module.binding; }),
            "bundled source std is available without explicit configuration");
    auto no_std = run("package demo; import \"std/missing_module\"; func main() {}");
    require(!no_std.success() && code(no_std, "E_IMPORT_NOT_FOUND"),
            "an absent bundled std module still fails");
    write(root / "stdlib/io/io.wz", "package io; pub func print() {}");
    opts.std_source_root = root / "stdlib";
    auto with_std = run("package demo; import \"std/io\"; func main() {}");
    require(with_std.success() && with_std.value->modules.size() == 2,
            "std source");
    opts.std_source_root.reset();
    write(root / "vendor/geo/lib/g.wz",
          "package geometry; import \"app\"; pub func area() {}");
    auto dep_loop = run("package demo; import \"geo\"; func main() {}");
    require(!dep_loop.success() && code(dep_loop, "E_IMPORT_CYCLE"),
            "dependency app uses dependency source_root");
    write(root / "vendor/geo/lib/g.wz",
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
    require(rendered.find("a.wz") != std::string::npos,
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
    binding_tests(root / "t10");
    json_key_tests(root / "json-keys");
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

#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <json-c/json.h>
#include <map>
#include <set>
#include <stdexcept>
using namespace lang;
using namespace lang::hir;
namespace fs = std::filesystem;
namespace {
size_t probes = 0;
static_assert(int(NodeKind::unsupported) == 23);
static_assert(int(NodeKind::constraint_call) == 29);
static_assert(int(NodeKind::try_) == 30 && int(NodeKind::defer_) == 31 &&
              int(NodeKind::throw_) == 32);
void need(bool b, const std::string &s) {
  if (!b)
    throw std::runtime_error(s);
}
bool has(const auto &r, std::string_view code) {
  for (const auto &d : r.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
std::string diagnostics(const auto &r) {
  std::string out;
  for (const auto &d : r.diagnostics)
    out += d.code + ": " + d.message + "\n";
  return out;
}
auto project(const fs::path &path, Backend backend) {
  SourceManager sm;
  auto p = modules::load_project({path, {}, {}}, sm);
  need(p.success(), diagnostics(p));
  p.value->backend = backend == Backend::native ? "native" : "python";
  auto r = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*p.value)));
  need(r.success(), diagnostics(r));
  return sema::check_types(r);
}
std::shared_ptr<const modules::ResolvedProject> resolve(const std::string &body,
                                                        Backend backend) {
  SourceManager sm;
  auto src = sm.update(sm.add_file("t08-static.wz"), "package app; " + body);
  auto tokens = lex(src);
  need(tokens.success(), diagnostics(tokens));
  auto ast = parse(*tokens.value);
  need(ast.success(), diagnostics(ast));
  auto p = std::make_shared<modules::Project>();
  p->backend = backend == Backend::native ? "native" : "python";
  modules::Module m;
  m.id = ModuleId{0};
  m.logical_path = "app";
  m.files.push_back(std::make_shared<const AstFile>(std::move(*ast.value)));
  p->modules.push_back(std::move(m));
  auto names = modules::resolve_project(p);
  need(names.success(), diagnostics(names));
  return std::make_shared<const modules::ResolvedProject>(
      std::move(*names.value));
}
HirModule accepted(const std::string &body, Backend b = Backend::native) {
  auto r = sema::check_types(resolve(body, b));
  need(r.success(), body + "\n" + diagnostics(r));
  need(verify_types(*r.value).success(), "second static verify failed");
  ++probes;
  return std::move(*r.value);
}
void rejected(const std::string &body, std::string_view code) {
  for (auto b : {Backend::native, Backend::python}) {
    auto r = sema::check_types(resolve(body, b));
    need(!r.value && !r.success() && has(r, code),
         body + "\n" + diagnostics(r));
    need(r.failure == (code == "E_CAPABILITY_UNAVAILABLE"
                           ? FailureKind::unsupported
                           : FailureKind::source),
         "wrong source error category");
    ++probes;
  }
}
HirNodeId find(const HirModule &m, NodeKind kind, size_t count = 0) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (m.nodes[i].kind() == kind && !count--)
      return HirNodeId{i};
  throw std::runtime_error("probe node missing: " + std::to_string(int(kind)));
}
HirNodeId planned(const HirModule &m, ExitKind kind, size_t defers = 0) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    for (const auto &e : m.nodes[i].exits)
      if (e.kind == kind && e.defers.size() >= defers)
        return HirNodeId{i};
  throw std::runtime_error("probe exit missing");
}
void invalid(const HirModule &input, const std::string &name,
             const std::function<void(HirModule &)> &edit) {
  auto m = input;
  edit(m);
  auto r = verify_types(m);
  need(!r.success() && !r.value && has(r, "E_IR_INVALID") &&
           r.exit_code() == ExitCode::internal_error,
       "damaged HIR accepted: " + name + "\n" + diagnostics(r));
  auto published = verify(m);
  need(!published.value && has(published, "E_IR_INVALID"),
       "damaged HIR reached backend capability check: " + name);
  ++probes;
  std::cout << "bad_hir " << name << " passed\n";
}
void invalid_recomputed(const HirModule &input, const std::string &name,
                        const std::function<void(HirModule &)> &edit) {
  auto m = input;
  edit(m);
  auto derived = detail::produce_ownership(m, {});
  need(derived.success(), name + " ownership recompute " + diagnostics(derived));
  auto r = verify_types(m);
  need(!r.success() && !r.value && has(r, "E_IR_INVALID"),
       "recomputed damaged HIR accepted: " + name + "\n" + diagnostics(r));
  auto published = verify(m);
  need(!published.value && has(published, "E_IR_INVALID"),
       "recomputed damaged HIR reached backend capability check: " + name);
  ++probes;
  std::cout << "bad_hir " << name << " passed\n";
}
void supported(const HirModule &m) {
  auto cap = check_backend_capabilities(m);
  auto token = verify(m);
  need(cap.success() && token.success(),
       "T08 execution support: " + diagnostics(cap) + diagnostics(token));
}
// Build a real source-backed T08 HIR without calling the source checker. Its
// error/cleanup records then receive the same verifier as checker-produced HIR.
HirModule independent() {
  HirModule m;
  m.resolved =
      resolve("enum E { Bad; } func f() throws E { defer {} throw E.Bad; }",
              Backend::native);
  m.identity = ModuleId{0};
  const auto &file = *m.resolved->project->modules[0].files[0];
  auto src = [&](AstNodeId a) {
    return Source{{ModuleId{0}, 0, a}, file.node(a).span};
  };
  auto named = [&](std::string_view name) -> SymbolId {
    for (const auto &s : m.resolved->symbols)
      if (s.name == name)
        return s.id;
    throw std::runtime_error("independent symbol missing");
  };
  auto e = named("E"), v = named("Bad"), f = named("f");
  auto ed = m.resolved->symbols[e.value].definition->node;
  auto vd = m.resolved->symbols[v.value].definition->node;
  auto fd = m.resolved->symbols[f.value].definition->node;
  const auto body = file.node(fd).children.back();
  const auto defer = file.node(body).children[0];
  const auto cleanup = file.node(defer).children[0];
  const auto thr = file.node(body).children[1];
  const auto val = file.node(thr).children[0];
  Type t;
  t.kind = TypeKind::Unit;
  auto unit = m.types.intern(t);
  t.kind = TypeKind::NominalEnum;
  t.declaration = e;
  auto error = m.types.intern(t);
  t = Type{};
  t.kind = TypeKind::Function;
  t.result = unit;
  t.throws_type = error;
  auto signature = m.types.intern(t);
  m.nominals.push_back(
      {e, TypeKind::NominalEnum, {}, {}, {{v, {}, src(vd)}}, src(ed)});
  m.scopes = {{f, {}, src(fd)}, {f, ScopeId{0}, src(cleanup)}};
  m.nodes = {
      {unit, src(cleanup), ScopeId{1}, {}, Block{}},
      {unit, src(defer), ScopeId{0}, {HirNodeId{0}}, Defer{ScopeId{0}, {}}},
      {error, src(val), ScopeId{0}, {}, EnumInit{v, {}}},
      {unit,
       src(thr),
       ScopeId{0},
       {HirNodeId{2}},
       Throw{},
       {error},
       {{ExitKind::error,
         error,
         FunctionExit{f},
         {ScopeId{0}},
         {HirNodeId{1}},
         true}}},
      {unit,
       src(body),
       ScopeId{0},
       {HirNodeId{1}, HirNodeId{3}},
       Block{},
       {error},
       {{ExitKind::normal,
         {},
         ScopeExit{{}},
         {ScopeId{0}},
         {HirNodeId{1}},
         false}}}};
  m.functions.push_back({f, signature, {}, HirNodeId{4}, ScopeId{0}, src(fd)});
  auto r = verify_types(m);
  need(r.success(), "independent builder rejected: " + diagnostics(r));
  supported(m);
  ++probes;
  return m;
}
void module_probes() {
  struct Temp {
    fs::path path;
    Temp() {
      char pattern[] = "/tmp/lang-t08-modules-XXXXXX";
      auto p = mkdtemp(pattern);
      need(p, "module probe temp directory");
      path = p;
      fs::create_directories(path / "src/a");
      fs::create_directories(path / "src/b");
    }
    ~Temp() {
      std::error_code ec;
      fs::remove_all(path, ec);
    }
  } temp;
  auto write = [&](const fs::path &relative, const std::string &s) {
    std::ofstream out(temp.path / relative);
    out << s;
    need(bool(out), "module probe write");
  };
  write("lang.toml",
        "schema_version=1\n[package]\nname=\"t08_modules\"\nsource_root="
        "\"src\"\nentry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
  write("src/a/lib.wz",
        "package a;pub enum E {Bad;} pub func fail() throws E {throw E.Bad;}");
  write("src/b/lib.wz", "package b;pub enum E {Bad;}");
  const std::string prefix =
      "package app; import \"app/a\" as a;import \"app/b\" as b; ";
  for (auto backend : {Backend::native, Backend::python}) {
    write("src/main.wz",
          prefix + "func f() {try {defer {} a.fail();}catch e a.E {}}");
    auto good = project(temp.path / "lang.toml", backend);
    need(good.success(), "qualified catch identity: " + diagnostics(good));
    supported(*good.value);
    write("src/main.wz", prefix + "func f() {try {a.fail();}catch e b.E {}}");
    auto wrong = project(temp.path / "lang.toml", backend);
    need(!wrong.value && has(wrong, "E_EFFECT_UNHANDLED"),
         "distinct modules' E types were equated");
    probes += 2;
  }
}
void source_probes() {
  const std::string e = "enum E { Bad; } enum F { Bad; } ";
  auto finite = accepted(
      "struct S[T] {f func() throws S[S[T]];} func main() i32 {return 0;}");
  need(finite.types.size() < 32 && verify(finite).success(),
       "throws in a function field expanded value layout");
  accepted(e + "func f() throws E { throw E.Bad; }");
  accepted(e + "func main() i32 throws E {throw E.Bad;}");
  accepted("enum Err[T] {Bad(v T);} func danger[T](x T) T throws Err[T] {throw "
           "Err[T].Bad(x);} "
           "func apply[T](f func(T) T throws Err[T],x T) T throws Err[T] "
           "{return f(x);} "
           "func main() {let f func(i32) i32 throws Err[i32]=danger;try "
           "{apply(f,i32(1));}catch e Err[i32] {}}");
  accepted(e +
           "func danger() throws E {} func make() (func() throws E) { return "
           "danger; } "
           "func accept(f func() throws E) {} func g() { accept(make()); }");
  accepted(e + "func f() { defer { let g=func() i32 throws E { throw E.Bad; }; "
               "try { g(); } catch e E {} } }");
  accepted(e +
           "func f() { defer { defer {} for var i i32=0;i<1;i+=1 { if i==0 "
           "{continue;} break; } "
           "try {throw E.Bad;} catch e E {try {throw F.Bad;}catch f F {}} } }");
  accepted(e + "func f() { switch E.Bad {case E.Bad: defer {} break;} }");
  accepted(e + "func error() F throws E {throw E.Bad;} func f() throws E {try "
               "{throw error();}catch f F {}}");
  auto returning = accepted(e + "func g() i32 throws E {throw E.Bad;} func f() "
                                "i32 throws E {defer {} return g();}");
  auto return_id = planned(returning, ExitKind::return_, 1);
  const auto &ret = returning.nodes[return_id.value];
  need(ret.exits[0].saves_value &&
           returning.nodes[ret.children[0].value].exits[0].kind ==
               ExitKind::error,
       "return expression failure and saved return were not represented "
       "separately");
  rejected(e + "func f() { throw E.Bad; }", "E_EFFECT_UNHANDLED");
  rejected(e + "func f() throws E {throw F.Bad;}", "E_EFFECT_UNHANDLED");
  rejected(
      e + "func f() {try {throw E.Bad;}catch e E {throw F.Bad;}catch f F {}}",
      "E_EFFECT_UNHANDLED");
  rejected(e + "func f() {try {throw E.Bad;}catch a E {}catch b E {}}",
           "E_CAPABILITY_UNAVAILABLE");
  rejected(e + "func f() throws E {defer {throw E.Bad;}}", "E_CLEANUP_ESCAPE");
  rejected(e + "func f() {try {defer {throw E.Bad;}}catch e E {}}",
           "E_CLEANUP_ESCAPE");
  rejected(e + "func f() {defer {try {throw E.Bad;}catch e E {throw F.Bad;}}}",
           "E_CLEANUP_ESCAPE");
  rejected(e + "func f() {defer {return;}}", "E_CLEANUP_ESCAPE");
  rejected(e + "func f() {defer {break;}}", "E_CLEANUP_ESCAPE");
  rejected(e + "func f() {defer {continue;}}", "E_CLEANUP_ESCAPE");
  rejected("func f(x i32) {x=1;}", "E_TYPE_MISMATCH");
  rejected("func f[X,Y](x X,y Y) {try {throw x;}catch a X {}catch b Y {}} func "
           "main() {f(1,2);}",
           "E_CAPABILITY_UNAVAILABLE");
  auto result =
      accepted(e + "enum Result[T,X] {Ok(v T); Err(e X);} "
                   "func f() Result[i32,E] {return Result[i32,E].Err(E.Bad);}");
  for (const auto &n : result.nodes)
    need(n.errors.empty(), "Result constructor acquired typed effect");
  auto panic = accepted("func f(x i32) {defer {let a=x/0;let b=x<<-1;}}");
  for (const auto &n : panic.nodes)
    need(n.errors.empty(), "Panic acquired typed effect");
  auto unused =
      accepted(e + "func f[T](x T) throws E {defer {let y=x;}throw E.Bad;}");
  auto token = verify(unused);
  need(token.success(),
       "unused template should not enter backend capability check: " +
           diagnostics(token));
  const auto frozen = dump(*token.value);
  std::get<Defer>(unused.nodes[find(unused, NodeKind::defer_).value].data)
      .references.clear();
  unused.nodes[find(unused, NodeKind::throw_).value].errors.clear();
  need(dump(*token.value) == frozen &&
           verify_types(token.value->module()).success(),
       "new metadata changed through mutable alias");
  ++probes;
}
void corruptions(const std::map<std::string, HirModule> &fixtures) {
  auto m = independent();
  invalid(m, "independent-throw-erased", [&](auto &v) {
    v.nodes[3].data = ExpressionStatement{};
    v.nodes[3].errors.clear();
    v.nodes[3].exits.clear();
    v.nodes[4].errors.clear();
  });
  invalid(m, "independent-error-set",
          [&](auto &v) { v.nodes[3].errors.clear(); });
  invalid(m, "independent-error-target", [&](auto &v) {
    v.nodes[3].exits[0].target = CatchTarget{HirNodeId{999999}, 0};
  });
  invalid(m, "independent-cleanup-missing",
          [&](auto &v) { v.nodes[3].exits[0].defers.clear(); });
  invalid(m, "independent-save-error",
          [&](auto &v) { v.nodes[3].exits[0].saves_value = false; });
  auto a = fixtures.at("case-02");
  auto ret = planned(a, ExitKind::return_, 2);
  invalid(a, "return-save-value",
          [&](auto &v) { v.nodes[ret.value].exits[0].saves_value = false; });
  invalid(a, "cleanup-order", [&](auto &v) {
    std::reverse(v.nodes[ret.value].exits[0].defers.begin(),
                 v.nodes[ret.value].exits[0].defers.end());
  });
  invalid(a, "cleanup-duplicate", [&](auto &v) {
    auto &ds = v.nodes[ret.value].exits[0].defers;
    ds.push_back(ds.back());
  });
  invalid(a, "cleanup-scopes", [&](auto &v) {
    v.nodes[ret.value].exits[0].exited_scopes.pop_back();
  });
  invalid(a, "normal-cleanup-plan-missing", [&](auto &v) {
    v.nodes[planned(v, ExitKind::normal, 1).value].exits.clear();
  });
  invalid(a, "defer-read-before-registration-visible", [&](auto &v) {
    auto outer = std::find_if(
        v.functions.begin(), v.functions.end(), [&](const auto &f) {
          return v.resolved->symbols[f.symbol.value].name == "probe";
        });
    need(outer != v.functions.end(), "probe function missing");
    auto &children = v.nodes[outer->body.value].children;
    auto binding = std::find_if(children.begin(), children.end(), [&](auto id) {
      return v.nodes[id.value].kind() == NodeKind::binding;
    });
    auto defer = std::find_if(children.begin(), children.end(), [&](auto id) {
      return v.nodes[id.value].kind() == NodeKind::defer_;
    });
    need(binding < defer && defer != children.end(),
         "registration probe ordering");
    std::iter_swap(binding, defer);
  });
  auto d = find(a, NodeKind::defer_);
  invalid(a, "defer-registration-owner", [&](auto &v) {
    std::get<Defer>(v.nodes[d.value].data).owner_scope = ScopeId{999999};
  });
  invalid(a, "defer-references-missing", [&](auto &v) {
    std::get<Defer>(v.nodes[d.value].data).references.clear();
  });
  invalid(a, "defer-references-duplicate", [&](auto &v) {
    auto &r = std::get<Defer>(v.nodes[d.value].data).references;
    r.push_back(r.front());
  });
  invalid(a, "defer-reference-id", [&](auto &v) {
    std::get<Defer>(v.nodes[d.value].data)
        .references.push_back(SymbolId{999999});
  });
  auto b = fixtures.at("case-04");
  auto tr = find(b, NodeKind::try_);
  invalid(b, "catch-type-id", [&](auto &v) {
    std::get<Try>(v.nodes[tr.value].data).catches[0].error_type =
        TypeId{999999};
  });
  invalid(b, "catch-binding-id", [&](auto &v) {
    std::get<Try>(v.nodes[tr.value].data).catches[0].binding = SymbolId{999999};
  });
  invalid(b, "catch-scope-id", [&](auto &v) {
    std::get<Try>(v.nodes[tr.value].data).catches[0].scope = ScopeId{999999};
  });
  invalid(b, "catch-body-id", [&](auto &v) {
    std::get<Try>(v.nodes[tr.value].data).catches[0].body = HirNodeId{999999};
  });
  invalid(b, "catch-is-not-pattern", [&](auto &v) {
    auto &c = std::get<Try>(v.nodes[tr.value].data).catches[0];
    c.source = v.nodes[find(v, NodeKind::binding).value].source;
  });
  invalid(b, "try-child-order", [&](auto &v) {
    std::swap(v.nodes[tr.value].children[0], v.nodes[tr.value].children[1]);
  });
  invalid(b, "error-exit-erased", [&](auto &v) {
    v.nodes[planned(v, ExitKind::error).value].exits.clear();
  });
  invalid(b, "throw-future-registration", [&](auto &v) {
    auto id = planned(v, ExitKind::error, 1);
    auto &p = v.nodes[id.value].exits[0];
    auto later = find(v, NodeKind::defer_, 2);
    p.defers.push_back(later);
  });
  auto c = fixtures.at("case-06");
  bool handler_probe = false;
  for (size_t i = 0; i < c.nodes.size(); ++i) {
    auto t = std::get_if<Try>(&c.nodes[i].data);
    if (!t || t->catches.size() != 2)
      continue;
    auto body = t->catches[0].body;
    for (size_t j = 0; j < c.nodes.size(); ++j) {
      if (c.nodes[j].kind() != NodeKind::throw_ ||
          c.nodes[j].scope != c.nodes[body.value].scope)
        continue;
      invalid(c, "handler-must-go-outer", [&, j, i](auto &v) {
        v.nodes[j].exits[0].target = CatchTarget{HirNodeId{i}, 1};
      });
      handler_probe = true;
    }
  }
  need(handler_probe, "handler routing probe not exercised");
  auto loop = fixtures.at("case-03");
  invalid(loop, "continue-exited-scope", [&](auto &v) {
    v.nodes[planned(v, ExitKind::continue_, 1).value]
        .exits[0]
        .exited_scopes.clear();
  });
  auto nested = accepted("enum E {Bad;} func f() {defer {let g=func() i32 "
                         "throws E {throw E.Bad;};try {g();}catch e E {}}}");
  invalid(nested, "nested-function-error-target", [&](auto &v) {
    auto id = find(v, NodeKind::throw_);
    auto &p = v.nodes[id.value].exits[0];
    p.target = FunctionExit{v.functions[0].symbol};
  });
  auto generic =
      accepted("enum Err[T] {Bad(v T);} func fail[T](x T) throws Err[T] {throw "
               "Err[T].Bad(x);} "
               "func use[T](x T) T throws Err[T] {var y T=x;defer {let z=y;} "
               "for var i i32=0;i<1;i+=1 {defer {let z=y;}try {fail(x);if i==0 "
               "{continue;}break;}catch e Err[T] {throw e;}} return y;} "
               "func main() {try {use(1);}catch e Err[i64] {}}");
  need(generic.instances.size() >= 2, "generic instances not produced");
  supported(generic);
  auto generated = generic.instances[0].symbol;
  auto is_generated = [&](const HirModule &v, const HirNode &n) {
    return v.scopes[n.scope.value].function == generated;
  };
  invalid(generic, "instance-error-substitution", [&](auto &v) {
    for (auto &n : v.nodes)
      if (is_generated(v, n) && !n.errors.empty()) {
        n.errors = {TypeId{0}};
        return;
      }
    throw std::runtime_error("missing instance errors");
  });
  invalid(generic, "instance-scope-source", [&](auto &v) {
    auto f = std::find_if(v.functions.begin(), v.functions.end(),
                          [&](const auto &f) { return f.symbol == generated; });
    need(f != v.functions.end(), "generated function missing");
    for (const auto &n : v.nodes)
      if (is_generated(v, n) && n.kind() == NodeKind::return_) {
        v.scopes[f->scope.value].source = n.source;
        return;
      }
    throw std::runtime_error("generated return missing");
  });
  invalid(generic, "instance-origin",
          [&](auto &v) { v.instances[0].origin = v.instances[0].symbol; });
  invalid(generic, "instance-catch-renaming", [&](auto &v) {
    for (auto &n : v.nodes)
      if (is_generated(v, n))
        if (auto t = std::get_if<Try>(&n.data)) {
          t->catches[0].binding = v.templates[0].function.parameters[0].symbol;
          return;
        }
    throw std::runtime_error("missing instance catch");
  });
  invalid(generic, "instance-defer-reference", [&](auto &v) {
    for (auto &n : v.nodes)
      if (is_generated(v, n))
        if (auto d = std::get_if<Defer>(&n.data)) {
          d->references = {v.templates[0].function.parameters[0].symbol};
          return;
        }
    throw std::runtime_error("missing instance defer");
  });
  invalid(generic, "instance-node-source", [&](auto &v) {
    for (auto &n : v.nodes)
      if (is_generated(v, n) && n.kind() == NodeKind::throw_) {
        n.source = v.nodes[find(v, NodeKind::throw_).value].source;
        return;
      }
    throw std::runtime_error("missing instance throw");
  });
  std::stop_source stop;
  stop.request_stop();
  need(verify_types(generic, stop.get_token()).state == PhaseState::cancelled,
       "verifier cancellation lost");
  ++probes;
}
void correction_probes() {
  const std::string errors = "enum E {Bad;} enum F {Bad;} ";
  const std::string generic = errors + "func g[A,B](a A) throws A {defer {} "
                                       "try {defer {} throw a;}catch e B {}} ";
  auto instance = [](const HirModule &m, std::string_view name) -> HirFunction {
    for (const auto &i : m.instances)
      if (m.resolved->symbols[i.origin.value].name == name)
        for (const auto &f : m.functions)
          if (f.symbol == i.symbol)
            return f;
    throw std::runtime_error("correction instance missing");
  };
  auto operation = [](const HirModule &m, const HirFunction &f, NodeKind k) {
    for (size_t i = 0; i < m.nodes.size(); ++i)
      if (m.scopes[m.nodes[i].scope.value].function == f.symbol &&
          m.nodes[i].kind() == k)
        return HirNodeId{i};
    throw std::runtime_error("correction instance operation missing");
  };
  for (auto backend : {Backend::native, Backend::python}) {
    for (bool equal : {true, false}) {
      auto m = accepted(generic + "func main() {try {g[E," +
                            (equal ? "E" : "F") + "](E.Bad);}catch e E {}}",
                        backend);
      const auto f = instance(m, "g");
      const auto id = operation(m, f, NodeKind::throw_);
      const auto &exit = m.nodes[id.value].exits.at(0);
      need(m.nodes[f.body.value].errors.empty() == equal,
           "instance effects did not reflect concrete catch matching");
      need(std::holds_alternative<CatchTarget>(exit.target) == equal &&
               exit.defers.size() == (equal ? 1u : 2u),
           "instance error target/cleanup was copied from the template");
      supported(m);
      invalid(m,
              equal ? "instance-equal-stale-effects"
                    : "instance-distinct-erased-effects",
              [&](auto &v) {
                auto &e = v.nodes[f.body.value].errors;
                if (equal)
                  e = {*v.types.find(f.signature)->throws_type};
                else
                  e.clear();
              });
      invalid(m,
              equal ? "instance-equal-stale-route"
                    : "instance-distinct-false-catch",
              [&](auto &v) {
                auto &e = v.nodes[id.value].exits[0];
                if (equal)
                  e.target = FunctionExit{f.symbol};
                else
                  e.target = CatchTarget{operation(v, f, NodeKind::try_), 0};
              });
      if (equal)
        invalid(m, "instance-equal-stale-cleanup", [&](auto &v) {
          auto &e = v.nodes[id.value].exits[0];
          const auto outer = v.nodes[f.body.value].children[0];
          e.defers.push_back(outer);
          e.exited_scopes.push_back(f.scope);
        });
      std::cout << "correction_source generic-"
                << (equal ? "equal" : "distinct") << " passed\n";
    }
    accepted(errors + "func g(a E) throws E {try {throw a;}catch e E {}}",
             backend);
    accepted(generic + "func main() {let f func(E) throws E=g[E,E];try "
                       "{f(E.Bad);}catch e E {}}",
             backend);
    accepted(generic + "func wrap[X,Y](x X) throws X {g[X,Y](x);} "
                       "func main() {try "
                       "{wrap[E,E](E.Bad);wrap[E,F](E.Bad);}catch e E {}}",
             backend);
    accepted(generic + "func wrap[X,Y](x X) throws X {let f func(X) throws "
                       "X=g[X,Y];f(x);} "
                       "func main() {try {wrap[E,E](E.Bad);}catch e E {}}",
             backend);
    accepted(errors + "func g[A,B](a A) {try {try {defer {} throw a;}catch b B "
                      "{defer {} throw a;}}catch e A {}} "
                      "func main() {g[E,E](E.Bad);g[E,F](E.Bad);}",
             backend);
    accepted("enum Err[T] {Bad(v T);} func g[A,B](a Err[A]) throws Err[A] {try "
             "{throw a;}catch b Err[B] {}} "
             "func main() {try "
             "{g[i32,i32](Err[i32].Bad(i32(1)));g[i32,i64](Err[i32].Bad(i32(1))"
             ");}catch e Err[i32] {}}",
             backend);
    accepted(errors + "func f() i32 {try {return 1;}catch e E {}}", backend);
    accepted(errors + "func f() i32 {try {throw E.Bad;}catch e E {return "
                      "1;}catch f F {}}",
             backend);
    accepted(errors + "func f() i32 {try {return 1;throw E.Bad;}catch e E {}}",
             backend);
    accepted(errors + "func f() i32 {for {try {continue;}catch e E {break;}}}",
             backend);
    accepted(errors + "func f() i32 {for {try {throw E.Bad;}catch e E "
                      "{continue;}catch f F {break;}}}",
             backend);
    accepted(errors + "func f() i32 {try {for false {throw E.Bad;}return "
                      "1;}catch e E {}}",
             backend);
    auto conditional = accepted(
        errors +
            "func f() i32 {try {if false {throw E.Bad;}return 1;}catch e E {}}",
        backend);
    invalid(conditional, "reachable-handler-missing-return", [&](auto &v) {
      std::get<BoolLiteral>(v.nodes[find(v, NodeKind::bool_literal).value].data)
          .value = true;
    });
    auto loop = accepted(errors + "func f() i32 {for {try {if false {throw "
                                  "E.Bad;}continue;}catch e E {break;}}}",
                         backend);
    invalid(loop, "reachable-handler-break-changes-loop-exit", [&](auto &v) {
      for (auto &n : v.nodes)
        if (auto b = std::get_if<BoolLiteral>(&n.data); b && !b->value) {
          b->value = true;
          return;
        }
      throw std::runtime_error("loop condition probe missing");
    });
  }
  rejected(
      errors +
          "func f(x bool) i32 {try {if x {throw E.Bad;}return 1;}catch e E {}}",
      "E_TYPE_MISMATCH");
  rejected(errors + "func f() i32 {for {try {throw E.Bad;}catch e E {break;}}}",
           "E_TYPE_MISMATCH");
  // Different template TypeIds are not proof that these handlers are dead.
  rejected(errors + "func g[A,B](a A) i32 throws A {try {throw a;}catch b B "
                    "{}} func main() {}",
           "E_TYPE_MISMATCH");
  rejected(errors + "func g[A,B](a A) i32 throws A {try {throw a;}catch b B "
                    "{}} func main() {g[E,E](E.Bad);}",
           "E_TYPE_MISMATCH");
  rejected("enum Err[T] {Bad(v T);} func g[A,B](a Err[A]) i32 throws Err[A] "
           "{try {throw a;}catch b Err[B] {}}",
           "E_TYPE_MISMATCH");
  rejected(errors + "func g[A,B](a A) i32 throws A {for {try {throw a;}catch b "
                    "B {break;}}}",
           "E_TYPE_MISMATCH");
  // Distinct concrete types really cannot reach the empty handler.
  accepted(errors + "func f(a E) i32 throws E {try {throw a;}catch f F {}}");
  auto handbuilt = independent();
  invalid(handbuilt, "source-defer-moved-after-throw", [&](auto &m) {
    auto &body = m.nodes[m.functions[0].body.value];
    std::swap(body.children[0], body.children[1]);
    m.nodes[3].exits[0].defers.clear();
  });
  auto after_throw =
      accepted(errors + "func f() throws E {throw E.Bad;defer {}}");
  invalid(after_throw, "source-defer-moved-before-throw", [&](auto &m) {
    auto &body = m.nodes[m.functions[0].body.value];
    auto throw_id = body.children[0], defer_id = body.children[1];
    std::swap(body.children[0], body.children[1]);
    m.nodes[throw_id.value].exits[0].defers = {defer_id};
  });
  auto before_return =
      accepted("func f(n inout i32) i32 {defer {n+=1;}return 0;}");
  invalid(before_return, "source-defer-moved-after-return", [&](auto &m) {
    auto &body = m.nodes[m.functions[0].body.value];
    auto ret = body.children[1];
    std::swap(body.children[0], body.children[1]);
    m.nodes[ret.value].exits.clear();
  });
  auto erased_return = accepted("func f() {defer {} return;defer {}}");
  invalid(erased_return, "source-return-erased-with-noop", [&](auto &m) {
    auto &ret = m.nodes[find(m, NodeKind::return_).value];
    ret.data = Noop{};
    ret.exits.clear();
  });
  auto after_return =
      accepted("func f(n inout i32) i32 {return 0;defer {n+=1;}}");
  invalid(after_return, "source-defer-moved-before-return", [&](auto &m) {
    auto &body = m.nodes[m.functions[0].body.value];
    const auto ret = body.children[0], defer = body.children[1];
    std::swap(body.children[0], body.children[1]);
    m.nodes[ret.value].exits = {{ExitKind::return_,
                                 {},
                                 FunctionExit{m.functions[0].symbol},
                                 {body.scope},
                                 {defer},
                                 true}};
  });
  auto two = accepted(
      "func f(log inout i32) {defer {log=log*10+1;}defer {log=log*10+2;}}");
  invalid(two, "source-two-defers-swapped-with-plans", [&](auto &m) {
    auto &body = m.nodes[m.functions[0].body.value];
    std::swap(body.children[0], body.children[1]);
    std::reverse(body.exits[0].defers.begin(), body.exits[0].defers.end());
  });
  auto branch = accepted(
      "func f(n inout i32,x bool) {if x {defer {n+=1;}}else {defer {n+=2;}}}");
  invalid(branch, "source-branch-owners-swapped", [&](auto &m) {
    auto &node = m.nodes[find(m, NodeKind::if_).value];
    std::swap(node.children[1], node.children[2]);
  });
  auto swap_registrations = [](HirModule &m, HirNodeId first,
                               HirNodeId second) {
    auto &a = m.nodes[first.value], &b = m.nodes[second.value];
    auto da = a.children[0], db = b.children[0];
    std::swap(a.children[0], b.children[0]);
    a.exits[0].defers = {db};
    b.exits[0].defers = {da};
    for (auto pair : {std::pair{da, b.scope}, std::pair{db, a.scope}}) {
      auto &d = m.nodes[pair.first.value];
      d.scope = pair.second;
      std::get<Defer>(d.data).owner_scope = pair.second;
      m.scopes[m.nodes[d.children[0].value].scope.value].parent = pair.second;
    }
  };
  invalid(branch, "source-defer-branch-registration-swapped", [&](auto &m) {
    const auto &node = m.nodes[find(m, NodeKind::if_).value];
    swap_registrations(m, node.children[1], node.children[2]);
  });
  auto tried = accepted(
      errors +
      "func f(n inout i32) {try {defer {n+=1;}}catch e E {defer {n+=2;}}}");
  invalid(tried, "source-defer-try-handler-registration-swapped", [&](auto &m) {
    const auto &node = m.nodes[find(m, NodeKind::try_).value];
    swap_registrations(m, node.children[0], node.children[1]);
  });
  auto order = accepted(
      "func f[T](x T) i32 {defer {let y=x;}return 0;} func main() {f(1);}");
  invalid(order, "source-generic-template-and-instance-reordered",
          [&](auto &m) {
            auto reorder = [&](const HirFunction &f) {
              auto &body = m.nodes[f.body.value];
              auto ret = body.children[1];
              std::swap(body.children[0], body.children[1]);
              m.nodes[ret.value].exits.clear();
            };
            reorder(m.templates[0].function);
            for (const auto &i : m.instances)
              for (const auto &f : m.functions)
                if (f.symbol == i.symbol)
                  reorder(f);
          });
  std::cout << "correction source effects/flow/order passed\n";
}
void unsafe_probes() {
  const std::string valid =
      "func empty(){unsafe{}}"
      "func only_return()i32{unsafe{return 1;}}"
      "func shadow(x i32)i32{unsafe{let x i32=x+1;unsafe{{let y i32=x;"
      "if y!=2{return 9;}}}}return x-1;}"
      "func generic[T](x T)T{unsafe{let y T=x;return y;}}"
      "func lambda()i32{let g func() i32=func() i32{unsafe{let x i32=1;"
      "return x;}};return g();}"
      "func main()i32{empty();if only_return()!=1{return 1;}"
      "if shadow(1)!=0{return 2;}if generic(4)!=4{return 3;}"
      "if lambda()!=1{return 4;}return 0;}";
  for (auto backend : {Backend::native, Backend::python})
    supported(accepted(valid, backend));

  auto rejected_clean = [&](const std::string &body, std::string_view code) {
    for (auto backend : {Backend::native, Backend::python}) {
      auto r = sema::check_types(resolve(body, backend));
      need(!r.success() && !r.value && has(r, code) &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
           body + "\n" + diagnostics(r));
      ++probes;
    }
  };
  rejected_clean("func main()i32{unsafe{let x i32=1;x=2;}return 0;}",
                 "E_TYPE_MISMATCH");
  rejected_clean("func take(v move Vec[i32])i32{return v[0];}"
                 "func main()i32{let v Vec[i32]=[1];let x=take(move v);"
                 "unsafe{return x+v[0];}}",
                 "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected_clean("func main()i32{unsafe{defer{return 1;}}return 0;}",
                 "E_CLEANUP_ESCAPE");
  rejected_clean("enum E{Bad;}func main()throws E{"
                 "unsafe{defer{throw E.Bad;}}}",
                 "E_CLEANUP_ESCAPE");
  rejected_clean("enum E{Bad;}func main(){unsafe{throw E.Bad;}}",
                 "E_EFFECT_UNHANDLED");
  rejected_clean("struct S{a i32;}func bump(x inout i32){}"
                 "func main(){let s=S(a:1);unsafe{bump(inout s.a);}}",
                 "E_TYPE_MISMATCH");
  // `unsafe` does not widen the inout value rule: an owning Vec slot is legal
  // now, while a Task handle stays an explicit capability rejection.
  rejected_clean("func bump(x inout Task[i32,Unit]){}"
                 "func main(){unsafe{}}",
                 "E_CAPABILITY_UNAVAILABLE");
  auto rejected_resolve = [&](const std::string &body, std::string_view code) {
    for (auto backend : {Backend::native, Backend::python}) {
      SourceManager sm;
      auto src =
          sm.update(sm.add_file("t08-static.wz"), "package app; " + body);
      auto tokens = lex(src);
      need(tokens.success(), diagnostics(tokens));
      auto ast = parse(*tokens.value);
      need(ast.success(), diagnostics(ast));
      auto p = std::make_shared<modules::Project>();
      p->backend = backend == Backend::native ? "native" : "python";
      modules::Module m;
      m.id = ModuleId{0};
      m.logical_path = "app";
      m.files.push_back(std::make_shared<const AstFile>(std::move(*ast.value)));
      p->modules.push_back(std::move(m));
      auto names = modules::resolve_project(p);
      need(!names.success() && has(names, code), body + "\n" + diagnostics(names));
      ++probes;
    }
  };
  rejected_resolve("func main()i32{unsafe{let x i32=1;}return x;}",
                   "E_NAME_NOT_FOUND");
  rejected_clean("func main()i32{for{unsafe{defer{break;}}}}",
                 "E_CLEANUP_ESCAPE");

  auto owned = accepted(
      "func f(log inout i64)i32{unsafe{{let n i32=0;}"
      "let v Vec[i32]=[2];defer{log=log*10+3;}return i32(v[0]);}}");
  supported(owned);
  const HirFunction *function = nullptr;
  for (const auto &candidate : owned.functions)
    if (owned.resolved->symbols.at(candidate.symbol.value).name == "f")
      function = &candidate;
  need(function, "unsafe HIR function missing");
  const auto &root = owned.nodes[function->body.value];
  need(root.kind() == NodeKind::block && root.children.size() == 1,
       "unsafe function root shape");
  const auto body = root.children[0];
  need(owned.nodes[body.value].kind() == NodeKind::block,
       "unsafe HIR body is not a lexical block");
  const auto &body_node = owned.nodes[body.value];
  need(body_node.children.size() == 4,
       "unsafe HIR body statement count");
  const auto nested = body_node.children[0];
  need(owned.nodes[nested.value].kind() == NodeKind::block,
       "unsafe HIR nested block missing");
  auto ref = body_node.source.node;
  const auto &file =
      *owned.resolved->project->module(ref.module).files.at(ref.file);
  need(file.node(ref.node).kind == AstKind::Block,
       "unsafe HIR body source is not its real block");
  std::optional<AstNodeId> unsafe_ast;
  for (size_t i = 0; i < file.nodes.size(); ++i) {
    const auto &candidate = file.nodes[i];
    if (candidate.kind == AstKind::Unsafe && candidate.children.size() == 1 &&
        candidate.children[0] == ref.node) {
      unsafe_ast = AstNodeId{i};
      break;
    }
  }
  need(unsafe_ast.has_value(), "unsafe source parent missing");
  auto unsafe_ref = ref;
  unsafe_ref.node = *unsafe_ast;
  const auto unsafe_source = Source{unsafe_ref, file.node(*unsafe_ast).span};
  const auto adjacent_source = owned.nodes[nested.value].source;
  invalid(owned, "unsafe-body-source-unsafe",
          [&](auto &m) { m.nodes[body.value].source = unsafe_source; });
  invalid_recomputed(owned, "unsafe-body-source-unsafe-recomputed",
                     [&](auto &m) { m.nodes[body.value].source = unsafe_source; });
  invalid(owned, "unsafe-body-source-adjacent-block",
          [&](auto &m) { m.nodes[body.value].source = adjacent_source; });
  invalid_recomputed(owned, "unsafe-body-source-adjacent-block-recomputed",
                     [&](auto &m) { m.nodes[body.value].source = adjacent_source; });
  invalid(owned, "unsafe-body-defer-after-return", [&](auto &m) {
    auto &children = m.nodes[body.value].children;
    need(children.size() == 4 &&
             m.nodes[children[2].value].kind() == NodeKind::defer_ &&
             m.nodes[children[3].value].kind() == NodeKind::return_,
         "unsafe reorder probe missing");
    std::swap(children[2], children[3]);
  });
  invalid(owned, "unsafe-body-defer-owner", [&](auto &m) {
    const auto owner = m.scopes[m.nodes[body.value].scope.value].parent;
    need(owner.has_value(), "unsafe body parent scope missing");
    std::get<Defer>(m.nodes[body_node.children[2].value].data).owner_scope =
        *owner;
  });
  invalid(owned, "unsafe-body-normal-cleanup-omitted", [&](auto &m) {
    auto &exits = m.nodes[body.value].exits;
    auto normal = std::find_if(exits.begin(), exits.end(), [](const auto &e) {
      return e.kind == ExitKind::normal;
    });
    need(normal != exits.end(), "unsafe normal cleanup plan missing");
    exits.erase(normal);
  });
  invalid(owned, "unsafe-body-scope-parent", [&](auto &m) {
    auto &scope = m.scopes[m.nodes[body.value].scope.value];
    need(scope.parent.has_value(), "unsafe body scope parent missing");
    scope.parent.reset();
  });
  std::cout << "unsafe lexical block source/cleanup probes passed\n";
}
void short_circuit_flow_probes() {
  const std::string prefix =
      "enum E {Bad;} func fail() bool throws E {throw E.Bad;} ";
  for (auto backend : {Backend::native, Backend::python}) {
    for (const auto *expression : {"false && fail()", "true || fail()"}) {
      auto m = accepted(prefix + "func f() i32 {try {let b=" + expression +
                            ";return 1;}catch e E{}}",
                        backend);
      invalid(m, "short-circuit-reachable-handler-missing-return", [](auto &v) {
        auto &b = std::get<BoolLiteral>(v.nodes[find(v, NodeKind::bool_literal).value].data);
        b.value = !b.value;
      });
    }
  }
  for (const auto *expression : {"true && fail()", "false || fail()"})
    rejected(prefix + "func f() i32 {try {let b=" + expression +
                 ";return 1;}catch e E{}}",
             "E_TYPE_MISMATCH");
  for (const auto *expression : {"x && fail()", "x || fail()"})
    rejected(prefix + "func f(x bool) i32 {try {let b=" + expression +
                 ";return 1;}catch e E{}}",
             "E_TYPE_MISMATCH");
}
void fixtures(const fs::path &root) {
  auto path = root / "tests/cleanup/cases.json";
  auto json = json_object_from_file(path.c_str());
  need(json, "cases.json load failed");
  auto cases = json_object_object_get(json, "cases");
  need(json_object_array_length(cases) == 19, "fixture count changed");
  std::map<std::string, HirModule> positive;
  for (auto b : {Backend::native, Backend::python}) {
    for (size_t i = 0; i < json_object_array_length(cases); ++i) {
      auto c = json_object_array_get_idx(cases, i);
      std::string id = json_object_get_string(json_object_object_get(c, "id"));
      std::string manifest =
          json_object_get_string(json_object_object_get(c, "manifest"));
      auto expected = json_object_object_get(c, "expected_static");
      bool accept =
          std::string_view(json_object_get_string(
              json_object_object_get(expected, "status"))) == "accept";
      auto r = project(root / manifest, b);
      if (accept) {
        need(r.success(), id + " " + diagnostics(r));
        auto v = verify_types(*r.value);
        need(v.success(), id + " " + diagnostics(v));
        supported(*r.value);
        need(dump(*r.value) == dump(*r.value), "dump changed");
        if (b == Backend::native)
          positive.emplace(id, *r.value);
      } else {
        std::string code =
            json_object_get_string(json_object_object_get(expected, "code"));
        need(!r.value && !r.success() && has(r, code),
             id + " " + diagnostics(r));
        need(r.failure == FailureKind::source, id + " wrong failure kind");
      }
      ++probes;
      std::cout << "fixture " << id
                << " backend=" << (b == Backend::native ? "native" : "python")
                << " " << (accept ? "accept" : "reject") << " passed\n";
    }
  }
  json_object_put(json);
  module_probes();
  source_probes();
  unsafe_probes();
  corruptions(positive);
  correction_probes();
  short_circuit_flow_probes();
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--types") {
      auto r = project(argv[2], Backend::native);
      std::cout << diagnostics(r);
      if (r.value)
        std::cout << dump(*r.value);
      return int(r.exit_code());
    }
    need(argc == 2, "usage: lang_cleanup_static_test REPO | --types MANIFEST");
    fixtures(argv[1]);
    std::cout << "passed: " << probes
              << " static checks; T08 execution not_run\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

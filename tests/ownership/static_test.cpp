#include "../../src/hir/ownership.hpp"
#include "lang/sema/check.hpp"
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
auto project(const fs::path &path, Backend backend, unsigned width = 64) {
  SourceManager sm;
  auto p = modules::load_project({path, {}, {}}, sm);
  need(p.success(), diagnostics(p));
  p.value->backend = backend == Backend::native ? "native" : "python";
  auto r = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*p.value)));
  need(r.success(), diagnostics(r));
  return sema::check_types(r);
}
std::shared_ptr<const modules::ResolvedProject>
resolve(const std::string &body, Backend backend, unsigned width = 64) {
  SourceManager sm;
  auto src = sm.update(sm.add_file("t09-static.lang"), "package app; " + body);
  auto tokens = lex(src);
  need(tokens.success(), diagnostics(tokens));
  auto ast = parse(*tokens.value);
  need(ast.success(), diagnostics(ast));
  auto p = std::make_shared<modules::Project>();
  p->backend = backend == Backend::native ? "native" : "python";
  p->word_bits = width;
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
HirModule accepted(const std::string &body, Backend b = Backend::native,
                   unsigned width = 64) {
  auto r = sema::check_types(resolve(body, b, width));
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

void facts(const HirModule &m) {
  need(!m.ownership.full_expressions.empty(),
       "ownership full expressions missing");
  need(!m.ownership.locals.empty(), "ownership locals missing");
  for (auto temp : m.ownership.temporaries) {
    auto k = m.nodes[temp.producer.value].kind();
    need(k != NodeKind::place && k != NodeKind::field_place &&
             k != NodeKind::index_place,
         "assignment place manufactured an owned value");
  }
  auto cap = check_backend_capabilities(m);
  auto pub = verify(m);
  if (m.config.word_bits == 64)
    need(cap.success() && pub.success() && pub.value,
         "supported ownership rejected: " + diagnostics(cap) + diagnostics(pub));
  else
    need(!cap.success() && has(cap, "E_CAPABILITY_UNAVAILABLE") && !pub.value &&
             has(pub, "E_CAPABILITY_UNAVAILABLE"),
         "unsupported ownership width published: " + diagnostics(cap) + diagnostics(pub));
}
const ExitPlan &exit(const HirModule &m, HirNodeId id, ExitKind k) {
  for (const auto &p : m.nodes[id.value].exits)
    if (p.kind == k)
      return p;
  throw std::runtime_error("required exit missing");
}
void test_cases(const fs::path &root) {
  auto path = root / "tests/ownership/cases.json";
  auto *json = json_object_from_file(path.c_str());
  need(json, "cases JSON missing");
  auto *cases = json_object_object_get(json, "cases");
  need(cases, "cases array missing");
  for (size_t i = 0; i < json_object_array_length(cases); ++i) {
    auto *c = json_object_array_get_idx(cases, i);
    std::string id = json_object_get_string(json_object_object_get(c, "id"));
    auto *expected = json_object_object_get(c, "expected_static");
    bool accept = std::string(json_object_get_string(
                      json_object_object_get(expected, "status"))) == "accept";
    for (auto b : {Backend::native, Backend::python}) {
      auto r = project(
          root / json_object_get_string(json_object_object_get(c, "manifest")),
          b);
      if (accept) {
        need(r.success(), id + " static failed: " + diagnostics(r));
        need(verify_types(*r.value).success(), id + " independent verify");
        facts(*r.value);
      } else {
        auto code_text =
            json_object_get_string(json_object_object_get(expected, "code"));
        std::string code = code_text ? code_text : "E_CALL_ARGUMENT";
        need(!r.value && has(r, code),
             id + " expected " + code + " got " + diagnostics(r));
      }
      ++probes;
      std::cout << "original " << id << " " << int(b) << " passed\n";
    }
  }
  json_object_put(json);
}
void flow_tests() {
  const std::string take = "func take(v move Vec[i32]) i32 { return v[0]; } ";
  rejected(
      take +
          "func f() { let x Vec[i32]=[1]; for true { let y=take(move x); } }",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(take + "func f(flag bool) { var x Vec[i32]=[1]; for flag { let "
                  "y=take(move x); continue; } }",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  auto a =
      accepted(take + "func f(flag bool) i32 { var x Vec[i32]=[1]; for flag { "
                      "let y=take(move x); x=[2]; continue; } return x[0]; }");
  facts(a);
  accepted(take + "func f() i32 { let x Vec[i32]=[1]; for true { let "
                  "y=take(move x); return y; } }");
  accepted(take + "func f() i32 { let x Vec[i32]=[1]; if false { let "
                  "a=take(move x); } return x[0]; }");
  accepted(take + "func f() i32 { let x Vec[i32]=[1]; if true { return 0; } "
                  "let y=take(move x); return x[0]; }");
  accepted(take + "func f() i32 { let x Vec[i32]=[1]; let no=false && "
                  "take(move x)==1; return x[0]; }");
  rejected(take + "func f(flag bool) i32 { let x Vec[i32]=[1]; let no=flag && "
                  "take(move x)==1; return x[0]; }",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  const std::string err =
      "enum E { Bad; } func bad() i32 throws E { throw E.Bad; } ";
  rejected(err + take +
               "func use(a i32,v move Vec[i32],b i32) {} func f() i32 { let x "
               "Vec[i32]=[1]; try {use(bad(),move x,bad());} catch e E {return "
               "x[0];} return 0; }",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  accepted(
      err + take +
      "func use(a i32,v move Vec[i32]) {} func f() i32 { let x Vec[i32]=[1]; "
      "try {use(bad(),move x);} catch e E {return x[0];} return 0; }");
  rejected(err + take +
               "func make(v move Vec[i32],a i32) Vec[i32] {return v;} func f() "
               "i32 {var x Vec[i32]=[1];try {x=make(move x,bad());}catch e "
               "E{return x[0];}return 0;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  accepted(
      err +
      "func make() Vec[i32] throws E { let x=bad(); return [1]; } func f() i32 "
      "{var x Vec[i32]=[1];try {x=make();}catch e E{return x[0];}return 0;}");
  rejected(err + take +
               "func use(v move Vec[i32],a i32) {} func f() { let x "
               "Vec[i32]=[1];try {try {use(move x,bad());}catch e E{throw "
               "E.Bad;}}catch outer E{let z=x[0];}}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  accepted(take + "func f(flag bool) { let x Vec[i32]=[1]; if flag {return;} "
                  "let v=take(move x); return; defer {let z=x[0];} }");
  accepted(take + "func f() { var x Vec[i32]=[1]; defer {let z=x[0];} defer "
                  "{x=[2];} let y=take(move x); }");
  rejected(take + "func f() { let x Vec[i32]=[1]; defer {let z=x[0];} defer "
                  "{let y=take(move x);} }",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(take + "func f(v Vec[i32]) {let y=take(move v);}",
           "E_CALL_ARGUMENT");
  rejected(take + "func f() {let x Vec[i32]=[1];let y=take(x);}",
           "E_CALL_ARGUMENT");
  accepted(take + "func f() i32 {return take([1]);}");
  rejected("func f() {let x=[];}", "E_TYPE_MISMATCH");
  rejected("func f() {let x i32=[1];}", "E_TYPE_MISMATCH");
  rejected("func f() {let x i32=1;let y=x[0];}", "E_TYPE_MISMATCH");
  rejected("func f(v Vec[i32]) {v=[2];}", "E_CAPABILITY_UNAVAILABLE");
  rejected("func f(v inout Vec[i32]) {}", "E_CAPABILITY_UNAVAILABLE");
  rejected("func f() {let x Vec[string]=[\"x\"];}", "E_CAPABILITY_UNAVAILABLE");
  rejected("struct S {v Vec[S];} func f(v S) {}", "E_CAPABILITY_UNAVAILABLE");
  rejected("enum E {A(v Vec[i32]);} func make() E {return E.A([1]);} func f() "
           "{switch make() {case E.A(v):let x=v;}}",
           "E_CAPABILITY_UNAVAILABLE");
  auto sl = accepted(
      "func f() {let x Vec[i32]=[1];let a=x[:];let b=x[:1];let c=x[0:];let "
      "d=x[0:1];let k=x[-1];let wide u64=18446744073709551615;let z=x[wide];}");
  facts(sl);
}
void extra_tests() {
  const std::string prefix =
      "enum E {Bad;} struct S {a Vec[i32];b Vec[i32];c Vec[i32];} func bad() "
      "Vec[i32] throws E {throw E.Bad;} ";
  for (auto [construction, count] : std::vector<std::pair<std::string, size_t>>{
           {"S(a:bad(),b:[2],c:[3])", 0},
           {"S(b:[2],a:bad(),c:[3])", 1},
           {"S(b:[2],a:[1],c:bad())", 2}}) {
    auto m =
        accepted(prefix + "func f() throws E {let s=" + construction + ";}");
    auto call = find(m, NodeKind::call);
    const auto &p = exit(m, call, ExitKind::error);
    std::vector<HirNodeId> drops;
    for (auto a : p.actions)
      if (a.owner.kind == OwnerKind::temporary &&
          a.kind == CleanupKind::destroy_if_live)
        drops.push_back(HirNodeId{a.owner.identity});
    need(drops.size() == count, "partial constructor prefix length");
    for (auto id : drops)
      need(m.nodes[id.value].kind() == NodeKind::vec_init,
           "partial constructor destroyed inactive result");
  }
  for (auto [construction, count] :
       std::vector<std::pair<std::string, size_t>>{{"[bad(),[2],[3]]", 0},
                                                   {"[[2],bad(),[3]]", 1},
                                                   {"[[2],[1],bad()]", 2}}) {
    auto m = accepted(prefix + "func f() throws E {let s Vec[Vec[i32]]=" +
                      construction + ";}");
    const auto &p = exit(m, find(m, NodeKind::call), ExitKind::error);
    size_t drops = 0;
    for (auto a : p.actions)
      drops += a.owner.kind == OwnerKind::temporary &&
               a.kind == CleanupKind::destroy_if_live;
    need(drops == count, "Vec construction failure prefix length");
  }
  auto fv = accepted("func take(v move Vec[i32]) i32 {return v[0];} func f() "
                     "i32 {let cb=take;let v Vec[i32]=[1];return cb(move v);}");
  facts(fv);
  rejected("func take(v move Vec[i32]) {} func f() {let v "
           "Vec[i32]=[1];take(move v);take((move v));}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(
      "func keep(v Vec[i32]) {} func f() {let v Vec[i32]=[1];keep((move v));}",
      "E_CALL_ARGUMENT");
  rejected("func both(a inout i32,b move i32) {} func f() {var n "
           "i32=1;both(inout n,move n);}",
           "E_CALL_ARGUMENT");
  rejected("struct S {v Vec[i32];marker i32;} func take(v move S) {} func f() "
           "{var s=S(v:[1],marker:0);take(move s);s.marker=1;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f[T](v move T) {} func g() {let v Vec[i32]=[1];f(move v);}",
           "E_CAPABILITY_UNAVAILABLE");
  auto cb = accepted("func f(cb func(Vec[i32]) i32) {} ");
  need(check_backend_capabilities(cb).success() && verify(cb).success(),
       "Vec callback signature rejected");
  auto v = accepted("func keep(v Vec[i32]) Vec[i32] {let a=v;return v;}");
  auto return_id = find(v, NodeKind::return_);
  auto read_id = v.nodes[return_id.value].children[0];
  auto local =
      std::get<Binding>(v.nodes[find(v, NodeKind::binding).value].data).symbol;
  invalid(v, "consistent records with forged source read", [&](auto &m) {
    std::get<Read>(m.nodes[read_id.value].data).symbol = local;
    for (auto &u : m.ownership.uses)
      if (u.consumer == read_id || u.consumer == return_id)
        u.root = local;
  });
  invalid(v, "forged full-expression owner", [](auto &m) {
    m.ownership.temporaries[0].full_expression = m.functions[0].body;
  });
  invalid(v, "omit call completion", [](auto &m) {
    m.ownership.calls.push_back({m.functions[0].body, {}});
  });
  auto calls =
      accepted("enum E {Bad;} func bad(v Vec[i32]) Vec[i32] throws E {throw "
               "E.Bad;} func f(v Vec[i32]) throws E {let copy=bad(v);}");
  need(!calls.ownership.calls.empty(),
       "ordinary argument borrow completion missing");
  invalid(calls, "delete borrow completion",
          [](auto &m) { m.ownership.calls.clear(); });
  rejected("struct S {x i32;} func f(v Vec[S]) {v[0].x=2;}",
           "E_CAPABILITY_UNAVAILABLE");
  auto before_iteration =
      accepted("enum E {Bad;} func source() Vec[i32] throws E {throw E.Bad;} "
               "func f() throws E {for e in source() {let v=e;}}");
  auto fail_id = find(before_iteration, NodeKind::call);
  const auto &before = exit(before_iteration, fail_id, ExitKind::error);
  need(
      std::none_of(before.actions.begin(), before.actions.end(),
                   [](auto a) { return a.owner.kind == OwnerKind::iteration; }),
      "collection failure ended unregistered iteration");
  auto pattern =
      accepted("enum E {A(v Vec[i32]);B;} func f() {let e=E.A([1]);switch e "
               "{case E.A(v):let kept=v;case E.B:}} ");
  need(std::any_of(
           pattern.ownership.uses.begin(), pattern.ownership.uses.end(),
           [](auto u) { return u.destination && u.kind == UseKind::copy; }),
       "pattern copy destination missing");
  auto switch_id = find(pattern, NodeKind::switch_);
  auto scrutinee = pattern.nodes[switch_id.value].children[0];
  need(std::none_of(pattern.ownership.temporaries.begin(),
                    pattern.ownership.temporaries.end(),
                    [&](auto t) { return t.producer == scrutinee; }),
       "bound owning enum switch must borrow");
  for (auto backend : {Backend::native, Backend::python}) {
    auto width = accepted("func f() {let v Vec[i32]=[1];let index "
                          "u64=18446744073709551615;let x=v[index];}",
                          backend, 32);
    auto index = find(width, NodeKind::index_read);
    auto subscript = width.nodes[index.value].children[1];
    need(width.types.find(width.nodes[subscript.value].type)->bits == 64,
         "index truncated to target word width");
    bool maximum = false;
    for (const auto &n : width.nodes)
      if (auto i = std::get_if<IntegerLiteral>(&n.data))
        maximum |=
            i->decimal == "18446744073709551615" && i->bits == UINT64_MAX;
    need(maximum, "index mathematical integer value lost");
    facts(width);
  }
  auto moved_generic =
      accepted("func relay[T](input T) T {let local=input;let result=move "
               "local;return result;} func f() Vec[i32] {return relay([1]);}");
  facts(moved_generic);
  invalid(moved_generic, "generic move retained template identity",
          [](auto &m) {
            for (auto &n : m.nodes)
              if (auto mv = std::get_if<Move>(&n.data);
                  mv && mv->binding &&
                  m.scopes[n.scope.value].function == m.instances[0].symbol) {
                mv->binding = m.instances[0].renamed[1].first;
                break;
              }
          });
  auto branch =
      accepted("func take(v move Vec[i32]) {} func f(flag bool) i32 {let v "
               "Vec[i32]=[1];if false {take(move v);}return v[0];}");
  invalid(branch, "forged reachable boolean", [](auto &m) {
    auto id = find(m, NodeKind::if_);
    std::get<BoolLiteral>(m.nodes[m.nodes[id.value].children[0].value].data)
        .value = true;
  });
  const std::string borrowing =
      "func use(first Vec[i32],second move Vec[i32]) {} func take(v move "
      "Vec[i32]) i32 {return 0;} ";
  rejected(borrowing + "func f() {let v Vec[i32]=[1];use(v,move v);}",
           "E_CALL_ARGUMENT");
  rejected("func use(first Vec[i32],second i32) {} func take(v move Vec[i32]) "
           "i32 {return 0;} func f() {let v Vec[i32]=[1];use(v,take(move v));}",
           "E_CALL_ARGUMENT");
  rejected(
      "struct S {items Vec[i32];marker i32;} func take(v move S) i32 {return "
      "0;} func f() {var s=S(items:[1],marker:0);s.marker=take(move s);}",
      "E_CALL_ARGUMENT");
  rejected("struct S {items Vec[i32];} func use(v Vec[i32],n i32) {} func "
           "take(v move S) i32 {return 0;} func f() {let "
           "s=S(items:[1]);use(s.items,take(move s));}",
           "E_CALL_ARGUMENT");
  auto borrow = accepted(
      "enum E {Bad;} func bad() i32 throws E {throw E.Bad;} func use(v "
      "Vec[i32],n i32) {} func f(v Vec[i32]) throws E {use(v,bad());}");
  auto fail = find(borrow, NodeKind::call);
  const auto &borrow_exit = exit(borrow, fail, ExitKind::error);
  need(!borrow_exit.actions.empty() &&
           borrow_exit.actions.front().kind == CleanupKind::end_borrow,
       "later argument failure must end earlier borrow");
  auto param = borrow.functions.back().parameters[0].symbol;
  bool fake_copy = false;
  for (auto u : borrow.ownership.uses)
    fake_copy |= u.root == param && u.kind == UseKind::copy;
  need(!fake_copy, "ordinary call binding borrow changed into copy");
  auto explicit_copy = accepted(
      borrowing + "func f() {let v Vec[i32]=[1];let copy=v;use(copy,move v);}");
  facts(explicit_copy);
  auto callback_aggregate =
      accepted("struct S {items Vec[i32];} func f(callback func(S) i32) {} ");
  need(check_backend_capabilities(callback_aggregate).success() &&
           verify(callback_aggregate).success(),
       "owning aggregate callback signature rejected");
  auto loop = accepted(
      "func f(v Vec[Vec[i32]]) Vec[i32] {for e in v {return e;}return [];}");
  const auto &rp = exit(loop, find(loop, NodeKind::return_), ExitKind::return_);
  need(!rp.actions.empty() &&
           rp.actions.front().kind == CleanupKind::save_output &&
           std::any_of(
               rp.actions.begin(), rp.actions.end(),
               [](auto a) { return a.kind == CleanupKind::end_iteration; }),
       "return iteration/output order");
}
void record_tests() {
  auto m = accepted(
      "func f(v Vec[i32]) Vec[i32] {let a=v;defer {let z=a[0];} return v;}");
  facts(m);
  auto ret = find(m, NodeKind::return_);
  const auto &plan = exit(m, ret, ExitKind::return_);
  need(plan.actions.size() >= 3 &&
           plan.actions[0].kind == CleanupKind::save_output &&
           plan.actions[1].kind == CleanupKind::defer_ &&
           plan.actions.back().kind == CleanupKind::destroy_if_live,
       "save/defer/local order");
  need(std::any_of(m.ownership.uses.begin(), m.ownership.uses.end(),
                   [](auto u) {
                     return u.kind == UseKind::copy && u.root.has_value();
                   }),
       "borrowed return/local copy missing");
  invalid(m, "delete all ownership", [](auto &m) { m.ownership = {}; });
  invalid(m, "delete temporary",
          [](auto &m) { m.ownership.temporaries.pop_back(); });
  invalid(m, "wrong public type",
          [](auto &m) { m.ownership.locals[0].type = TypeId{1}; });
  invalid(m, "forged source identity",
          [](auto &m) { m.ownership.uses[0].root = SymbolId{9999}; });
  invalid(m, "cleanup before output", [&](auto &m) {
    std::swap(m.nodes[ret.value].exits[0].actions[0],
              m.nodes[ret.value].exits[0].actions[1]);
  });
  invalid(m, "omit save output", [&](auto &m) {
    m.nodes[ret.value].exits[0].actions.erase(
        m.nodes[ret.value].exits[0].actions.begin());
  });
  invalid(m, "wrong activity side", [&](auto &m) {
    m.nodes[ret.value].exits[0].actions[0].owner.kind = OwnerKind::error_output;
  });
  invalid(m, "duplicate destroy", [&](auto &m) {
    auto &a = m.nodes[ret.value].exits[0].actions;
    a.push_back(a.back());
  });
  invalid(m, "forged defer projection",
          [&](auto &m) { m.nodes[ret.value].exits[0].defers.clear(); });
  invalid(m, "move cleanup other exit", [&](auto &m) {
    auto a = m.nodes[ret.value].exits[0].actions;
    m.nodes[ret.value].exits[0].actions.clear();
    m.nodes.back().exits.back().actions = a;
  });
  const std::string src =
      "enum E {Bad;} struct S {a Vec[i32];b Vec[i32];c Vec[i32];} func bad() "
      "Vec[i32] throws E {throw E.Bad;} func f() throws E {let "
      "s=S(b:[2],a:[1],c:bad());}";
  auto partial = accepted(src);
  facts(partial);
  auto call = find(partial, NodeKind::call);
  const auto &failure = exit(partial, call, ExitKind::error);
  std::vector<uint64_t> drops;
  for (auto a : failure.actions)
    if (a.owner.kind == OwnerKind::temporary &&
        a.kind == CleanupKind::destroy_if_live)
      drops.push_back(a.owner.identity);
  auto first = find(partial, NodeKind::vec_init, 0),
       second = find(partial, NodeKind::vec_init, 1);
  need(drops == std::vector<uint64_t>{second.value, first.value},
       "partial constructor must destroy actual completed a then b, never "
       "c/full S");
  invalid(partial, "reordered partial prefix", [&](auto &m) {
    auto &a = m.nodes[call.value].exits[0].actions;
    std::reverse(a.begin(), a.end());
  });
  auto fi = accepted("func f(v Vec[Vec[i32]],flag bool) {for e in v {if flag "
                     "{continue;}break;}}");
  facts(fi);
  auto cont = find(fi, NodeKind::continue_), br = find(fi, NodeKind::break_);
  auto has_iteration = [](auto &p) {
    return std::any_of(p.actions.begin(), p.actions.end(), [](auto a) {
      return a.kind == CleanupKind::end_iteration;
    });
  };
  need(!has_iteration(exit(fi, cont, ExitKind::continue_)) &&
           has_iteration(exit(fi, br, ExitKind::break_)),
       "continue must retain iteration; break ends it");
  invalid(fi, "continue ends iteration", [&](auto &m) {
    m.nodes[cont.value].exits[0].actions.push_back(
        {CleanupKind::end_iteration,
         {OwnerKind::iteration, find(m, NodeKind::for_in).value},
         TypeId{0}});
  });
  auto gen = accepted(
      "struct Box[T] {value T;} func keep[T](v T) T {let local=v;return "
      "local;} func f() Vec[i32] {let fn=keep[Vec[i32]];let "
      "box=Box[Vec[i32]](value:[1]);return fn(box.value);}");
  facts(gen);
  need(!gen.instances.empty(), "generic instance missing");
  auto sid = gen.instances[0].symbol;
  bool concrete_copy = false;
  for (auto u : gen.ownership.uses)
    if (u.root &&
        gen.scopes[gen.nodes[u.consumer.value].scope.value].function == sid &&
        u.kind == UseKind::copy &&
        gen.types.find(u.type)->kind == TypeKind::Vec)
      concrete_copy = true;
  need(concrete_copy, "generic clone did not recompute concrete copy");
  invalid(gen, "generic ownership stale identity", [](auto &m) {
    for (auto &u : m.ownership.uses)
      if (u.root) {
        u.root = m.instances[0].origin;
        break;
      }
  });
}

// Recompute every submitted fact as a malicious/incorrect producer could do.
// The public verifier must still derive source identity and evaluation order.
void invalid_recomputed(const HirModule &input, const std::string &name,
                        const std::function<void(HirModule &)> &edit) {
  invalid(input, name, [&](auto &m) {
    edit(m);
    auto derived = detail::produce_ownership(m, {});
    need(derived.success(),
         "recompute failed before independent verification: " + name + "\n" +
             diagnostics(derived));
  });
}
void correction_move() {
  const std::string take = "func take(x move Vec[i32]) {} ";
  for (auto b : {Backend::native, Backend::python}) {
    for (const auto &operand : {"x", "(((x)))"}) {
      auto m = accepted(
          take + "func f(){var x Vec[i32]=[1];take(move " + operand + ");}", b);
      invalid_recomputed(
          m,
          "move binding disguised as copied temporary " + std::string(operand),
          [](auto &m) {
            auto id = find(m, NodeKind::move);
            auto &n = m.nodes[id.value];
            auto &mv = std::get<Move>(n.data);
            auto binding = *mv.binding;
            auto source = n.source;
            const auto &file = *m.resolved->project->module(source.node.module)
                                    .files[source.node.file];
            source.node.node = file.node(source.node.node).children[0];
            while (file.node(source.node.node).kind == AstKind::Parenthesized)
              source.node.node = file.node(source.node.node).children[0];
            source.span = file.node(source.node.node).span;
            mv.binding.reset();
            n.children = {HirNodeId{m.nodes.size()}};
            auto type = n.type;
            auto scope = n.scope;
            m.nodes.push_back({type, source, scope, {}, Read{binding}});
          });
    }
    auto generic = accepted(
        "func relay[T](input T) T {let local=input;let result=move "
        "(((local)));return result;} func f() Vec[i32] {return relay([1]);}",
        b);
    invalid_recomputed(
        generic, "concrete generic move binding disguised as copy",
        [](auto &m) {
          for (size_t i = 0; i < m.nodes.size(); ++i) {
            auto &n = m.nodes[i];
            auto *mv = std::get_if<Move>(&n.data);
            if (!mv || !mv->binding ||
                m.scopes[n.scope.value].function != m.instances[0].symbol)
              continue;
            auto binding = *mv->binding;
            auto source = n.source;
            auto type = n.type;
            auto scope = n.scope;
            const auto &file = *m.resolved->project->module(source.node.module)
                                    .files[source.node.file];
            source.node.node = file.node(source.node.node).children[0];
            while (file.node(source.node.node).kind == AstKind::Parenthesized)
              source.node.node = file.node(source.node.node).children[0];
            source.span = file.node(source.node.node).span;
            mv->binding.reset();
            n.children = {HirNodeId{m.nodes.size()}};
            m.nodes.push_back({type, source, scope, {}, Read{binding}});
            return;
          }
          throw std::runtime_error("concrete generic Move missing");
        });
    auto computed = accepted(
        take + "func make(x Vec[i32]) Vec[i32] {var zero i32=0;let "
               "ignored=1/zero;return x;} func f(){let x Vec[i32]=[1];"
               "take(move make(x));let still=x[0];}",
        b);
    invalid_recomputed(
        computed, "computed move replaced its effectful call with an argument",
        [](auto &m) {
          auto move = find(m, NodeKind::move);
          auto removed = m.nodes[move.value].children[0];
          auto argument =
              std::get<Call>(m.nodes[removed.value].data).args[0].expression;
          m.nodes[move.value].children[0] = argument;
          m.nodes.erase(m.nodes.begin() + removed.value);
          auto remap = [&](HirNodeId &id) {
            if (id.value > removed.value)
              --id.value;
          };
          for (auto &n : m.nodes) {
            for (auto &child : n.children)
              remap(child);
            if (auto c = std::get_if<Call>(&n.data)) {
              if (c->callee)
                remap(*c->callee);
              for (auto &arg : c->args)
                remap(arg.expression);
            }
            n.exits.clear();
          }
          for (auto &f : m.functions)
            remap(f.body);
        });
    auto temporary =
        accepted(take + "func make() Vec[i32] {return [2];} func f(){take(move "
                        "((make())));take(move ([3]));}",
                 b);
    need(!std::get<Move>(
              temporary.nodes[find(temporary, NodeKind::move).value].data)
              .binding,
         "computed move incorrectly bound to a local");
    facts(temporary);
  }
}
void correction_slice() {
  const std::string setup =
      "func lo(log inout i32) i32 {log=log*10+1;return 0;} func hi(log inout "
      "i32) i32 {log=log*10+2;return 0;} ";
  for (auto b : {Backend::native, Backend::python}) {
    auto m =
        accepted(setup + "func f() i32 {var log i32=0;let x Vec[i32]=[1,2];let "
                         "y=x[lo(inout log):hi(inout log)];return log;}",
                 b);
    invalid_recomputed(m, "slice swapped endpoints and all recomputed records",
                       [](auto &m) {
                         auto id = find(m, NodeKind::vec_slice);
                         auto &n = m.nodes[id.value];
                         auto &sl = std::get<VecSlice>(n.data);
                         std::swap(n.children[1], n.children[2]);
                         std::swap(sl.start, sl.end);
                       });
    invalid_recomputed(
        m, "slice endpoint replaced by a nested argument read", [](auto &m) {
          auto sid = find(m, NodeKind::vec_slice);
          auto start = *std::get<VecSlice>(m.nodes[sid.value].data).start;
          auto place =
              std::get<Call>(m.nodes[start.value].data).args[0].expression;
          auto symbol = std::get<Place>(m.nodes[place.value].data).symbol;
          m.nodes[start.value].source = m.nodes[place.value].source;
          m.nodes[start.value].data = Read{symbol};
          m.nodes[start.value].children.clear();
          m.nodes.erase(m.nodes.begin() + place.value);
          auto remap = [&](HirNodeId &id) {
            if (id.value > place.value)
              --id.value;
          };
          for (auto &n : m.nodes) {
            for (auto &ch : n.children)
              remap(ch);
            if (auto c = std::get_if<Call>(&n.data)) {
              if (c->callee)
                remap(*c->callee);
              for (auto &arg : c->args)
                remap(arg.expression);
            }
            if (auto sl = std::get_if<VecSlice>(&n.data)) {
              if (sl->start)
                remap(*sl->start);
              if (sl->end)
                remap(*sl->end);
            }
          }
          for (auto &f : m.functions)
            remap(f.body);
        });
    auto omitted = accepted(
        setup + "func f(){var log i32=0;let x Vec[i32]=[1,2];let a=x[:];let "
                "b=(x)[lo(inout log):];let c=x[:(hi(inout log))];let "
                "d=(x[:])[((lo(inout log))):((hi(inout log)))];}",
        b);
    facts(omitted);
    accepted(setup + "func f(){var log i32=0;let x Vec[i32]=[1,2];let "
                     "y=x[(+lo(inout log)):(+(hi(inout log)))];}",
             b);
    auto receiver = accepted("func f(){let x Vec[i32]=[1];let other "
                             "Vec[i32]=[2];let z=other;let a=x[:];}",
                             b);
    invalid_recomputed(
        receiver, "slice receiver replaced by another real source read",
        [](auto &m) {
          auto id = find(m, NodeKind::vec_slice);
          auto receiver = m.nodes[id.value].children[0];
          for (size_t i = 0; i < m.nodes.size(); ++i)
            if (i != receiver.value && m.nodes[i].kind() == NodeKind::read) {
              m.nodes[receiver.value].data = m.nodes[i].data;
              m.nodes[receiver.value].source = m.nodes[i].source;
              return;
            }
          throw std::runtime_error("alternate slice receiver source missing");
        });
  }
}
void correction_cleanup() {
  const std::string take = "func take(v move Vec[i32]) i32 {return v[0];} ";
  const std::string err = "enum E {Bad;} func bad() throws E {throw E.Bad;} ";
  for (auto b : {Backend::native, Backend::python}) {
    for (auto body :
         {"for true {}", "for true {continue;}", "for true {break;}"}) {
      accepted("func f(){defer{" + std::string(body) + "}}", b);
      auto owned = accepted(
          "func f(){let v Vec[i32]=[1];defer{" + std::string(body) + "}}", b);
      facts(owned);
    }
    accepted(take + "func f(){let v Vec[i32]=[1];let n=take(move v);defer{let "
                    "z=v[0];}defer{for true {}}}",
             b);
    accepted(take + "func f(){let v Vec[i32]=[1];let n=take(move v);{defer{for "
                    "true {continue;}}}let z=v[0];}",
             b);
    accepted(take + err +
                 "func f(){let v Vec[i32]=[1];let n=take(move v);try{defer{for "
                 "true {}}throw E.Bad;}catch e E {let z=v[0];}}",
             b);
    accepted(take + "func f(){let v Vec[i32]=[1];for true{let n=take(move "
                    "v);defer{for true {}}continue;}}",
             b);
    accepted(
        take +
            "func f(flag bool){var v Vec[i32]=[1];defer{let z=v[0];}defer{if "
            "flag{for true{}}else{v=[2];}}let n=take(move v);}",
        b);
    accepted("func f() i32 {let v Vec[i32]=[1];defer{for true{}}return v[0];}",
             b);
    accepted(err + "func f(){let v Vec[i32]=[1];defer{for "
                   "true{try{bad();}catch e E {break;}}}}",
             b);
  }
  rejected(take + "func f(){let v Vec[i32]=[1];let n=take(move v);defer{let "
                  "z=v[0];}defer{for true{break;}}}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(
      take +
          "func f(){let v Vec[i32]=[1];defer{for true{let n=take(move v);}}}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f(){let v Vec[i32]=[1];defer{return;}}", "E_CLEANUP_ESCAPE");
  rejected("func f(){let v Vec[i32]=[1];for true{defer{break;}break;}}",
           "E_CLEANUP_ESCAPE");
  rejected("func f(){let v Vec[i32]=[1];for true{defer{continue;}break;}}",
           "E_CLEANUP_ESCAPE");
  rejected(err + "func f(){let v Vec[i32]=[1];defer{bad();}}",
           "E_CLEANUP_ESCAPE");
}
void correction_constraint() {
  const std::string trait =
      "trait Pairer{func pair(self Self,a Vec[i32],n i32)i32;} func take(v "
      "move Vec[i32])i32{return v[0];} ";
  const std::string impl =
      "impl Pairer for i32{func pair(self Self,a Vec[i32],n i32)i32{return "
      "n;}} func main()i32{return f[i32](0);} ";
  const std::string conflict = "func f[T Pairer](r T)i32{let x "
                               "Vec[i32]=[1];return r.pair(x,take(move x));} ";
  const std::string control = "func f[T Pairer](r T)i32{let x Vec[i32]=[1];let "
                              "y Vec[i32]=[2];return r.pair(x,take(move y));} ";
  rejected(trait + conflict, "E_CALL_ARGUMENT");
  rejected(trait + conflict + impl, "E_CALL_ARGUMENT");
  for (auto b : {Backend::native, Backend::python}) {
    auto templ = accepted(trait + control, b);
    facts(templ);
    auto concrete = accepted(trait + control + impl, b);
    facts(concrete);
    auto constraint = find(templ, NodeKind::constraint_call);
    const auto &call =
        std::get<ConstraintCall>(templ.nodes[constraint.value].data);
    auto arg = call.args[1].expression;
    auto root = std::get<Read>(templ.nodes[arg.value].data).symbol;
    need(std::any_of(templ.ownership.uses.begin(), templ.ownership.uses.end(),
                     [&](auto u) {
                       return u.consumer == constraint && u.root == root &&
                              u.kind == UseKind::borrow;
                     }),
         "ConstraintCall borrow ValueUse missing");
    need(std::none_of(templ.ownership.temporaries.begin(),
                      templ.ownership.temporaries.end(),
                      [&](auto t) { return t.producer == arg; }),
         "ConstraintCall silently copied borrowed binding");
    need(std::any_of(templ.ownership.calls.begin(), templ.ownership.calls.end(),
                     [&](const auto &c) {
                       return c.node == constraint &&
                              std::any_of(
                                  c.on_return.begin(), c.on_return.end(),
                                  [&](auto a) {
                                    return a.kind == CleanupKind::end_borrow &&
                                           a.owner.kind == OwnerKind::binding &&
                                           a.owner.identity == root.value;
                                  });
                     }),
         "ConstraintCall completion missing");
    invalid(templ, "ConstraintCall removed completion", [&](auto &m) {
      std::erase_if(m.ownership.calls,
                    [&](const auto &c) { return c.node == constraint; });
    });
    const std::string scalar = "trait Scalar {func pair(self Self,n i32)i32;} "
                               "func f[T Scalar](r T)i32{return r.pair(1);} ";
    accepted(scalar, b);
    accepted(scalar + "impl Scalar for i32{func pair(self Self,n "
                      "i32)i32{return n;}}func main()i32{return f[i32](0);}",
             b);
    auto failure = accepted(
        "enum E{Bad;} trait Pairer{func pair(self Self,a Vec[i32],n i32)i32 "
        "throws E;} func bad()i32 throws E{throw E.Bad;} func f[T Pairer](r "
        "T)i32 throws E{let x Vec[i32]=[1];return r.pair(x,bad());}",
        b);
    auto cid = find(failure, NodeKind::constraint_call);
    const auto &cc = std::get<ConstraintCall>(failure.nodes[cid.value].data);
    auto borrowed =
        std::get<Read>(failure.nodes[cc.args[1].expression.value].data).symbol;
    auto ends = [&](const ExitPlan &p) {
      return std::any_of(p.actions.begin(), p.actions.end(), [&](auto a) {
        return a.kind == CleanupKind::end_borrow &&
               a.owner.kind == OwnerKind::binding &&
               a.owner.identity == borrowed.value;
      });
    };
    need(ends(exit(failure, cid, ExitKind::error)),
         "ConstraintCall own failure borrow prefix missing");
    need(ends(exit(failure, find(failure, NodeKind::call), ExitKind::error)),
         "ConstraintCall later-argument failure borrow prefix missing");
  }
}
void corrections(std::string_view group) {
  if (group == "all" || group == "move")
    correction_move();
  if (group == "all" || group == "slice")
    correction_slice();
  if (group == "all" || group == "cleanup")
    correction_cleanup();
  if (group == "all" || group == "constraint")
    correction_constraint();
  std::cout << "correction " << group << " passed\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    need(argc == 2 || argc == 3,
         "source root and optional correction group required");
    if (argc == 3) {
      corrections(argv[2]);
      std::cout << "T09 correction probes " << probes << " passed\n";
      return 0;
    }
    test_cases(argv[1]);
    flow_tests();
    record_tests();
    extra_tests();
    for (auto b : {Backend::native, Backend::python}) {
      auto r = project(fs::path(argv[1]) /
                           "tests/ownership/static-extra/independent/lang.toml",
                       b);
      need(r.success(), "independent case: " + diagnostics(r));
      facts(*r.value);
      ++probes;
    }
    corrections("all");
    std::cout << "T09 probes " << probes << " passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

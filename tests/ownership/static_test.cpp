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

void invalid_recomputed(const HirModule &, const std::string &,
                        const std::function<void(HirModule &)> &);
std::vector<TypeId> nominal_types(const HirModule &m, std::string_view name) {
  std::vector<TypeId> result;
  for (uint64_t i = 1; i <= m.types.size(); ++i) {
    const auto *t = m.types.find(TypeId{i});
    if (!t || (t->kind != TypeKind::NominalStruct &&
               t->kind != TypeKind::NominalEnum))
      continue;
    if (t->declaration.value < m.resolved->symbols.size() &&
        m.resolved->symbols[t->declaration.value].name == name)
      result.push_back(TypeId{i});
  }
  return result;
}
const NominalDecl &nominal_named(const HirModule &m, std::string_view name) {
  for (const auto &d : m.nominals)
    if (d.symbol.value < m.resolved->symbols.size() &&
        m.resolved->symbols[d.symbol.value].name == name)
      return d;
  throw std::runtime_error("nominal missing: " + std::string(name));
}
size_t field_index_named(const HirModule &m, const NominalDecl &d,
                         std::string_view name) {
  for (size_t i = 0; i < d.fields.size(); ++i)
    if (m.resolved->symbols[d.fields[i].symbol.value].name == name)
      return i;
  throw std::runtime_error("field missing: " + std::string(name));
}
bool concrete_nominal(const HirModule &m, TypeId id) {
  std::vector<TypeId> pending{id};
  std::set<uint64_t> seen;
  while (!pending.empty()) {
    auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current.value).second)
      continue;
    const auto &t = *m.types.find(current);
    if (t.kind == TypeKind::TypeParameter)
      return false;
    pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
  }
  return true;
}
// The recursive value graph is a reference graph: a Vec element back-edge is
// a finite shape, while a direct nominal value field is an infinite layout.
void recursive_value_tests() {
  auto node = accepted(
      "struct Node {value i32;children Vec[Node];} "
      "func make(v i32) Node {return Node(value:v,children:[]);} "
      "func main() i32 {let leaf=make(7);let root=Node(value:5,children:"
      "[leaf]);return root.value+root.children[0].value-12;}");
  facts(node);
  need(!infinite_value_layout(node), "Vec-recursive Node is not infinite");
  auto nodes = nominal_types(node, "Node");
  need(nodes.size() == 1, "single Node declaration expected");
  auto node_ops = classify_value(node, nodes[0]);
  need(node_ops.kind == ValueKind::structure && node_ops.needs_drop &&
           node_ops.fields.size() == 2,
       "recursive Node classification incomplete");
  auto children_ops = classify_value(node, node_ops.fields[1].type);
  need(children_ops.kind == ValueKind::vec && children_ops.needs_drop,
       "recursive Vec field not owned");
  need(node.types.find(children_ops.fields.empty() ? node_ops.fields[1].type
                                                   : children_ops.fields[0].type),
       "recursive field TypeId left the public context");
  const auto &children_type =
      *node.types.find(node_ops.fields[1].type);
  need(children_type.kind == TypeKind::Vec &&
           children_type.type_args.at(0) == nodes[0],
       "recursive Vec element is not the exact Node TypeId");

  auto stable = accepted(
      "struct Node[T] {value T;children Vec[Node[T]];} "
      "func make[T](v T) Node[T] {return Node[T](value:v,children:[]);} "
      "func first[T](n Node[T]) T {return n.value;} "
      "func echo[T](n Node[T]) Node[T] {return n;} "
      "func main() i32 {let a=Node[i32](value:1,children:[]);let "
      "b=Node[i32](value:2,children:[a]);let c=make[i32](3);let "
      "d=Node[bool](value:true,children:[]);let e=make[bool](false);let "
      "f=echo[i32](b);if first[i32](b)!=2||f.value!=2||f.children[0].value!=1"
      "||first[i32](c)!=3||!d.value||e.value{return 1;}return 0;}");
  facts(stable);
  auto stable_nodes = nominal_types(stable, "Node");
  size_t concrete = 0, saw_i32 = 0, saw_bool = 0;
  for (auto id : stable_nodes) {
    if (!concrete_nominal(stable, id))
      continue;
    ++concrete;
    auto ops = classify_value(stable, id);
    need(ops.kind == ValueKind::structure && ops.needs_drop,
         "concrete Node[T] is not owned");
    for (const auto &f : ops.fields)
      need(stable.types.find(f.type),
           "concrete Node[T] field escaped the public TypeContext");
    const auto &args = stable.types.find(id)->type_args;
    need(!args.empty(), "concrete Node type has no actual argument");
    auto arg_kind = stable.types.find(args[0])->kind;
    saw_i32 += arg_kind == TypeKind::SignedInt;
    saw_bool += arg_kind == TypeKind::Bool;
  }
  need(concrete >= 2 && saw_i32 && saw_bool,
       "Node[T] did not keep two concrete instances");

  auto periodic = accepted(
      "struct Flip[A,B] {tag A;children Vec[Flip[B,A]];} "
      "struct Box[T] {value T;} struct S[T] {f func() S[S[T]];} "
      "func bottom() S[S[i32]] {return bottom();} "
      "func pass(x S[i32]) S[i32] {return x;} "
      "func main() i32 {let a=Flip[i32,bool](tag:1,children:[]);let "
      "b=Flip[bool,i32](tag:true,children:[a]);let "
      "c=Flip[i32,bool](tag:12,children:[b]);if c.tag!=12||!c.children[0].tag"
      "||c.children[0].children[0].tag!=1{return 1;}let "
      "box=Box[Box[i32]](value:Box[i32](value:7));if "
      "box.value.value!=7{return 2;}let x=S[i32](f:bottom);let y=pass(x);"
      "return 0;}");
  facts(periodic);
  auto flips = nominal_types(periodic, "Flip");
  need(!flips.empty(), "Flip declaration missing");
  bool saw_swap = false;
  for (auto id : flips) {
    if (!concrete_nominal(periodic, id))
      continue;
    const auto &decl = nominal_named(periodic, "Flip");
    auto ops = classify_value(periodic, id);
    auto children = field_index_named(periodic, decl, "children");
    const auto &vec = *periodic.types.find(ops.fields[children].type);
    need(vec.kind == TypeKind::Vec, "Flip children is not Vec");
    auto other = vec.type_args.at(0);
    need(other.value != id.value, "periodic Flip did not change TypeId");
    auto other_ops = classify_value(periodic, other);
    auto other_children = field_index_named(periodic, decl, "children");
    const auto &back =
        *periodic.types.find(other_ops.fields[other_children].type);
    need(back.kind == TypeKind::Vec && back.type_args.at(0) == id,
         "Flip period did not return to the exact TypeId");
    saw_swap = true;
  }
  need(saw_swap, "no concrete Flip period checked");
  auto unused = accepted(
      "struct Grow[T] {children Vec[Grow[Vec[T]]];} "
      "func main() i32 {let v Vec[i32]=[1];return v[0]-1;}");
  need(!infinite_value_layout(unused),
       "unused growing declaration was treated as infinite layout");
  need(check_backend_capabilities(unused).success(),
       "unused growing declaration rejected by backend capability check");
  for (auto backend : {Backend::native, Backend::python}) {
    auto r = sema::check_types(resolve(
        "struct Grow[T] {children Vec[Grow[Vec[T]]];} "
        "func main() i32 {let v=Grow[i32](children: []);return 0;}",
        backend));
    need(!r.success() && has(r, "E_CAPABILITY_UNAVAILABLE") &&
             !r.diagnostics.empty(),
         "growing concrete family was not budget-rejected");
    const auto &d = r.diagnostics.front();
    need(d.message.find("budget") != std::string::npos,
         "growth diagnostic does not name the budget");
    need(d.primary && d.primary->end_byte > d.primary->begin_byte,
         "growth diagnostic has no source span");
  }

  for (auto body : {std::string("struct Bad {value Bad;}"),
                    std::string("struct A {b B;} struct B {a A;}"),
                    std::string(
                        "struct Grow[T] {next Grow[Vec[T]];}")}) {
    auto r = sema::check_types(resolve(
        body + " func main() i32 {return 0;}", Backend::native));
    need(!r.success() && has(r, "E_TYPE_MISMATCH"),
         "infinite value layout was not rejected: " + body);
  }
  auto finite_mutated = node;
  auto &node_decl = const_cast<NominalDecl &>(
      nominal_named(finite_mutated, "Node"));
  node_decl.fields.push_back({node_decl.fields[0].symbol, nodes[0], false,
                              node_decl.fields[0].source});
  need(infinite_value_layout(finite_mutated) == node_decl.symbol,
       "finite-layout helper missed a direct value cycle");
  bool classifier_stopped = false;
  try {
    (void)classify_value(finite_mutated, nodes[0]);
  } catch (const std::invalid_argument &) {
    classifier_stopped = true;
  }
  need(classifier_stopped,
       "standalone classifier did not stop on a value-storage cycle");

  auto cycle = accepted(
      "struct Node {value i32;children Vec[Node];} "
      "func main() i32 {var v Vec[Node]=[Node(value:1,children:[])];"
      "v[0]=Node(value:2,children:v);return 0;}");
  facts(cycle);

  auto copied = accepted(
      "struct Node {value i32;children Vec[Node];} "
      "func make(v i32) Node {return Node(value:v,children:[]);} "
      "func main() i32 {let n=make(1);let alias=n;return alias.value-1;}");
  facts(copied);
  bool removed = false;
  invalid(copied, "recursive copy use removed", [&](auto &m) {
    for (auto it = m.ownership.uses.begin(); it != m.ownership.uses.end();
         ++it) {
      const auto &t = *m.types.find(it->type);
      if (it->kind == UseKind::copy && it->root &&
          (t.kind == TypeKind::NominalStruct ||
           t.kind == TypeKind::NominalEnum)) {
        m.ownership.uses.erase(it);
        removed = true;
        return;
      }
    }
  });
  need(removed, "recursive copy use was not present");
  bool field_changed = false;
  invalid(copied, "recursive public member TypeId missing", [&](auto &m) {
    auto &d = const_cast<NominalDecl &>(nominal_named(m, "Node"));
    d.fields[field_index_named(m, d, "children")].type =
        TypeId{m.types.size() + 1};
    field_changed = true;
  });
  need(field_changed, "recursive member field missing");
  invalid_recomputed(
      copied, "recursive Vec element replaced with another concrete type",
      [](auto &m) {
        TypeId ints;
        for (uint64_t i = 1; i <= m.types.size(); ++i)
          if (m.types.find(TypeId{i})->kind == TypeKind::SignedInt)
            ints = TypeId{i};
        need(ints.value, "i32 TypeId missing");
        Type vec;
        vec.kind = TypeKind::Vec;
        vec.type_args = {ints};
        auto vec_int = m.types.intern(std::move(vec));
        auto &d = const_cast<NominalDecl &>(nominal_named(m, "Node"));
        d.fields[field_index_named(m, d, "children")].type = vec_int;
      });
  invalid_recomputed(copied, "recursive struct initializer field index",
                     [](auto &m) {
                       auto id = find(m, NodeKind::struct_init);
                       auto &init = std::get<StructInit>(m.nodes[id.value].data);
                       need(init.fields.size() > 1, "Node initializer fields");
                       std::swap(init.fields[0].parameter_index,
                                 init.fields[1].parameter_index);
                     });
  invalid(copied, "recursive value field without Vec", [&](auto &m) {
    auto &d = const_cast<NominalDecl &>(nominal_named(m, "Node"));
    d.fields.push_back({d.fields[0].symbol, nodes[0], false,
                        d.fields[0].source});
  });

  auto instance = accepted(
      "struct Node[T] {value T;children Vec[Node[T]];} "
      "func make[T](v T) Node[T] {return Node[T](value:v,children:[]);} "
      "func main() i32 {let n=make[i32](1);return n.value-1;}");
  facts(instance);
  need(!instance.instances.empty(), "concrete recursive instance missing");
  invalid_recomputed(instance, "recursive instance actual argument changed",
                     [](auto &m) {
                       TypeId bools;
                       for (uint64_t i = 1; i <= m.types.size(); ++i)
                         if (m.types.find(TypeId{i})->kind == TypeKind::Bool)
                           bools = TypeId{i};
                       need(bools.value, "bool TypeId missing");
                       m.instances[0].type_args = {bools};
                     });

  auto growth = accepted(
      "struct Grow[T] {children Vec[Grow[Vec[T]]];} "
      "func main() i32 {let v Vec[i32]=[1];return v[0]-1;}");
  auto growth_bad = growth;
  TypeId ints;
  for (uint64_t i = 1; i <= growth_bad.types.size(); ++i)
    if (growth_bad.types.find(TypeId{i})->kind == TypeKind::SignedInt)
      ints = TypeId{i};
  const auto &grow_decl = nominal_named(growth_bad, "Grow");
  Type grow_type;
  grow_type.kind = TypeKind::NominalStruct;
  grow_type.declaration = grow_decl.symbol;
  grow_type.type_args = {ints};
  auto grow_i32 = growth_bad.types.intern(std::move(grow_type));
  growth_bad.nodes[find(growth_bad, NodeKind::binding).value].type = grow_i32;
  auto stopped = detail::verify_ownership(growth_bad, {});
  need(!stopped.success() && has(stopped, "E_IR_INVALID"),
       "independent ownership verifier did not stop on a growing concrete "
       "family");
}

void folded_constant_move_tests() {
  const std::string setup =
      "const x string=\"ok\";let y=move x;";
  rejected("func main() i32 {" + setup +
               "switch x {case \"ok\":return 0;default:return 1;}}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f() string {" + setup + "return x;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f(flag bool) string {const x string=\"ok\";"
           "if flag {let y=move x;}return x;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f() {const x string=\"ok\";defer {let z=x;}"
           "let y=move x;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f[T]() string {" + setup + "return x;}"
           "func main() i32 {let z=f[i32]();return 0;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  rejected("func f() bool {const x bool=true;let y=move x;return x;}",
           "E_OWNERSHIP_USE_AFTER_MOVE");
  for (auto b : {Backend::native, Backend::python}) {
    facts(accepted("func main() i32 {" + setup +
                   "switch y {case \"ok\":return 0;default:return 1;}}", b));
    facts(accepted("func f() string {const x string=\"ok\";const alias=x;"
                   "let y=move x;return alias;}", b));
    facts(accepted("func f() string {const x string=\"ok\";"
                   "if false {let y=move x;}return x;}", b));
  }
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
  rejected("func f(v Vec[i32]) {v=[2];}", "E_TYPE_MISMATCH");
  rejected("func f(v inout Vec[i32]) {}", "E_CAPABILITY_UNAVAILABLE");
  auto string_vec = accepted("func f() {let x Vec[string]=[\"x\"];}");
  facts(string_vec);
  bool saw_string = false;
  for (uint64_t t = 1; t <= string_vec.types.size(); ++t) {
    if (string_vec.types.find(TypeId{t})->kind != TypeKind::String)
      continue;
    auto ops = classify_value(string_vec, TypeId{t});
    need(ops.kind == ValueKind::trivial && !ops.needs_drop && !ops.pod_inout,
         "String must be trivial/no-drop/non-POD");
    saw_string = true;
  }
  need(saw_string, "Vec[string] did not publish a String type");
  auto mixed = accepted(
      "struct Mix{text string;items Vec[i32];} func make() Mix{return "
      "Mix(text:\"x\",items:[1]);} func f(){let m=make();let alias=m;}");
  TypeId mixed_string{}, mixed_struct{};
  for (uint64_t t = 1; t <= mixed.types.size(); ++t) {
    const auto kind = mixed.types.find(TypeId{t})->kind;
    if (kind == TypeKind::String)
      mixed_string = TypeId{t};
    if (kind == TypeKind::NominalStruct)
      mixed_struct = TypeId{t};
  }
  need(mixed_string.value && mixed_struct.value, "mixed types missing");
  bool injected = false;
  invalid(mixed, "mixed String+Vec exit drops String instead of aggregate",
          [&](auto &m) {
            for (auto &n : m.nodes)
              for (auto &plan : n.exits)
                for (auto &a : plan.actions)
                  if (a.kind == CleanupKind::destroy_if_live &&
                      a.type == mixed_struct) {
                    a.type = mixed_string;
                    injected = true;
                    return;
                  }
          });
  need(injected, "mixed String+Vec destroy action missing");
  // A nominal type may refer back to itself through Vec.  The exact TypeId
  // back-edge is a finite reference cycle, not an infinite value layout.
  auto recursive_s = accepted("struct S {v Vec[S];} func f(v S) {}");
  auto recursive_s_types = nominal_types(recursive_s, "S");
  need(recursive_s_types.size() == 1 &&
           classify_value(recursive_s, recursive_s_types[0]).needs_drop,
       "recursive struct field is not classified as owned");
  // Computed owning enum switch is now supported: the subject is evaluated
  // once and held to the end of the selected arm (SEMANTICS switch rule).
  auto computed = accepted(
      "enum E {A(v Vec[i32]);B;} func make() E {return E.A([1]);} func f() "
      "{switch make() {case E.A(v):let x=v;case E.B:}}");
  facts(computed);
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
  auto generic_move_call =
      accepted("func f[T](v move T) {} func g() {let v Vec[i32]=[1];f(move "
               "v);}");
  facts(generic_move_call);
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
  auto indexed_field = accepted(
      "struct S {x i32;} func f(v Vec[S]) {v[0].x=2;}");
  facts(indexed_field);
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
void computed_owning_switch_tests() {
  // The computed owning subject is a real Temporary of the switch full
  // expression; the switch full expression itself must not destroy it.
  auto m = accepted(
      "enum Data {Empty; Items(v Vec[i32]);} func make() Data {return "
      "Data.Items(v:[1]);} func f() i32 {switch make() {case "
      "Data.Empty:return 0;case Data.Items(v):let x=v;return 1;}}");
  facts(m);
  auto sw = find(m, NodeKind::switch_);
  auto subject = m.nodes[sw.value].children[0];
  auto mf = m.functions.back().symbol;
  HirNodeId f_return{0};
  for (size_t i = 0; i < m.nodes.size() && !f_return.value; ++i)
    if (m.nodes[i].kind() == NodeKind::return_ &&
        m.scopes.at(m.nodes[i].scope.value).function == mf)
      f_return = HirNodeId{i};
  need(f_return.value != 0, "f return missing");
  need(std::any_of(
           m.ownership.temporaries.begin(), m.ownership.temporaries.end(),
           [&](auto t) {
             return t.producer == subject && t.full_expression == sw;
           }),
       "computed owning subject is not a real switch temporary");
  need(std::any_of(m.ownership.uses.begin(), m.ownership.uses.end(),
                   [](auto u) {
                     return u.destination && u.kind == UseKind::copy &&
                            !u.root;
                   }),
       "computed pattern copy destination missing");
  for (const auto &fe : m.ownership.full_expressions)
    if (fe.node == sw)
      need(std::none_of(
               fe.actions.begin(), fe.actions.end(),
               [&](auto a) {
                 return a.kind == CleanupKind::destroy_if_live &&
                        a.owner.identity == subject.value;
               }),
           "switch full expression destroys the held subject");
  {
    bool checked_return = false;
    for (size_t i = 0; i < m.nodes.size(); ++i)
      if (m.nodes[i].kind() == NodeKind::return_ &&
          m.scopes.at(m.nodes[i].scope.value).function == mf) {
        const auto &rp = exit(m, HirNodeId{i}, ExitKind::return_);
        auto found = std::find_if(
            rp.actions.begin(), rp.actions.end(), [&](auto a) {
              return a.kind == CleanupKind::destroy_if_live &&
                     a.owner.kind == OwnerKind::temporary &&
                     a.owner.identity == subject.value;
            });
        need(found != rp.actions.end(), "arm return lacks subject drop");
        auto binding = std::find_if(rp.actions.begin(), found, [](auto a) {
          return a.kind == CleanupKind::destroy_if_live &&
                 a.owner.kind == OwnerKind::binding;
        });
        if (binding != found) {
          need(binding != found, "pattern/local drop after subject drop");
          checked_return = true;
        }
      }
    need(checked_return, "no arm return with a pattern binding");
  }
  // Arm defers and pattern bindings run before the subject drop, which in
  // turn runs before any enclosing scope cleanup, for every exit kind.
  auto ordered = accepted(
      "enum Data {Empty; Items(v Vec[i32]);} func mark(v i32) {} func f() i32 "
      "throws Data {let outer Vec[i32]=[2]; switch Data.Items(v:[1]) {case "
      "Data.Empty:return 0;case Data.Items(v):defer {mark(v[0]);} throw "
      "Data.Items(v:v);} return 0;}");
  facts(ordered);
  auto os = find(ordered, NodeKind::switch_);
  auto osubject = ordered.nodes[os.value].children[0];
  auto f2_symbol = ordered.functions.back().symbol;
  std::optional<SymbolId> outer_symbol;
  for (const auto &l : ordered.ownership.locals)
    if (l.scope == ordered.functions.back().scope)
      outer_symbol = l.symbol;
  need(outer_symbol.has_value(), "outer local missing");
  for (auto kind : {ExitKind::normal, ExitKind::return_, ExitKind::break_,
                    ExitKind::continue_, ExitKind::error}) {
    bool saw_subject_drop = false;
    for (size_t i = 0; i < ordered.nodes.size(); ++i)
      if (ordered.scopes.at(ordered.nodes[i].scope.value).function ==
          f2_symbol)
        for (const auto &p : ordered.nodes[i].exits)
          if (p.kind == kind) {
            auto drop = std::find_if(
                p.actions.begin(), p.actions.end(), [&](auto a) {
                  return a.kind == CleanupKind::destroy_if_live &&
                         a.owner.identity == osubject.value;
                });
            if (drop == p.actions.end())
              continue;
            saw_subject_drop = true;
            for (auto d : p.defers) {
              auto da = std::find_if(p.actions.begin(), drop, [&](auto a) {
                return a.kind == CleanupKind::defer_ &&
                       a.owner.identity == d.value;
              });
              need(da != drop, "arm defer runs after subject drop");
            }
            for (auto it = p.actions.begin(); it != drop; ++it)
              need(!(it->kind == CleanupKind::destroy_if_live &&
                     it->owner.kind == OwnerKind::binding &&
                     it->owner.identity == outer_symbol->value),
                   "outer local dropped before the arm subject");
            for (auto it = std::next(drop); it != p.actions.end(); ++it)
              need(!(it->kind == CleanupKind::destroy_if_live &&
                     it->owner.kind == OwnerKind::binding &&
                     it->owner.identity != outer_symbol->value),
                   "arm binding dropped after the arm subject");
          }
    if (kind == ExitKind::normal || kind == ExitKind::error)
      need(saw_subject_drop, "arm exit kind missing subject drop");
  }
  // A subject-evaluation failure never cleans the unproduced result, while
  // an error leaving the arm does (after the arm's own cleanup).
  const std::string err =
      "enum E {Bad;} func bad() i32 throws E {throw E.Bad;} ";
  auto fail = accepted(
      err + "enum Data {Empty; Items(v Vec[i32]);} func make(n i32, v "
      "Vec[i32]) Data throws E {let x=bad(); return Data.Empty;} func f() "
      "i32 throws E {switch make(1,[9]) {case Data.Empty:return 0;case "
      "Data.Items(v):let x=bad();return 1;}}");
  facts(fail);
  auto fs = find(fail, NodeKind::switch_);
  auto fsubject = fail.nodes[fs.value].children[0];
  auto inside = [&](HirNodeId node, HirNodeId container) {
    std::function<bool(HirNodeId)> search = [&](HirNodeId current) {
      if (current == node)
        return true;
      for (auto child : fail.nodes[current.value].children)
        if (search(child))
          return true;
      return false;
    };
    return node != container && search(container);
  };
  auto f_symbol = fail.functions.back().symbol;
  for (size_t i = 0; i < fail.nodes.size(); ++i)
    if (fail.scopes.at(fail.nodes[i].scope.value).function == f_symbol)
      for (const auto &e : fail.nodes[i].exits)
        if (e.kind == ExitKind::error) {
          bool in_subject = HirNodeId{i} == fsubject ||
                            inside(HirNodeId{i}, fsubject);
        bool drops_subject = std::any_of(
            e.actions.begin(), e.actions.end(), [&](auto a) {
              return a.kind == CleanupKind::destroy_if_live &&
                     a.owner.identity == fsubject.value;
            });
        need(drops_subject != in_subject,
             in_subject ? "subject failure cleans an unproduced result"
                        : "arm error does not release the subject");
      }
  invalid(fail, "computed switch uninitialized subject cleanup",
          [&](auto &x) {
            for (auto &e : x.nodes[fsubject.value].exits)
              if (e.kind == ExitKind::error) {
                for (const auto &t : x.ownership.temporaries)
                  if (t.producer == fsubject) {
                    e.actions.insert(
                        e.actions.begin(),
                        {CleanupKind::destroy_if_live,
                         {OwnerKind::temporary, fsubject.value}, t.type});
                    return;
                  }
              }
          });
  // A caught error that stays inside the arm keeps the subject alive.
  auto caught = accepted(
      err + "enum Data {Empty; Items(v Vec[i32]);} func f() i32 {switch "
      "Data.Items(v:[1]) {case Data.Empty:return 0;case Data.Items(v):try "
      "{let n=bad();}catch e E {let y=v[0];}return 1;}}");
  facts(caught);
  auto cs = find(caught, NodeKind::switch_);
  auto csubject = caught.nodes[cs.value].children[0];
  for (size_t i = 0; i < caught.nodes.size(); ++i)
    for (const auto &e : caught.nodes[i].exits)
      if (e.kind == ExitKind::error)
        need(std::none_of(e.actions.begin(), e.actions.end(), [&](auto a) {
          return a.kind == CleanupKind::destroy_if_live &&
                 a.owner.identity == csubject.value;
        }),
             "caught in-arm error released the held subject");
  // Nested computed owning switches drop the inner subject first.
  auto nested = accepted(
      "enum Data {Empty; Items(v Vec[i32]);} func f() i32 {switch "
      "Data.Items(v:[1]) {case Data.Empty:return 0;case Data.Items(v):switch "
      "Data.Items(v:[2]) {case Data.Empty:return 1;case "
      "Data.Items(w):return 2;}}}");
  facts(nested);
  HirNodeId outer_subject, inner_subject;
  size_t switches = 0;
  for (size_t i = 0; i < nested.nodes.size(); ++i)
    if (nested.nodes[i].kind() == NodeKind::switch_) {
      if (!switches++)
        outer_subject = nested.nodes[i].children[0];
      else
        inner_subject = nested.nodes[i].children[0];
    }
  HirNodeId deep_return{0};
  for (size_t i = 0; i < nested.nodes.size(); ++i)
    if (nested.nodes[i].kind() == NodeKind::return_)
      for (const auto &e : nested.nodes[i].exits)
        if (e.kind == ExitKind::return_ &&
            std::any_of(e.actions.begin(), e.actions.end(), [&](auto a) {
              return a.owner.identity == inner_subject.value;
            })) {
          deep_return = HirNodeId{i};
          break;
        }
  need(deep_return.value != 0, "nested arm return missing");
  const auto &np = exit(nested, deep_return, ExitKind::return_);
  auto inner_drop = std::find_if(np.actions.begin(), np.actions.end(),
                                 [&](auto a) {
                                   return a.owner.identity ==
                                          inner_subject.value;
                                 });
  auto outer_drop2 = std::find_if(np.actions.begin(), np.actions.end(),
                                  [&](auto a) {
                                    return a.owner.identity ==
                                           outer_subject.value;
                                  });
  need(inner_drop != np.actions.end() && outer_drop2 != np.actions.end() &&
           inner_drop < outer_drop2,
       "nested subject drops out of inner-to-outer order");
  // Recursive pattern: a partially failing arm keeps the same single
  // subject temporary until a later arm matches and binds.
  auto recursive = accepted(
      "enum Inner {None; Some(v Vec[i32]);} enum Outer {Empty; Wrap(i "
      "Inner); Pair(i Inner, t i32);} func make() Outer {return Outer.Wrap(i: "
      "Inner.Some(v: [7]));} func f() i32 {switch make() {case "
      "Outer.Empty:return 0;case Outer.Wrap(Inner.None):return 1;case "
      "Outer.Wrap(Inner.Some(v)):return v[0]-7;case "
      "Outer.Pair(i,t):return 2;}}");
  facts(recursive);
  auto rs = find(recursive, NodeKind::switch_);
  auto rsubject = recursive.nodes[rs.value].children[0];
  need(std::count_if(recursive.ownership.temporaries.begin(),
                     recursive.ownership.temporaries.end(),
                     [&](auto t) { return t.producer == rsubject; }) == 1,
       "recursive pattern duplicated the subject temporary");
  HirNodeId recursive_return{0};
  auto rf = recursive.functions.back().symbol;
  for (size_t i = 0; i < recursive.nodes.size(); ++i)
    if (recursive.nodes[i].kind() == NodeKind::return_ &&
        recursive.scopes.at(recursive.nodes[i].scope.value).function == rf) {
      recursive_return = HirNodeId{i};
      break;
    }
  need(recursive_return.value != 0, "recursive f return missing");
  const auto &rp2 = exit(recursive, recursive_return, ExitKind::return_);
  need(std::any_of(rp2.actions.begin(), rp2.actions.end(), [&](auto a) {
    return a.kind == CleanupKind::destroy_if_live &&
           a.owner.kind == OwnerKind::temporary &&
           a.owner.identity == rsubject.value;
  }),
       "recursive pattern arm return lacks subject drop");
  // The named owning switch keeps its old borrowed behaviour.
  auto named = accepted(
      "enum Data {Empty; Items(v Vec[i32]);} func f() i32 {let d = "
      "Data.Items(v:[1]); switch d {case Data.Empty:return 0;case "
      "Data.Items(v):let x=v;return 1;}}");
  facts(named);
  auto ns = find(named, NodeKind::switch_);
  auto nsubject = named.nodes[ns.value].children[0];
  need(std::get_if<Read>(&named.nodes[nsubject.value].data),
       "named owning switch subject changed shape");
  need(std::none_of(named.ownership.temporaries.begin(),
                    named.ownership.temporaries.end(),
                    [&](auto t) { return t.producer == nsubject; }),
       "named owning switch subject became a temporary");
  // Real bad HIR: deleted, moved, wrong-owner, duplicated and fabricated
  // subject drops must all be rejected by the independent recomputation.
  auto damaged = m;
  invalid(damaged, "computed switch subject temporary removed", [&](auto &x) {
    std::erase_if(x.ownership.temporaries,
                  [&](const auto &t) { return t.producer == subject; });
  });
  invalid(damaged, "computed switch subject drop deleted", [&](auto &x) {
    for (auto &node : x.nodes)
      for (auto &e : node.exits)
        std::erase_if(e.actions, [&](const auto &a) {
          return a.kind == CleanupKind::destroy_if_live &&
                 a.owner.kind == OwnerKind::temporary &&
                 a.owner.identity == subject.value;
        });
  });
  invalid(damaged, "computed switch subject drop duplicated", [&](auto &x) {
    for (auto &node : x.nodes)
      for (auto &e : node.exits)
        for (auto &a : e.actions)
          if (a.kind == CleanupKind::destroy_if_live &&
              a.owner.kind == OwnerKind::temporary &&
              a.owner.identity == subject.value) {
            e.actions.push_back(a);
            return;
          }
  });
  invalid(damaged, "computed switch wrong subject owner", [&](auto &x) {
    for (auto &node : x.nodes)
      for (auto &e : node.exits)
        for (auto &a : e.actions)
          if (a.kind == CleanupKind::destroy_if_live &&
              a.owner.kind == OwnerKind::temporary &&
              a.owner.identity == subject.value) {
            a.owner.kind = OwnerKind::binding;
            return;
          }
  });
  invalid(damaged, "computed switch subject drop moved before bindings",
          [&](auto &x) {
            for (auto &node : x.nodes)
              for (auto &e : node.exits) {
                auto drop = std::find_if(
                    e.actions.begin(), e.actions.end(), [&](auto &a) {
                      return a.kind == CleanupKind::destroy_if_live &&
                             a.owner.identity == subject.value;
                    });
                if (drop == e.actions.end() || drop == e.actions.begin())
                  continue;
                auto before = std::find_if(e.actions.begin(), drop,
                                           [](auto &a) {
                                             return a.kind ==
                                                    CleanupKind::destroy_if_live;
                                           });
                if (before == drop)
                  continue;
                std::iter_swap(drop, before);
                return;
              }
          });
  invalid(damaged, "computed switch early drop in full expression",
          [&](auto &x) {
            for (auto &fe : x.ownership.full_expressions)
              if (fe.node == sw) {
                for (const auto &t : x.ownership.temporaries)
                  if (t.producer == subject) {
                    fe.actions.push_back(
                        {CleanupKind::destroy_if_live,
                         {OwnerKind::temporary, subject.value}, t.type});
                    return;
                  }
              }
          });
  // Repeated loop iterations initialize a fresh subject each round.
  auto loop_switch = accepted(
      "enum Data {Empty; Items(v Vec[i32]);} func make(calls inout i32) Data "
      "{calls+=1; return Data.Items(v:[1]);} func f() i32 {var calls i32=0; "
      "var i i32=0; for i<2 {switch make(inout calls) {case "
      "Data.Empty:continue;case Data.Items(v):break;} i+=1;} return calls;}");
  facts(loop_switch);
  auto lf = loop_switch.functions.back().symbol;
  for (auto kind : {ExitKind::normal, ExitKind::break_, ExitKind::continue_}) {
    bool found = false;
    for (size_t i = 0; i < loop_switch.nodes.size() && !found; ++i)
      if (loop_switch.scopes.at(loop_switch.nodes[i].scope.value).function ==
          lf)
        for (const auto &lp : loop_switch.nodes[i].exits)
          if (lp.kind == kind &&
              std::any_of(lp.actions.begin(), lp.actions.end(), [&](auto a) {
                return a.kind == CleanupKind::destroy_if_live &&
                       a.owner.kind == OwnerKind::temporary;
              }))
            found = true;
    need(found, "loop switch exit lacks subject release");
  }
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

SymbolId function_named(const HirModule &m, std::string_view name) {
  auto consider = [&](const HirFunction &f) -> std::optional<SymbolId> {
    const auto &candidate = m.resolved->symbols[f.symbol.value].name;
    if (candidate == name)
      return f.symbol;
    return std::nullopt;
  };
  for (const auto &f : m.functions)
    if (auto symbol = consider(f))
      return *symbol;
  for (const auto &t : m.templates)
    if (auto symbol = consider(t.function))
      return *symbol;
  throw std::runtime_error("probe function missing: " + std::string(name));
}
struct IndexReadRecord {
  HirNodeId receiver;
  SymbolId function;
  std::optional<UseKind> use;
  std::optional<SymbolId> root;
  bool temporary = false;
  std::optional<SymbolId> reader;
};
std::vector<IndexReadRecord> index_receiver_records(const HirModule &m,
                                                    NodeKind kind) {
  std::vector<IndexReadRecord> out;
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const auto &n = m.nodes[i];
    if (n.kind() != kind)
      continue;
    IndexReadRecord r{n.children[0],
                      m.scopes.at(n.scope.value).function,
                      std::nullopt,
                      std::nullopt,
                      false,
                      std::nullopt};
    bool plain_read = false;
    if (auto read = std::get_if<Read>(&m.nodes[r.receiver.value].data)) {
      r.reader = read->symbol;
      plain_read = true;
    }
    for (const auto &u : m.ownership.uses)
      if (plain_read && u.consumer == r.receiver) {
        r.use = u.kind;
        r.root = u.root;
      }
    for (const auto &t : m.ownership.temporaries)
      if (t.producer == r.receiver)
        r.temporary = true;
    out.push_back(r);
  }
  return out;
}
std::vector<IndexReadRecord> index_read_records(const HirModule &m) {
  return index_receiver_records(m, NodeKind::index_read);
}
std::vector<IndexReadRecord> index_place_records(const HirModule &m) {
  return index_receiver_records(m, NodeKind::index_place);
}
IndexReadRecord only_index_read(const HirModule &m, std::string_view function) {
  auto symbol = function_named(m, function);
  std::vector<IndexReadRecord> found;
  for (auto &r : index_read_records(m))
    if (r.function == symbol)
      found.push_back(r);
  need(found.size() == 1,
       std::string(function) + " must contain exactly one index read");
  return found.front();
}
IndexReadRecord only_index_place(const HirModule &m,
                                 std::string_view function) {
  auto symbol = function_named(m, function);
  std::vector<IndexReadRecord> found;
  for (auto &r : index_place_records(m))
    if (r.function == symbol)
      found.push_back(r);
  need(found.size() == 1,
       std::string(function) + " must contain exactly one index place");
  return found.front();
}
void expect_use(const IndexReadRecord &r, UseKind kind, bool temporary,
                std::string_view what) {
  need(r.use && *r.use == kind,
       std::string(what) + ": wrong receiver use " +
           (r.use ? std::to_string(int(*r.use)) : "missing"));
  need(r.temporary == temporary,
       std::string(what) + ": wrong receiver temporary");
}
const CleanupStep *temporary_destroy(const HirModule &m, HirNodeId fe,
                                     HirNodeId producer) {
  for (const auto &f : m.ownership.full_expressions)
    if (f.node == fe)
      for (const auto &a : f.actions)
        if (a.kind == CleanupKind::destroy_if_live &&
            a.owner.kind == OwnerKind::temporary &&
            a.owner.identity == producer.value)
          return &a;
  return nullptr;
}
HirNodeId return_node(const HirModule &m, SymbolId function) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (m.nodes[i].kind() == NodeKind::return_ &&
        m.scopes.at(m.nodes[i].scope.value).function == function)
      return HirNodeId{i};
  throw std::runtime_error("probe return missing");
}

// A direct trivial-element index read of a real local binding borrows that
// binding's handle for the complete full expression; every listed negative
// keeps the old receiver copy and temporary. The receiver is evaluated once
// and before the index either way, which the shared verifier re-derives.
void local_index_borrow_tests() {
  for (auto b : {Backend::native, Backend::python}) {
    auto literal = accepted(
        "func f() i32 {let v Vec[i32]=[1,2];let x=v[0];return x;}", b);
    facts(literal);
    auto literal_read = only_index_read(literal, "f");
    expect_use(literal_read, UseKind::borrow, false, "local literal read");
    need(literal_read.reader && literal_read.root &&
             *literal_read.root == *literal_read.reader,
         "local literal read root is not its binding");

    auto binding_index = accepted(
        "func f() i32 {let v Vec[i32]=[1,2];var i i32=1;let x=v[i];return "
        "x;}",
        b);
    facts(binding_index);
    auto binding_read = only_index_read(binding_index, "f");
    expect_use(binding_read, UseKind::borrow, false, "local binding index");
    need(binding_read.reader && binding_read.root &&
             *binding_read.root == *binding_read.reader,
         "local binding index root is not its binding");

    auto ordinary = accepted("func f(v Vec[i32]) i32 {return v[0];}", b);
    facts(ordinary);
    expect_use(only_index_read(ordinary, "f"), UseKind::borrow, false,
               "ordinary parameter read");
  }
  auto rebound = accepted(
      "func f() i32 {var v Vec[i32]=[1];v=[2];let x=v[0];return x;}");
  facts(rebound);
  expect_use(only_index_read(rebound, "f"), UseKind::borrow, false,
             "local var after rebind");
  auto shadowed = accepted(
      "func f() i32 {let v Vec[i32]=[1];{let v Vec[i32]=[2];let "
      "inner=v[0];}let outer=v[0];return outer;}");
  facts(shadowed);
  auto shadow_uses = index_read_records(shadowed);
  need(shadow_uses.size() == 2, "shadow probe index reads");
  for (auto &r : shadow_uses) {
    expect_use(r, UseKind::borrow, false, "shadowed local read");
    need(r.reader && r.root && *r.root == *r.reader,
         "shadowed read root is not its own SymbolId");
  }
  need(shadow_uses[0].reader->value != shadow_uses[1].reader->value,
       "shadowed locals must keep distinct SymbolIds");

  auto move_parameter =
      accepted("func f(v move Vec[i32]) i32 {return v[0];}");
  facts(move_parameter);
  expect_use(only_index_read(move_parameter, "f"), UseKind::copy, true,
             "move parameter read");

  auto dependent = accepted(
      "func f[T](v Vec[T]) T {let copy=v;let x=copy[0];return x;}");
  facts(dependent);
  expect_use(only_index_read(dependent, "f"), UseKind::copy, true,
             "dependent element read");

  auto owning = accepted(
      "func f() i32 {let v Vec[Vec[i32]]=[[1]];let x=v[0];return x[0];}");
  facts(owning);
  bool owning_checked = false;
  for (auto &r : index_read_records(owning)) {
    const auto &type = *owning.types.find(owning.nodes[r.receiver.value].type);
    if (type.kind == TypeKind::Vec && type.type_args.size() == 1 &&
        owning.types.find(type.type_args[0])->kind == TypeKind::Vec) {
      expect_use(r, UseKind::copy, true, "owning element read");
      owning_checked = true;
    }
  }
  need(owning_checked, "owning element index read missing");
  // String is a trivial value in this ownership model (needs_drop == false),
  // so a Vec[string] element read takes the same borrow path.
  auto string_element =
      accepted("func f() i32 {let v Vec[string]=[\"x\"];let s=v[0];return 0;}");
  facts(string_element);
  expect_use(only_index_read(string_element, "f"), UseKind::borrow, false,
             "trivial string element read");

  auto computed_index = accepted(
      "func f() i32 {let v Vec[i32]=[1];var i i32=0;let x=v[i+1];return x;}");
  facts(computed_index);
  expect_use(only_index_read(computed_index, "f"), UseKind::borrow, false,
             "computed index read");
  auto arithmetic_index = accepted(
      "func f() i32 {let v Vec[i32]=[1,2,3,4];var i i32=1;let "
      "x=v[i*3-1];return x;}");
  facts(arithmetic_index);
  expect_use(only_index_read(arithmetic_index, "f"), UseKind::borrow, false,
             "arithmetic index read");
  auto unary_index = accepted(
      "func f() i32 {let v Vec[i32]=[1,2];var i i32=1;let x=v[-(-i)];return "
      "x;}");
  facts(unary_index);
  expect_use(only_index_read(unary_index, "f"), UseKind::borrow, false,
             "unary arithmetic index read");
  auto bit_index = accepted(
      "func f() i32 {let v Vec[i32]=[1,2,3];var i i32=1;let x=v[(i&1)|2];return "
      "x;}");
  facts(bit_index);
  expect_use(only_index_read(bit_index, "f"), UseKind::borrow, false,
             "bit index read");
  auto convert_index = accepted(
      "func f() i32 {let v Vec[i32]=[1,2];var i i32=1;let x=v[usize(i)];return "
      "x;}");
  facts(convert_index);
  expect_use(only_index_read(convert_index, "f"), UseKind::copy, true,
             "converted index read");
  auto inout_index = accepted(
      "func bump(x inout i32) i32 {x=x+1;return x;} func f() i32 {let v "
      "Vec[i32]=[1,2];var i i32=0;let x=v[bump(inout i)];return x;}");
  facts(inout_index);
  expect_use(only_index_read(inout_index, "f"), UseKind::copy, true,
             "inout index read");

  auto place = accepted(
      "func f() i32 {var v Vec[i32]=[1,2,3];var i i32=0;v[i+1]=7;return "
      "v[2];}");
  facts(place);
  expect_use(only_index_place(place, "f"), UseKind::borrow, false,
             "local arithmetic index place");
  auto place_compound = accepted(
      "func f() i32 {var v Vec[i32]=[1,2,3];var i i32=0;v[i+1]+=2;return "
      "v[2];}");
  facts(place_compound);
  expect_use(only_index_place(place_compound, "f"), UseKind::borrow, false,
             "compound local arithmetic index place");
  auto place_move = accepted(
      "func consume(v move Vec[i32]) i32 {return 0;} func f() i32 {var v "
      "Vec[i32]=[1,2];var i i32=0;v[i+1]=consume(move v);return 0;}");
  facts(place_move);
  expect_use(only_index_place(place_move, "f"), UseKind::copy, true,
             "index place with same full-expression move");
  auto place_call_index = accepted(
      "func pick() i32 {return 0;} func f() i32 {var v Vec[i32]=[1,2];"
      "v[pick()+1]=7;return 0;}");
  facts(place_call_index);
  expect_use(only_index_place(place_call_index, "f"), UseKind::copy, true,
             "index place with call index");
  auto place_field = accepted(
      "struct P {x i32;} func f() i32 {var v Vec[P]=[P(x:1)];var i i32=0;"
      "v[i+0].x=7;return v[0].x;}");
  facts(place_field);
  expect_use(only_index_place(place_field, "f"), UseKind::copy, true,
             "indexed field place keeps receiver copy");
  auto place_owning = accepted(
      "func f() i32 {var v Vec[Vec[i32]]=[[1]];var i i32=0;v[i+0]=[2];return "
      "v[0][0];}");
  facts(place_owning);
  expect_use(only_index_place(place_owning, "f"), UseKind::copy, true,
             "owning element index place");
  auto call_index = accepted(
      "func pick() i32 {return 0;} func f() i32 {let v Vec[i32]=[1];return "
      "v[pick()];}");
  facts(call_index);
  expect_use(only_index_read(call_index, "f"), UseKind::copy, true,
             "call index read");

  auto field_receiver = accepted(
      "struct S {items Vec[i32];} func f(s S) i32 {return s.items[0];}");
  facts(field_receiver);
  auto parameter_field = only_index_read(field_receiver, "f");
  need(!parameter_field.reader && !parameter_field.temporary,
       "parameter field keeps its existing borrowed receiver");
  auto local_field = accepted(
      "struct S {items Vec[i32];} func f() i32 {let s=S(items:[1]);return s.items[0];}");
  facts(local_field);
  auto local_field_read = only_index_read(local_field, "f");
  need(!local_field_read.reader && local_field_read.temporary,
       "local field receiver keeps its owned temporary");
  auto computed_receiver = accepted(
      "func make() Vec[i32] {return [1];} func f() i32 {return make()[0];}");
  need(verify(computed_receiver).success(), "computed receiver public verify");
  auto computed_read = only_index_read(computed_receiver, "f");
  need(!computed_read.reader && computed_read.temporary,
       "computed receiver keeps its owned temporary");

  auto moved = accepted(
      "func consume(v move Vec[i32]) i32 {return v[0];} func f() i32 {let v "
      "Vec[i32]=[1];return v[0]+consume(move v);}");
  facts(moved);
  auto moved_read = only_index_read(moved, "f");
  expect_use(moved_read, UseKind::copy, true, "same full-expression move");
  auto fe = return_node(moved, function_named(moved, "f"));
  need(temporary_destroy(moved, fe, moved_read.receiver),
       "same full-expression move keeps receiver temporary until the end");
  auto index_moved = accepted(
      "func take(v move Vec[i32]) i32 {return 0;} func f() i32 {let v "
      "Vec[i32]=[1];return v[take(move v)];}");
  facts(index_moved);
  expect_use(only_index_read(index_moved, "f"), UseKind::copy, true,
             "index consumes receiver");
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
void trait_static_calls() {
  const std::string zero =
      "trait Factory{func make()Self;}"
      "struct Box{items Vec[i32];}"
      "impl Factory for Box{func make()Self{return Box(items:[7]);}}"
      "func build[T Factory]()T{return T.make();}"
      "func main()i32{let b=build[Box]();return b.items[0]-7;}";
  const std::string ordinary =
      "trait Echo{func copy(owner Self,value Vec[i32])Self;}"
      "struct Box{items Vec[i32];}"
      "impl Echo for Box{func copy(owner Self,value Vec[i32])Self{"
      "return owner;}}"
      "func copy_value[T Echo](owner T,value Vec[i32])T{"
      "return T.copy(value:value,owner:owner);}"
      "func main()i32{let b=Box(items:[1]);let v Vec[i32]=[2];"
      "let r=copy_value[Box](b,v);return 0;}";
  const std::string move_entry =
      "trait Take{func take(value move Self)Self;}"
      "func f[T Take](x T)T{return T.take(move x);}";
  const std::string inout_entry =
      "trait Bump{func bump(slot inout Self);}"
      "func f[T Bump](x T){T.bump(inout x);}";
  // Actual Self Type/T modes are now open; moving or borrowing an ordinary
  // parameter remains the ordinary source argument error.
  rejected(move_entry, "E_CALL_ARGUMENT");
  rejected(inout_entry, "E_CALL_ARGUMENT");
  for (auto b : {Backend::native, Backend::python}) {
    auto empty = accepted(zero, b);
    facts(empty);
    auto empty_id = find(empty, NodeKind::constraint_call);
    const auto &ec =
        std::get<ConstraintCall>(empty.nodes[empty_id.value].data);
    need(ec.args.empty() && empty.nodes[empty_id.value].children.empty(),
         "zero-argument ConstraintCall fabricated args/children");
    need(empty.types.find(ec.receiver) &&
             empty.types.find(ec.receiver)->kind == TypeKind::TypeParameter,
         "zero-argument ConstraintCall lost its type receiver");
    need(!std::any_of(empty.ownership.uses.begin(),
                      empty.ownership.uses.end(),
                      [&](const auto &u) { return u.consumer == empty_id; }),
         "zero-argument ConstraintCall invented a receiver use");
    invalid(empty, "trait-static fabricated receiver", [&](auto &m) {
      auto &c = std::get<ConstraintCall>(
          m.nodes[empty_id.value].data);
      c.args.push_back({0, 0, {}, ParamMode::ordinary, empty_id});
    });

    auto borrowed = accepted(ordinary, b);
    facts(borrowed);
    auto cid = find(borrowed, NodeKind::constraint_call);
    const auto &call =
        std::get<ConstraintCall>(borrowed.nodes[cid.value].data);
    need(call.args.size() == 2 &&
             borrowed.nodes[cid.value].children.size() == 2,
         "static ConstraintCall retained a synthetic receiver");
    need(call.args[0].source_index == 0 &&
             call.args[0].parameter_index == 1 &&
             call.args[0].label == std::string("value") &&
             call.args[1].source_index == 1 &&
             call.args[1].parameter_index == 0 &&
             call.args[1].label == std::string("owner") &&
             call.args[0].mode == ParamMode::ordinary &&
             call.args[1].mode == ParamMode::ordinary,
         "static ConstraintCall source order/labels/modes changed");
    auto value_root =
        std::get<Read>(borrowed.nodes[call.args[0].expression.value].data)
            .symbol;
    need(std::any_of(borrowed.ownership.uses.begin(),
                     borrowed.ownership.uses.end(), [&](const auto &u) {
                       return u.consumer == cid && u.root == value_root &&
                              u.kind == UseKind::borrow;
                     }),
         "static ConstraintCall lost its ordinary Vec borrow");
    need(std::none_of(borrowed.ownership.temporaries.begin(),
                      borrowed.ownership.temporaries.end(),
                      [&](const auto &t) {
                        return t.producer == call.args[0].expression;
                      }),
         "static ConstraintCall silently moved/copied its borrowed Vec");
    need(std::any_of(borrowed.ownership.calls.begin(),
                     borrowed.ownership.calls.end(),
                     [&](const auto &c) {
                       return c.node == cid &&
                              std::any_of(
                                  c.on_return.begin(), c.on_return.end(),
                                  [&](const auto &a) {
                                    return a.kind == CleanupKind::end_borrow &&
                                           a.owner.kind ==
                                               OwnerKind::binding &&
                                           a.owner.identity ==
                                               value_root.value;
                                  });
                     }),
         "static ConstraintCall completion missing");
    invalid(borrowed, "trait-static argument index", [&](auto &m) {
      auto &c =
          std::get<ConstraintCall>(m.nodes[cid.value].data);
      c.args[0].parameter_index = 7;
    });
    invalid(borrowed, "trait-static argument source index", [&](auto &m) {
      auto &c =
          std::get<ConstraintCall>(m.nodes[cid.value].data);
      c.args[0].source_index = 3;
    });
    invalid(borrowed, "trait-static argument label", [&](auto &m) {
      auto &c =
          std::get<ConstraintCall>(m.nodes[cid.value].data);
      c.args[0].label = std::string("wrong");
    });
    invalid(borrowed, "trait-static argument mode", [&](auto &m) {
      auto &c =
          std::get<ConstraintCall>(m.nodes[cid.value].data);
      c.args[0].mode = ParamMode::inout;
    });
    invalid(borrowed, "trait-static fabricated receiver", [&](auto &m) {
      auto &c =
          std::get<ConstraintCall>(m.nodes[cid.value].data);
      c.args.insert(c.args.begin(),
                    {0, 0, {}, ParamMode::ordinary,
                     c.args[0].expression});
    });
    invalid(borrowed, "trait-static completion removed", [&](auto &m) {
      std::erase_if(m.ownership.calls,
                    [&](const auto &c) { return c.node == cid; });
    });
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
    folded_constant_move_tests();
    flow_tests();
    recursive_value_tests();
    record_tests();
    local_index_borrow_tests();
    extra_tests();
    computed_owning_switch_tests();
    for (auto b : {Backend::native, Backend::python}) {
      // This signature-only regression intentionally has no owned locals.
      auto signature = project(
          fs::path(argv[1]) /
              "tests/ownership/fixtures/throws-before-parameter/lang.toml",
          b);
      need(signature.success(),
           "throws-before-parameter rejected: " + diagnostics(signature));
      need(verify_types(*signature.value).success() &&
               verify(*signature.value).success(),
           "throws-before-parameter independent verification failed");
      ++probes;
      auto r = project(fs::path(argv[1]) /
                           "tests/ownership/static-extra/independent/lang.toml",
                       b);
      need(r.success(), "independent case: " + diagnostics(r));
      facts(*r.value);
      ++probes;
    }
    trait_static_calls();
    corrections("all");
    std::cout << "T09 probes " << probes << " passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

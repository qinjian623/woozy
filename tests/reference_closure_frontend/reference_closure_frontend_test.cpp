// LANG01 closure/frontend execution test.
//
// Real lexer -> parser -> resolver -> Sema/HIR -> independent HIR verifier ->
// NativeIR lowering -> independent IR verifier for the accepted closure,
// move-func, bound-method and function-value cases. The runtime executions
// themselves are performed by the fixture runner (Native O0/O2 and Python
// 3.12.15); this binary never claims execution from verifier success alone.
//
// Forged HIR/IR metadata is rejected independently of the producer, including
// C1-C7 capture order/source/move intent, local-vs-origin substitution,
// synthetic Move, bound receiver and capture-slot identity.
#include "lang/hir/hir.hpp"
#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lang;
using namespace lang::hir;
namespace fs = std::filesystem;
namespace {
void need(bool value, const std::string &message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string diagnostics(const auto &result) {
  std::string text;
  for (const auto &d : result.diagnostics)
    text += d.code + ": " + d.message + "\n";
  return text;
}
bool has(const auto &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &d) { return d.code == code; });
}
struct Temp {
  fs::path path;
  SourceManager sources;
  Temp() {
    char pattern[] = "/tmp/woozy-closure-frontend-XXXXXX";
    auto *created = mkdtemp(pattern);
    need(created, "mkdtemp failed");
    path = created;
    write("lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
  }
  ~Temp() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
  void write(const fs::path &name, const std::string &text) {
    fs::create_directories((path / name).parent_path());
    std::ofstream out(path / name);
    out << text;
    need(bool(out), "fixture write failed");
  }
  auto resolve(const std::string &main, const fs::path &std_root) {
    write("src/main.wz", main);
    auto loaded =
        modules::load_project({path / "lang.toml", std_root, {}}, sources);
    need(loaded.success(), "load failed: " + diagnostics(loaded));
    return modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*loaded.value)));
  }
};

std::shared_ptr<modules::ResolvedProject> load_fixture(const fs::path &root,
                                                       std::string_view name,
                                                       SourceManager &sources) {
  auto loaded = modules::load_project(
      {root / "tests/reference_values/fixtures" / name / "lang.toml",
       root / "stdlib", {}},
      sources);
  need(loaded.success(), std::string(name) + ": load failed\n" +
                             diagnostics(loaded));
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*loaded.value)));
  need(resolved.success(), std::string(name) + ": resolve failed\n" +
                               diagnostics(resolved));
  return std::make_shared<modules::ResolvedProject>(std::move(*resolved.value));
}

const HirFunction *function_named(const HirModule &m, std::string_view name,
                                 const std::vector<TypeId> &args = {}) {
  for (const auto &f : m.functions) {
    if (m.resolved->symbols.at(f.symbol.value).name != name)
      continue;
    bool instance_match = args.empty();
    for (const auto &i : m.instances)
      if (i.symbol == f.symbol && i.type_args == args)
        instance_match = true;
    if (instance_match)
      return &f;
  }
  return nullptr;
}
std::vector<const HirNode *> function_values(const HirModule &m,
                                             const HirFunction &f) {
  std::vector<const HirNode *> result;
  for (const auto &n : m.nodes)
    if (m.scopes.at(n.scope.value).function == f.symbol &&
        n.kind() == NodeKind::function_value)
      result.push_back(&n);
  return result;
}
const HirNode *first_function_value(const HirModule &m, const HirFunction &f) {
  auto values = function_values(m, f);
  need(!values.empty(), "function value missing");
  return values.front();
}
const HirNode *read_node(const HirModule &m, const HirFunction &f,
                         SymbolId symbol) {
  for (const auto &n : m.nodes)
    if (m.scopes.at(n.scope.value).function == f.symbol &&
        n.kind() == NodeKind::read &&
        std::get<Read>(n.data).symbol == symbol)
      return &n;
  return nullptr;
}
const ir::Instruction *ir_function_instruction(const ir::Module &m,
                                               HirNodeId origin) {
  for (const auto &f : m.functions)
    for (const auto &b : f.blocks)
      for (const auto &i : b.instructions)
        if (i.op == ir::Op::function && i.origin == origin)
          return &i;
  return nullptr;
}
void reject_hir(const HirModule &good, const std::string &name,
                const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = verify_types(changed);
  need(!checked.success() && has(checked, "E_IR_INVALID"),
       name + " forged HIR accepted\n" + diagnostics(checked));
}
void reject_ir(const ir::Module &good, const std::string &name,
               const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = ir::verify(changed);
  need(!checked.success() && has(checked, "E_IR_INVALID"),
       name + " forged NativeIR accepted\n" + diagnostics(checked));
}
const Type &type_of(const HirModule &m, TypeId id) {
  const auto *t = m.types.find(id);
  need(t, "type missing");
  return *t;
}

void verify_source_fixture(const fs::path &root, std::string_view name,
                           SourceManager &sources,
                           std::optional<std::string> expected_code = {}) {
  auto resolved = load_fixture(root, name, sources);
  if (expected_code) {
    auto checked = sema::check_project(resolved);
    need(!checked.success(), std::string(name) + ": negative fixture passed");
    need(has(checked, *expected_code),
         std::string(name) + ": wrong rejection category\n" +
             diagnostics(checked));
    return;
  }
  auto checked = sema::check_project(resolved);
  need(checked.success(), std::string(name) + ": HIR failed\n" +
                              diagnostics(checked));
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), std::string(name) + ": NativeIR lowering failed\n" +
                              diagnostics(lowered));
  auto verified = ir::verify(lowered.value->module());
  need(verified.success(), std::string(name) + ": NativeIR verify failed\n" +
                               diagnostics(verified));
}

void structural_checks(const fs::path &root, SourceManager &sources) {
  // Scalar captures are copies at creation in capture order; a bound method
  // has one receiver child and no capture list.
  {
    auto resolved = load_fixture(root, "closure-two-capture-types", sources);
    auto checked = sema::check_project(resolved);
    need(checked.success(), "two-capture HIR: " + diagnostics(checked));
    const auto &m = checked.value->module();
    const auto *main = function_named(m, "main");
    need(main && main->captures.empty(), "main gained captures");
    const auto *value = first_function_value(m, *main);
    const auto &fv = std::get<FunctionValue>(value->data);
    need(fv.captures.size() == 2 && !fv.move_capture && !fv.bound,
         "two scalar captures missing");
    need(value->children.size() == 2, "capture child count");
    for (size_t i = 0; i < fv.captures.size(); ++i) {
      const auto &child = m.nodes.at(value->children[i].value);
      need(child.kind() == NodeKind::read &&
               std::get<Read>(child.data).symbol == fv.captures[i].source,
           "copied capture child is not a source read");
      const auto *read = read_node(m, *main, fv.captures[i].source);
      need(read != nullptr, "capture source binding missing");
    }
  }
  // Nested/transitive: the inner lambda source is the outer lambda's capture
  // local, while both capture bindings keep the same original origin.
  {
    auto resolved = load_fixture(root, "closure-nested-transitive", sources);
    auto checked = sema::check_project(resolved);
    need(checked.success(), "nested capture HIR: " + diagnostics(checked));
    const auto &m = checked.value->module();
    const HirFunction *outer = nullptr, *inner = nullptr;
    for (const auto &f : m.functions) {
      if (f.captures.size() == 1)
        outer = &f;
      if (f.captures.size() == 2)
        inner = &f;
    }
    need(outer && inner, "nested lambda capture entries missing");
    bool transitive = false;
    for (const auto &b : inner->captures)
      for (const auto &o : outer->captures)
        if (b.origin == o.origin && b.local != o.local) {
          for (const auto *node : function_values(m, *outer))
            for (const auto &source :
                 std::get<FunctionValue>(node->data).captures)
              if (source.origin == b.origin && source.source == o.local)
                transitive = true;
        }
    need(transitive, "grandparent origin is not shared transitively");
  }
  // Generic instances keep the origin stable and allocate distinct locals.
  {
    auto resolved = load_fixture(root, "closure-generic-two-instances", sources);
    auto checked = sema::check_project(resolved);
    need(checked.success(), "generic capture HIR: " + diagnostics(checked));
    const auto &m = checked.value->module();
    std::vector<const HirFunction *> instances;
    for (const auto &f : m.functions) {
      if (f.captures.empty())
        continue;
      for (const auto &i : m.instances)
        if (i.symbol == f.symbol && i.origin != i.symbol &&
            m.resolved->symbols.at(i.origin.value).name.empty())
          instances.push_back(&f);
    }
    need(instances.size() == 2, "generic capture instances missing");
    need(instances[0]->captures[0].origin == instances[1]->captures[0].origin,
         "generic instance capture origin changed");
    need(instances[0]->captures[0].local != instances[1]->captures[0].local,
         "generic instance capture local was reused");
    need(instances[0]->captures[0].type != instances[1]->captures[0].type,
         "generic instance capture types did not specialize");
    for (const auto *f : instances)
      if (classify_value(m, f->captures[0].type).needs_drop) {
        bool readonly = false;
        for (const auto &local : m.ownership.locals)
          if (local.symbol == f->captures[0].local)
            readonly = local.read_only_capture;
        need(readonly, "owning capture local is not a read-only local");
      }
  }
  // Move func: owning captures are synthetic Moves, scalars are copies.
  {
    auto resolved = load_fixture(root, "closure-move-owning-and-scalar",
                                 sources);
    auto checked = sema::check_project(resolved);
    need(checked.success(), "move closure HIR: " + diagnostics(checked));
    const auto &m = checked.value->module();
    const auto *main = function_named(m, "main");
    const auto *value = first_function_value(m, *main);
    const auto &fv = std::get<FunctionValue>(value->data);
    need(fv.move_capture && fv.captures.size() == 2,
         "move func capture schema");
    bool saw_move = false, saw_read = false;
    for (size_t i = 0; i < fv.captures.size(); ++i) {
      const auto &child = m.nodes.at(value->children[i].value);
      const auto &capture = fv.captures[i];
      if (capture.move) {
        const auto *move = std::get_if<Move>(&child.data);
        need(move && move->binding &&
                 *move->binding == capture.source &&
                 classify_value(m, capture.type).needs_drop,
             "owning move capture is not a synthetic source move");
        saw_move = true;
      } else {
        need(child.kind() == NodeKind::read, "scalar capture was moved");
        saw_read = true;
      }
    }
    need(saw_move && saw_read, "move func did not mix move and copy captures");
  }
  // Bound methods: ordinary and move Self preserve the real receiver child and
  // drop exactly the first Self parameter from the callable type.
  for (const auto &[name, mode] :
       std::vector<std::pair<std::string, ParamMode>>{
           {"bound-ordinary-repeat-copy", ParamMode::ordinary},
           {"bound-move-repeat-copy", ParamMode::move}}) {
    auto resolved = load_fixture(root, name, sources);
    auto checked = sema::check_project(resolved);
    need(checked.success(), name + " HIR: " + diagnostics(checked));
    const auto &m = checked.value->module();
    const auto *main = function_named(m, "main");
    const auto *value = first_function_value(m, *main);
    const auto &fv = std::get<FunctionValue>(value->data);
    need(fv.bound && fv.captures.empty() && value->children.size() == 1,
         name + ": bound schema missing");
    need(fv.bound->mode == mode, name + ": bound Self mode changed");
    need(m.nodes.at(value->children[0].value).type == fv.bound->type,
         name + ": bound receiver child type");
    const auto callable = type_of(m, value->type);
    need(callable.kind == TypeKind::Function, name + ": bound is not callable");
  }
}

void generic_move_instance(const fs::path &stdlib, SourceManager &sources) {
  Temp fixture;
  auto resolved = fixture.resolve(R"WZ(package app;
func make[T](value T) (func() T) {
    let owned T = value;
    return move func() T { return owned; };
}
func main() i32 {
    var scalar i32 = 5;
    let read_scalar = make[i32](scalar);
    if read_scalar() != 5 { return 1; }
    var values Vec[i32] = [7];
    let read_vec = make[Vec[i32]](values);
    if read_vec()[0] != 7 { return 2; }
    return 0;
}
)WZ",
                                stdlib);
  auto checked = sema::check_project(resolved);
  need(checked.success(),
       "generic move closure HIR: " + diagnostics(checked));
  const auto &m = checked.value->module();
  std::set<uint64_t> move_instances, read_instances;
  for (const auto &i : m.instances) {
    if (m.resolved->symbols.at(i.origin.value).name != "make")
      continue;
    const auto *f = function_named(m, "make", i.type_args);
    need(f != nullptr, "generic move instance function missing");
    auto values = function_values(m, *f);
    need(!values.empty(), "generic move function value missing");
    const auto &fv = std::get<FunctionValue>(values.front()->data);
    need(fv.move_capture && fv.captures.size() == 1,
         "generic move capture schema");
    const auto &child = m.nodes.at(values.front()->children[0].value);
    const HirFunction *lambda = nullptr;
    for (const auto &candidate : m.functions)
      if (candidate.symbol == fv.symbol)
        lambda = &candidate;
    need(lambda && lambda->captures.size() == 1,
         "generic move lambda capture entry missing");
    if (i.type_args.size() == 1 &&
        type_of(m, i.type_args[0]).kind == TypeKind::SignedInt) {
      need(!fv.captures[0].move && child.kind() == NodeKind::read,
           "scalar generic move instance did not copy");
      read_instances.insert(lambda->captures[0].local.value);
    } else {
      need(fv.captures[0].move && child.kind() == NodeKind::move,
           "owning generic move instance did not move");
      move_instances.insert(lambda->captures[0].local.value);
    }
  }
  need(move_instances.size() == 1 && read_instances.size() == 1,
       "generic move instance conversion missing");
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), "generic move NativeIR lowering: " +
                              diagnostics(lowered));
  need(ir::verify(lowered.value->module()).success(),
       "generic move NativeIR rejected");
}

void forged_hir(const fs::path &root, SourceManager &sources) {
  auto resolved = load_fixture(root, "closure-two-capture-types", sources);
  auto checked = sema::check_project(resolved);
  need(checked.success(), "forged-HIR baseline: " + diagnostics(checked));
  const HirModule &good = checked.value->module();
  const auto *main = function_named(good, "main");
  const auto *value = first_function_value(good, *main);
  const auto node_id = [&](const HirNode *n) {
    return HirNodeId{size_t(n - good.nodes.data())};
  };

  reject_hir(good, "capture order swapped", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[node_id(value).value].data);
    std::swap(fv.captures[0], fv.captures[1]);
  });
  reject_hir(good, "capture origin forged", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[node_id(value).value].data);
    fv.captures[0].origin = fv.captures[1].origin;
  });
  reject_hir(good, "capture type forged", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[node_id(value).value].data);
    fv.captures[0].type = fv.captures[1].type;
  });
  reject_hir(good, "plain literal claims move", [&](HirModule &m) {
    std::get<FunctionValue>(m.nodes[node_id(value).value].data).move_capture =
        true;
  });
  reject_hir(good, "ordinary capture child became Move", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[node_id(value).value].data);
    auto child = m.nodes[node_id(value).value].children[0];
    m.nodes[child.value].data = Move{fv.captures[0].source};
  });
  reject_hir(good, "lambda capture local swapped", [&](HirModule &m) {
    for (auto &f : m.functions)
      if (f.captures.size() == 2)
        std::swap(f.captures[0].local, f.captures[1].local);
  });
  reject_hir(good, "lambda body read kept origin", [&](HirModule &m) {
    const HirFunction *lambda = nullptr;
    for (const auto &f : m.functions)
      if (f.captures.size() == 2)
        lambda = &f;
    need(lambda != nullptr, "lambda for read corruption missing");
    const auto origin = lambda->captures[0].origin;
    for (auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == lambda->symbol) {
        auto *read = std::get_if<Read>(&n.data);
        if (read && read->symbol == lambda->captures[0].local) {
          read->symbol = origin;
          return;
        }
      }
    throw std::runtime_error("lambda read node missing");
  });
  reject_hir(good, "capture local forged as source binding", [&](HirModule &m) {
    for (const auto &f : m.functions)
      if (f.captures.size() == 2) {
        auto project =
            std::make_shared<modules::ResolvedProject>(*m.resolved);
        auto &local = project->symbols[f.captures[0].local.value];
        const auto &origin = project->symbols[f.captures[0].origin.value];
        local.definition = origin.definition;
        local.declaration = origin.declaration;
        m.resolved = std::move(project);
        return;
      }
  });

  auto move_resolved =
      load_fixture(root, "closure-move-owning-and-scalar", sources);
  auto move_checked = sema::check_project(move_resolved);
  need(move_checked.success(),
       "forged move-capture baseline: " + diagnostics(move_checked));
  const HirModule &move_good = move_checked.value->module();
  const auto *move_main = function_named(move_good, "main");
  const auto *move_value = first_function_value(move_good, *move_main);
  const auto move_id = HirNodeId{size_t(move_value - move_good.nodes.data())};
  reject_hir(move_good, "synthetic Move source replaced", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[move_id.value].data);
    for (size_t i = 0; i < fv.captures.size(); ++i)
      if (fv.captures[i].move) {
        auto child = m.nodes[move_id.value].children[i];
        std::get<Move>(m.nodes[child.value].data).binding =
            fv.captures[i == 0 ? 1 : 0].source;
        return;
      }
    throw std::runtime_error("move capture child missing");
  });
  reject_hir(move_good, "owning capture child became copy", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[move_id.value].data);
    for (size_t i = 0; i < fv.captures.size(); ++i)
      if (fv.captures[i].move) {
        auto child = m.nodes[move_id.value].children[i];
        m.nodes[child.value].data = Read{fv.captures[i].source};
        return;
      }
    throw std::runtime_error("move capture child missing");
  });

  auto bound_resolved =
      load_fixture(root, "bound-ordinary-repeat-copy", sources);
  auto bound_checked = sema::check_project(bound_resolved);
  need(bound_checked.success(),
       "forged bound baseline: " + diagnostics(bound_checked));
  const HirModule &bound = bound_checked.value->module();
  const auto *bound_main = function_named(bound, "main");
  const auto *bound_value = first_function_value(bound, *bound_main);
  const auto bound_id = HirNodeId{size_t(bound_value - bound.nodes.data())};
  reject_hir(bound, "bound Self mode forged", [&](HirModule &m) {
    std::get<FunctionValue>(m.nodes[bound_id.value].data).bound->mode =
        ParamMode::inout;
  });
  reject_hir(bound, "bound receiver child replaced", [&](HirModule &m) {
    auto &fv = std::get<FunctionValue>(m.nodes[bound_id.value].data);
    fv.bound->type = m.nodes[bound_value->children[0].value].type;
    m.nodes[bound_value->children[0].value].type = fv.bound->type;
    m.nodes[bound_value->children[0].value].data = UnitLiteral{};
  });
}

void forged_ir(const fs::path &root, SourceManager &sources) {
  auto resolved = load_fixture(root, "closure-move-owning-and-scalar", sources);
  auto checked = sema::check_project(resolved);
  need(checked.success(), "forged-IR baseline: " + diagnostics(checked));
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), "forged-IR lowering: " + diagnostics(lowered));
  const ir::Module &good = lowered.value->module();
  const auto &hir = good.hir.module();
  const auto *main = function_named(hir, "main");
  const auto *value = first_function_value(hir, *main);
  const auto origin = HirNodeId{size_t(value - hir.nodes.data())};
  const auto *instruction = ir_function_instruction(good, origin);
  need(instruction != nullptr, "move capture function instruction missing");

  reject_ir(good, "function origin dropped", [&](ir::Module &m) {
    for (auto &f : m.functions)
      for (auto &b : f.blocks)
        for (auto &i : b.instructions)
          if (i.op == ir::Op::function && i.origin &&
              i.origin->value == origin.value)
            i.origin.reset();
  });
  reject_ir(good, "function origin replaced", [&](ir::Module &m) {
    for (auto &f : m.functions)
      for (auto &b : f.blocks)
        for (auto &i : b.instructions)
          if (i.op == ir::Op::function && i.origin &&
              i.origin->value == origin.value)
            i.origin = HirNodeId{main->body.value};
  });
  reject_ir(good, "function capture input reordered", [&](ir::Module &m) {
    for (auto &f : m.functions)
      for (auto &b : f.blocks)
        for (auto &i : b.instructions)
          if (i.op == ir::Op::function && i.origin &&
              i.origin->value == origin.value && i.inputs.size() == 2)
            std::swap(i.inputs[0], i.inputs[1]);
  });
  reject_ir(good, "function capture input dropped", [&](ir::Module &m) {
    for (auto &f : m.functions)
      for (auto &b : f.blocks)
        for (auto &i : b.instructions)
          if (i.op == ir::Op::function && i.origin &&
              i.origin->value == origin.value)
            i.inputs.pop_back();
  });
  reject_ir(good, "capture slot binding forged", [&](ir::Module &m) {
    for (auto &f : m.functions) {
      const HirFunction *hf = nullptr;
      for (const auto &candidate : hir.functions)
        if (candidate.symbol == f.symbol)
          hf = &candidate;
      if (!hf || hf->captures.empty())
        continue;
      for (auto &slot : f.slots)
        if (slot.binding &&
            slot.binding->value == hf->captures[0].local.value) {
          slot.binding = hf->captures[0].origin;
          return;
        }
    }
  });
}

void source_rejections(const fs::path &stdlib, SourceManager &sources) {
  auto deny = [&](const std::string &body, std::string_view code,
                  std::string_view message, const std::string &name) {
    Temp fixture;
    auto resolved = fixture.resolve(body, stdlib);
    need(resolved.success(), name + ": resolve failed\n" +
                                 diagnostics(resolved));
    auto checked = sema::check_project(resolved);
    need(!checked.success() && has(checked, code),
         name + ": wrong acceptance/category\n" + diagnostics(checked));
    need(diagnostics(checked).find(message) != std::string::npos,
         name + ": missing expected reason\n" + diagnostics(checked));
  };
  deny(R"WZ(package app;
func main() i32 {
  let x i32 = 1;
  const F = func() i32 { return x; };
  return F();
}
)WZ",
       "E_TYPE_MISMATCH", "constant function value cannot capture",
       "local const capturing lambda");
  deny(R"WZ(package app;
struct Counter { value i32; }
impl Counter {
  func bump(self inout Self, delta i32) i32 {
    let old = self.value; self.value = old + delta; return old;
  }
}
func main() i32 {
  var counter = Counter(value: 1);
  let bump = counter.bump;
  return bump(1);
}
)WZ",
       "E_CALL_ARGUMENT", "inout-Self method", "bound inout Self");
  deny(R"WZ(package app;
func make(slot inout i32) (func() i32) {
  return func() i32 { return slot; };
}
func main() i32 { var x i32 = 1; let f = make(inout x); return f(); }
)WZ",
       "E_CALL_ARGUMENT", "inout parameter cannot be captured",
       "inout capture");
  deny(R"WZ(package app;
func main() i32 {
  var values Vec[i32] = [41];
  let read = move func() i32 { return values[0]; };
  let illegal = values[0];
  return read() + illegal;
}
)WZ",
       "E_OWNERSHIP_USE_AFTER_MOVE", "moved", "owning move read after");
  deny(R"WZ(package app;
func main() i32 {
  var x i32 = 1;
  let bad = func() i32 { x = 2; return x; };
  return bad();
}
)WZ",
       "E_TYPE_MISMATCH", "read-only", "read-only capture assignment");
}

void runtime_fixtures(const fs::path &root, SourceManager &sources) {
  for (const auto *name :
       {"closure-copy-at-creation-sibling", "closure-creator-scope-escape",
        "closure-generic-two-instances", "closure-move-owning-and-scalar",
        "closure-nested-transitive", "closure-readonly-shared-mutation",
        "closure-two-capture-types", "fn-return-field-vec",
        "fn-throws-defer-capture", "bound-ordinary-repeat-copy",
        "bound-move-repeat-copy", "indirect-callee-arg-throw",
        "thread-capture-scalar-arg"})
    verify_source_fixture(root, name, sources);
  for (const auto &[name, code] :
       std::vector<std::pair<std::string, std::string>>{
           {"closure-move-owning-read-after", "E_OWNERSHIP_USE_AFTER_MOVE"},
           {"closure-readonly-assign", "E_TYPE_MISMATCH"},
           {"bound-inout-reject", "E_CALL_ARGUMENT"}})
    verify_source_fixture(root, name, sources, code);
}

void suite(const fs::path &root) {
  SourceManager sources;
  runtime_fixtures(root, sources);
  structural_checks(root, sources);
  generic_move_instance(root / "stdlib", sources);
  forged_hir(root, sources);
  forged_ir(root, sources);
  source_rejections(root / "stdlib", sources);
  std::cout << "reference closure frontend passed: source HIR/NativeIR "
               "accepted for closure/move-func/bound/non-Weak cases; forged "
               "capture, move, local-origin, bound and IR metadata rejected\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "usage: reference_closure_frontend_test REPO_ROOT");
    suite(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

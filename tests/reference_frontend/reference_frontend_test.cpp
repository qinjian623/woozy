// First source Weak frontend stage: real lexer/parser/resolver/checker input,
// independent HIR verification, and NativeIR metadata rejection.
//
// Runtime emission and function-ownership execution belong to the parallel
// Native/Python owners. This test never claims MEM01 execution.
#include "lang/hir/hir.hpp"
#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
struct Fixture {
  fs::path path;
  SourceManager sources;
  Fixture() {
    char pattern[] = "/tmp/woozy-reference-frontend-XXXXXX";
    auto *created = mkdtemp(pattern);
    need(created, "mkdtemp failed");
    path = created;
    write("lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
  }
  ~Fixture() {
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

const char *weak_program = R"WZ(package app;
import "std/weak" as weak;
import "std/option" as option;
import "std/box" as box;
import "std/thread" as thread;
import "std/sync" as sync;
import "std/ipc" as ipc;

struct Payload {
    value i32;
}

struct SameShape {
    state Vec[i32];
}

func wvec(v Vec[i32]) Weak[Vec[i32]] { return weak.downgrade(v); }
func wgen[T](v Vec[T]) Weak[Vec[T]] { return weak.downgrade[Vec[T]](v); }
func wfn(f func() i32) Weak[func() i32] { return weak.downgrade(f); }
func wbox(v box.Box[i32]) Weak[box.Box[i32]] { return weak.downgrade(v); }
func wthread(v thread.Thread) Weak[thread.Thread] { return weak.downgrade(v); }
func wmutex(v sync.Mutex) Weak[sync.Mutex] { return weak.downgrade(v); }
func wcond(v sync.Condvar) Weak[sync.Condvar] { return weak.downgrade(v); }
func watomic(v sync.AtomicI64) Weak[sync.AtomicI64] { return weak.downgrade(v); }
func wipc(v ipc.Memory) Weak[ipc.Memory] { return weak.downgrade(v); }
func wpayload(v Payload) Payload { return v; }
func wsame(v SameShape) SameShape { return v; }

func main() i32 {
    let values Vec[i32] = [1];
    let generic_target Weak[Vec[i32]] = wgen[i32](values);
    let empty Weak[Vec[i32]] = weak.empty[Vec[i32]]();
    let observed option.Option[Vec[i32]] = weak.upgrade(empty);
    switch observed {
    case option.Option.Some(values):
        return 1;
    case option.Option.None:
        return 0;
    }
}
)WZ";

const char *python_program = R"WZ(package app;
import "std/weak" as weak;
import "std/option" as option;
import "std/box" as box;

func main() i32 {
    let values Vec[i32] = [1];
    let w Weak[Vec[i32]] = weak.downgrade(values);
    let cell box.Box[i32] = box.new[i32](3);
    let wb Weak[box.Box[i32]] = weak.downgrade(cell);
    let o option.Option[Vec[i32]] = weak.upgrade(w);
    switch o {
    case option.Option.Some(v):
        return v[0] - 1;
    case option.Option.None:
        return 2;
    }
}
)WZ";

std::shared_ptr<modules::ResolvedProject>
retarget(std::shared_ptr<modules::ResolvedProject> resolved, Backend backend,
         unsigned word_bits) {
  auto project = std::make_shared<modules::Project>(*resolved->project);
  project->backend = backend == Backend::python ? "python" : "native";
  project->word_bits = word_bits;
  resolved->project = project;
  return resolved;
}

void deny_code(const fs::path &root, const std::string &body,
               std::string_view code, std::string_view expected_message,
               const std::string &name) {
  Fixture fixture;
  auto resolved = fixture.resolve(body, root / "stdlib");
  need(resolved.success(), name + ": resolve failed\n" + diagnostics(resolved));
  auto checked = sema::check_types(resolved);
  need(!checked.success(), name + ": checker accepted an invalid Weak use");
  need(has(checked, code),
       name + ": wrong diagnostic category\n" + diagnostics(checked));
  need(diagnostics(checked).find(expected_message) != std::string::npos,
       name + ": missing expected reason\n" + diagnostics(checked));
}
void deny(const fs::path &root, const std::string &body,
          std::string_view expected_message, const std::string &name) {
  deny_code(root, body, "E_TYPE_MISMATCH", expected_message, name);
}
void accept_source(const fs::path &root, const std::string &body,
                   const std::string &name) {
  Fixture fixture;
  auto resolved = fixture.resolve(body, root / "stdlib");
  need(resolved.success(), name + ": resolve failed\n" +
                               diagnostics(resolved));
  auto checked = sema::check_project(resolved);
  need(checked.success(), name + ": valid Weak declaration rejected\n" +
                              diagnostics(checked));
}
// A concrete invalid Weak buried in a nominal field/payload must be a Sema
// source error with the field declaration as its primary location, not a
// later E_IR_INVALID from publishing/lowering.
void deny_buried_weak(const fs::path &root, const std::string &body,
                      const std::string &name) {
  Fixture fixture;
  auto resolved = fixture.resolve(body, root / "stdlib");
  need(resolved.success(), name + ": resolve failed\n" +
                               diagnostics(resolved));
  auto checked = sema::check_types(resolved);
  need(!checked.success(), name + ": buried invalid Weak was accepted");
  need(has(checked, "E_TYPE_MISMATCH"),
       name + ": wrong diagnostic category\n" + diagnostics(checked));
  need(diagnostics(checked).find("Weak target must be") != std::string::npos,
       name + ": missing Weak target reason\n" + diagnostics(checked));
  const auto &primary = checked.diagnostics.front().primary;
  need(primary.has_value(), name + ": diagnostic has no source location");
  need(primary->begin_byte < primary->end_byte &&
           primary->end_byte <= body.size(),
       name + ": diagnostic span is outside the fixture");
  need(body.substr(primary->begin_byte,
                   primary->end_byte - primary->begin_byte)
               .find("Weak") != std::string::npos,
       name + ": diagnostic does not point at the Weak declaration");
}

TypeId nominal_type_named(const HirModule &m, std::string_view name) {
  for (uint64_t i = 1; i <= m.types.size(); ++i) {
    const auto &t = *m.types.find(TypeId{i});
    if (t.kind != TypeKind::NominalStruct || !t.declaration.value)
      continue;
    if (m.resolved->symbols.at(t.declaration.value).name == name)
      return TypeId{i};
  }
  throw std::runtime_error("fixture nominal type missing");
}

void reject_hir(const HirModule &good, const std::string &name,
                const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = verify_types(changed);
  need(!checked.success() && has(checked, "E_IR_INVALID"),
       name + " forged HIR accepted\n" + diagnostics(checked));
}

bool dependent_type(const HirModule &m, TypeId id) {
  std::vector<TypeId> pending{id};
  std::set<uint64_t> seen;
  while (!pending.empty()) {
    auto current = pending.back();
    pending.pop_back();
    if (!seen.insert(current.value).second)
      continue;
    const auto &t = *m.types.find(current);
    if (t.kind == TypeKind::TypeParameter)
      return true;
    pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
    for (auto p : t.parameters)
      pending.push_back(p.type);
    if (t.kind == TypeKind::Function) {
      pending.push_back(t.result);
      if (t.throws_type)
        pending.push_back(*t.throws_type);
    }
  }
  return false;
}

const ir::Instruction *system_instruction(const ir::Module &module,
                                          SystemOperation operation) {
  for (const auto &function : module.functions)
    for (const auto &block : function.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.op == ir::Op::system_call &&
            instruction.system == operation)
          return &instruction;
  return nullptr;
}

void reject_ir(const ir::Module &good, const std::string &name,
               const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = ir::verify(changed);
  need(!checked.success() && !checked.diagnostics.empty() &&
           checked.diagnostics.front().code == "E_IR_INVALID",
       name + " forged NativeIR accepted\n" + diagnostics(checked));
}

void suite(const fs::path &root) {
  Fixture fixture;
  auto resolved = fixture.resolve(weak_program, root / "stdlib");
  need(resolved.success(), "Weak fixture resolve failed\n" +
                               diagnostics(resolved));
  auto checked = sema::check_types(resolved);
  need(checked.success(), "Weak fixture HIR failed\n" + diagnostics(checked));
  const HirModule &good = *checked.value;

  // Every canonical target kind appears as a real source type application.
  size_t weak_types = 0, weak_calls = 0;
  for (uint64_t i = 1; i <= good.types.size(); ++i)
    if (good.types.find(TypeId{i})->kind == TypeKind::Weak)
      ++weak_types;
  for (const auto &node : good.nodes)
    if (const auto *call = std::get_if<Call>(&node.data);
        call && call->kind == CalleeKind::system && call->system)
      if (*call->system == SystemOperation::weak_empty ||
          *call->system == SystemOperation::weak_downgrade ||
          *call->system == SystemOperation::weak_upgrade) {
        ++weak_calls;
        need(call->type_args.size() == 1,
             "Weak operation lost its target type argument");
        if (!dependent_type(good, call->type_args[0]) &&
            !weak_target(good, call->type_args[0])) {
          const auto &t = *good.types.find(call->type_args[0]);
          std::string name = "?";
          if (t.declaration.value)
            name = good.resolved->symbols.at(t.declaration.value).name;
          throw std::runtime_error(
              "Weak operation accepted a non-canonical target: kind=" +
              std::to_string(int(t.kind)) + " name=" + name);
        }
      }
  need(weak_types >= 8, "canonical Weak source types are missing");
  const TypeId a_weak = [&] {
    for (uint64_t i = 1; i <= good.types.size(); ++i)
      if (good.types.find(TypeId{i})->kind == TypeKind::Weak)
        return TypeId{i};
    throw std::runtime_error("Weak TypeId missing");
  }();
  const auto weak_ops = classify_value(good, a_weak);
  need(weak_ops.kind == ValueKind::weak && weak_ops.needs_drop &&
           !weak_ops.pod_inout,
       "Weak must stay an owning, non-POD-inout value");
  need(weak_calls >= 10, "Weak builtin calls are missing");

  // upgrade must produce the canonical std/option.Option[T] declaration.
  const auto *upgrade_call = [&]() -> const Call * {
    for (const auto &node : good.nodes)
      if (const auto *call = std::get_if<Call>(&node.data);
          call && call->system == SystemOperation::weak_upgrade)
        return call;
    return nullptr;
  }();
  need(upgrade_call, "Weak upgrade call missing");
  const auto &upgrade_result = *good.types.find(upgrade_call->signature);
  const auto &option = *good.types.find(upgrade_result.result);
  need(option.kind == TypeKind::NominalEnum && option.type_args.size() == 1,
       "upgrade result is not Option[T]");
  const auto &option_symbol =
      good.resolved->symbols.at(option.declaration.value);
  need(good.resolved->project->module(option_symbol.module).logical_path ==
               "std/option" && option_symbol.name == "Option",
       "upgrade result is not the canonical std/option.Option");

  // Python/64 uses the same source rules; only emission differs. The old
  // Python emitter has no Weak value operations yet, so this is a frontend and
  // capability result, not an execution claim.
  Fixture python_fixture;
  auto python_names =
      python_fixture.resolve(python_program, root / "stdlib");
  need(python_names.success(), "Python Weak fixture resolve failed\n" +
                                   diagnostics(python_names));
  auto python_resolved = retarget(
      std::make_shared<modules::ResolvedProject>(*python_names.value),
      Backend::python, 64);
  auto python_checked = sema::check_project(python_resolved);
  need(python_checked.success(),
       "Python/64 Weak frontend rejected\n" + diagnostics(python_checked));

  // The 16-byte Weak ABI is defined only for the 64-bit profiles. A 32-bit
  // project must report an explicit capability failure, never define a
  // different layout.
  auto narrow_names =
      python_fixture.resolve(python_program, root / "stdlib");
  auto narrow_resolved = retarget(
      std::make_shared<modules::ResolvedProject>(*narrow_names.value),
      Backend::native, 32);
  auto narrow = sema::check_project(narrow_resolved);
  need(!narrow.success() && has(narrow, "E_CAPABILITY_UNAVAILABLE"),
       "32-bit Weak profile was not rejected\n" + diagnostics(narrow));

  // Native HIR publication and first NativeIR lowering.
  auto published = sema::check_project(resolved);
  need(published.success(), "Weak HIR publication failed\n" +
                                diagnostics(published));
  auto lowered = ir::lower(*published.value);
  need(lowered.success(), "Weak NativeIR lowering failed\n" +
                              diagnostics(lowered));
  const ir::Module &good_ir = lowered.value->module();
  need(ir::verify(good_ir).success(), "unchanged Weak NativeIR rejected");

  // Forged HIR: non-canonical Weak targets are rejected independently of the
  // checker, including a user struct with the same private field shape.
  const auto i32 = [&] {
    for (uint64_t i = 1; i <= good.types.size(); ++i) {
      const auto &t = *good.types.find(TypeId{i});
      if (t.kind == TypeKind::SignedInt && t.bits == 32)
        return TypeId{i};
    }
    throw std::runtime_error("fixture i32 missing");
  }();
  reject_hir(good, "Weak scalar target", [&](HirModule &m) {
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {i32};
    (void)m.types.intern(weak);
  });
  reject_hir(good, "Weak user struct target", [&](HirModule &m) {
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {nominal_type_named(m, "Payload")};
    (void)m.types.intern(weak);
  });
  reject_hir(good, "Weak same-shape struct target", [&](HirModule &m) {
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {nominal_type_named(m, "SameShape")};
    (void)m.types.intern(weak);
  });
  reject_hir(good, "Weak Slice target", [&](HirModule &m) {
    Type slice;
    slice.kind = TypeKind::Slice;
    slice.type_args = {i32};
    auto slice_id = m.types.intern(slice);
    need(!weak_target(m, slice_id), "Slice was accepted as a Weak target");
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {slice_id};
    (void)m.types.intern(weak);
  });
  reject_hir(good, "Weak foreign target", [&](HirModule &m) {
    Type foreign;
    foreign.kind = TypeKind::ForeignCallable;
    foreign.binding = ForeignBindingId{1};
    auto foreign_id = m.types.intern(foreign);
    need(!weak_target(m, foreign_id),
         "foreign callable was accepted as a Weak target");
    Type weak;
    weak.kind = TypeKind::Weak;
    weak.type_args = {foreign_id};
    (void)m.types.intern(weak);
  });
  reject_hir(good, "Weak operation target replaced", [&](HirModule &m) {
    for (auto &node : m.nodes)
      if (auto *call = std::get_if<Call>(&node.data);
          call && call->system == SystemOperation::weak_downgrade) {
        call->type_args = {i32};
        call->signature = system_operation_signature(
            m.types, m, SystemOperation::weak_downgrade, call->type_args);
        break;
      }
  });

  // Forged NativeIR: the system operation identity and fixed signature must
  // match the verified HIR call; no new opcode was introduced for Weak.
  need(system_instruction(good_ir, SystemOperation::weak_downgrade),
       "Weak downgrade NativeIR instruction missing");
  reject_ir(good_ir, "Weak operation swapped", [](ir::Module &m) {
    for (auto &function : m.functions)
      for (auto &block : function.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.op == ir::Op::system_call &&
              instruction.system == SystemOperation::weak_downgrade) {
            instruction.system = SystemOperation::weak_upgrade;
            return;
          }
  });
  reject_ir(good_ir, "Weak operation origin replaced", [](ir::Module &m) {
    std::optional<HirNodeId> empty_call;
    const auto &nodes = m.hir.module().nodes;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (const auto *call = std::get_if<Call>(&nodes[i].data);
          call && call->system == SystemOperation::weak_empty) {
        empty_call = HirNodeId{i};
        break;
      }
    for (auto &function : m.functions)
      for (auto &block : function.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.op == ir::Op::system_call &&
              instruction.system == SystemOperation::weak_downgrade) {
            instruction.origin = empty_call;
            return;
          }
  });
  reject_ir(good_ir, "Weak call origin dropped", [](ir::Module &m) {
    for (auto &function : m.functions)
      for (auto &block : function.blocks)
        for (auto &instruction : block.instructions)
          if (instruction.op == ir::Op::system_call &&
              instruction.system == SystemOperation::weak_downgrade) {
            instruction.origin.reset();
            return;
          }
  });

  // Source negatives: ordinary values, descriptor wrappers, Slice, scalar and
  // dependent-instantiated targets are rejected explicitly.
  deny(root, R"WZ(package app;
import "std/weak" as weak;
func main() i32 { let t string = "x"; let w Weak[string] = weak.downgrade(t); return 0; }
)WZ", "Weak target must be", "Weak[string]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
struct Payload { value i32; }
func main() i32 { let p = Payload(value: 1); let w Weak[Payload] = weak.downgrade(p); return 0; }
)WZ", "Weak target must be", "Weak[user struct]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
struct SameShape { state Vec[i32]; }
func main() i32 { let p = SameShape(state: [1]); let w Weak[SameShape] = weak.downgrade(p); return 0; }
)WZ", "Weak target must be", "Weak[same-shape struct]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
import "std/os" as os;
func main() i32 { let f = os.stdin(); let w Weak[os.File] = weak.downgrade(f); return 0; }
)WZ", "Weak target must be", "Weak[os.File]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
import "std/slice" as slice;
func main() i32 { let v Vec[i32] = [1]; let s Slice[i32] = slice.view(v, 0, 1); let w Weak[Slice[i32]] = weak.downgrade(s); return 0; }
)WZ", "Weak target must be", "Weak[Slice]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
func main() i32 { let w Weak[i32] = weak.empty[i32](); return 0; }
)WZ", "Weak target must be", "Weak[scalar]");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
func main() i32 { let w Weak = weak.empty(); return 0; }
)WZ", "Weak requires one target type", "Weak arity");
  deny(root, R"WZ(package app;
import "std/weak" as weak;
func bad[T](v T) Weak[T] { return weak.downgrade[T](v); }
func main() i32 { let w = bad[i32](1); return 0; }
)WZ", "Weak target must be", "dependent bad instance");
  deny(root, R"WZ(package app;
func f(x Weak[Vec[i32], Vec[i32]]) {}
func main() i32 { return 0; }
)WZ", "Weak requires one target type", "Weak two arguments");
  deny_code(root, R"WZ(package app;
import "std/weak" as weak;
func f(w inout Weak[Vec[i32]]) {}
func main() i32 { var w Weak[Vec[i32]] = weak.empty[Vec[i32]](); f(inout w); return 0; }
)WZ", "E_CAPABILITY_UNAVAILABLE", "POD aggregate", "Weak inout");
  deny_code(root, R"WZ(package app;
func main() i32 { let w Weak[Vec[i32]] = __woozy_weak_empty[Vec[i32]](); return 0; }
)WZ", "E_CAPABILITY_UNAVAILABLE", "owner std module", "direct builtin call");

  // F1: a concrete invalid Weak inside a substituted nominal member must be a
  // Sema source error even when the function is never executed. Cover struct
  // fields, enum payloads, function signatures/returns, function-typed fields
  // and nested generic nominals; valid concrete and dependent counterparts
  // stay accepted.
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored(x Hold[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "struct field Hold[i32]");
  deny_buried_weak(root, R"WZ(package app;
enum Wrap[T] { Some(value Weak[T]); }
func ignored(x Wrap[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "enum payload Wrap[i32]");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored(cb func() Hold[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "function signature Hold[i32]");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored() Hold[i32] { for true {} }
func main() i32 { return 0; }
)WZ", "function return Hold[i32]");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { cb func() Weak[T]; }
func ignored(x Hold[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "function-typed field");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { cb func() i32 throws Weak[T]; }
func ignored(x Hold[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "throwing function-typed field");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
struct Outer[T] { inner Hold[T]; }
func ignored(x Outer[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "nested generic struct");
  deny_buried_weak(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
struct Outer[T] { cb func() Hold[T]; }
func ignored(x Outer[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "nested nominal behind function signature");
  deny_buried_weak(root, R"WZ(package app;
enum Hold[T] { Some(value Weak[T]); }
struct Outer[T] { inner Hold[T]; }
func ignored(x Outer[i32]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "nested generic enum");

  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored(x Hold[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid struct field Hold[Vec]");
  accept_source(root, R"WZ(package app;
enum Wrap[T] { Some(value Weak[T]); }
func ignored(x Wrap[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid enum payload Wrap[Vec]");
  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored(cb func() Hold[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid function signature Hold[Vec]");
  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
func ignored() Hold[Vec[i32]] { for true {} }
func main() i32 { return 0; }
)WZ", "valid function return Hold[Vec]");
  accept_source(root, R"WZ(package app;
struct Hold[T] { cb func() Weak[T]; }
func ignored(x Hold[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid function-typed field");
  accept_source(root, R"WZ(package app;
struct Hold[T] { cb func() i32 throws Weak[T]; }
func ignored(x Hold[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid throwing function-typed field");
  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
struct Outer[T] { inner Hold[T]; }
func ignored(x Outer[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid nested generic struct");
  accept_source(root, R"WZ(package app;
enum Hold[T] { Some(value Weak[T]); }
struct Outer[T] { inner Hold[T]; }
func ignored(x Outer[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid nested generic enum");
  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
struct Outer[T] { cb func() Hold[T]; }
func ignored(x Outer[Vec[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "valid nested nominal behind function signature");
  accept_source(root, R"WZ(package app;
import "std/box" as box;
import "std/thread" as thread;
struct Holder { cell Weak[box.Box[i32]]; worker Weak[thread.Thread]; }
func main() i32 { return 0; }
)WZ", "canonical wrapper Weak fields before declaration completion");
  deny_buried_weak(root, R"WZ(package app;
struct Payload { value i32; }
struct Holder { value Weak[Payload]; }
func main() i32 { return 0; }
)WZ", "unused invalid Weak nominal field");
  // Signature nesting must not hide an invalid concrete nominal member.
  for (unsigned depth : {5u, 7u}) {
    std::string field = "Inner[T]";
    for (unsigned i = 0; i < depth; ++i) field = "func(" + field + ") i32";
    const std::string prefix =
        "package app; struct Inner[T] { item Weak[T]; } struct Outer[T] { f " +
        field + "; } func ignored(x Outer[";
    deny_buried_weak(root, prefix + "i32]) i32 { return 0; } "
        "func main() i32 { return 0; }", "deep function parameter nominal");
    accept_source(root, prefix + "Vec[i32]]) i32 { return 0; } "
        "func main() i32 { return 0; }", "valid deep function parameter nominal");
  }
  // Existing accepted recursive function-type expansion must stay finite and
  // accepted; the validation budget cannot chase this concrete type family.
  accept_source(root, R"WZ(package app;
struct S[T] { f func() S[S[T]]; }
func take(value S[S[i32]]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "recursive function-type generic family");
  accept_source(root, R"WZ(package app;
struct Hold[T] { value Weak[T]; }
enum Wrap[T] { Some(value Weak[T]); }
func generic[T](x Hold[T], y Wrap[T]) i32 { return 0; }
func main() i32 { return 0; }
)WZ", "dependent generic templates");

  std::cout << "reference_frontend weak passed: types=" << weak_types
            << " calls=" << weak_calls
            << "; canonical Vec/Fn/Box/Thread/Mutex/Condvar/Atomic/Ipc accepted;"
               " source+forged HIR/NativeIR negatives rejected\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "usage: reference_frontend_test REPO_ROOT");
    suite(argv[1]);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

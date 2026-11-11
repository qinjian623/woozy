#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
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
  for (const auto &d : result.diagnostics) {
    text += d.code + ": " + d.message;
    if (d.primary)
      text += " @" + std::to_string(d.primary->begin_byte) + ":" +
              std::to_string(d.primary->end_byte);
    text += "\n";
  }
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
    char pattern[] = "/tmp/woozy-concurrency-XXXXXX";
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

fs::path copy_stdlib(const fs::path &root, const fs::path &destination) {
  fs::copy(root / "stdlib", destination,
           fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  return destination;
}

void append(const fs::path &file, const std::string &text) {
  std::ofstream out(file, std::ios::app);
  out << text;
  need(bool(out), "fixture append failed");
}

size_t operation(const HirModule &m, SystemOperation wanted) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (auto *call = std::get_if<Call>(&m.nodes[i].data);
        call && call->system == wanted)
      return i;
  throw std::runtime_error("system operation missing: " +
                           std::to_string(static_cast<unsigned>(wanted)));
}

Call &call_at(HirModule &m, SystemOperation wanted) {
  return std::get<Call>(m.nodes[operation(m, wanted)].data);
}

const Call &call_at(const HirModule &m, SystemOperation wanted) {
  return std::get<Call>(m.nodes[operation(m, wanted)].data);
}

void bad(const HirModule &good, const std::string &name, const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = verify_types(changed);
  need(!checked.success() && has(checked, "E_IR_INVALID"),
       name + " direct verify accepted\n" + diagnostics(checked));
  auto published = verify(changed, {}, VerifyMode::static_only);
  need(!published.success() && has(published, "E_IR_INVALID"),
       name + " publish accepted");
  auto rebuilt = detail::produce_ownership(changed, {});
  if (rebuilt.success()) {
    auto rechecked = verify_types(changed);
    need(!rechecked.success() && has(rechecked, "E_IR_INVALID"),
         name + " recomputed ownership bypassed verification");
  }
}

void deny(const fs::path &root, const std::string &main, std::string_view code) {
  Fixture fixture;
  auto resolved = fixture.resolve(main, root / "stdlib");
  auto checked = sema::check_types(resolved);
  need(!checked.success() && has(checked, code),
       "wrong denial: expected " + std::string(code) + "\n" +
           diagnostics(checked));
}

ir::Instruction &system_instruction(ir::Module &module,
                                    SystemOperation operation) {
  for (auto &function : module.functions)
    for (auto &block : function.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.op == ir::Op::system_call &&
            instruction.system == operation)
          return instruction;
  throw std::runtime_error("system IR instruction missing");
}

void rejected_ir(const ir::Module &good, const std::string &name,
                 const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = ir::verify(changed);
  need(!checked.success() && !checked.diagnostics.empty() &&
           checked.diagnostics.front().code == "E_IR_INVALID",
       name + " accepted by NativeIR\n" + diagnostics(checked));
}

std::shared_ptr<modules::ResolvedProject>
target_resolved(const HirModule &module, Backend backend, unsigned word_bits) {
  auto resolved =
      std::make_shared<modules::ResolvedProject>(*module.resolved);
  auto project = std::make_shared<modules::Project>(*resolved->project);
  project->backend = backend == Backend::python ? "python" : "native";
  project->word_bits = word_bits;
  resolved->project = project;
  return resolved;
}

const char *valid_program = R"(package app;

import "std/thread" as thread;
import "std/sync" as sync;
import "std/os" as os;

struct Payload {
    values Vec[i32];
    count i32;
}

func add_one(x i32) i64 { return i64(x) + 1; }

func sum(values Vec[i32]) i64 {
    var total i64 = 0;
    for value in values { total += i64(value); }
    return total;
}

func take_payload(payload Payload) i64 { return i64(payload.count); }

func launch[T](entry func(T) i64, value T) thread.Thread throws os.Error {
    return thread.spawn[T](entry, value);
}

func nested_callback(callback func(thread.Thread) i64) i64 { return 0; }

func main() i32 throws os.Error {
    let values Vec[i32] = [1, 2, 3];
    let payload = Payload(values: values, count: 3);
    let worker = thread.spawn[Payload](take_payload, payload);
    let _ = thread.join(worker);
    let sum_worker = thread.spawn[Vec[i32]](sum, values);
    let _ = thread.join(sum_worker);
    let inferred = thread.spawn(add_one, 7);
    let _ = thread.join(inferred);
    let entry = func(x i32) i64 { return i64(x) + 2; };
    let fn_worker = thread.spawn[i32](entry, 5);
    let _ = thread.join(fn_worker);
    let generic_worker = launch[i32](add_one, 6);
    let _ = thread.join(generic_worker);
    let m = sync.mutex();
    sync.lock(m);
    let c = sync.condvar();
    sync.wait(c, m);
    sync.notify_one(c);
    sync.notify_all(c);
    sync.unlock(m);
    let a = sync.atomic_i64(1);
    sync.store(a, 2);
    let old = sync.fetch_add(a, 3);
    let _ = sync.compare_exchange(a, old + 3, 9);
    let _ = sync.load(a);
    return 0;
}
)";

const std::set<SystemOperation> concurrency_operations = {
    SystemOperation::thread_spawn, SystemOperation::thread_error,
    SystemOperation::thread_wait, SystemOperation::thread_result,
    SystemOperation::mutex_new, SystemOperation::mutex_lock,
    SystemOperation::mutex_unlock, SystemOperation::condvar_new,
    SystemOperation::condvar_wait, SystemOperation::condvar_notify_one,
    SystemOperation::condvar_notify_all, SystemOperation::atomic_i64_new,
    SystemOperation::atomic_i64_load, SystemOperation::atomic_i64_store,
    SystemOperation::atomic_i64_fetch_add,
    SystemOperation::atomic_i64_compare_exchange,
};

void suite(const fs::path &root) {
  const fs::path std_root = root / "stdlib";
  Fixture fixture;
  auto resolved = fixture.resolve(valid_program, std_root);
  need(resolved.success(), "valid fixture resolve failed\n" +
                               diagnostics(resolved));
  auto checked = sema::check_types(resolved);
  need(checked.success(), "valid concurrency HIR failed\n" +
                              diagnostics(checked));
  const HirModule good = *checked.value;

  auto published = sema::check_project(resolved);
  need(published.success(), "valid concurrency publication failed\n" +
                                diagnostics(published));
  auto lowered = ir::lower(*published.value);
  need(lowered.success(), "valid concurrency lowering failed\n" +
                              diagnostics(lowered));
  const ir::Module &good_ir = lowered.value->module();
  need(ir::verify(good_ir).success(), "unchanged concurrency IR rejected");

  std::set<SystemOperation> found;
  for (const auto &node : good.nodes)
    if (auto *call = std::get_if<Call>(&node.data);
        call && call->kind == CalleeKind::system) {
      need(call->system.has_value(), "system kind without operation");
      const auto *spec = system_operation_spec(*call->system);
      need(spec, "unknown system operation reached HIR");
      found.insert(*call->system);
      const auto &mod =
          good.resolved->project->module(node.source.node.module);
      need(mod.logical_path == spec->owner,
           "system operation came from the wrong owner module");
      need(call->direct_symbol && !call->callee,
           "system operation lost its direct builtin target");
      const auto &target = good.resolved->symbols[call->direct_symbol->value];
      need(target.kind == modules::SymbolKind::builtin_function &&
               target.name == spec->name,
           "system operation target is not its private builtin");
      const bool type_args =
          system_operation_has_type_arguments(*call->system);
      need(call->type_args.size() == (type_args ? 1 : 0),
           "system operation type argument count in HIR");
    }
  for (auto operation : concurrency_operations)
    need(found.count(operation) == 1,
         "concurrency operation did not reach HIR: " +
             std::to_string(static_cast<unsigned>(operation)));

  const auto &spawn = call_at(good, SystemOperation::thread_spawn);
  need(spawn.type_args.size() == 1, "spawn must carry one source type argument");
  auto spawn_signature = *good.types.find(spawn.signature);
  need(spawn_signature.kind == TypeKind::Function &&
           spawn_signature.parameters.size() == 2 &&
           spawn_signature.parameters[0].mode == ParamMode::ordinary &&
           spawn_signature.parameters[1].mode == ParamMode::ordinary,
       "spawn signature shape");
  const auto &entry_type = *good.types.find(spawn_signature.parameters[0].type);
  need(entry_type.kind == TypeKind::Function &&
           entry_type.parameters.size() == 1 &&
           entry_type.parameters[0].mode == ParamMode::ordinary &&
           !entry_type.throws_type,
       "spawn entry must be one ordinary non-throwing parameter");
  need(entry_type.parameters[0].type == spawn.type_args[0] &&
           spawn_signature.parameters[1].type == spawn.type_args[0],
       "spawn A must determine both entry parameter and argument");
  const auto &thread_handle = *good.types.find(spawn_signature.result);
  need(thread_handle.kind == TypeKind::ThreadHandle,
       "spawn result must be the private thread handle");
  auto handle = classify_value(good, spawn_signature.result);
  need(handle.kind == ValueKind::runtime_handle && handle.needs_drop,
       "thread handle ownership classification");

  const auto &mutex_new = call_at(good, SystemOperation::mutex_new);
  const TypeId mutex_handle_id = good.types.find(mutex_new.signature)->result;
  const auto &mutex_type = *good.types.find(mutex_handle_id);
  need(mutex_type.kind == TypeKind::MutexHandle,
       "mutex constructor result type");
  const auto &condvar_type = *good.types.find(
      good.types.find(call_at(good, SystemOperation::condvar_new).signature)
          ->result);
  need(condvar_type.kind == TypeKind::CondvarHandle,
       "condvar constructor result type");
  const auto &atomic_type = *good.types.find(
      good.types.find(call_at(good, SystemOperation::atomic_i64_new).signature)
          ->result);
  need(atomic_type.kind == TypeKind::AtomicI64Handle,
       "atomic constructor result type");
  need(classify_value(good, spawn_signature.result).kind ==
               ValueKind::runtime_handle &&
           classify_value(good, mutex_handle_id).kind ==
               ValueKind::runtime_handle,
       "runtime handle classifier");

  bool saw_thread_wrapper = false;
  for (const auto &nominal : good.nominals)
    if (good.resolved->symbols[nominal.symbol.value].name == "Thread") {
      saw_thread_wrapper = nominal.fields.size() == 1;
      if (saw_thread_wrapper)
        need(good.types.find(nominal.fields[0].type)->kind ==
                 TypeKind::ThreadHandle,
             "public Thread must wrap the private handle");
    }
  need(saw_thread_wrapper, "public Thread wrapper declaration missing");

  bool generic_instance = false;
  for (const auto &instance : good.instances)
    if (good.resolved->symbols[instance.origin.value].name == "launch")
      generic_instance = true;
  need(generic_instance, "generic spawn helper was not instantiated");
  bool concrete_spawn_arg = false;
  for (const auto &node : good.nodes)
    if (auto *call = std::get_if<Call>(&node.data);
        call && call->system == SystemOperation::thread_spawn) {
      for (auto arg : call->type_args) {
        const auto &t = *good.types.find(arg);
        if (t.kind != TypeKind::TypeParameter)
          concrete_spawn_arg = true;
      }
    }
  need(concrete_spawn_arg, "spawn instances did not substitute concrete A");

  auto with_config = [&](const HirModule &source, Config config) {
    auto target = source;
    target.config = config;
    for (auto &instance : target.instances)
      instance.target = config;
    target.resolved =
        target_resolved(source, config.backend, config.word_bits);
    return target;
  };
  for (const auto config : {Config{Backend::python, 64},
                            Config{Backend::native, 32}}) {
    auto result = check_backend_capabilities(with_config(good, config));
    need(!result.success() && has(result, "E_CAPABILITY_UNAVAILABLE"),
         "unsupported runtime-handle profile accepted: " +
             diagnostics(result));
  }
  need(check_backend_capabilities(with_config(
             good, Config{Backend::native, 64})).success(),
       "native/64 runtime handles rejected");
  need(!check_backend_capabilities(with_config(
              good, Config{Backend::python, 32})).success(),
       "python/32 runtime handles accepted");

  // A private std/thread function with a direct function(handle) parameter
  // proves capability rejection reaches handles nested inside function types.
  Fixture nested_fixture;
  const auto nested_std = copy_stdlib(root, nested_fixture.path / "stdlib");
  append(nested_std / "thread/thread.wz",
         "\nfunc nested_handle_callback(callback func(__woozy_Thread) i64) i64 "
         "{ return 0; }\n");
  auto nested_resolved = nested_fixture.resolve(
      "package app; import \"std/thread\" as thread; func main() i32 { return 0; }",
      nested_std);
  auto nested_checked = sema::check_types(nested_resolved);
  need(nested_checked.success(), "function-nested handle source failed\n" +
                                     diagnostics(nested_checked));
  for (const auto config : {Config{Backend::python, 64},
                            Config{Backend::native, 32}}) {
    auto result = check_backend_capabilities(
        with_config(*nested_checked.value, config));
    need(!result.success() && has(result, "E_CAPABILITY_UNAVAILABLE"),
         "function-nested runtime handle profile accepted: " +
             diagnostics(result));
  }

  // The private spawn operation requires exactly one explicit type argument
  // even when the owner module writes the call itself.
  Fixture typearg_fixture;
  const auto typearg_std = copy_stdlib(root, typearg_fixture.path / "stdlib");
  append(typearg_std / "thread/thread.wz",
         "\npub func missing_typearg[A](entry func(A) i64, argument A) i64 {\n"
         "    let handle = __woozy_thread_spawn(entry, argument);\n"
         "    return 0;\n"
         "}\n");
  auto typearg_resolved = typearg_fixture.resolve(
      "package app; import \"std/thread\" as thread; func main() i32 { return 0; }",
      typearg_std);
  auto typearg_checked = sema::check_types(typearg_resolved);
  need(!typearg_checked.success() &&
           has(typearg_checked, "E_CALL_ARGUMENT"),
       "private spawn accepted a missing type argument: " +
           diagnostics(typearg_checked));

  // Wrong owner inside a real std module: std/thread may not call std/sync.
  Fixture owner_fixture;
  const auto owner_std = copy_stdlib(root, owner_fixture.path / "stdlib");
  append(owner_std / "thread/thread.wz",
         "\npub func wrong_owner() i64 {\n"
         "    let m = __woozy_mutex_new();\n"
         "    __woozy_mutex_lock(m);\n"
         "    return 0;\n"
         "}\n");
  auto owner_resolved = owner_fixture.resolve(
      "package app; import \"std/thread\" as thread; func main() i32 { return 0; }",
      owner_std);
  auto owner_checked = sema::check_types(owner_resolved);
  need(!owner_checked.success() &&
           has(owner_checked, "E_CAPABILITY_UNAVAILABLE"),
       "std/thread called a std/sync private operation: " +
           diagnostics(owner_checked));

  const std::string imports =
      "package app;\nimport \"std/thread\" as thread;\n"
      "import \"std/sync\" as sync;\nimport \"std/os\" as os;\n";
  deny(root,
       imports +
           "func entry(x i32) i64 throws os.Error { return i64(x); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x i32, y i32) i64 { return i64(x + y); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry() i64 { return 0; }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x move i32) i64 { return i64(x); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x i64) i64 { return x; }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x i32) i64 { return i64(x); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](3, 1); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x i32) i64 { return i64(x); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry); return 0; }",
       "E_CALL_ARGUMENT");
  deny(root,
       imports +
           "func entry(x i32) i64 { return i64(x); }\n"
           "func main() i32 throws os.Error { let t = thread.spawn[i32](entry, true); return 0; }",
       "E_TYPE_MISMATCH");
  deny(root,
       imports +
           "func entry(x i32) i64 { return i64(x); }\n"
           "func main() i32 { let t = __woozy_thread_spawn[i32](entry, 1); return 0; }",
       "E_CAPABILITY_UNAVAILABLE");
  deny(root,
       imports +
           "func main() i32 { let m = __woozy_mutex_new(); return 0; }",
       "E_CAPABILITY_UNAVAILABLE");
  deny(root,
       "package app;\nstruct Bad { state __woozy_Thread; }\n"
       "func main() i32 { return 0; }",
       "E_PRIVATE_ACCESS");

  Fixture shadow;
  auto shadow_resolved = shadow.resolve(
      "package app;\nfunc __woozy_thread_spawn(x i32) i64 { return 17; }\n"
      "func main() i32 { let v = __woozy_thread_spawn(1); return 0; }",
      std_root);
  auto shadow_checked = sema::check_project(shadow_resolved);
  need(shadow_checked.success(), "user shadow of private spelling failed\n" +
                                     diagnostics(shadow_checked));
  for (const auto &node : shadow_checked.value->module().nodes)
    if (auto *call = std::get_if<Call>(&node.data))
      need(call->kind == CalleeKind::direct && !call->system,
           "user function gained private system privilege");

  bad(good, "thread operation swap", [](auto &m) {
    call_at(m, SystemOperation::thread_error).system =
        SystemOperation::thread_wait;
  });
  bad(good, "spawn missing type argument", [](auto &m) {
    call_at(m, SystemOperation::thread_spawn).type_args.clear();
  });
  bad(good, "non-spawn extra type argument", [](auto &m) {
    auto &spawn = call_at(m, SystemOperation::thread_spawn);
    auto &wait = call_at(m, SystemOperation::thread_wait);
    wait.type_args.push_back(spawn.type_args.front());
  });
  bad(good, "wrong builtin target", [](auto &m) {
    auto &error = call_at(m, SystemOperation::thread_error);
    error.direct_symbol =
        call_at(m, SystemOperation::thread_wait).direct_symbol;
  });
  bad(good, "wrong fixed signature", [](auto &m) {
    auto &wait = call_at(m, SystemOperation::thread_wait);
    wait.signature = call_at(m, SystemOperation::thread_spawn).signature;
  });
  bad(good, "spawn type argument differs from source", [](auto &m) {
    auto &spawn = call_at(m, SystemOperation::thread_spawn);
    Type other;
    other.kind = TypeKind::SignedInt;
    other.bits = 64;
    spawn.type_args[0] = m.types.intern(other);
    spawn.signature = system_operation_signature(
        m.types, SystemOperation::thread_spawn, spawn.type_args);
  });
  bad(good, "owner module spoof", [](auto &m) {
    auto r = std::make_shared<modules::ResolvedProject>(*m.resolved);
    auto p = std::make_shared<modules::Project>(*r->project);
    const auto spawn = operation(m, SystemOperation::thread_spawn);
    p->modules[m.nodes[spawn].source.node.module.value].logical_path = "app";
    r->project = p;
    m.resolved = r;
  });

  rejected_ir(good_ir, "thread operation change", [](auto &m) {
    system_instruction(m, SystemOperation::thread_spawn).system =
        SystemOperation::thread_error;
  });
  rejected_ir(good_ir, "missing thread operation", [](auto &m) {
    system_instruction(m, SystemOperation::thread_wait).system.reset();
  });
  rejected_ir(good_ir, "thread origin/operation swap", [](auto &m) {
    auto &a = system_instruction(m, SystemOperation::thread_error);
    auto &b = system_instruction(m, SystemOperation::thread_wait);
    std::swap(a.origin, b.origin);
    std::swap(a.system, b.system);
  });

  std::cout << "passed: public thread/sync stdlib, generic spawn, handle ownership,\n"
               "        source denials, HIR metadata, NativeIR, and capability checks\n";
}

} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "usage: concurrency_test <repository-root>");
    suite(fs::path(argv[1]));
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

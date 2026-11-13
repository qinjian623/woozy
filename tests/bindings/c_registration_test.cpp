// Retained capturing C callbacks: LLVM-independent parser, Source and HIR
// contract checks for the approved development/docs contract.
//
// Native runtime linking and closure trampolines belong to the
// backend/runtime owners. This test only proves that frozen binding metadata is
// parsed strictly, rebased per bundle, and that Source/HIR/IR use the derived
// ordinary Fn signature. No synthetic HIR mutation probes are used.
#include "lang/hir/hir.hpp"
#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/foreign.hpp"
#include <algorithm>
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
fs::path repo, output;
size_t probes = 0;

void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
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
std::string read(const fs::path &path) {
  std::ifstream file(path);
  need(bool(file), "read " + path.string());
  return {std::istreambuf_iterator<char>(file), {}};
}
void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path);
  file << text;
  need(bool(file), "write " + path.string());
}
std::string replace_first(std::string text, const std::string &from,
                          const std::string &to) {
  auto at = text.find(from);
  need(at != std::string::npos, "fixture replacement missing: " + from);
  text.replace(at, from.size(), to);
  return text;
}

// Three managed resources cover callback userdata first, middle and last.
// Every registration resource declares no release/retain pair: its unregister
// symbol becomes the internal release. The close symbol is a separate explicit
// close callable with one matching resource parameter and a Unit result.
const char *events_bundle = R"JSON(
{
  "schema_version": 2,
  "contract": "compiler-dev/1",
  "abi": "c",
  "target": "x86_64-linux-gnu",
  "requires_hosted": true,
  "modules": [
    {"logical_path": "events/lib", "provider": "native_library", "library": "events_fixture"}
  ],
  "types": [
    {"id": "events.Callback", "module": "events/lib", "name": "Callback", "kind": "callback", "result": "i32", "parameters": [
      {"name": "kind", "type": "i32"},
      {"name": "context", "type": "PtrMut[Unit]"},
      {"name": "value", "type": "i32"}
    ]},
    {"id": "events.FirstCallback", "module": "events/lib", "name": "FirstCallback", "kind": "callback", "result": "bool", "parameters": [
      {"name": "context", "type": "PtrMut[Unit]"},
      {"name": "value", "type": "i32"}
    ]},
    {"id": "events.LastCallback", "module": "events/lib", "name": "LastCallback", "kind": "callback", "result": "Unit", "parameters": [
      {"name": "value", "type": "i32"},
      {"name": "context", "type": "PtrMut[Unit]"}
    ]},
    {"id": "events.Subscription", "module": "events/lib", "name": "Subscription", "kind": "resource",
     "registration": {"callback": "events.Callback", "unregister_symbol": "events_unsubscribe"}},
    {"id": "events.First", "module": "events/lib", "name": "First", "kind": "resource",
     "registration": {"callback": "events.FirstCallback", "unregister_symbol": "events_first_close"}},
    {"id": "events.Last", "module": "events/lib", "name": "Last", "kind": "resource",
     "registration": {"callback": "events.LastCallback", "unregister_symbol": "events_last_close"}}
  ],
  "callables": [
    {"id": "events.subscribe", "module": "events/lib", "name": "subscribe", "symbol": "events_subscribe", "parameters": [
      {"name": "callback", "type": "events.Callback"},
      {"name": "userdata", "type": "PtrMut[Unit]"},
      {"name": "priority", "type": "i32"}
    ], "result": {"type": "events.Subscription", "ownership": "owned"},
     "registers": {"callback_parameter": "callback", "userdata_parameter": "userdata", "callback_userdata_parameter": "context"}},
    {"id": "events.unsubscribe", "module": "events/lib", "name": "unsubscribe", "symbol": "events_unsubscribe", "parameters": [
      {"name": "subscription", "type": "events.Subscription"}
    ], "result": {"type": "Unit", "ownership": "unit"}},
    {"id": "events.poll", "module": "events/lib", "name": "poll", "symbol": "events_poll", "parameters": [
      {"name": "subscription", "type": "events.Subscription"}
    ], "result": {"type": "i32", "ownership": "value"}},
    {"id": "events.subscribe_first", "module": "events/lib", "name": "subscribe_first", "symbol": "events_subscribe_first", "parameters": [
      {"name": "userdata", "type": "PtrMut[Unit]"},
      {"name": "callback", "type": "events.FirstCallback"},
      {"name": "id", "type": "i32"}
    ], "result": {"type": "events.First", "ownership": "owned"},
     "registers": {"callback_parameter": "callback", "userdata_parameter": "userdata", "callback_userdata_parameter": "context"}},
    {"id": "events.close_first", "module": "events/lib", "name": "close_first", "symbol": "events_first_close", "parameters": [
      {"name": "first", "type": "events.First"}
    ], "result": {"type": "Unit", "ownership": "unit"}},
    {"id": "events.subscribe_last", "module": "events/lib", "name": "subscribe_last", "symbol": "events_subscribe_last", "parameters": [
      {"name": "callback", "type": "events.LastCallback"},
      {"name": "tag", "type": "i32"},
      {"name": "userdata", "type": "PtrMut[Unit]"}
    ], "result": {"type": "events.Last", "ownership": "owned"},
     "registers": {"callback_parameter": "callback", "userdata_parameter": "userdata", "callback_userdata_parameter": "context"}},
    {"id": "events.close_last", "module": "events/lib", "name": "close_last", "symbol": "events_last_close", "parameters": [
      {"name": "last", "type": "events.Last"}
    ], "result": {"type": "Unit", "ownership": "unit"}}
  ]
}
)JSON";

// A second bundle proves that registration callback indexes are rebased into
// the project-wide C type table.
const char *other_bundle = R"JSON(
{
  "schema_version": 2,
  "contract": "compiler-dev/1",
  "abi": "c",
  "target": "x86_64-linux-gnu",
  "requires_hosted": true,
  "modules": [
    {"logical_path": "other/lib", "provider": "native_library", "library": "other_fixture"}
  ],
  "types": [
    {"id": "other.Handler", "module": "other/lib", "name": "Handler", "kind": "callback", "result": "i32", "parameters": [
      {"name": "value", "type": "i32"},
      {"name": "context", "type": "PtrMut[Unit]"}
    ]},
    {"id": "other.Session", "module": "other/lib", "name": "Session", "kind": "resource",
     "registration": {"callback": "other.Handler", "unregister_symbol": "other_close"}}
  ],
  "callables": [
    {"id": "other.open", "module": "other/lib", "name": "open", "symbol": "other_open", "parameters": [
      {"name": "handler", "type": "other.Handler"},
      {"name": "context", "type": "PtrMut[Unit]"}
    ], "result": {"type": "other.Session", "ownership": "owned"},
     "registers": {"callback_parameter": "handler", "userdata_parameter": "context", "callback_userdata_parameter": "context"}},
    {"id": "other.close", "module": "other/lib", "name": "close", "symbol": "other_close", "parameters": [
      {"name": "session", "type": "other.Session"}
    ], "result": {"type": "Unit", "ownership": "unit"}}
  ]
}
)JSON";

struct Dep {
  std::string name, file, json;
};

struct Loaded {
  bool ok = false;
  std::string error;
  std::shared_ptr<modules::ResolvedProject> resolved;
};

Loaded load_project(const std::string &text, const std::string &name,
                    const std::vector<Dep> &deps, bool hosted = true) {
  auto root = output / name;
  std::error_code ignored;
  fs::remove_all(root, ignored);
  write(root / "src/main.wz", text);
  std::string manifest =
      "schema_version=1\n[package]\nname=\"main\"\nsource_root=\"src\"\n"
      "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n"
      "[build.native]\nruntime=\"" +
      std::string(hosted ? "hosted" : "freestanding") + "\"\n";
  for (const auto &dep : deps) {
    write(root / dep.file, dep.json);
    manifest += "[dependencies." + dep.name + "]\nkind=\"native\"\nbinding=\"" +
                dep.file + "\"\nlibraries=[]\nlibrary_dirs=[\".\"]\n";
  }
  write(root / "lang.toml", manifest);
  SourceManager sources;
  auto loaded = modules::load_project({root / "lang.toml", {}, {}}, sources);
  if (!loaded.success()) return {false, diagnostics(loaded), {}};
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*loaded.value)));
  if (!resolved.success()) return {false, diagnostics(resolved), {}};
  return {true, {},
          std::make_shared<modules::ResolvedProject>(std::move(*resolved.value))};
}

struct Checked {
  std::shared_ptr<modules::ResolvedProject> resolved;
  HirModule module;
};

// Full LLVM-independent frontend publication plus NativeIR verification. This
// is the same source/HIR/IR pipeline the native backend consumes, without
// emitting runtime code.
Checked accepted(const std::string &text, const std::string &name,
                 const std::vector<Dep> &deps) {
  auto loaded = load_project(text, name, deps);
  need(loaded.ok, name + " load " + loaded.error);
  auto typed = sema::check_types(loaded.resolved);
  need(typed.success(), name + " check " + diagnostics(typed));
  auto owned = verify_types(*typed.value);
  need(owned.success(), name + " ownership " + diagnostics(owned));
  auto published = verify(*typed.value, {}, VerifyMode::static_only);
  need(published.success(), name + " HIR " + diagnostics(published));
  auto lowered = ir::lower(*published.value);
  need(lowered.success(), name + " IR lower " + diagnostics(lowered));
  auto verified = ir::verify(lowered.value->module());
  need(verified.success(), name + " IR verify " + diagnostics(verified));
  ++probes;
  std::cout << "passed " << name << '\n';
  return {loaded.resolved, std::move(*typed.value)};
}

void rejected_load(const std::string &text, const std::string &name,
                   const std::vector<Dep> &deps, const std::string &code,
                   bool hosted = true) {
  auto loaded = load_project(text, name, deps, hosted);
  need(!loaded.ok, name + " loaded malformed binding");
  need(loaded.error.find(code) != std::string::npos,
       name + " expected " + code + " got " + loaded.error);
  ++probes;
  std::cout << "passed reject-binding " << name << ' ' << code << '\n';
}

void rejected_source(const std::string &text, const std::string &name,
                     const std::vector<Dep> &deps, const std::string &code) {
  auto loaded = load_project(text, name, deps);
  need(loaded.ok, name + " load " + loaded.error);
  auto typed = sema::check_types(loaded.resolved);
  need(!typed.success() && !typed.value, name + " accepted bad source");
  need(has(typed, code),
       name + " expected " + code + " got " + diagnostics(typed));
  ++probes;
  std::cout << "passed reject-source " << name << ' ' << code << '\n';
}

const modules::CTypeDecl &c_type(const modules::Project &p,
                                 const std::string &identity) {
  for (const auto &decl : p.c_types)
    if (decl.identity == identity) return decl;
  throw std::runtime_error("missing C type " + identity);
}
const modules::CCallableDecl &c_callable(const modules::Project &p,
                                         const std::string &identity) {
  for (const auto &decl : p.c_callables)
    if (decl.identity == identity) return decl;
  throw std::runtime_error("missing C callable " + identity);
}
const HirNode &c_call_node(const HirModule &m, uint32_t callable) {
  for (const auto &node : m.nodes)
    if (auto call = std::get_if<Call>(&node.data);
        call && call->c_callable && *call->c_callable == callable)
      return node;
  throw std::runtime_error("missing C call " + std::to_string(callable));
}
TypeId scalar_id(TypeContext &ctx, TypeKind kind, unsigned bits = 0) {
  Type type;
  type.kind = kind;
  type.bits = bits;
  return ctx.intern(std::move(type));
}

void check_call_shape(const HirModule &m, uint32_t callable, size_t arity,
                      std::string_view name) {
  const auto &node = c_call_node(m, callable);
  const auto &call = std::get<Call>(node.data);
  const auto &signature = *m.types.find(call.signature);
  need(signature.kind == TypeKind::Function && !signature.throws_type &&
           signature.parameters.size() == arity && call.args.size() == arity,
       std::string(name) + ": derived source arity/signature");
  for (size_t i = 0; i < arity; ++i) {
    const auto &arg = call.args[i];
    need(arg.source_index == i && arg.parameter_index == i &&
             arg.mode == ParamMode::ordinary && !arg.label,
         std::string(name) + ": source-order call metadata");
  }
}

const std::string positions_source = R"WZ(package app;
import "events/lib" as events;

func middle(kind i32, value i32) i32 { return kind + value; }
func first(value i32) bool { return value > 0; }
func last(value i32) Unit { }

func use() Unit {
    let a = events.subscribe(middle, 3);
    let b = events.subscribe_first(first, 4);
    let c = events.subscribe_last(last, 5);
    let p i32 = events.poll(a);
    events.unsubscribe(a);
    events.close_first(b);
    events.close_last(c);
}
)WZ";

const std::string capture_source = R"WZ(package app;
import "events/lib" as events;

func middle(kind i32, value i32) i32 { return kind + value; }

func capture(base i32) events.Subscription {
    return events.subscribe(func(kind i32, value i32) i32 {
        return kind + value + base;
    }, 1);
}

func use() Unit {
    let a = capture(5);
    events.unsubscribe(a);
}
)WZ";

const std::string variable_source = R"WZ(package app;
import "events/lib" as events;

func through_variable(base i32) events.Subscription {
    let cb = func(kind i32, value i32) i32 {
        return kind * base;
    };
    let value = cb;
    return events.subscribe(value, 2);
}

func use() Unit {
    let a = through_variable(6);
    events.unsubscribe(a);
}
)WZ";

const std::string bound_source = R"WZ(package app;
import "events/lib" as events;

struct Adder {
    bias i32;
}

impl Adder {
    func combine(self Self, kind i32, value i32) i32 {
        return self.bias + kind + value;
    }
}

func bound(bias i32) events.Subscription {
    let adder = Adder(bias: bias);
    return events.subscribe(adder.combine, 3);
}

func use() Unit {
    let a = bound(7);
    events.unsubscribe(a);
}
)WZ";

const std::string exits_source = R"WZ(package app;
import "events/lib" as events;

struct Boom {
    code i32;
}

func middle(kind i32, value i32) i32 { return kind + value; }

func guarded(flag bool) Unit throws Boom {
    let s = events.subscribe(middle, 4);
    if flag {
        throw Boom(code: 2);
    }
    events.unsubscribe(s);
}

func automatic() Unit {
    let s = events.subscribe(middle, 5);
}

func main() i32 {
    try {
        guarded(false);
    } catch e Boom {
        return 1;
    }
    automatic();
    return 0;
}
)WZ";

const std::string implicit_close_source = R"WZ(package app;
import "events/lib" as events;

func first(value i32) bool { return value > 0; }

func use() Unit {
    let b = events.subscribe_first(first, 4);
}
)WZ";

const std::string multiple_bundles_source = R"WZ(package app;
import "events/lib" as events;
import "other/lib" as other;

func middle(kind i32, value i32) i32 { return kind + value; }
func handler(value i32) i32 { return value; }

func use() Unit {
    let a = events.subscribe(middle, 1);
    let b = other.open(handler);
    events.unsubscribe(a);
    other.close(b);
}
)WZ";

const std::string legacy_static_source = R"WZ(package app;
import "fixture/lib" as fixture;

func callback(value i32, context u64) i32 {
    return value + i32(context);
}

func use(value i32) i32 {
    return fixture.cf_apply(callback, 100, value);
}
)WZ";
} // namespace

int main(int argc, char **argv) try {
  need(argc == 3, "usage: repository output");
  repo = argv[1];
  output = argv[2];
  std::vector<Dep> events = {{"events", "events.json", events_bundle}};
  std::vector<Dep> both = {{"events", "events.json", events_bundle},
                           {"other", "other.json", other_bundle}};
  std::vector<Dep> legacy = {
      {"fixture", "fixture.json",
       read(repo / "tests/native/c_ffi/bindings/fixture.json")}};

  // ---------------------------------------------------------------- parser --
  auto positions = accepted(positions_source, "positions", events);
  const auto &project = *positions.resolved->project;
  const auto &subscription = c_type(project, "events.Subscription");
  const auto &first = c_type(project, "events.First");
  const auto &last = c_type(project, "events.Last");
  const auto &subscribe = c_callable(project, "events.subscribe");
  const auto &subscribe_first = c_callable(project, "events.subscribe_first");
  const auto &subscribe_last = c_callable(project, "events.subscribe_last");

  need(subscription.registration.has_value() &&
           first.registration.has_value() && last.registration.has_value(),
       "resource registration parsed");
  need(subscription.release_symbol == "events_unsubscribe" &&
           subscription.retain_symbol.empty(),
       "managed resource uses unregister as internal release and is move-only");
  need(c_type(project, "events.Callback").id ==
           subscription.registration->callback_type &&
           c_type(project, "events.FirstCallback").id ==
               first.registration->callback_type &&
           c_type(project, "events.LastCallback").id ==
               last.registration->callback_type,
       "registration callback indexes resolve to callback declarations");
  need(subscribe.registers.has_value() &&
           subscribe.registers->callback_parameter == 0 &&
           subscribe.registers->userdata_parameter == 1 &&
           subscribe.registers->callback_userdata_parameter == 1,
       "middle callback userdata indexes");
  need(subscribe_first.registers->callback_parameter == 1 &&
           subscribe_first.registers->userdata_parameter == 0 &&
           subscribe_first.registers->callback_userdata_parameter == 0,
       "first callback userdata indexes");
  need(subscribe_last.registers->callback_parameter == 0 &&
           subscribe_last.registers->userdata_parameter == 2 &&
           subscribe_last.registers->callback_userdata_parameter == 1,
       "last callback userdata indexes");
  need(modules::c_source_parameter_indices(subscribe) ==
               std::vector<size_t>({0, 2}) &&
           modules::c_source_parameter_indices(subscribe_first) ==
               std::vector<size_t>({1, 2}) &&
           modules::c_source_parameter_indices(subscribe_last) ==
               std::vector<size_t>({0, 1}),
       "raw C indexes survive while the source omits only callable userdata");

  TypeContext ctx;
  auto i32 = scalar_id(ctx, TypeKind::SignedInt, 32);
  auto boolean = scalar_id(ctx, TypeKind::Bool);
  auto unit = scalar_id(ctx, TypeKind::Unit);
  auto derived = *ctx.find(
      hir::detail::c_callable_signature(project, ctx, subscribe));
  auto middle_closure = hir::detail::c_callback_closure_signature(
      project, ctx, subscription.registration->callback_type,
      subscribe.registers->callback_userdata_parameter);
  auto first_closure = hir::detail::c_callback_closure_signature(
      project, ctx, first.registration->callback_type,
      subscribe_first.registers->callback_userdata_parameter);
  auto last_closure = hir::detail::c_callback_closure_signature(
      project, ctx, last.registration->callback_type,
      subscribe_last.registers->callback_userdata_parameter);
  need(derived.kind == TypeKind::Function && !derived.throws_type &&
           derived.parameters.size() == 2 &&
           derived.parameters[0].type == middle_closure &&
           derived.parameters[1].type == i32 &&
           derived.parameters[1].mode == ParamMode::ordinary,
       "derived source signature replaces the callback argument with Fn");
  need(ctx.find(middle_closure)->kind == TypeKind::Function &&
           !ctx.find(middle_closure)->throws_type &&
           ctx.find(middle_closure)->parameters.size() == 2 &&
           ctx.find(middle_closure)->parameters[0].type == i32 &&
           ctx.find(middle_closure)->parameters[1].type == i32 &&
           ctx.find(middle_closure)->result == i32,
       "middle callback userdata slot is omitted from the closure type");
  need(ctx.find(first_closure)->parameters.size() == 1 &&
           ctx.find(first_closure)->parameters[0].type == i32 &&
           ctx.find(first_closure)->result == boolean,
       "first callback userdata slot is omitted from the closure type");
  need(ctx.find(last_closure)->parameters.size() == 1 &&
           ctx.find(last_closure)->parameters[0].type == i32 &&
           ctx.find(last_closure)->result == unit,
       "last callback userdata slot is omitted from the closure type");
  need(ctx.find(derived.result)->kind == TypeKind::CForeignResource &&
           ctx.find(derived.result)->c_type == subscription.id,
       "derived source result is the owned managed resource");

  // Call metadata stays in source order; the frozen binding supplies the raw
  // C parameter mapping, including a userdata slot before the callback.
  check_call_shape(positions.module, subscribe.id, 2, "subscribe");
  check_call_shape(positions.module, subscribe_first.id, 2, "subscribe_first");
  check_call_shape(positions.module, subscribe_last.id, 2, "subscribe_last");
  const auto &poll = c_callable(project, "events.poll");
  check_call_shape(positions.module, poll.id, 1, "token borrowing call");

  // ----------------------------------------------------- ordinary Fn values --
  auto captured = accepted(capture_source, "capture", events);
  const auto &captured_callable =
      c_callable(*captured.resolved->project, "events.subscribe");
  const auto &captured_call =
      std::get<Call>(c_call_node(captured.module, captured_callable.id).data);
  const auto &captured_argument =
      captured.module.nodes.at(captured_call.args[0].expression.value);
  need(captured_argument.kind() == NodeKind::function_value &&
           !std::get<FunctionValue>(captured_argument.data).captures.empty(),
       "capturing closure reaches HIR as an ordinary Fn argument");

  auto variable = accepted(variable_source, "variable", events);
  const auto &variable_callable =
      c_callable(*variable.resolved->project, "events.subscribe");
  const auto &variable_call =
      std::get<Call>(c_call_node(variable.module, variable_callable.id).data);
  need(variable.module.nodes.at(variable_call.args[0].expression.value).kind() ==
           NodeKind::read,
       "function variable reaches HIR as an ordinary Fn value");

  auto bound = accepted(bound_source, "bound", events);
  const auto &bound_callable =
      c_callable(*bound.resolved->project, "events.subscribe");
  const auto &bound_call =
      std::get<Call>(c_call_node(bound.module, bound_callable.id).data);
  const auto &bound_argument =
      bound.module.nodes.at(bound_call.args[0].expression.value);
  need(bound_argument.kind() == NodeKind::function_value &&
           std::get<FunctionValue>(bound_argument.data).bound.has_value(),
       "bound method value reaches HIR as an ordinary Fn argument");

  // Normal exit, typed-error exit and automatic scope close all verify.
  auto exits = accepted(exits_source, "exits", events);
  const auto &exits_callable =
      c_callable(*exits.resolved->project, "events.subscribe");
  const auto &exits_call =
      std::get<Call>(c_call_node(exits.module, exits_callable.id).data);
  need(exits.module.nodes.at(exits_call.args[0].expression.value).kind() ==
           NodeKind::function_value,
       "error-exit program keeps the ordinary Fn callback");

  // ------------------------------------------------------- bundle rebasing --
  auto bundles = accepted(multiple_bundles_source, "multiple-bundles", both);
  const auto &bundle_project = *bundles.resolved->project;
  const auto &other_session = c_type(bundle_project, "other.Session");
  const auto &other_handler = c_type(bundle_project, "other.Handler");
  need(bundle_project.c_types.size() ==
           c_type(bundle_project, "events.Callback").id + 7,
       "both bundles contribute C types");
  need(other_handler.id == 7 && other_session.registration.has_value() &&
           other_session.registration->callback_type == other_handler.id,
       "second-bundle registration callback index is rebased");
  need(other_session.release_symbol == "other_close" &&
           other_session.retain_symbol.empty(),
       "second-bundle managed resource release");
  const auto &other_open = c_callable(bundle_project, "other.open");
  need(other_open.registers.has_value() &&
           other_open.registers->callback_parameter == 0 &&
           other_open.registers->userdata_parameter == 1 &&
           other_open.registers->callback_userdata_parameter == 1,
       "second-bundle callable registration indexes");
  check_call_shape(bundles.module, other_open.id, 1, "other.open");

  // ------------------------------------------------------ legacy static ABI --
  auto legacy_static = accepted(legacy_static_source, "legacy-static", legacy);
  const auto &legacy_project = *legacy_static.resolved->project;
  const auto &apply = c_callable(legacy_project, "fixture.cf_apply");
  need(!apply.registers.has_value(), "legacy callable keeps no registers");
  need(!c_type(legacy_project, "fixture.Callback").registration.has_value(),
       "legacy callback keeps no registration");
  TypeContext legacy_ctx;
  const auto &legacy_signature =
      *legacy_ctx.find(hir::detail::c_callable_signature(legacy_project,
                                                         legacy_ctx, apply));
  need(legacy_signature.parameters.size() == 3 &&
           legacy_ctx.find(legacy_signature.parameters[0].type)->kind ==
               TypeKind::CForeignCallback,
       "legacy static callback signature is unchanged");
  check_call_shape(legacy_static.module, apply.id, 3, "legacy static callback");

  // ------------------------------------------------------- source rejects --
  rejected_source(R"WZ(package app;
import "events/lib" as events;
func middle(kind i32, value i32) i32 { return kind + value; }
func use() Unit { events.subscribe(middle); }
)WZ",
                  "source-arity", events, "E_CALL_ARGUMENT");
  rejected_source(R"WZ(package app;
import "events/lib" as events;
func full(kind i32, context i32, value i32) i32 { return kind + context + value; }
func use() Unit { events.subscribe(full, 1); }
)WZ",
                  "source-explicit-callback-userdata", events,
                  "E_CALL_ARGUMENT");
  rejected_source(R"WZ(package app;
import "events/lib" as events;
struct Boom { code i32; }
func throwing(kind i32, value i32) i32 throws Boom { return kind + value; }
func use() Unit { events.subscribe(throwing, 1); }
)WZ",
                  "source-throws", events, "E_CALL_ARGUMENT");
  rejected_source(R"WZ(package app;
import "events/lib" as events;
func moving(kind move i32, value i32) i32 { return kind + value; }
func use() Unit { events.subscribe(moving, 1); }
)WZ",
                  "source-mode", events, "E_CALL_ARGUMENT");
  rejected_source(R"WZ(package app;
import "events/lib" as events;
func wide(kind i32, value i32) i64 { return i64(kind + value); }
func use() Unit { events.subscribe(wide, 1); }
)WZ",
                  "source-signature", events, "E_CALL_ARGUMENT");
  rejected_source(R"WZ(package app;
import "fixture/lib" as fixture;
func make(base i32) i32 {
    let cb = func(value i32, context u64) i32 {
        return value + i32(context) + base;
    };
    return fixture.cf_apply(cb, 0, 3);
}
)WZ",
                  "legacy-static-capture", legacy, "E_CAPABILITY_UNAVAILABLE");
  rejected_source(R"WZ(package app;
import "fixture/lib" as fixture;
func make() i32 {
    let value = func(value i32, context u64) i32 {
        return value + i32(context);
    };
    return fixture.cf_apply(value, 0, 3);
}
)WZ",
                  "legacy-static-variable", legacy, "E_CAPABILITY_UNAVAILABLE");

  // ------------------------------------------------------ binding rejects --
  rejected_load(positions_source, "binding-unknown-callback",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"callback\": \"events.Callback\", \"unregister_symbol\"",
                                "\"callback\": \"events.Missing\", \"unregister_symbol\"")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-registration-kind",
                {{"events", "events.json",
                  replace_first(
                      events_bundle,
                      "{\"id\": \"events.FirstCallback\", \"module\": \"events/lib\", \"name\": \"FirstCallback\", \"kind\": \"callback\", \"result\": \"bool\", \"parameters\": [",
                      "{\"id\": \"events.FirstCallback\", \"module\": \"events/lib\", \"name\": \"FirstCallback\", \"kind\": \"callback\", \"registration\": {\"callback\": \"events.Callback\", \"unregister_symbol\": \"events_unused\"}, \"result\": \"bool\", \"parameters\": [")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-declared-release",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"name\": \"Subscription\", \"kind\": \"resource\",",
                                "\"name\": \"Subscription\", \"kind\": \"resource\", \"release_symbol\": \"events_release\",")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-declared-retain",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"name\": \"Subscription\", \"kind\": \"resource\",",
                                "\"name\": \"Subscription\", \"kind\": \"resource\", \"retain_symbol\": \"events_retain\",")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-unknown-userdata-name",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"callback_parameter\": \"callback\", \"userdata_parameter\": \"userdata\"",
                                "\"callback_parameter\": \"callback\", \"userdata_parameter\": \"missing\"")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-userdata-type",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "{\"name\": \"userdata\", \"type\": \"PtrMut[Unit]\"},\n      {\"name\": \"priority\", \"type\": \"i32\"}",
                                "{\"name\": \"userdata\", \"type\": \"u64\"},\n      {\"name\": \"priority\", \"type\": \"i32\"}")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-userdata-readonly",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "{\"name\": \"userdata\", \"type\": \"PtrMut[Unit]\"},\n      {\"name\": \"priority\", \"type\": \"i32\"}",
                                "{\"name\": \"userdata\", \"type\": \"Ptr[Unit]\"},\n      {\"name\": \"priority\", \"type\": \"i32\"}")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-callback-userdata-type",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "{\"name\": \"kind\", \"type\": \"i32\"},\n      {\"name\": \"context\", \"type\": \"PtrMut[Unit]\"},",
                                "{\"name\": \"kind\", \"type\": \"i32\"},\n      {\"name\": \"context\", \"type\": \"u64\"},")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-managed-result-without-registers",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"result\": {\"type\": \"events.Subscription\", \"ownership\": \"owned\"},\n     \"registers\": {\"callback_parameter\": \"callback\", \"userdata_parameter\": \"userdata\", \"callback_userdata_parameter\": \"context\"}},",
                                "\"result\": {\"type\": \"events.Subscription\", \"ownership\": \"owned\"}},")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-registers-nonmanaged-result",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"result\": {\"type\": \"events.Subscription\", \"ownership\": \"owned\"},",
                                "\"result\": {\"type\": \"i32\", \"ownership\": \"value\"},")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-managed-callback-parameter",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "{\"name\": \"value\", \"type\": \"i32\"},\n      {\"name\": \"context\", \"type\": \"PtrMut[Unit]\"}",
                                "{\"name\": \"value\", \"type\": \"events.Subscription\"},\n      {\"name\": \"context\", \"type\": \"PtrMut[Unit]\"}")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-conflicting-unregister",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"result\": {\"type\": \"Unit\", \"ownership\": \"unit\"}},\n    {\"id\": \"events.poll\"",
                                "\"result\": {\"type\": \"Unit\", \"ownership\": \"unit\"}},\n    {\"id\": \"events.unsubscribe_conflict\", \"module\": \"events/lib\", \"name\": \"unsubscribe_conflict\", \"symbol\": \"events_unsubscribe\", \"parameters\": [\n      {\"name\": \"subscription\", \"type\": \"events.Subscription\"}\n    ], \"result\": {\"type\": \"Unit\", \"ownership\": \"unit\"}},\n    {\"id\": \"events.poll\"")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-close-shape",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"symbol\": \"events_unsubscribe\", \"parameters\": [\n      {\"name\": \"subscription\", \"type\": \"events.Subscription\"}\n    ], \"result\"",
                                "\"symbol\": \"events_unsubscribe\", \"parameters\": [\n      {\"name\": \"subscription\", \"type\": \"events.Subscription\"},\n      {\"name\": \"extra\", \"type\": \"i32\"}\n    ], \"result\"")}},
                "E_BINDING_INVALID");
  // An unexported unregister symbol is valid: automatic teardown is the only
  // close path. It still becomes the internal release of the resource.
  auto implicit = accepted(
      implicit_close_source, "implicit-close",
      {{"events", "events.json",
        replace_first(events_bundle, "\"symbol\": \"events_first_close\"",
                      "\"symbol\": \"events_first_close_other\"")}});
  const auto &implicit_project = *implicit.resolved->project;
  const auto &implicit_first = c_type(implicit_project, "events.First");
  need(implicit_first.registration.has_value() &&
           implicit_first.release_symbol == "events_first_close" &&
           implicit_first.retain_symbol.empty(),
       "unexported unregister remains the internal release");
  need(std::none_of(implicit_project.c_callables.begin(),
                    implicit_project.c_callables.end(),
                    [](const auto &callable) {
                      return callable.symbol == "events_first_close";
                    }),
       "unexported unregister symbol needs no close callable");
  check_call_shape(
      implicit.module,
      c_callable(implicit_project, "events.subscribe_first").id, 2,
      "implicit-only registration constructor");
  rejected_load(positions_source, "binding-duplicate-unregister",
                {{"events", "events.json",
                  replace_first(
                      events_bundle,
                      "\"registration\": {\"callback\": \"events.FirstCallback\", \"unregister_symbol\": \"events_first_close\"}",
                      "\"registration\": {\"callback\": \"events.FirstCallback\", \"unregister_symbol\": \"events_unsubscribe\"}")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-inconsistent-userdata-position",
                {{"events", "events.json",
                  replace_first(
                      events_bundle,
                      "\"callback_userdata_parameter\": \"context\"}},",
                      "\"callback_userdata_parameter\": \"context\"}},\n    {\"id\": \"events.subscribe_alt\", \"module\": \"events/lib\", \"name\": \"subscribe_alt\", \"symbol\": \"events_subscribe_alt\", \"parameters\": [\n      {\"name\": \"callback\", \"type\": \"events.Callback\"},\n      {\"name\": \"userdata\", \"type\": \"PtrMut[Unit]\"},\n      {\"name\": \"priority\", \"type\": \"i32\"}\n    ], \"result\": {\"type\": \"events.Subscription\", \"ownership\": \"owned\"},\n     \"registers\": {\"callback_parameter\": \"callback\", \"userdata_parameter\": \"userdata\", \"callback_userdata_parameter\": \"kind\"}},")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-unknown-registers-key",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"registers\": {\"callback_parameter\": \"callback\"",
                                "\"registers\": {\"extra\": true, \"callback_parameter\": \"callback\"")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-callback-not-string",
                {{"events", "events.json",
                  replace_first(events_bundle,
                                "\"callback\": \"events.Callback\", \"unregister_symbol\"",
                                "\"callback\": 3, \"unregister_symbol\"")}},
                "E_BINDING_INVALID");
  rejected_load(positions_source, "binding-requires-hosted", events,
                "E_CAPABILITY_UNAVAILABLE", false);

  std::cout << "passed probes=" << probes << '\n';
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}

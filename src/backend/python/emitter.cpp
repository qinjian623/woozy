#include "lang/backend/python.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace lang::backend::python {
namespace {
using namespace hir;

// A private code-generation tree. Blocks/branches/loops are structural, and
// leaves contain only compiler-generated names, literals and fixed operations.
// Source text and source names are never Python code.
struct Statement {
  std::string text;
  std::optional<SourceSpan> source;
  bool suite = false;
  std::vector<Statement> body, otherwise;
};
using Suite = std::vector<Statement>;
enum class RejectReason { other, control_depth, indentation, resources };
struct Rejected {
  std::string message;
  std::optional<SourceSpan> source;
  RejectReason reason = RejectReason::other;
};
bool integer(const Type &t) {
  return t.kind == TypeKind::SignedInt || t.kind == TypeKind::UnsignedInt ||
         t.kind == TypeKind::Isize || t.kind == TypeKind::Usize;
}
std::string name(SymbolId s) { return "_s" + std::to_string(s.value); }
std::string function_name(SymbolId s) { return "_f" + std::to_string(s.value); }
std::string string_literal(std::string_view utf8) {
  // Escape bytes into a Python bytes literal, then decode UTF-8 as text once.
  // Escaping each byte as a Unicode character would corrupt non-ASCII text.
  static constexpr char hex[] = "0123456789abcdef";
  std::string result = "b'";
  for (unsigned char c : utf8) {
    result += "\\x";
    result += hex[c >> 4];
    result += hex[c & 15];
  }
  return result + "'.decode('utf-8')";
}
std::string join(const std::vector<std::string> &values) {
  std::string result;
  for (const auto &v : values) {
    if (!result.empty())
      result += ", ";
    result += v;
  }
  return result;
}
std::string tuple(const std::vector<std::string> &values) {
  return "(" + join(values) + (values.empty() ? "" : ",") + ")";
}
std::string operation(BinaryOp op) {
  switch (op) {
  case BinaryOp::add:
    return "add";
  case BinaryOp::subtract:
    return "sub";
  case BinaryOp::multiply:
    return "mul";
  case BinaryOp::divide:
    return "div";
  case BinaryOp::remainder:
    return "rem";
  case BinaryOp::shift_left:
    return "shl";
  case BinaryOp::shift_right:
    return "shr";
  case BinaryOp::bit_and:
    return "and";
  case BinaryOp::bit_or:
    return "or";
  case BinaryOp::bit_xor:
    return "xor";
  case BinaryOp::equal:
    return "eq";
  case BinaryOp::not_equal:
    return "ne";
  case BinaryOp::less:
    return "lt";
  case BinaryOp::less_equal:
    return "le";
  case BinaryOp::greater:
    return "gt";
  case BinaryOp::greater_equal:
    return "ge";
  }
  throw std::logic_error("unknown verified binary operator");
}
class Emitter {
  const HirModule &m;
  std::stop_token stop;
  std::set<SymbolId> cells, named;
  SymbolId current;
  uint64_t serial = 0;
  struct Loop {
    HirNodeId id;
    uint64_t code_scope = 0;
    bool crosses_helper = false;
  };
  std::vector<Loop> loops;
  std::vector<HirNodeId> break_targets;
  std::vector<HirNodeId> active_defers;
  bool uses_cleanup_guard = false;
  bool uses_foreign_access = false;
  bool fallback = false, outlining = false, checking_only = false;
  uint64_t code_scope = 0, next_scope = 0, helper_count = 0;
  uint64_t capture_work = 0;
  unsigned expression_depth = 0;
  std::set<SymbolId> available;
  // Lambda capture locals are read-only environment aliases, never a source
  // binding and never an owning local slot: they must not be dropped at
  // function exit or read through '.value'.
  std::set<SymbolId> capture_locals;
  Suite helpers;
  Suite adapters;
  std::map<std::string, std::string> bound_adapters;
  uint64_t bound_adapter_count = 0;
  // Computed owning switch subjects currently held by an enclosing
  // structured try/finally. Their drops are emitted by that finally after
  // the arm block cleanup, so an expression failure handler must never
  // execute the same drop early from an ExitPlan's temporaries-only step.
  std::vector<HirNodeId> held_subjects;
  Output output;
  std::set<uint64_t> temporary_ids;
  std::set<SymbolId> owned_bindings;
  std::set<uint64_t> value_types;
  bool owned(TypeId id) const {
    // Function/Weak are owning reference values. The shared classifier still
    // reports their stage-A result until its reference-values branch lands, so
    // honour the published ValueKind here without redefining it.
    auto v = classify_value(m, id);
    return v.needs_drop || v.kind == ValueKind::function ||
           v.kind == ValueKind::weak;
  }
  std::string ops(TypeId id) const { return "_v" + std::to_string(id.value); }
  std::string key(OwnerKind kind, uint64_t id) const {
    return "(" + std::to_string(static_cast<unsigned>(kind)) + ", " +
           std::to_string(id) + ")";
  }
  std::string take(HirNodeId id, const std::string &value) const {
    return temporary_ids.contains(id.value)
               ? "_frame.take(" + key(OwnerKind::temporary, id.value) + ")"
               : value;
  }
  void steps(const std::vector<CleanupStep> &actions, Suite &suite,
             const HirNode &n, bool temporaries_only = false) {
    for (const auto &a : actions) {
      if (a.kind == CleanupKind::end_borrow)
        line(suite,
             "_frame.end_borrow(" + key(a.owner.kind, a.owner.identity) + ")",
             n);
      if (a.kind == CleanupKind::destroy_if_live &&
          (!temporaries_only || a.owner.kind == OwnerKind::temporary) &&
          !(temporaries_only && a.owner.kind == OwnerKind::temporary &&
            std::find(held_subjects.begin(), held_subjects.end(),
                      HirNodeId{a.owner.identity}) != held_subjects.end()))
        line(suite, "_frame.drop(" + key(a.owner.kind, a.owner.identity) + ")",
             n);
    }
  }
  void full(HirNodeId id, Suite &suite) {
    for (const auto &f : m.ownership.full_expressions)
      if (f.node == id)
        steps(f.actions, suite, node(id));
  }
  void local_drops(ScopeId scope, Suite &suite, const HirNode &n) {
    std::vector<LocalLifetime> locals;
    for (auto l : m.ownership.locals)
      if (l.scope == scope && l.mode == ParamMode::move && owned(l.type) &&
          !l.read_only_capture && !capture_locals.contains(l.symbol))
        locals.push_back(l);
    std::sort(locals.begin(), locals.end(),
              [](auto a, auto b) { return a.order > b.order; });
    for (auto l : locals)
      line(suite,
           "_frame.drop(" + key(OwnerKind::binding, l.symbol.value) + ")", n);
  }
  void guarded(Suite body, Suite cleanup, Suite &suite, const HirNode &n) {
    if (cleanup.empty()) {
      suite.insert(suite.end(), std::make_move_iterator(body.begin()),
                   std::make_move_iterator(body.end()));
      return;
    }
    uses_cleanup_guard = true;
    auto callback = "_cleanup" + std::to_string(serial++);
    suite.push_back({"def " + callback + "()", n.source.span, true,
                     std::move(cleanup), {}});
    suite.push_back({"with _LangCleanup(" + callback + ")", n.source.span,
                     true, std::move(body), {}});
  }
  void scope_wrap(ScopeId scope, Suite body, Suite &suite, const HirNode &n) {
    Suite cleanup;
    local_drops(scope, cleanup, n);
    guarded(std::move(body), std::move(cleanup), suite, n);
  }
  void collect_type(TypeId id) {
    if (!value_types.insert(id.value).second)
      return;
    const auto &t = type(id);
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice)
      collect_type(t.type_args[0]);
    auto v = classify_value(m, id);
    for (auto f : v.fields)
      collect_type(f.type);
  }
  void emit_type(TypeId id, Suite &program, std::set<uint64_t> &done) {
    if (!done.insert(id.value).second)
      return;
    auto v = classify_value(m, id);
    const auto &t = type(id);
    std::string desc = "_rt.ValueType()";
    if (t.kind == TypeKind::Vec)
      desc = "_rt.ValueType('vec')";
    else if (t.kind == TypeKind::Slice)
      desc = "_rt.ValueType('slice')";
    else if (t.kind == TypeKind::String)
      desc = "_rt.ValueType('string', stride=32)";
    else if (t.kind == TypeKind::Bool || t.kind == TypeKind::Unit)
      desc = "_rt.ValueType(stride=1)";
    else if (t.kind == TypeKind::SignedInt || t.kind == TypeKind::UnsignedInt ||
             t.kind == TypeKind::Float)
      desc = "_rt.ValueType(stride=" + std::to_string(t.bits / 8) + ")";
    else if (t.kind == TypeKind::Function)
      desc = "_rt.ValueType('function')";
    else if (t.kind == TypeKind::Weak)
      desc = "_rt.ValueType('weak')";
    else if (t.kind == TypeKind::TraitObject)
      desc = "_rt.ValueType('dyn')";

    if (v.kind == ValueKind::function)
      desc = "_rt.ValueType('function')";
    if (v.kind == ValueKind::weak)
      desc = "_rt.ValueType('weak')";
    if (v.kind == ValueKind::trait_object)
      desc = "_rt.ValueType('dyn')";
    if (v.kind == ValueKind::foreign_reference ||
        v.kind == ValueKind::foreign_callable)
      desc = "_rt.ValueType('foreign')";
    if (v.kind == ValueKind::foreign_error)
      desc = "_rt.ValueType('error')";
    if (v.kind == ValueKind::structure)
      desc = "_rt.ValueType('struct')";
    if (v.kind == ValueKind::enumeration)
      desc = "_rt.ValueType('enum')";
    // Recursive aggregates must share the same descriptor object before any
    // of their field/variant descriptors can refer back to it.
    program.push_back({ops(id) + " = " + desc, {}, false, {}, {}});
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Slice)
      emit_type(t.type_args[0], program, done);
    for (auto f : v.fields)
      emit_type(f.type, program, done);
    if (v.kind == ValueKind::structure) {
      std::vector<std::string> fields;
      for (auto f : v.fields)
        fields.push_back(ops(f.type));
      program.push_back(
          {ops(id) + ".fields = " + tuple(fields), {}, false, {}, {}});
    }
    if (v.kind == ValueKind::enumeration) {
      std::vector<std::string> variants;
      for (const auto &d : m.nominals)
        if (d.symbol == t.declaration)
          for (const auto &variant : d.variants) {
            std::vector<std::string> fields;
            for (auto f : v.fields)
              if (f.variant == variant.symbol)
                fields.push_back(ops(f.type));
            variants.push_back(std::to_string(variant.symbol.value) + ": " +
                               tuple(fields));
          }
      program.push_back(
          {ops(id) + ".variants = {" + join(variants) + "}", {}, false, {}, {}});
    }
  }

  const HirNode &node(HirNodeId id) const { return m.nodes.at(id.value); }
  const Type &type(TypeId id) const { return *m.types.find(id); }
  size_t field_index(TypeId receiver, SymbolId field) const {
    for (const auto &d : m.nominals)
      if (d.symbol == type(receiver).declaration)
        for (size_t i = 0; i < d.fields.size(); ++i)
          if (d.fields[i].symbol == field)
            return i;
    throw std::logic_error("verified field has no declaration index");
  }
  bool indexed_field_place(HirNodeId id) const {
    auto base = id;
    while (node(base).kind() == NodeKind::field_place)
      base = node(base).children[0];
    return node(base).kind() == NodeKind::index_place;
  }
  // Weak target projection is selected by the verified canonical declaration,
  // not by layout or a type-key spelling. Python maps Vec and function values
  // directly; Box is the one canonical wrapper whose private cell Vec is the
  // shared identity and must be re-wrapped on upgrade. Native-only handles
  // remain an explicit capability error rather than a simulated host handle.
  const modules::Symbol *canonical_nominal(TypeId id, std::string_view path,
                                         std::string_view name,
                                         size_t arity) const {
    const auto &t = type(id);
    if (t.kind != TypeKind::NominalStruct || t.type_args.size() != arity ||
        t.declaration.value >= m.resolved->symbols.size())
      return nullptr;
    const auto &s = m.resolved->symbols.at(t.declaration.value);
    if (s.kind != modules::SymbolKind::type || !s.exported || !s.definition ||
        s.foreign || s.name != name)
      return nullptr;
    const auto &module = m.resolved->project->module(s.module);
    if (module.binding || module.logical_path != path) return nullptr;
    return &s;
  }
  bool canonical_weak_box(TypeId id) const {
    const auto *s = canonical_nominal(id, "std/box", "Box", 1);
    if (!s) return false;
    const NominalDecl *nominal = nullptr;
    for (const auto &d : m.nominals)
      if (d.symbol == type(id).declaration) {
        nominal = &d;
        break;
      }
    if (!nominal || nominal->kind != TypeKind::NominalStruct ||
        nominal->params.size() != 1 || nominal->fields.size() != 1 ||
        !nominal->variants.empty())
      return false;
    const auto &field = nominal->fields.front();
    if (field.exported ||
        m.resolved->symbols.at(field.symbol.value).name != "cell")
      return false;
    const auto *field_type = m.types.find(field.type);
    if (!field_type || field_type->kind != TypeKind::Vec ||
        field_type->type_args.size() != 1)
      return false;
    const auto *element = m.types.find(field_type->type_args[0]);
    return element && element->kind == TypeKind::TypeParameter &&
           element->declaration == nominal->params.front().symbol;
  }
  bool canonical_weak_target(TypeId id) const {
    const auto &t = type(id);
    if (t.kind == TypeKind::Vec || t.kind == TypeKind::Function) return true;
    if (canonical_weak_box(id)) return true;
    return canonical_nominal(id, "std/thread", "Thread", 0) ||
           canonical_nominal(id, "std/sync", "Mutex", 0) ||
           canonical_nominal(id, "std/sync", "Condvar", 0) ||
           canonical_nominal(id, "std/sync", "AtomicI64", 0) ||
           canonical_nominal(id, "std/ipc", "Memory", 0);
  }
  TypeId weak_target_type(SystemOperation operation, const HirNode &n) const {
    if (operation == SystemOperation::weak_upgrade) {
      const auto &option = type(n.type);
      if (option.kind != TypeKind::NominalEnum || option.type_args.size() != 1)
        throw std::logic_error("verified weak.upgrade lost its Option target");
      return option.type_args.at(0);
    }
    const auto &weak = type(n.type);
    if (weak.kind != TypeKind::Weak || weak.type_args.size() != 1)
      throw std::logic_error("verified weak operation lost its target");
    return weak.type_args.at(0);
  }
  const modules::ForeignDeclaration &
  foreign_declaration(ForeignBindingId id) const {
    return m.resolved->project->foreign_binding(id);
  }
  std::string host_module_name(const std::string &logical) const {
    for (const auto &module : m.resolved->project->modules)
      if (module.logical_path == logical) {
        const auto *binding = m.resolved->project->binding_module(module.id);
        if (binding)
          return binding->host_module;
      }
    throw std::logic_error("foreign module is not loaded: " + logical);
  }
  std::string source_path(const Source &source) const {
    const auto &module = m.resolved->project->module(source.node.module);
    if (source.node.file >= module.files.size())
      throw std::logic_error("foreign source file index is invalid");
    return module.files[source.node.file]->source->path;
  }
  std::string strings(const std::vector<std::string> &values) const {
    std::vector<std::string> literals;
    for (const auto &value : values)
      literals.push_back(string_literal(value));
    return "[" + join(literals) + "]";
  }
  std::string path_tuple(const std::vector<std::string> &values) const {
    std::vector<std::string> literals;
    for (const auto &value : values)
      literals.push_back(string_literal(value));
    return tuple(literals);
  }
  std::string binding_info() const {
    std::vector<std::string> bundles;
    for (const auto &bundle : m.resolved->project->bindings) {
      std::vector<std::string> modules;
      for (const auto &module : bundle.modules)
        modules.push_back(
            "{'logical_path': " + string_literal(module.logical_path) +
            ", 'host_module': " + string_literal(module.host_module) +
            ", 'provider': " + string_literal(module.provider) + "}");
      std::vector<std::string> requirements;
      for (const auto &requirement : bundle.package_requirements)
        requirements.push_back(
            "{'package': " + string_literal(requirement.package) +
            ", 'version_policy': " +
            string_literal(requirement.version_policy) + "}");
      bundles.push_back(
          "{'path': " + string_literal(bundle.path.string()) +
          ", 'sha256': " + string_literal(bundle.sha256) +
          ", 'contract': " + string_literal(bundle.contract) +
          ", 'review_status': " + string_literal(bundle.review_status) +
          ", 'allowed_backends': " + strings(bundle.allowed_backends) +
          ", 'package_requirements': [" + join(requirements) +
          "], 'modules': [" + join(modules) + "]}");
    }
    return "{'runtime_version': " + string_literal(runtime_version) +
           ", 'bundles': [" + join(bundles) + "]}";
  }
  std::string foreign_location(const HirNode &n,
                               const modules::ForeignDeclaration &declaration) const {
    return string_literal(source_path(n.source)) + ", " +
           std::to_string(n.source.span.begin_byte) + ", " +
           std::to_string(n.source.span.end_byte) + ", " +
           string_literal(declaration.identity);
  }
  std::string host_access_invoke(HirNodeId id, const HirNode &n,
                                 const std::string &call, bool unit,
                                 Suite &suite) {
    Suite invoke;
    std::string value;
    if (unit)
      line(invoke, call, n);
    else
      value = value_save(invoke, call, id);
    bool error = false;
    Suite failure;
    for (const auto &plan : n.exits)
      if (plan.kind == ExitKind::error) {
        steps(plan.actions, failure, n, true);
        error = true;
      }
    if (error) {
      suite.push_back({"try", n.source.span, true, std::move(invoke), {}});
      line(failure, "raise", n);
      suite.push_back(
          {"except _rt.TypedError", n.source.span, true, std::move(failure), {}});
    } else
      suite.insert(suite.end(), std::make_move_iterator(invoke.begin()),
                   std::make_move_iterator(invoke.end()));
    return value;
  }
  void emit_foreign_access_helpers(Suite &program) {
    auto function = [&](const std::string &header, Suite body) {
      program.push_back({header, {}, true, std::move(body), {}});
    };
    function(
        "def _lang_host_error(exc, file, begin_byte, end_byte, operation)",
        {{"return _rt.TypedError(_rt.ERROR_TYPE_KEY, "
          "_rt.Error(exc, file, begin_byte, end_byte, operation), "
          "_rt.ERROR_OPS)",
          {},
          false,
          {},
          {}}});
    function(
        "def _lang_host_get(base, path, file, begin_byte, end_byte, operation, "
        "returns_text)",
        {{"try", {}, true,
          {{"value = base", {}, false, {}, {}},
           {"for attribute in path", {}, true,
            {{"value = getattr(value, attribute)", {}, false, {}, {}}}, {}},
           {"return _rt._host_text(value) if returns_text else value",
            {}, false, {}, {}}},
          {}},
         {"except _rt.TypedError", {}, true, {{"raise", {}, false, {}, {}}}, {}},
         {"except Exception as exc", {},
          true,
          {{"raise _lang_host_error(exc, file, begin_byte, end_byte, "
            "operation) from exc",
            {},
            false,
            {},
            {}}},
          {}}});
    function(
        "def _lang_host_set(base, path, value, file, begin_byte, end_byte, "
        "operation)",
        {{"try", {}, true,
          {{"target = base", {}, false, {}, {}},
           {"for attribute in path[:-1]", {}, true,
            {{"target = getattr(target, attribute)", {}, false, {}, {}}}, {}},
           {"setattr(target, path[-1], value)", {}, false, {}, {}}},
          {}},
         {"except _rt.TypedError", {}, true, {{"raise", {}, false, {}, {}}}, {}},
         {"except Exception as exc", {},
          true,
          {{"raise _lang_host_error(exc, file, begin_byte, end_byte, "
            "operation) from exc",
            {},
            false,
            {},
            {}}},
          {}}});
    function(
        "def _lang_host_getitem(base, key, file, begin_byte, end_byte, "
        "operation, returns_text)",
        {{"try", {}, true,
          {{"value = base[key]", {}, false, {}, {}},
           {"return _rt._host_text(value) if returns_text else value",
            {}, false, {}, {}}},
          {}},
         {"except _rt.TypedError", {}, true, {{"raise", {}, false, {}, {}}}, {}},
         {"except Exception as exc", {},
          true,
          {{"raise _lang_host_error(exc, file, begin_byte, end_byte, "
            "operation) from exc",
            {},
            false,
            {},
            {}}},
          {}}});
    function(
        "def _lang_host_setitem(base, key, value, file, begin_byte, end_byte, "
        "operation)",
        {{"try", {}, true,
          {{"base[key] = value", {}, false, {}, {}}}, {}},
         {"except _rt.TypedError", {}, true, {{"raise", {}, false, {}, {}}}, {}},
         {"except Exception as exc", {},
          true,
          {{"raise _lang_host_error(exc, file, begin_byte, end_byte, "
            "operation) from exc",
            {},
            false,
            {},
            {}}},
          {}}});
  }
  void check_signature(TypeId id, SourceSpan source) const {
    const auto &t = type(id);
    for (const auto &p : t.parameters)
      if (p.mode == ParamMode::inout &&
          !classify_value(m, p.type).pod_inout)
        throw Rejected{"Python value profile supports ordinary/move or "
                       "trivial numeric/bool/POD aggregate inout parameters",
                       source};
  }
  // Stable identity for typed-error dispatch.  A foreign type in error
  // position must itself be a declared runtime_error; a source nominal's type
  // arguments are payload identity, where an ordinary host reference is legal
  // and fully preserved.  Ordinary host identities are length-framed and
  // start with '@', which cannot start a source key or identifier, so their
  // text cannot imitate type structure; the fixed runtime_error bridge keeps
  // its identity.
  static std::string foreign_type_key(std::string_view identity) {
    return "@" + std::to_string(identity.size()) + ":" +
           std::string(identity);
  }
  std::string type_key(TypeId id, bool error_position) const {
    const auto &t = type(id);
    switch (t.kind) {
    case TypeKind::Unit:
      return "Unit";
    case TypeKind::Bool:
      return "bool";
    case TypeKind::String:
      return "string";
    case TypeKind::Isize:
      return "isize";
    case TypeKind::Usize:
      return "usize";
    case TypeKind::SignedInt:
      return "i" + std::to_string(t.bits);
    case TypeKind::UnsignedInt:
      return "u" + std::to_string(t.bits);
    case TypeKind::Float:
      return "f" + std::to_string(t.bits);
    case TypeKind::NominalStruct:
    case TypeKind::NominalEnum: {
      const auto &s = m.resolved->symbols.at(t.declaration.value);
      auto key =
          m.resolved->project->module(s.module).logical_path + "." + s.name;
      if (!t.type_args.empty()) {
        key += "[";
        for (size_t i = 0; i < t.type_args.size(); ++i) {
          if (i)
            key += ",";
          key += type_key(t.type_args[i], false);
        }
        key += "]";
      }
      return key;
    }
    case TypeKind::Vec:
      return "Vec[" + type_key(t.type_args[0], error_position) + "]";
    case TypeKind::Slice:
      return "Slice[" + type_key(t.type_args[0], error_position) + "]";
    case TypeKind::TraitObject: {
      const auto &s = m.resolved->symbols.at(t.declaration.value);
      std::string key =
          "dyn " + m.resolved->project->module(s.module).logical_path + "." +
          s.name;
      if (!t.type_args.empty()) {
        key += "[";
        for (size_t i = 0; i < t.type_args.size(); ++i) {
          if (i)
            key += ",";
          key += type_key(t.type_args[i], false);
        }
        key += "]";
      }
      return key;
    }
    case TypeKind::ForeignOpaque: {
      const auto &d = m.resolved->project->foreign_binding(t.binding);
      const auto *binding = std::get_if<modules::BindingType>(&d.detail);
      if (binding == nullptr)
        throw std::logic_error("foreign type declaration is invalid");
      if (error_position && binding->kind != "runtime_error")
        throw Rejected{"Python foreign error payload requires a runtime_error declaration",
                       d.source};
      if (binding->kind == "runtime_error")
        return d.identity;
      return foreign_type_key(d.identity);
    }
    case TypeKind::Function: {
      std::string key = "func(";
      for (size_t i = 0; i < t.parameters.size(); ++i) {
        if (i)
          key += ",";
        const auto &p = t.parameters[i];
        if (p.mode == ParamMode::inout)
          key += "inout ";
        if (p.mode == ParamMode::move)
          key += "move ";
        key += type_key(p.type, error_position);
      }
      key += ")->(" + type_key(t.result, error_position) + ")";
      if (t.throws_type)
        key += " throws(" + type_key(*t.throws_type, true) + ")";
      return key;
    }
    default:
      throw std::logic_error("non-concrete Python error type identity");
    }
  }
  // The structured block cleanups implement the verified plans. Check the
  // registrations in each plan against exactly those active at this point;
  // never substitute a block-tail plan for an earlier return/call failure.
  void exits(const HirNode &n, bool normal = false) const {
    for (const auto &plan : n.exits) {
      if ((plan.kind == ExitKind::normal) != normal)
        continue;
      std::vector<HirNodeId> cleanups;
      for (auto d = active_defers.rbegin(); d != active_defers.rend(); ++d) {
        const auto scope = std::get<Defer>(node(*d).data).owner_scope;
        if (std::find(plan.exited_scopes.begin(), plan.exited_scopes.end(),
                      scope) != plan.exited_scopes.end())
          cleanups.push_back(*d);
      }
      if (cleanups != plan.defers)
        throw std::logic_error(
            "Python lexical cleanup differs from verified ExitPlan");
    }
  }
  void pattern_names(const Pattern &p) {
    if (p.binding)
      named.insert(*p.binding);
    for (const auto &child : p.payload)
      pattern_names(child);
  }
  static constexpr size_t pattern_depth_limit = 128;
  std::string pattern_test(const Pattern &p, const std::string &value,
                           size_t depth = 0) const {
    if (depth > pattern_depth_limit)
      throw Rejected{
          "Python pattern generation exceeds its safe nesting depth",
          p.source.span};
    if (p.literal) {
      if (p.variant || p.binding || !p.payload.empty())
        throw Rejected{
            "Python literal pattern has invalid fields", p.source.span};
      return std::visit(
          [&](const auto &literal) -> std::string {
            using T = std::decay_t<decltype(literal)>;
            if constexpr (std::is_same_v<T, BoolLiteral>)
              return value + (literal.value ? " is True" : " is False");
            else if constexpr (std::is_same_v<T, IntegerLiteral>)
              return value + " == " + literal.decimal;
            else if constexpr (std::is_same_v<T, FloatLiteral>)
              return "_rt.float_binary('eq', " + value + ", _rt.ieee(" +
                     std::to_string(literal.bits) + ", " +
                     std::to_string(type(p.type).bits) + "), " +
                     std::to_string(type(p.type).bits) + ")";
            else
              return value + " == " + string_literal(literal.utf8);
          },
          *p.literal);
    }
    if (!p.variant)
      return "True";
    std::string test = value + "[0] == " + std::to_string(p.variant->value);
    for (size_t i = 0; i < p.payload.size(); ++i)
      if (p.payload[i].variant || p.payload[i].literal)
        test += " and (" +
                pattern_test(p.payload[i],
                             value + "[1][" + std::to_string(i) + "]",
                             depth + 1) +
                ")";
    return test;
  }
  void pattern_bind(const Pattern &p, const std::string &value, Suite &suite,
                    size_t depth = 0) {
    if (depth > pattern_depth_limit)
      throw Rejected{
          "Python pattern generation exceeds its safe nesting depth",
          p.source.span};
    if (p.binding)
      suite.push_back(
          {name(*p.binding) + " = " +
               (owned(p.type) ? "_frame.slot(" +
                                    key(OwnerKind::binding, p.binding->value) +
                                    ", " + ops(p.type) + ")"
                              : value),
           p.source.span,
           false,
           {},
           {}});
    if (p.binding)
      available.insert(*p.binding);
    if (p.binding && owned(p.type))
      suite.push_back({name(*p.binding) + ".install(" + ops(p.type) + ".copy(" +
                           value + "))",
                       p.source.span,
                       false,
                       {},
                       {}});
    for (size_t i = 0; i < p.payload.size(); ++i)
      pattern_bind(p.payload[i], value + "[1][" + std::to_string(i) + "]",
                   suite, depth + 1);
  }
  std::string numeric(TypeId id) const {
    const auto &t = type(id);
    // Boolean not/equality return before the runtime's width-dependent code.
    if (t.kind == TypeKind::Bool)
      return "0, False";
    return std::to_string(bit_width(t, m.config)) + ", " +
           (t.kind == TypeKind::SignedInt || t.kind == TypeKind::Isize
                ? "True"
                : "False");
  }
  void line(Suite &suite, std::string text, const HirNode &n) {
    check_cancelled(stop);
    suite.push_back({std::move(text), n.source.span, false, {}, {}});
  }
  std::string save(Suite &suite, const std::string &value, const HirNode &n) {
    auto result =
        "_t" + std::to_string(current.value) + "_" + std::to_string(serial++);
    line(suite, result + " = " + value, n);
    return result;
  }
  std::string value_save(Suite &suite, const std::string &value, HirNodeId id) {
    if (!temporary_ids.contains(id.value))
      return save(suite, value, node(id));
    return save(suite,
                "_frame.put(" + key(OwnerKind::temporary, id.value) + ", " +
                    ops(node(id).type) + ", " + value + ")",
                node(id));
  }
  std::string read(SymbolId s) const {
    // A capture local is a borrowed row field, not an Owner slot.
    const bool owner = (cells.contains(s) || owned_bindings.contains(s)) &&
                       !capture_locals.contains(s);
    return name(s) + (owner ? ".value" : "");
  }
  std::string binary(BinaryOp op, const std::string &a, const std::string &b,
                     TypeId operand) const {
    if (type(operand).kind == TypeKind::String)
      return "_rt.text_binary('" + operation(op) + "', " + a + ", " + b + ")";
    if (type(operand).kind == TypeKind::Float)
      return "_rt.float_binary('" + operation(op) + "', " + a + ", " + b +
             ", " + std::to_string(type(operand).bits) + ")";
    return "_rt.binary('" + operation(op) + "', " + a + ", " + b + ", " +
           numeric(operand) + ")";
  }
  std::string constant_value(const ConstantValue &value, Suite &suite,
                             const HirNode &n) {
    return std::visit(
        [&](const auto &v) -> std::string {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, UnitLiteral>)
            return save(suite, "None", n);
          else if constexpr (std::is_same_v<T, BoolLiteral>)
            return save(suite, v.value ? "True" : "False", n);
          else if constexpr (std::is_same_v<T, IntegerLiteral>)
            return save(suite, v.decimal, n);
          else if constexpr (std::is_same_v<T, FloatLiteral>)
            return save(suite,
                        "_rt.ieee(" + std::to_string(v.bits) + ", " +
                            std::to_string(type(value.type).bits) + ")",
                        n);
          else if constexpr (std::is_same_v<T, StringLiteral>)
            return save(suite, string_literal(v.utf8), n);
          else if constexpr (std::is_same_v<T, FunctionValue>) {
            if (!v.captures.empty() || v.move_capture || v.bound)
              throw Rejected{"A constant function value cannot capture or "
                             "bind a receiver",
                             n.source.span};
            return save(suite, function_name(v.symbol), n);
          } else {
            std::vector<std::string> fields;
            for (const auto &field : v.fields)
              fields.push_back(constant_value(field, suite, n));
            auto result = tuple(fields);
            if (v.variant)
              result =
                  "(" + std::to_string(v.variant->value) + ", " + result + ")";
            return save(suite, result, n);
          }
        },
        value.data);
  }
  // The environment is exactly one record row installed into a Vec. Its
  // element descriptor recurses into the capture field types so copy/drop/
  // trace of the resulting Vec handle every captured owning value.
  std::string environment(const std::vector<TypeId> &types,
                          const std::vector<std::string> &values,
                          const HirNode &n, Suite &suite) {
    if (types.empty() || types.size() != values.size())
      throw std::logic_error("function environment has no matching row");
    std::vector<std::string> fields;
    for (auto t : types)
      fields.push_back(ops(t));
    auto row = save(suite, tuple(values), n);
    return save(suite,
                "_rt.Vec.create(_rt.ValueType('struct', fields=" +
                    tuple(fields) + "), " + tuple({row}) + ")",
                n);
  }
  // A bound receiver lives in the environment's single row field. An
  // ordinary Self receives that borrowed field; a move Self gets a fresh
  // ordinary copy on every invocation, so the adapter never takes or clears
  // the environment field and a bound Fn stays repeatable/copyable.
  std::string bound_adapter(const FunctionValue &value, const HirNode &n) {
    if (!value.bound)
      throw std::logic_error("unbound function value requested an adapter");
    const auto &bound = *value.bound;
    const HirFunction *target = nullptr;
    for (const auto &f : m.functions)
      if (f.symbol == value.symbol)
        target = &f;
    if (!target)
      throw std::logic_error("bound method target is not a verified function");
    const auto &signature = type(target->signature);
    if (signature.parameters.empty())
      throw std::logic_error("bound method has no Self parameter");
    const auto mode = signature.parameters.front().mode;
    if (bound.mode != mode)
      throw std::logic_error(
          "bound receiver mode differs from the method signature");
    if (mode == ParamMode::inout)
      throw Rejected{"Python cannot bind an inout-Self method as a function "
                     "value",
                     n.source.span};
    const auto cache = std::to_string(value.symbol.value) + ":" +
                       std::to_string(static_cast<unsigned>(mode)) + ":" +
                       std::to_string(bound.type.value);
    if (auto found = bound_adapters.find(cache);
        found != bound_adapters.end())
      return found->second;
    const auto helper = "_bound" + std::to_string(bound_adapter_count++);
    std::vector<std::string> parameters{"_env"}, arguments;
    for (size_t i = 1; i < signature.parameters.size(); ++i) {
      parameters.push_back("_a" + std::to_string(i - 1));
      arguments.push_back("_a" + std::to_string(i - 1));
    }
    Suite body;
    std::string receiver = "_env.get(0)[0]";
    if (mode == ParamMode::move) {
      // ops.copy retains/replicates the receiver exactly once per call; the
      // copy, not the environment field, is what the move Self consumes.
      body.push_back({"_receiver = " + ops(bound.type) + ".copy(" +
                          receiver + ")",
                      n.source.span,
                      false,
                      {},
                      {}});
      receiver = "_receiver";
    }
    arguments.insert(arguments.begin(), receiver);
    body.push_back({"return " + function_name(value.symbol) + "(" +
                        join(arguments) + ")",
                    n.source.span,
                    false,
                    {},
                    {}});
    adapters.push_back({"def " + helper + "(" + join(parameters) + ")",
                        n.source.span,
                        true,
                        std::move(body),
                        {}});
    bound_adapters.emplace(cache, helper);
    return helper;
  }
  std::string function_value_expression(const HirNode &n, HirNodeId id,
                                        Suite &suite) {
    const auto &value = std::get<FunctionValue>(n.data);
    if (value.bound) {
      if (!value.captures.empty() || n.children.size() != 1)
        throw std::logic_error(
            "verified bound function value shape changed");
      auto receiver = expression(n.children[0], suite);
      auto environment_value = environment(
          {value.bound->type}, {take(n.children[0], receiver)}, n, suite);
      return value_save(
          suite,
          "_rt.FunctionValue(" + bound_adapter(value, n) + ", " +
              environment_value + ")",
          id);
    }
    if (value.captures.empty()) {
      if (!n.children.empty())
        throw std::logic_error(
            "verified noncapturing function value gained children");
      // A noncapturing value keeps the old immutable-function identity. It
      // still goes through value_save so an owning Function value would get
      // its temporary owner slot once the shared classifier publishes that.
      return value_save(suite, function_name(value.symbol), id);
    }
    if (n.children.size() != value.captures.size())
      throw std::logic_error(
          "verified function value capture arity changed");
    std::vector<TypeId> types;
    std::vector<std::string> values;
    for (size_t i = 0; i < value.captures.size(); ++i) {
      auto child = n.children[i];
      auto captured = expression(child, suite);
      // An owning Read produced its one copy as an owning temporary; a
      // synthetic move transfer is already an owning temporary. Taking here
      // installs that exact value once and never copies it again.
      values.push_back(take(child, captured));
      types.push_back(value.captures[i].type);
    }
    auto environment_value = environment(types, values, n, suite);
    return value_save(suite,
                      "_rt.FunctionValue(" + function_name(value.symbol) +
                          ", " + environment_value + ")",
                      id);
  }
  std::string expression(HirNodeId id, Suite &suite) {
    if (outlining && expression_depth >= 16)
      return outlined_expression(id, suite);
    ++expression_depth;
    try {
      auto value = expression_inner(id, suite);
      --expression_depth;
      return value;
    } catch (...) {
      --expression_depth;
      throw;
    }
  }
  std::string expression_inner(HirNodeId id, Suite &suite) {
    check_cancelled(stop);
    const auto &n = node(id);
    exits(n);
    switch (n.kind()) {
    case NodeKind::constant_literal:
      return value_save(suite, constant_value(std::get<ConstantLiteral>(n.data).value, suite, n), id);
    case NodeKind::unit_literal:
      return save(suite, "None", n);
    case NodeKind::bool_literal:
      return save(suite, std::get<BoolLiteral>(n.data).value ? "True" : "False",
                  n);
    case NodeKind::integer_literal:
      return save(suite, std::get<IntegerLiteral>(n.data).decimal, n);
    case NodeKind::float_literal:
      return save(suite,
                  "_rt.ieee(" +
                      std::to_string(std::get<FloatLiteral>(n.data).bits) +
                      ", " + std::to_string(type(n.type).bits) + ")",
                  n);
    case NodeKind::string_literal:
      return value_save(suite, string_literal(std::get<StringLiteral>(n.data).utf8), id);
    case NodeKind::read:
      return value_save(suite,
                        temporary_ids.contains(id.value)
                            ? ops(n.type) + ".copy(" +
                                  read(std::get<Read>(n.data).symbol) + ")"
                            : read(std::get<Read>(n.data).symbol),
                        id);
    case NodeKind::place:
      return save(suite, name(std::get<Place>(n.data).symbol), n);
    case NodeKind::function_value:
      return function_value_expression(n, id, suite);
    case NodeKind::trait_object_init: {
      const auto &init = std::get<TraitObjectInit>(n.data);
      auto concrete = expression(n.children[0], suite);
      std::vector<std::string> methods;
      for (auto method : init.methods)
        methods.push_back(function_name(method));
      return value_save(
          suite,
          "_rt.DynObject.create(" + ops(init.concrete) + ", " + concrete +
              ", " + tuple(methods) + ")",
          id);
    }
    case NodeKind::trait_dispatch: {
      const auto &dispatch = std::get<TraitDispatch>(n.data);
      auto receiver = expression(n.children[0], suite);
      std::vector<std::string> arguments(dispatch.args.size() - 1);
      for (size_t i = 1; i < dispatch.args.size(); ++i) {
        const auto &argument = dispatch.args[i];
        if (argument.parameter_index == 0 ||
            argument.parameter_index > arguments.size())
          throw std::logic_error("trait dispatch argument index");
        arguments[argument.parameter_index - 1] =
            expression(argument.expression, suite);
      }
      auto value = value_save(
          suite, receiver + ".call(" + std::to_string(dispatch.index) + ", " +
                     tuple(arguments) + ")", id);
      for (const auto &lifetime : m.ownership.calls)
        if (lifetime.node == id)
          steps(lifetime.on_return, suite, n);
      return value;
    }
    case NodeKind::struct_init: {
      const auto &init = std::get<StructInit>(n.data);
      std::vector<std::string> values(init.fields.size());
      for (const auto &arg : init.fields) {
        auto value = expression(arg.expression, suite);
        values.at(arg.parameter_index) = take(arg.expression, value);
      }
      return value_save(suite, tuple(values), id);
    }
    case NodeKind::enum_init: {
      const auto &init = std::get<EnumInit>(n.data);
      std::vector<std::string> values(init.payload.size());
      for (const auto &arg : init.payload) {
        auto value = expression(arg.expression, suite);
        values.at(arg.parameter_index) = take(arg.expression, value);
      }
      return value_save(suite,
                        "(" + std::to_string(init.variant.value) + ", " +
                            tuple(values) + ")",
                        id);
    }
    case NodeKind::field_read:
    case NodeKind::field_place: {
      const bool place = n.kind() == NodeKind::field_place;
      auto receiver = expression(n.children[0], suite);
      auto field = place ? std::get<FieldPlace>(n.data).field
                         : std::get<FieldRead>(n.data).field;
      auto index = std::to_string(field_index(node(n.children[0]).type, field));
      auto value = place ? "_rt.Field(" + receiver + ", " + index + ")"
                         : receiver + "[" + index + "]";
      if (!place && temporary_ids.contains(id.value))
        value = ops(n.type) + ".copy(" + value + ")";
      return value_save(suite, value, id);
    }
    case NodeKind::move: {
      const auto &mv = std::get<Move>(n.data);
      auto value = mv.binding
                       ? (owned(n.type) ? name(*mv.binding) + ".take()"
                                        : read(*mv.binding))
                       : take(n.children[0], expression(n.children[0], suite));
      return value_save(suite, value, id);
    }
    case NodeKind::vec_init: {
      std::vector<std::string> values;
      for (auto c : n.children) {
        auto value = expression(c, suite);
        values.push_back(take(c, value));
      }
      return value_save(suite,
                        "_rt.Vec.create(" + ops(type(n.type).type_args[0]) +
                            ", " + tuple(values) + ")",
                        id);
    }
    case NodeKind::index_read:
    case NodeKind::index_place: {
      auto receiver = expression(n.children[0], suite);
      auto index = expression(n.children[1], suite);
      if (n.kind() == NodeKind::index_place)
        return save(suite, "_rt.Index(" + receiver + ", " + index + ")", n);
      auto value = type(node(n.children[0]).type).kind == TypeKind::String
                       ? "_rt.text_at(" + receiver + ", " + index + ")"
                       : receiver + ".get(" + index + ")";
      if (owned(n.type))
        value = ops(n.type) + ".copy(" + value + ")";
      return value_save(suite, value, id);
    }
    case NodeKind::vec_slice: {
      const auto &slice = std::get<VecSlice>(n.data);
      auto receiver = expression(n.children[0], suite);
      auto start = slice.start ? expression(*slice.start, suite) : "None";
      auto end = slice.end ? expression(*slice.end, suite) : "None";
      return value_save(suite,
          type(n.type).kind == TypeKind::String
              ? "_rt.text_slice(" + receiver + ", " + start + ", " + end + ")"
              : type(node(n.children[0]).type).kind == TypeKind::Slice
                    ? receiver + ".sub(" + start + ", " + end + ")"
                    : receiver + ".slice(" + start + ", " + end + ")", id);
    }
    case NodeKind::unary: {
      auto operand = expression(n.children[0], suite);
      const auto op = std::get<Unary>(n.data).op;
      auto text = op == UnaryOp::negate        ? "neg"
                  : op == UnaryOp::logical_not ? "not"
                                               : "inv";
      if (type(node(n.children[0]).type).kind == TypeKind::Float)
        return save(suite,
                    "_rt.float_neg(" + operand + ", " +
                        std::to_string(type(n.type).bits) + ")",
                    n);
      return save(suite,
                  "_rt.unary('" + std::string(text) + "', " + operand + ", " +
                      numeric(node(n.children[0]).type) + ")",
                  n);
    }
    case NodeKind::binary: {
      auto a = expression(n.children[0], suite);
      auto b = expression(n.children[1], suite);
      return value_save(suite,
          binary(std::get<Binary>(n.data).op, a, b, node(n.children[0]).type), id);
    }
    case NodeKind::integer_convert: {
      auto value = expression(n.children[0], suite);
      const auto source_kind = type(node(n.children[0]).type).kind;
      if (source_kind == TypeKind::Float &&
          type(n.type).kind == TypeKind::Float) {
        // The runtime stores f32 after its own rounding, and every finite
        // binary32 value is exactly representable in Python's binary64 float.
        // Identity and widening therefore keep the saved value and its source
        // order. A binary64 -> binary32 narrowing must round once to the
        // target storage width; the runtime does that from the exact Python
        // float ratio, including overflow, subnormals and signed zero.
        if (type(node(n.children[0]).type).bits == 64 &&
            type(n.type).bits == 32)
          return save(suite,
                      "_rt.float_to_float(" + value + ", 32)", n);
        return save(suite, value, n);
      }
      if (type(n.type).kind == TypeKind::Float)
        return save(suite,
                    "_rt.int_to_float(" + value + ", " +
                        std::to_string(type(n.type).bits) + ")",
                    n);
      if (source_kind == TypeKind::Float)
        return save(suite,
                    "_rt.float_to_int(" + value + ", " + numeric(n.type) +
                        ")",
                    n);
      return save(suite, "_rt.convert(" + value + ", " + numeric(n.type) + ")",
                  n);
    }
    case NodeKind::short_circuit: {
      auto a = expression(n.children[0], suite);
      auto result = save(suite, a, n);
      Statement branch{std::get<ShortCircuit>(n.data).op == LogicalOp::and_
                           ? "if " + result
                           : "if not " + result,
                       n.source.span,
                       true,
                       {},
                       {}};
      auto b = expression(n.children[1], branch.body);
      line(branch.body, result + " = " + b, n);
      suite.push_back(std::move(branch));
      return result;
    }
    case NodeKind::call: {
      const auto &call = std::get<Call>(n.data);
      if (call.kind == CalleeKind::system) {
        std::vector<std::string> args(call.args.size());
        for (const auto &arg : call.args) {
          args[arg.parameter_index] = expression(arg.expression, suite);
          for (const auto &use : m.ownership.uses)
            if (use.consumer == id && use.operand == arg.source_index &&
                use.kind == UseKind::borrow && !use.destination) {
              auto owner = use.root ? key(OwnerKind::binding, use.root->value)
                                   : key(OwnerKind::temporary, arg.expression.value);
              line(suite, "_frame.borrow(" + owner + ")", n);
            }
        }
        const auto *spec = system_operation_spec(*call.system);
        std::string element = "None", tags = "{}";
        const bool weak_operation =
            spec->operation == SystemOperation::weak_empty ||
            spec->operation == SystemOperation::weak_downgrade ||
            spec->operation == SystemOperation::weak_upgrade;
        bool weak_box = false;
        if (weak_operation) {
          const auto target = weak_target_type(spec->operation, n);
          weak_box = canonical_weak_box(target);
          if (!weak_box && !canonical_weak_target(target))
            throw Rejected{"Python Weak target is not canonical", n.source.span};
          const auto &target_type = type(target);
          if (target_type.kind == TypeKind::NominalStruct && !weak_box)
            throw Rejected{"Python supports Weak only for Vec, function "
                           "values and std/box.Box; native-only shared handles "
                           "are unavailable",
                           n.source.span};
        }
        if (spec->operation == SystemOperation::vec_empty ||
            spec->operation == SystemOperation::vec_make ||
            spec->operation == SystemOperation::text_bytes ||
            spec->operation == SystemOperation::unicode_boundaries)
          element = ops(type(n.type).type_args.at(0));
        if (type(n.type).kind == TypeKind::NominalEnum) {
          std::vector<std::string> entries;
          for (const auto &declaration : m.nominals)
            if (declaration.symbol == type(n.type).declaration)
              for (const auto &variant : declaration.variants)
                entries.push_back(string_literal(m.resolved->symbols.at(variant.symbol.value).name) +
                                  ": " + std::to_string(variant.symbol.value));
          tags = "{" + join(entries) + "}";
        }
        std::string call;
        switch (spec->operation) {
        case SystemOperation::slice_view:
          call = "_rt.Slice.create(" + join(args) + ")";
          break;
        case SystemOperation::slice_sub:
          call = args.at(0) + ".sub(" + args.at(1) + ", " + args.at(2) + ")";
          break;
        case SystemOperation::slice_len:
          call = args.at(0) + ".len()";
          break;
        case SystemOperation::slice_to_vec:
          call = args.at(0) + ".to_vec(" +
                 ops(type(n.type).type_args.at(0)) + ")";
          break;
        default:
          call = "_rt.value_call(" + string_literal(spec->name.substr(8)) +
                 ", " + tuple(args) + ", " + element + ", " + tags +
                 (weak_operation ? (weak_box ? ", True" : ", False") : "") +
                 ")";
          break;
        }
        auto value = value_save(suite, call, id);
        for (const auto &lifetime : m.ownership.calls)
          if (lifetime.node == id) steps(lifetime.on_return, suite, n);
        return value;
      }
      if (call.kind == CalleeKind::foreign) {
        if (!call.callee || !call.binding)
          throw std::logic_error("verified foreign call lost target/binding");
        const auto &declaration = foreign_declaration(*call.binding);
        const auto &declared =
            std::get<modules::BindingCallable>(declaration.detail);
        auto callee = expression(*call.callee, suite);
        std::vector<std::string> positional, keywords;
        size_t operand = 1;
        for (const auto &arg : call.args) {
          auto value = expression(arg.expression, suite);
          for (const auto &u : m.ownership.uses)
            if (u.consumer == id && u.operand == operand &&
                u.kind == UseKind::borrow && !u.destination) {
              auto key = u.root
                             ? this->key(OwnerKind::binding, u.root->value)
                             : this->key(OwnerKind::temporary,
                                         arg.expression.value);
              line(suite, "_frame.borrow(" + key + ")", n);
            }
          ++operand;
          if (arg.mode != ParamMode::ordinary)
            throw Rejected{"Python foreign calls support ordinary arguments "
                           "only",
                           n.source.span};
          if (arg.label)
            keywords.push_back(
                string_literal(declared.parameters.at(arg.parameter_index)
                                   .name) +
                ": " + value);
          else
            positional.push_back(value);
        }
        const bool unit =
            type(type(call.signature).result).kind == TypeKind::Unit;
        Suite invoke;
        auto value = value_save(
            invoke,
            "_rt.foreign_call(" + callee + ", " + tuple(positional) + ", {" +
                join(keywords) + "}, " +
                foreign_location(n, declaration) + ", " +
                (unit ? "True" : "False") + ", " +
                (type(type(call.signature).result).kind == TypeKind::String ? "True" : "False") + ")",
            id);
        bool error = false;
        Suite failure;
        for (const auto &plan : n.exits)
          if (plan.kind == ExitKind::error) {
            steps(plan.actions, failure, n, true);
            error = true;
          }
        if (error) {
          suite.push_back({"try", n.source.span, true, std::move(invoke), {}});
          line(failure, "raise", n);
          suite.push_back({"except _rt.TypedError",
                           n.source.span,
                           true,
                           std::move(failure),
                           {}});
        } else
          suite.insert(suite.end(), invoke.begin(), invoke.end());
        for (const auto &c : m.ownership.calls)
          if (c.node == id)
            steps(c.on_return, suite, n);
        return value;
      }
      auto callee = call.kind == CalleeKind::direct
                        ? save(suite, function_name(*call.direct_symbol), n)
                        : expression(*call.callee, suite);
      // An owned indirect callee is borrowed after its own evaluation and
      // before any argument evaluation, so a throwing argument cannot enter
      // the callee body. The matching end_borrow steps come from the
      // verified success/typed-error plans; the owning value itself is
      // released only at the end of the full expression.
      if (call.kind != CalleeKind::direct && call.callee &&
          owned(node(*call.callee).type) &&
          temporary_ids.contains(call.callee->value))
        line(suite,
             "_frame.borrow(" +
                 key(OwnerKind::temporary, call.callee->value) + ")",
             n);
      std::vector<std::string> args(call.args.size());
      // Evaluate the published source-order vector before arranging formals.
      size_t operand = call.callee ? 1 : 0;
      for (const auto &arg : call.args) {
        args.at(arg.parameter_index) = expression(arg.expression, suite);
        for (const auto &u : m.ownership.uses)
          if (u.consumer == id && u.operand == operand &&
              u.kind == UseKind::borrow && !u.destination) {
            auto k = u.root ? key(OwnerKind::binding, u.root->value)
                            : key(OwnerKind::temporary, arg.expression.value);
            line(suite, "_frame.borrow(" + k + ")", n);
          }
        ++operand;
      }
      for (const auto &arg : call.args)
        if (arg.mode == ParamMode::move)
          args.at(arg.parameter_index) =
              take(arg.expression, args.at(arg.parameter_index));
      Suite invoke;
      auto value = value_save(invoke, callee + "(" + join(args) + ")", id);
      bool error = false;
      Suite failure;
      for (const auto &plan : n.exits)
        if (plan.kind == ExitKind::error) {
          steps(plan.actions, failure, n, true);
          error = true;
        }
      if (error) {
        suite.push_back({"try", n.source.span, true, std::move(invoke), {}});
        line(failure, "raise", n);
        suite.push_back({"except _rt.TypedError",
                         n.source.span,
                         true,
                         std::move(failure),
                         {}});
      } else
        suite.insert(suite.end(), invoke.begin(), invoke.end());
      for (const auto &c : m.ownership.calls)
        if (c.node == id)
          steps(c.on_return, suite, n);
      return value;
    }
    case NodeKind::foreign_read: {
      const auto &read = std::get<ForeignRead>(n.data);
      const auto &declaration = foreign_declaration(read.binding);
      if (read.access) {
        const auto location = foreign_location(n, declaration);
        const std::string text =
            type(n.type).kind == TypeKind::String ? "True" : "False";
        if (*read.access == ForeignAccessKind::property_read) {
          const auto &property =
              std::get<modules::BindingProperty>(declaration.detail);
          auto base = expression(n.children[0], suite);
          return host_access_invoke(
              id, n,
              "_lang_host_get(" + base + ", " + path_tuple(property.getter) +
                  ", " + location + ", " + text + ")",
              false, suite);
        }
        if (*read.access == ForeignAccessKind::index_read) {
          auto base = expression(n.children[0], suite);
          auto index = expression(n.children[1], suite);
          return host_access_invoke(
              id, n,
              "_lang_host_getitem(" + base + ", " + index + ", " + location +
                  ", " + text + ")",
              false, suite);
        }
        if (*read.access == ForeignAccessKind::slice_read) {
          auto base = expression(n.children[0], suite);
          auto start = read.start ? expression(*read.start, suite) : "None";
          auto end = read.end ? expression(*read.end, suite) : "None";
          return host_access_invoke(
              id, n,
              "_lang_host_getitem(" + base + ", slice(" + start + ", " + end +
                  "), " + location + ", " + text + ")",
              false, suite);
        }
        throw std::logic_error("foreign access place in expression position");
      }
      const auto &binding = m.foreign_bindings.at(read.binding.value);
      const modules::BindingCallable *declared = nullptr;
      if (binding.signature)
        declared = &std::get<modules::BindingCallable>(declaration.detail);
      std::string base, path;
      if (binding.owner) {
        if (!declared || n.children.size() != 1)
          throw std::logic_error("verified foreign receiver arity changed");
        base = expression(n.children[0], suite);
        if (declared->call_kind == "callable")
          path = "()";
        else if (declared->call_kind == "method" &&
                 !declaration.host_attribute.empty())
          path = path_tuple(declaration.host_attribute);
        else
          throw std::logic_error("verified foreign method has no host path");
      } else {
        if (!n.children.empty() || declaration.host_attribute.empty())
          throw std::logic_error("verified module read has no host path");
        base = save(
            suite,
            "_rt.host_module(" + string_literal(declaration.module) + ", " +
                string_literal(host_module_name(declaration.module)) + ")",
            n);
        path = path_tuple(declaration.host_attribute);
      }
      Suite invoke;
      auto value = value_save(
          invoke,
          "_rt.foreign_read(" + base + ", " + path + ", " +
              foreign_location(n, declaration) + ", " +
              (type(n.type).kind == TypeKind::String ? "True" : "False") + ")",
          id);
      bool error = false;
      Suite failure;
      for (const auto &plan : n.exits)
        if (plan.kind == ExitKind::error) {
          steps(plan.actions, failure, n, true);
          error = true;
        }
      if (error) {
        suite.push_back({"try", n.source.span, true, std::move(invoke), {}});
        line(failure, "raise", n);
        suite.push_back({"except _rt.TypedError",
                         n.source.span,
                         true,
                         std::move(failure),
                         {}});
      } else
        suite.insert(suite.end(), invoke.begin(), invoke.end());
      return value;
    }
    default:
      throw std::logic_error("non-expression in verified expression position");
    }
  }
  static bool structural(NodeKind kind) {
    return kind == NodeKind::block || kind == NodeKind::if_ ||
           kind == NodeKind::for_ || kind == NodeKind::for_in ||
           kind == NodeKind::switch_ || kind == NodeKind::try_;
  }
  std::vector<SymbolId> captures(HirNodeId root) {
    std::set<SymbolId> used;
    std::vector<HirNodeId> pending{root};
    while (!pending.empty()) {
      check_cancelled(stop);
      if (++capture_work > 2000000)
        throw Rejected{"Python outlining exceeds its source traversal budget",
                       node(root).source.span, RejectReason::resources};
      auto id = pending.back();
      pending.pop_back();
      const auto &n = node(id);
      if (auto r = std::get_if<Read>(&n.data))
        used.insert(r->symbol);
      if (auto p = std::get_if<Place>(&n.data))
        used.insert(p->symbol);
      if (auto v = std::get_if<Move>(&n.data); v && v->binding)
        used.insert(*v->binding);
      if (auto d = std::get_if<Defer>(&n.data))
        used.insert(d->references.begin(), d->references.end());
      pending.insert(pending.end(), n.children.begin(), n.children.end());
    }
    std::vector<SymbolId> result;
    for (auto symbol : used)
      if (available.contains(symbol))
        result.push_back(symbol);
    return result;
  }
  std::string outline(HirNodeId id, Suite &suite, bool expression_body) {
    if (++helper_count > 200000)
      throw Rejected{"Python outlining exceeds its helper budget",
                     node(id).source.span, RejectReason::resources};
    auto captured = captures(id);
    std::vector<std::string> arguments{"_frame"};
    for (auto symbol : captured)
      arguments.push_back(name(symbol));
    const auto helper = "_outline_" + std::to_string(current.value) + "_" +
                        std::to_string(helper_count);
    // Raw Cells/Owners cross code objects by reference. HIR identities select
    // captures; evaluating read(symbol) here would copy an owning value or
    // lose the storage required by an inout place.
    auto saved_available = available;
    auto saved_scope = code_scope;
    auto saved_depth = expression_depth;
    available = std::set<SymbolId>(captured.begin(), captured.end());
    code_scope = ++next_scope;
    expression_depth = 0;
    Suite body;
    try {
      if (expression_body) {
        auto value = expression_inner(id, body);
        line(body, "return " + value, node(id));
      } else {
        statement_inner(id, body);
      }
    } catch (...) {
      available = std::move(saved_available);
      code_scope = saved_scope;
      expression_depth = saved_depth;
      throw;
    }
    available = std::move(saved_available);
    code_scope = saved_scope;
    expression_depth = saved_depth;
    helpers.push_back({"def " + helper + "(" + join(arguments) + ")",
                       node(id).source.span,
                       true,
                       std::move(body),
                       {}});
    auto call = helper + "(" + join(arguments) + ")";
    if (expression_body)
      return save(suite, call, node(id));
    line(suite, call, node(id));
    return {};
  }
  std::string outlined_expression(HirNodeId id, Suite &suite) {
    return outline(id, suite, true);
  }
  void loop_body(Suite body, Suite &suite, const Loop &loop, const HirNode &n) {
    if (!loop.crosses_helper) {
      suite.insert(suite.end(), std::make_move_iterator(body.begin()),
                   std::make_move_iterator(body.end()));
      return;
    }
    suite.push_back({"try", n.source.span, true, std::move(body), {}});
    suite.push_back({"except _LangLoopJump as _jump",
                     n.source.span,
                     true,
                     {{"if _jump.target != " + std::to_string(loop.id.value),
                       n.source.span,
                       true,
                       {{"raise", n.source.span, false, {}, {}}},
                       {}},
                      {"if _jump.continuing",
                       n.source.span,
                       true,
                       {{"continue", n.source.span, false, {}, {}}},
                       {}},
                      {"break", n.source.span, false, {}, {}}},
                     {}});
  }
  void block(const HirNode &n, Suite &suite) {
    std::vector<HirNodeId> registrations;
    for (auto id : n.children)
      if (node(id).kind() == NodeKind::defer_)
        registrations.push_back(id);
    bool has_locals = false;
    for (auto l : m.ownership.locals)
      has_locals |= l.scope == n.scope && l.mode == ParamMode::move &&
                    owned(l.type) && !l.read_only_capture &&
                    !capture_locals.contains(l.symbol);
    if (registrations.empty() && !has_locals) {
      for (auto id : n.children)
        statement(id, suite);
      exits(n, true);
      return;
    }
    // Direct registrations in a lexical block execute in source order, so
    // their active set is a prefix. Nested blocks own separate counters.
    // This records only reachability, never copies a referenced source value.
    auto registered = save(suite, "0", n);
    Suite body;
    size_t ordinal = 0;
    for (auto id : n.children) {
      if (node(id).kind() == NodeKind::defer_) {
        active_defers.push_back(id);
        line(body, registered + " = " + std::to_string(++ordinal),
             node(id));
      } else {
        statement(id, body);
      }
    }
    exits(n, true);
    Suite cleanup;
    for (auto it = registrations.rbegin(); it != registrations.rend(); ++it) {
      active_defers.pop_back();
      const auto &d = node(*it);
      Statement branch{"if " + registered + " >= " + std::to_string(ordinal--),
                       d.source.span,
                       true,
                       {},
                       {}};
      statement(d.children[0], branch.body);
      cleanup.push_back(std::move(branch));
    }
    local_drops(n.scope, cleanup, n);
    // A Panic in a cleanup aborts its siblings and skips enclosing cleanups.
    guarded(std::move(body), std::move(cleanup), suite, n);
  }
  void statement(HirNodeId id, Suite &suite) {
    if (outlining && structural(node(id).kind())) {
      outline(id, suite, false);
      return;
    }
    statement_inner(id, suite);
  }
  void statement_inner(HirNodeId id, Suite &suite) {
    check_cancelled(stop);
    const auto &n = node(id);
    exits(n);
    switch (n.kind()) {
    case NodeKind::block:
      block(n, suite);
      break;
    case NodeKind::binding: {
      const auto &b = std::get<Binding>(n.data);
      auto value = expression(n.children[0], suite);
      if (owned(node(n.children[0]).type)) {
        line(suite,
             name(b.symbol) + " = _frame.slot(" +
                 key(OwnerKind::binding, b.symbol.value) + ", " +
                 ops(node(n.children[0]).type) + ")",
             n);
        line(suite,
             name(b.symbol) + ".install(" + take(n.children[0], value) + ")",
             n);
      } else
        line(suite,
             name(b.symbol) + " = " +
                 (b.writable ? "_rt.Cell(" + value + ")" : value),
             n);
      full(id, suite);
      available.insert(b.symbol);
      break;
    }
    case NodeKind::assign: {
      const auto &place_node = node(n.children[0]);
      if (const auto *read = std::get_if<ForeignRead>(&place_node.data);
          read && read->access &&
          (*read->access == ForeignAccessKind::property_place ||
           *read->access == ForeignAccessKind::index_place ||
           *read->access == ForeignAccessKind::slice_place)) {
        const auto &declaration = foreign_declaration(read->binding);
        const auto location = foreign_location(place_node, declaration);
        std::string call;
        if (*read->access == ForeignAccessKind::property_place) {
          const auto &property =
              std::get<modules::BindingProperty>(declaration.detail);
          if (property.setter.empty())
            throw std::logic_error("verified read-only property write");
          auto base = expression(place_node.children[0], suite);
          auto value = take(n.children[1], expression(n.children[1], suite));
          call = "_lang_host_set(" + base + ", " +
                 path_tuple(property.setter) + ", " + value + ", " +
                 location + ")";
        } else if (*read->access == ForeignAccessKind::index_place) {
          auto base = expression(place_node.children[0], suite);
          auto index = expression(place_node.children[1], suite);
          auto value = take(n.children[1], expression(n.children[1], suite));
          call = "_lang_host_setitem(" + base + ", " + index + ", " + value +
                 ", " + location + ")";
        } else {
          auto base = expression(place_node.children[0], suite);
          auto start =
              read->start ? expression(*read->start, suite) : "None";
          auto end = read->end ? expression(*read->end, suite) : "None";
          auto value = take(n.children[1], expression(n.children[1], suite));
          call = "_lang_host_setitem(" + base + ", slice(" + start + ", " +
                 end + "), " + value + ", " + location + ")";
        }
        host_access_invoke(n.children[0], place_node, call, true, suite);
        full(id, suite);
        break;
      }
      auto place = expression(n.children[0], suite);
      const auto &assign = std::get<Assign>(n.data);
      // Capture the old value before evaluating a potentially mutating RHS.
      std::string before;
      if (assign.compound)
        before = save(suite, place + ".value", n);
      auto value = expression(n.children[1], suite);
      if (assign.compound)
        value = save(
            suite,
            binary(*assign.compound, before, value, node(n.children[0]).type),
            n);
      auto installed = assign.compound && type(node(n.children[0]).type).kind == TypeKind::String
                           ? value : take(n.children[1], value);
      if (owned(node(n.children[0]).type) &&
          !indexed_field_place(n.children[0])) {
        // Field is a short-lived place and its old field owns exactly once.
        // An indexed field replaces the element through its real storage, so
        // Vec.set performs that old-field drop with the sibling fields.
        if (node(n.children[0]).kind() == NodeKind::field_place)
          line(suite,
               ops(node(n.children[0]).type) + ".drop(" + place + ".value)", n);
        else {
          line(suite, place + ".drop()", n);
        }
      }
      line(suite, place + ".value = " + installed, n);
      full(id, suite);
      break;
    }
    case NodeKind::expression_statement:
      (void)expression(n.children[0], suite);
      full(id, suite);
      break;
    case NodeKind::if_: {
      auto condition = expression(n.children[0], suite);
      full(n.children[0], suite);
      Statement branch{"if " + condition, n.source.span, true, {}, {}};
      statement(n.children[1], branch.body);
      if (n.children.size() == 3) {
        statement(n.children[2], branch.otherwise);
        // Preserve an empty else block as a suite too.
        if (branch.otherwise.empty())
          line(branch.otherwise, "pass", n);
      }
      suite.push_back(std::move(branch));
      break;
    }
    case NodeKind::for_: {
      Suite for_scope;
      statement(n.children[0], for_scope);
      Statement loop{"while True", n.source.span, true, {}, {}};
      if (node(n.children[2]).kind() != NodeKind::noop) {
        // A native continue runs body cleanup before reaching the next step.
        auto first = save(for_scope, "True", n);
        Statement step{"if " + first, n.source.span, true,
                       {{first + " = False", n.source.span, false, {}, {}}},
                       {}};
        statement(n.children[2], step.otherwise);
        loop.body.push_back(std::move(step));
      }
      auto condition = expression(n.children[1], loop.body);
      full(n.children[1], loop.body);
      loop.body.push_back({"if not " + condition,
                           n.source.span,
                           true,
                           {{"break", n.source.span, false, {}, {}}},
                           {}});
      loops.push_back({id, code_scope});
      break_targets.push_back(id);
      Suite body;
      statement(n.children[3], body);
      auto completed_loop = loops.back();
      break_targets.pop_back();
      loops.pop_back();
      loop_body(std::move(body), loop.body, completed_loop, n);
      // Safe point at the iteration header, before the step, condition or
      // body. A source continue reaches this line on the next iteration.
      loop.body.insert(loop.body.begin(),
                       {"_rt._gc_poll()", n.source.span, false, {}, {}});
      for_scope.push_back(std::move(loop));
      scope_wrap(n.scope, std::move(for_scope), suite, n);
      break;
    }
    case NodeKind::for_in: {
      const auto &fi = std::get<ForIn>(n.data);
      auto collection = expression(n.children[0], suite);
      auto iteration_key = key(OwnerKind::iteration, id.value);
      line(suite,
           "_frame.put(" + iteration_key + ", " +
               ops(node(n.children[0]).type) + ", " +
               take(n.children[0], collection) + ")",
           n);
      full(id, suite);
      const bool text = type(node(n.children[0]).type).kind == TypeKind::String;
      const bool slice = type(node(n.children[0]).type).kind == TypeKind::Slice;
      auto length = save(suite,
                         text ? "len(" + collection + ")"
                              : slice ? collection + ".len()"
                                      : collection + ".begin()",
                         n);
      auto index = save(suite, "0", n);
      auto first = save(suite, "True", n);
      Statement loop{"while True", n.source.span, true,
                     {{"if " + first, n.source.span, true,
                       {{first + " = False", n.source.span, false, {}, {}}},
                       {{index + " += 1", n.source.span, false, {}, {}}}},
                      {"if " + index + " >= " + length, n.source.span, true,
                       {{"break", n.source.span, false, {}, {}}}, {}}}, {}};
      TypeId element_type{};
      if (text) {
        for (const auto &local : m.ownership.locals)
          if (local.symbol == fi.element) element_type = local.type;
      } else element_type = type(node(n.children[0]).type).type_args[0];
      if (owned(element_type)) {
        line(loop.body,
             name(fi.element) + " = _frame.slot(" +
                 key(OwnerKind::binding, fi.element.value) + ", " +
                 ops(element_type) + ")",
             n);
        line(loop.body,
             name(fi.element) + ".install(" + ops(element_type) + ".copy(" +
                 collection + ".get(" + index + ")))",
             n);
      } else
        line(loop.body,
             name(fi.element) + " = " + (text ? "_rt.text_at(" + collection + ", " + index + ")"
                                                : collection + ".get(" + index + ")"), n);
      available.insert(fi.element);
      loops.push_back({id, code_scope});
      break_targets.push_back(id);
      Suite body;
      statement(fi.body, body);
      auto completed_loop = loops.back();
      loops.pop_back();
      break_targets.pop_back();
      loop_body(std::move(body), loop.body, completed_loop, n);
      Suite cleanup;
      if (!text && !slice) line(cleanup, collection + ".end()", n);
      line(cleanup, "_frame.drop(" + iteration_key + ")", n);
      loop.body.insert(loop.body.begin(),
                       {"_rt._gc_poll()", n.source.span, false, {}, {}});
      guarded({std::move(loop)}, std::move(cleanup), suite, n);
      break;
    }
    case NodeKind::switch_: {
      const bool computed_owning =
          owned(node(n.children[0]).type) &&
          node(n.children[0]).kind() != NodeKind::read;
      auto value = expression(n.children[0], suite);
      if (computed_owning)
        full(id, suite);
      const auto &arms = std::get<Switch>(n.data).arms;
      Suite branches;
      break_targets.push_back(id);
      if (computed_owning)
        held_subjects.push_back(n.children[0]);
      for (auto arm = arms.rbegin(); arm != arms.rend(); ++arm) {
        auto saved_available = available;
        Statement branch{"if " + pattern_test(arm->pattern, value),
                         arm->pattern.source.span,
                         true,
                         {},
                         {}};
        pattern_bind(arm->pattern, value, branch.body);
        if (!computed_owning)
          full(id, branch.body);
        statement(arm->body, branch.body);
        branch.otherwise = std::move(branches);
        branches = {std::move(branch)};
        available = std::move(saved_available);
      }
      break_targets.pop_back();
      if (computed_owning) {
        // The subject result is released by this structured finally after
        // the selected arm's defer/local drops and before enclosing scopes.
        // Panic skips the language cleanup, as in scope_wrap/block; a Panic
        // inside the drop itself also aborts the remaining outer cleanups.
        auto allow = save(suite, "True", n);
        suite.push_back(
            {"try", n.source.span, true, std::move(branches), {}});
        suite.push_back({"except _rt.SwitchBreak as _jump",
                         n.source.span,
                         true,
                         {{"if _jump.target != " + std::to_string(id.value),
                           n.source.span,
                           true,
                           {{"raise", n.source.span, false, {}, {}}},
                           {}}},
                         {}});
        suite.push_back({"except _rt.Panic",
                         n.source.span,
                         true,
                         {{allow + " = False", n.source.span, false, {}, {}},
                          {"raise", n.source.span, false, {}, {}}},
                         {}});
        Suite cleanup;
        line(cleanup,
             "_frame.drop(" + key(OwnerKind::temporary, n.children[0].value) +
                 ")",
             n);
        suite.push_back(
            {"finally",
             n.source.span,
             true,
             {{"if " + allow, n.source.span, true, std::move(cleanup), {}}},
             {}});
        held_subjects.pop_back();
      } else {
        suite.push_back(
            {"try", n.source.span, true, std::move(branches), {}});
        suite.push_back({"except _rt.SwitchBreak as _jump",
                         n.source.span,
                         true,
                         {{"if _jump.target != " + std::to_string(id.value),
                           n.source.span,
                           true,
                           {{"raise", n.source.span, false, {}, {}}},
                           {}}},
                         {}});
      }
      break;
    }
    case NodeKind::return_: {
      auto value =
          n.children.empty() ? "None" : expression(n.children[0], suite);
      if (!n.children.empty())
        value = save(suite, take(n.children[0], value), n);
      full(id, suite);
      line(suite,
           outlining ? "raise _LangReturn(" + std::to_string(current.value) +
                           ", " + value + ")"
                     : "return " + value,
           n);
      break;
    }
    case NodeKind::throw_: {
      auto value = expression(n.children[0], suite);
      value = save(suite, take(n.children[0], value), n);
      full(id, suite);
      line(suite,
           "raise _rt.TypedError(" +
               string_literal(type_key(node(n.children[0]).type, true)) + ", " +
               value + ", " + ops(node(n.children[0]).type) + ")",
           n);
      break;
    }
    case NodeKind::try_: {
      Statement body{"try", n.source.span, true, {}, {}};
      statement(n.children[0], body.body);
      suite.push_back(std::move(body));
      auto error = "_error" + std::to_string(current.value) + "_" +
                   std::to_string(serial++);
      Suite handlers{{"raise", n.source.span, false, {}, {}}};
      const auto &catches = std::get<Try>(n.data).catches;
      for (auto c = catches.rbegin(); c != catches.rend(); ++c) {
        auto saved_available = available;
        Statement branch{"if " + error + ".type_key == " +
                             string_literal(type_key(c->error_type, true)),
                         c->source.span,
                         true,
                         {},
                         std::move(handlers)};
        if (owned(c->error_type)) {
          branch.body.push_back({name(c->binding) + " = _frame.slot(" +
                                     key(OwnerKind::binding, c->binding.value) +
                                     ", " + ops(c->error_type) + ")",
                                 c->source.span,
                                 false,
                                 {},
                                 {}});
          branch.body.push_back(
              {name(c->binding) + ".install(" + error + ".take())",
               c->source.span,
               false,
               {},
               {}});
        } else
          branch.body.push_back({name(c->binding) + " = " + error + ".take()",
                                 c->source.span,
                                 false,
                                 {},
                                 {}});
        available.insert(c->binding);
        statement(c->body, branch.body);
        handlers = {std::move(branch)};
        available = std::move(saved_available);
      }
      suite.push_back({"except _rt.TypedError as " + error,
                       n.source.span,
                       true,
                       std::move(handlers),
                       {}});
      break;
    }
    case NodeKind::break_: {
      auto target = std::get<Break>(n.data).loop;
      if (break_targets.empty() || break_targets.back() != target)
        throw std::logic_error(
            "verified break target is not the enclosing loop/switch");
      if (node(target).kind() == NodeKind::switch_)
        line(suite,
             "raise _rt.SwitchBreak(" + std::to_string(target.value) + ")", n);
      else if (loops.back().code_scope != code_scope) {
        loops.back().crosses_helper = true;
        line(suite,
             "raise _LangLoopJump(" + std::to_string(target.value) + ", False)",
             n);
      } else
        line(suite, "break", n);
      break;
    }
    case NodeKind::continue_: {
      if (loops.empty() || loops.back().id != std::get<Continue>(n.data).loop)
        throw std::logic_error(
            "verified continue target is not the enclosing loop");
      if (loops.back().code_scope != code_scope) {
        loops.back().crosses_helper = true;
        line(suite,
             "raise _LangLoopJump(" + std::to_string(loops.back().id.value) +
                 ", True)",
             n);
      } else
        line(suite, "continue", n);
      break;
    }
    case NodeKind::noop:
      break;
    default:
      throw std::logic_error("non-statement in verified statement position");
    }
  }
  uint64_t printed_lines = 0;
  void print_line(const std::string &text, unsigned indent,
                  std::optional<SourceSpan> source) {
    check_cancelled(stop);
    if (indent > 80)
      throw Rejected{
          "generated Python indentation exceeds the supported profile (80)",
          source, RejectReason::indentation};
    if (checking_only)
      return;
    output.code += std::string(indent * 4, ' ') + text + "\n";
    ++printed_lines;
    if (source)
      output.source_map.push_back({printed_lines, *source});
  }
  // CPython 3.12.15's CO_MAXBLOCKS is 21. The compiler pushes one
  // fblock for a while body, one for a try/except body, two for an except
  // handler body (EXCEPTION_HANDLER and HANDLER_CLEANUP), one for a
  // try/finally body, and one for the exceptional copy of a finally body.
  // A try/except/finally combines those costs. These are the exact
  // structures emitted below, checked without starting Python.
  static constexpr unsigned python_control_block_limit = 21;
  void check_control_depth(unsigned depth,
                           const std::optional<SourceSpan> &source) {
    if (depth > python_control_block_limit)
      throw Rejected{"generated Python control blocks exceed the CPython "
                     "3.12 static limit (21)",
                     source, RejectReason::control_depth};
  }
  void print_if_chain(const Statement &s, unsigned indent,
                      unsigned control_depth, bool as_elif) {
    if (!s.suite || !s.text.starts_with("if "))
      throw std::logic_error("invalid generated if-chain");
    std::string text = s.text;
    if (as_elif)
      text = "elif " + text.substr(3);
    print_line(text + ":", indent, s.source);
    // An `if` does not create a CPython static control block; keep the same
    // depth accounting as the normal print body.
    check_control_depth(control_depth, s.source);
    print(s.body, indent + 1, control_depth);
    if (s.otherwise.empty())
      return;
    if (s.otherwise.size() == 1 && s.otherwise[0].suite &&
        s.otherwise[0].text.starts_with("if "))
      print_if_chain(s.otherwise[0], indent, control_depth, true);
    else {
      print_line("else:", indent, s.source);
      check_control_depth(control_depth, s.source);
      print(s.otherwise, indent + 1, control_depth);
    }
  }
  void print(const Suite &suite, unsigned indent, unsigned control_depth = 0) {
    if (suite.empty())
      print_line("pass", indent, {});
    for (size_t i = 0; i < suite.size(); ++i) {
      const auto &s = suite[i];
      if (s.text == "try") {
        // The emitter writes a try and its except/finally siblings as one
        // contiguous group. Account for the fblocks CPython pushes for that
        // group instead of charging every arm as if it nested all three.
        size_t next = i + 1;
        std::vector<const Statement *> handlers;
        const Statement *finalizer = nullptr;
        while (next < suite.size()) {
          const auto &part = suite[next];
          if (part.text == "finally") {
            finalizer = &part;
            ++next;
            break;
          }
          if (!part.text.starts_with("except "))
            break;
          handlers.push_back(&part);
          ++next;
        }
        const unsigned finally_cost = finalizer ? 1u : 0u;
        const unsigned body_depth =
            control_depth + finally_cost + (handlers.empty() ? 0u : 1u);
        check_control_depth(body_depth, s.source);
        print_line("try:", indent, s.source);
        print(s.body, indent + 1, body_depth);
        // Any handler body has EXCEPTION_HANDLER and HANDLER_CLEANUP active.
        const unsigned handler_depth = control_depth + finally_cost + 2u;
        for (const auto *handler : handlers) {
          check_control_depth(handler_depth, handler->source);
          print_line(handler->text + ":", indent, handler->source);
          print(handler->body, indent + 1, handler_depth);
        }
        if (finalizer) {
          // CPython compiles the finally body on the normal path after
          // FINALLY_TRY is popped, then again with FINALLY_END active for
          // the exceptional path; the latter is the deeper copy.
          const unsigned finally_depth = control_depth + 1u;
          check_control_depth(finally_depth, finalizer->source);
          print_line("finally:", indent, finalizer->source);
          print(finalizer->body, indent + 1, finally_depth);
        }
        i = next - 1;
        continue;
      }
      print_line(s.text + (s.suite ? ":" : ""), indent, s.source);
      if (s.suite) {
        auto depth = control_depth;
        if (s.text.starts_with("def "))
          depth = 0; // Each generated function is a separate Python code object.
        else if (s.text.starts_with("while ") || s.text.starts_with("with "))
          ++depth;
        check_control_depth(depth, s.source);
        print(s.body, indent + 1, depth);
      }
      if (!s.otherwise.empty()) {
        if (s.otherwise.size() == 1 && s.otherwise[0].suite &&
            s.otherwise[0].text.starts_with("if "))
          print_if_chain(s.otherwise[0], indent, control_depth, true);
        else {
          print_line("else:", indent, s.source);
          check_control_depth(control_depth, s.source);
          print(s.otherwise, indent + 1, control_depth);
        }
      }
    }
  }

  Statement function_tree(const HirFunction &f, bool use_outlining) {
    current = f.symbol;
    serial = 0;
    outlining = use_outlining;
    code_scope = ++next_scope;
    expression_depth = 0;
    available.clear();
    for (const auto &p : f.parameters)
      available.insert(p.symbol);
    for (const auto &c : f.captures) {
      capture_locals.insert(c.local);
      named.insert(c.local);
      available.insert(c.local);
    }
    std::vector<std::string> parameters;
    // A lambda with captures receives the environment record row as its
    // first argument. The original immutable function has no environment.
    if (!f.captures.empty())
      parameters.push_back("_env");
    for (const auto &p : f.parameters)
      parameters.push_back(name(p.symbol));
    Statement function{"def " + function_name(f.symbol) + "(" +
                           join(parameters) + ")",
                       f.source.span,
                       true,
                       {},
                       {}};
    // Source function entry is a safe point.  It is deliberately not added
    // to expression-outlining helpers or generated copy/drop snippets.
    line(function.body, "_rt._gc_poll()", node(f.body));
    line(function.body, "_frame = _rt.Frame()", node(f.body));
    if (!f.captures.empty()) {
      // One row holds every capture in creation order. Loading it is a pure
      // borrow of environment storage: the entry never copies a field and
      // never drops one on exit.
      line(function.body, "_row = _env.get(0)", node(f.body));
      for (size_t i = 0; i < f.captures.size(); ++i)
        line(function.body,
             name(f.captures[i].local) + " = _row[" + std::to_string(i) +
                 "]",
             node(f.body));
    }
    for (size_t i = 0; i < f.parameters.size(); ++i) {
      const auto &p = f.parameters[i];
      const auto &pt = type(f.signature).parameters[i];
      if (pt.mode == ParamMode::move && owned(pt.type)) {
        line(function.body,
             "_frame.put(" + key(OwnerKind::binding, p.symbol.value) + ", " +
                 ops(pt.type) + ", " + name(p.symbol) + ")",
             node(f.body));
        line(function.body,
             name(p.symbol) + " = _frame.slot(" +
                 key(OwnerKind::binding, p.symbol.value) + ", " + ops(pt.type) +
                 ")",
             node(f.body));
      }
    }
    statement(f.body, function.body);
    if (use_outlining) {
      Suite original = std::move(function.body);
      function.body.push_back(
          {"try", f.source.span, true, std::move(original), {}});
      function.body.push_back(
          {"except _LangReturn as _returned",
           f.source.span,
           true,
           {{"if _returned.target != " + std::to_string(f.symbol.value),
             f.source.span,
             true,
             {{"raise", f.source.span, false, {}, {}}},
             {}},
            {"return _returned.take()", f.source.span, false, {}, {}}},
           {}});
    }
    return function;
  }

public:
  Emitter(const HirModule &module, std::stop_token token, bool retry = false)
      : m(module), stop(token), fallback(retry) {}
  Output generate(SymbolId entry, const Options &options) {
    check_cancelled(stop);
    if (m.config.backend != Backend::python || m.config.word_bits != 64)
      throw Rejected{
          "Python value emitter requires a Python/64 verified profile", {}};
    auto found = std::find_if(m.functions.begin(), m.functions.end(),
                              [&](const auto &f) { return f.symbol == entry; });
    if (found == m.functions.end())
      throw std::logic_error("entry SymbolId is not a verified function");
    const auto &signature = type(found->signature);
    const auto &result = type(signature.result);
    if (!signature.parameters.empty() ||
        !(result.kind == TypeKind::Unit ||
          (result.kind == TypeKind::SignedInt && result.bits == 32)))
      throw Rejected{
          "Python entry must have no parameters and return Unit or i32",
          found->source.span};
    for (const auto &t : m.ownership.temporaries)
      temporary_ids.insert(t.producer.value);
    // Every lambda capture local is a read-only environment alias. It is
    // registered before owned_bindings so it can never become an Owner slot.
    for (const auto &f : m.functions)
      for (const auto &c : f.captures) {
        capture_locals.insert(c.local);
        named.insert(c.local);
      }
    for (const auto &l : m.ownership.locals)
      if (l.mode == ParamMode::move && owned(l.type) &&
          !l.read_only_capture && !capture_locals.contains(l.symbol))
        owned_bindings.insert(l.symbol);
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      check_signature(f.signature, f.source.span);
      collect_type(type(f.signature).result);
      if (type(f.signature).throws_type) {
        // A declared error is an error position even when the body has no
        // throw/catch for it.  Ordinary host refs remain legal only inside a
        // source nominal's generic arguments.
        type_key(*type(f.signature).throws_type, true);
        collect_type(*type(f.signature).throws_type);
      }
      for (auto p : type(f.signature).parameters)
        collect_type(p.type);
      output.symbols.push_back({f.symbol, function_name(f.symbol)});
      for (size_t i = 0; i < f.parameters.size(); ++i) {
        auto symbol = f.parameters[i].symbol;
        named.insert(symbol);
        if (type(f.signature).parameters[i].mode == ParamMode::inout)
          cells.insert(symbol);
      }
      // The arena also owns non-executable template bodies. Only concrete
      // functions and their body trees contribute types, locals and Cells.
      std::vector<HirNodeId> pending{f.body};
      while (!pending.empty()) {
        check_cancelled(stop);
        auto id = pending.back();
        pending.pop_back();
        const auto &n = node(id);
        collect_type(n.type);
        if (auto fi = std::get_if<ForIn>(&n.data))
          named.insert(fi->element);
        if (auto c = std::get_if<Call>(&n.data))
          check_signature(c->signature, n.source.span);
        if (auto s = std::get_if<Switch>(&n.data))
          for (const auto &arm : s->arms)
            pattern_names(arm.pattern);
        if (auto t = std::get_if<Try>(&n.data))
          for (const auto &c : t->catches)
            named.insert(c.binding);
        if (const auto *b = std::get_if<Binding>(&n.data)) {
          named.insert(b->symbol);
          if (b->writable)
            cells.insert(b->symbol);
        }
        if (n.kind() == NodeKind::unsupported)
          throw Rejected{"Unsupported operation in Python value profile",
                         n.source.span};
        if (const auto *read = std::get_if<ForeignRead>(&n.data);
            read && read->access)
          uses_foreign_access = true;
        pending.insert(pending.end(), n.children.begin(), n.children.end());
      }
    }
    for (auto s : named)
      output.symbols.push_back({s, name(s)});
    Suite program{
        {"# Generated from verified HIR; compiler-dev/1; Python/64",
         {},
         false,
         {},
         {}},
        {"import langrt as _rt", {}, false, {}, {}},
        {"if _rt.RUNTIME_VERSION != '" + std::string(runtime_version) + "'",
         {},
         true,
         {{"raise RuntimeError('incompatible langrt version')",
           {},
           false,
           {},
           {}}},
         {}}};
    program.push_back(
        {"_LANG_BINDING_INFO = " + binding_info(), {}, false, {}, {}});
    if (uses_foreign_access)
      emit_foreign_access_helpers(program);
    std::set<uint64_t> emitted_types;
    for (auto t : value_types)
      emit_type(TypeId{t}, program, emitted_types);
    for (const auto &f : m.functions) {
      auto function = function_tree(f, false);
      if (fallback) {
        // A fresh emitter retries only functions whose unchanged private
        // generation tree exceeds CPython's static limits. Checking emits no
        // text or SourceMap entries; ordinary functions keep their bytes.
        checking_only = true;
        try {
          print(Suite{function}, 0);
          checking_only = false;
        } catch (const Rejected &e) {
          checking_only = false;
          if (e.reason != RejectReason::control_depth &&
              e.reason != RejectReason::indentation)
            throw;
          function = function_tree(f, true);
        }
      }
      program.push_back(std::move(function));
    }
    if (uses_cleanup_guard) {
      // __exit__ receives this scope's actual exception, even when its caller
      // is handling another Panic. Cleanup callbacks cannot escape by return.
      program.insert(program.begin() + 3,
          {"class _LangCleanup", {}, true,
           {{"def __init__(self, cleanup)", {}, true,
             {{"self.cleanup = cleanup", {}, false, {}, {}}}, {}},
            {"def __enter__(self)", {}, true,
             {{"return None", {}, false, {}, {}}}, {}},
            {"def __exit__(self, _type, error, _traceback)", {}, true,
             {{"if not isinstance(error, _rt.Panic)", {}, true,
               {{"self.cleanup()", {}, false, {}, {}}}, {}},
              {"return False", {}, false, {}, {}}}, {}}}, {}});
    }
    if (!adapters.empty())
      program.insert(program.end(), std::make_move_iterator(adapters.begin()),
                     std::make_move_iterator(adapters.end()));
    if (!helpers.empty()) {
      program.push_back(
          {"class _LangReturn(BaseException)",
           {},
           true,
           {{"def __init__(self, target, value)",
             {},
             true,
             {{"self.target = target", {}, false, {}, {}},
              {"self.value = value", {}, false, {}, {}}},
             {}},
            {"def take(self)",
             {},
             true,
             {{"value, self.value = self.value, None", {}, false, {}, {}},
              {"return value", {}, false, {}, {}}},
             {}}},
           {}});
      program.push_back(
          {"class _LangLoopJump(BaseException)",
           {},
           true,
           {{"def __init__(self, target, continuing)",
             {},
             true,
             {{"self.target = target", {}, false, {}, {}},
              {"self.continuing = continuing", {}, false, {}, {}}},
             {}}},
           {}});
      program.insert(program.end(), std::make_move_iterator(helpers.begin()),
                     std::make_move_iterator(helpers.end()));
    }
    if (options.entrypoint)
      program.push_back(
          {"if __name__ == '__main__'",
           {},
           true,
           {{"_rt.run(" + function_name(entry) + ")", {}, false, {}, {}}},
           {}});
    print(program, 0);
    output.runtime = detail::runtime;
    return std::move(output);
  }
};
} // namespace
PhaseResult<Output> emit(const hir::VerifiedHirModule &verified, SymbolId entry,
                         const Options &options, std::stop_token stop) {
  PhaseResult<Output> result;
  try {
    auto checked = hir::check_backend_capabilities(verified.module(), stop);
    const bool supported = checked.success();
    result.state = checked.state;
    result.failure = checked.failure;
    result.diagnostics = std::move(checked.diagnostics);
    if (!supported)
      return result;
    try {
      result.value = Emitter(verified.module(), stop).generate(entry, options);
    } catch (const Rejected &e) {
      if (e.reason != RejectReason::control_depth &&
          e.reason != RejectReason::indentation)
        throw;
      result.value =
          Emitter(verified.module(), stop, true).generate(entry, options);
    }
    result.state = PhaseState::complete;
  } catch (const Cancelled &) {
    result.state = PhaseState::cancelled;
  } catch (const Rejected &e) {
    result.state = PhaseState::failed;
    result.failure = FailureKind::unsupported;
    Diagnostic d;
    d.code = "E_CAPABILITY_UNAVAILABLE";
    d.phase = "codegen";
    d.severity = "error";
    d.message = e.message;
    d.primary = e.source;
    result.diagnostics.push_back(std::move(d));
  } catch (const std::exception &e) {
    result.state = PhaseState::failed;
    result.failure = FailureKind::internal;
    Diagnostic d;
    d.code = "E_IR_INVALID";
    d.phase = "codegen";
    d.severity = "error";
    d.message = e.what();
    result.diagnostics.push_back(std::move(d));
  }
  return result;
}
} // namespace lang::backend::python

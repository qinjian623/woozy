#include "lang/backend/python.hpp"
#include "runtime.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>

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
struct Rejected {
  std::string message;
  std::optional<SourceSpan> source;
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
    HirNodeId id, step;
  };
  std::vector<Loop> loops;
  std::vector<HirNodeId> break_targets;
  std::vector<HirNodeId> active_defers;
  Output output;
  std::set<uint64_t> temporary_ids;
  std::set<SymbolId> owned_bindings;
  std::set<uint64_t> value_types;
  bool owned(TypeId id) const { return classify_value(m, id).needs_drop; }
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
          (!temporaries_only || a.owner.kind == OwnerKind::temporary))
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
      if (l.scope == scope && l.mode == ParamMode::move && owned(l.type))
        locals.push_back(l);
    std::sort(locals.begin(), locals.end(),
              [](auto a, auto b) { return a.order > b.order; });
    for (auto l : locals)
      line(suite,
           "_frame.drop(" + key(OwnerKind::binding, l.symbol.value) + ")", n);
  }
  void scope_wrap(ScopeId scope, Suite body, Suite &suite, const HirNode &n) {
    Suite cleanup;
    local_drops(scope, cleanup, n);
    if (cleanup.empty()) {
      suite.insert(suite.end(), std::make_move_iterator(body.begin()),
                   std::make_move_iterator(body.end()));
      return;
    }
    auto allow = save(suite, "True", n);
    suite.push_back({"try", n.source.span, true, std::move(body), {}});
    suite.push_back({"except _rt.Panic",
                     n.source.span,
                     true,
                     {{allow + " = False", n.source.span, false, {}, {}},
                      {"raise", n.source.span, false, {}, {}}},
                     {}});
    suite.push_back(
        {"finally",
         n.source.span,
         true,
         {{"if " + allow, n.source.span, true, std::move(cleanup), {}}},
         {}});
  }
  void collect_type(TypeId id) {
    if (!value_types.insert(id.value).second)
      return;
    const auto &t = type(id);
    if (t.kind == TypeKind::Vec)
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
    for (auto f : v.fields)
      emit_type(f.type, program, done);
    if (t.kind == TypeKind::Vec)
      emit_type(t.type_args[0], program, done);
    std::string desc = "_rt.ValueType()";
    if (v.kind == ValueKind::vec)
      desc = "_rt.ValueType('vec')";
    if (v.kind == ValueKind::foreign_reference ||
        v.kind == ValueKind::foreign_callable)
      desc = "_rt.ValueType('foreign')";
    if (v.kind == ValueKind::foreign_error)
      desc = "_rt.ValueType('error')";
    if (v.kind == ValueKind::structure) {
      std::vector<std::string> fields;
      for (auto f : v.fields)
        fields.push_back(ops(f.type));
      desc = "_rt.ValueType('struct', " + tuple(fields) + ")";
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
      desc = "_rt.ValueType('enum', variants={" + join(variants) + "})";
    }
    program.push_back({ops(id) + " = " + desc, {}, false, {}, {}});
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
  void check_signature(TypeId id, SourceSpan source) const {
    const auto &t = type(id);
    for (const auto &p : t.parameters)
      if (p.mode == ParamMode::inout && !integer(type(p.type)) &&
          type(p.type).kind != TypeKind::Float &&
          type(p.type).kind != TypeKind::Bool)
        throw Rejected{"Python value profile supports ordinary/move or "
                       "numeric/bool inout parameters",
                       source};
  }
  std::string type_key(TypeId id) const {
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
          key += type_key(t.type_args[i]);
        }
        key += "]";
      }
      return key;
    }
    case TypeKind::Vec:
      return "Vec[" + type_key(t.type_args[0]) + "]";
    case TypeKind::ForeignOpaque: {
      const auto &d = m.resolved->project->foreign_binding(t.binding);
      const auto *binding = std::get_if<modules::BindingType>(&d.detail);
      if (binding == nullptr)
        throw std::logic_error("foreign type declaration is invalid");
      if (binding->kind != "runtime_error")
        throw Rejected{"Python foreign error payload requires a runtime_error declaration",
                       d.source};
      return d.identity;
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
        key += type_key(p.type);
      }
      key += ")->(" + type_key(t.result) + ")";
      if (t.throws_type)
        key += " throws(" + type_key(*t.throws_type) + ")";
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
  std::string pattern_test(const Pattern &p, const std::string &value) const {
    if (!p.variant)
      return "True";
    std::string test = value + "[0] == " + std::to_string(p.variant->value);
    for (size_t i = 0; i < p.payload.size(); ++i)
      if (p.payload[i].variant)
        test += " and (" +
                pattern_test(p.payload[i],
                             value + "[1][" + std::to_string(i) + "]") +
                ")";
    return test;
  }
  void pattern_bind(const Pattern &p, const std::string &value, Suite &suite) {
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
    if (p.binding && owned(p.type))
      suite.push_back({name(*p.binding) + ".install(" + ops(p.type) + ".copy(" +
                           value + "))",
                       p.source.span,
                       false,
                       {},
                       {}});
    for (size_t i = 0; i < p.payload.size(); ++i)
      pattern_bind(p.payload[i], value + "[1][" + std::to_string(i) + "]",
                   suite);
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
    return name(s) +
           ((cells.contains(s) || owned_bindings.contains(s)) ? ".value" : "");
  }
  std::string binary(BinaryOp op, const std::string &a, const std::string &b,
                     TypeId operand) const {
    if (type(operand).kind == TypeKind::Float)
      return "_rt.float_binary('" + operation(op) + "', " + a + ", " + b +
             ", " + std::to_string(type(operand).bits) + ")";
    return "_rt.binary('" + operation(op) + "', " + a + ", " + b + ", " +
           numeric(operand) + ")";
  }
  std::string expression(HirNodeId id, Suite &suite) {
    check_cancelled(stop);
    const auto &n = node(id);
    exits(n);
    switch (n.kind()) {
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
      return save(suite, string_literal(std::get<StringLiteral>(n.data).utf8),
                  n);
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
      return save(suite, function_name(std::get<FunctionValue>(n.data).symbol),
                  n);
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
      auto value = receiver + ".get(" + index + ")";
      if (owned(n.type))
        value = ops(n.type) + ".copy(" + value + ")";
      return value_save(suite, value, id);
    }
    case NodeKind::vec_slice: {
      const auto &slice = std::get<VecSlice>(n.data);
      auto receiver = expression(n.children[0], suite);
      auto start = slice.start ? expression(*slice.start, suite) : "None";
      auto end = slice.end ? expression(*slice.end, suite) : "None";
      return value_save(suite, receiver + ".slice(" + start + ", " + end + ")",
                        id);
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
      return save(
          suite,
          binary(std::get<Binary>(n.data).op, a, b, node(n.children[0]).type),
          n);
    }
    case NodeKind::integer_convert: {
      auto value = expression(n.children[0], suite);
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
                (unit ? "True" : "False") + ")",
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
              foreign_location(n, declaration) + ")",
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
  void block(const HirNode &n, Suite &suite) {
    std::vector<HirNodeId> registrations;
    for (auto id : n.children)
      if (node(id).kind() == NodeKind::defer_)
        registrations.push_back(id);
    bool has_locals = false;
    for (auto l : m.ownership.locals)
      has_locals |=
          l.scope == n.scope && l.mode == ParamMode::move && owned(l.type);
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
    auto allow = save(suite, "True", n);
    Statement body{"try", n.source.span, true, {}, {}};
    size_t ordinal = 0;
    for (auto id : n.children) {
      if (node(id).kind() == NodeKind::defer_) {
        active_defers.push_back(id);
        line(body.body, registered + " = " + std::to_string(++ordinal),
             node(id));
      } else {
        statement(id, body.body);
      }
    }
    exits(n, true);
    suite.push_back(std::move(body));
    suite.push_back({"except _rt.Panic",
                     n.source.span,
                     true,
                     {{allow + " = False", n.source.span, false, {}, {}},
                      {"raise", n.source.span, false, {}, {}}},
                     {}});
    Statement cleanup{"if " + allow, n.source.span, true, {}, {}};
    for (auto it = registrations.rbegin(); it != registrations.rend(); ++it) {
      active_defers.pop_back();
      const auto &d = node(*it);
      Statement branch{"if " + registered + " >= " + std::to_string(ordinal--),
                       d.source.span,
                       true,
                       {},
                       {}};
      statement(d.children[0], branch.body);
      cleanup.body.push_back(std::move(branch));
    }
    local_drops(n.scope, cleanup.body, n);
    // A Panic in a cleanup aborts the remaining siblings naturally; enclosing
    // blocks' Panic handlers also skip their cleanups.
    suite.push_back({"finally", n.source.span, true, {std::move(cleanup)}, {}});
  }
  void statement(HirNodeId id, Suite &suite) {
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
      break;
    }
    case NodeKind::assign: {
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
      auto installed = take(n.children[1], value);
      if (owned(node(n.children[0]).type) &&
          node(n.children[0]).kind() != NodeKind::index_place) {
        // Field is a short-lived place and its old field owns exactly once.
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
      auto condition = expression(n.children[1], loop.body);
      full(n.children[1], loop.body);
      loop.body.push_back({"if not " + condition,
                           n.source.span,
                           true,
                           {{"break", n.source.span, false, {}, {}}},
                           {}});
      loops.push_back({id, n.children[2]});
      break_targets.push_back(id);
      Statement body{"try", n.source.span, true, {}, {}};
      statement(n.children[3], body.body);
      loop.body.push_back(std::move(body));
      loop.body.push_back({"except _rt.LoopContinue as _jump",
                           n.source.span,
                           true,
                           {{"if _jump.target != " + std::to_string(id.value),
                             n.source.span,
                             true,
                             {{"raise", n.source.span, false, {}, {}}},
                             {}}},
                           {}});
      break_targets.pop_back();
      loops.pop_back();
      statement(n.children[2], loop.body);
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
      auto length = save(suite, collection + ".begin()", n);
      auto index = save(suite, "0", n);
      auto allow = save(suite, "True", n);
      Statement loop{
          "while " + index + " < " + length, n.source.span, true, {}, {}};
      auto element_type = type(node(n.children[0]).type).type_args[0];
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
             name(fi.element) + " = " + collection + ".get(" + index + ")", n);
      loops.push_back({id, id});
      break_targets.push_back(id);
      Suite body;
      statement(fi.body, body);
      loop.body.push_back({"try", n.source.span, true, std::move(body), {}});
      loop.body.push_back({"except _rt.LoopContinue as _jump",
                           n.source.span,
                           true,
                           {{"if _jump.target != " + std::to_string(id.value),
                             n.source.span,
                             true,
                             {{"raise", n.source.span, false, {}, {}}},
                             {}}},
                           {}});
      loops.pop_back();
      break_targets.pop_back();
      line(loop.body, index + " += 1", n);
      suite.push_back({"try", n.source.span, true, {std::move(loop)}, {}});
      suite.push_back({"except _rt.Panic",
                       n.source.span,
                       true,
                       {{allow + " = False", n.source.span, false, {}, {}},
                        {"raise", n.source.span, false, {}, {}}},
                       {}});
      Suite cleanup;
      line(cleanup, collection + ".end()", n);
      line(cleanup, "_frame.drop(" + iteration_key + ")", n);
      suite.push_back(
          {"finally",
           n.source.span,
           true,
           {{"if " + allow, n.source.span, true, std::move(cleanup), {}}},
           {}});
      break;
    }
    case NodeKind::switch_: {
      auto value = expression(n.children[0], suite);
      const auto &arms = std::get<Switch>(n.data).arms;
      Suite branches;
      break_targets.push_back(id);
      for (auto arm = arms.rbegin(); arm != arms.rend(); ++arm) {
        Statement branch{"if " + pattern_test(arm->pattern, value),
                         arm->pattern.source.span,
                         true,
                         {},
                         {}};
        pattern_bind(arm->pattern, value, branch.body);
        full(id, branch.body);
        statement(arm->body, branch.body);
        branch.otherwise = std::move(branches);
        branches = {std::move(branch)};
      }
      break_targets.pop_back();
      suite.push_back({"try", n.source.span, true, std::move(branches), {}});
      suite.push_back({"except _rt.SwitchBreak as _jump",
                       n.source.span,
                       true,
                       {{"if _jump.target != " + std::to_string(id.value),
                         n.source.span,
                         true,
                         {{"raise", n.source.span, false, {}, {}}},
                         {}}},
                       {}});
      break;
    }
    case NodeKind::return_: {
      auto value =
          n.children.empty() ? "None" : expression(n.children[0], suite);
      if (!n.children.empty())
        value = save(suite, take(n.children[0], value), n);
      full(id, suite);
      line(suite, "return " + std::string(value), n);
      break;
    }
    case NodeKind::throw_: {
      auto value = expression(n.children[0], suite);
      value = save(suite, take(n.children[0], value), n);
      full(id, suite);
      line(suite,
           "raise _rt.TypedError(" +
               string_literal(type_key(node(n.children[0]).type)) + ", " +
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
        Statement branch{"if " + error + ".type_key == " +
                             string_literal(type_key(c->error_type)),
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
        statement(c->body, branch.body);
        handlers = {std::move(branch)};
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
      line(suite,
           node(target).kind() == NodeKind::switch_
               ? "raise _rt.SwitchBreak(" + std::to_string(target.value) + ")"
               : "break",
           n);
      break;
    }
    case NodeKind::continue_: {
      if (loops.empty() || loops.back().id != std::get<Continue>(n.data).loop)
        throw std::logic_error(
            "verified continue target is not the enclosing loop");
      line(suite,
           "raise _rt.LoopContinue(" + std::to_string(loops.back().id.value) +
               ")",
           n);
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
          source};
    output.code += std::string(indent * 4, ' ') + text + "\n";
    ++printed_lines;
    if (source)
      output.source_map.push_back({printed_lines, *source});
  }
  void print(const Suite &suite, unsigned indent, unsigned control_depth = 0) {
    if (suite.empty())
      print_line("pass", indent, {});
    for (const auto &s : suite) {
      print_line(s.text + (s.suite ? ":" : ""), indent, s.source);
      if (s.suite) {
        auto depth = control_depth;
        if (s.text.starts_with("def "))
          depth =
              0; // Each generated function is a separate Python code object.
        else if (s.text == "try" || s.text == "finally" ||
                 s.text.starts_with("except "))
          depth += 3;
        else if (s.text.starts_with("while "))
          ++depth;
        // CPython 3.12 permits 20 static control blocks. Charge each try arm
        // conservatively for handler/finally bookkeeping and reserve two for
        // transfers during cleanup. This is checked on generated structure,
        // without importing or executing Python in the compiler.
        if (depth > 18)
          throw Rejected{"generated Python control blocks exceed the supported "
                         "profile (18)",
                         s.source};
        print(s.body, indent + 1, depth);
      }
      if (!s.otherwise.empty()) {
        print_line("else:", indent, s.source);
        print(s.otherwise, indent + 1, control_depth);
      }
    }
  }

public:
  Emitter(const HirModule &module, std::stop_token token)
      : m(module), stop(token) {}
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
    for (const auto &l : m.ownership.locals)
      if (l.mode == ParamMode::move && owned(l.type))
        owned_bindings.insert(l.symbol);
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      check_signature(f.signature, f.source.span);
      collect_type(type(f.signature).result);
      if (type(f.signature).throws_type)
        collect_type(*type(f.signature).throws_type);
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
    std::set<uint64_t> emitted_types;
    for (auto t : value_types)
      emit_type(TypeId{t}, program, emitted_types);
    for (const auto &f : m.functions) {
      current = f.symbol;
      serial = 0;
      std::vector<std::string> parameters;
      for (const auto &p : f.parameters)
        parameters.push_back(name(p.symbol));
      Statement function{"def " + function_name(f.symbol) + "(" +
                             join(parameters) + ")",
                         f.source.span,
                         true,
                         {},
                         {}};
      line(function.body, "_frame = _rt.Frame()", node(f.body));
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
                   key(OwnerKind::binding, p.symbol.value) + ", " +
                   ops(pt.type) + ")",
               node(f.body));
        }
      }
      statement(f.body, function.body);
      program.push_back(std::move(function));
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
    result.value = Emitter(verified.module(), stop).generate(entry, options);
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

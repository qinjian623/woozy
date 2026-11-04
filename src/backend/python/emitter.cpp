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
std::string join(const std::vector<std::string> &values) {
  std::string result;
  for (const auto &v : values) {
    if (!result.empty())
      result += ", ";
    result += v;
  }
  return result;
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
  std::set<TypeId> checked_types;
  SymbolId current;
  uint64_t serial = 0;
  struct Loop {
    HirNodeId id, step;
  };
  std::vector<Loop> loops;
  Output output;

  const HirNode &node(HirNodeId id) const { return m.nodes.at(id.value); }
  const Type &type(TypeId id) const { return *m.types.find(id); }
  void capability(TypeId id, std::optional<SourceSpan> source) {
    if (!checked_types.insert(id).second)
      return;
    const auto &t = type(id);
    if (integer(t) || t.kind == TypeKind::Unit || t.kind == TypeKind::Bool ||
        t.kind == TypeKind::Float)
      return;
    if (t.kind != TypeKind::Function)
      throw Rejected{"Python scalar profile does not support this type",
                     source};
    if (t.throws_type)
      throw Rejected{"Python scalar profile does not support throws", source};
    capability(t.result, source);
    for (const auto &p : t.parameters) {
      capability(p.type, source);
      if (p.mode == ParamMode::move ||
          (p.mode == ParamMode::inout && !integer(type(p.type)) &&
           type(p.type).kind != TypeKind::Float &&
           type(p.type).kind != TypeKind::Bool))
        throw Rejected{"Python scalar profile requires ordinary or "
                       "numeric/bool inout parameters",
                       source};
    }
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
  std::string read(SymbolId s) const {
    return name(s) + (cells.contains(s) ? ".value" : "");
  }
  std::string binary(BinaryOp op, const std::string &a, const std::string &b,
                     TypeId operand) const {
    return "_rt.binary('" + operation(op) + "', " + a + ", " + b + ", " +
           numeric(operand) + ")";
  }
  std::string expression(HirNodeId id, Suite &suite) {
    check_cancelled(stop);
    const auto &n = node(id);
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
    case NodeKind::read:
      return save(suite, read(std::get<Read>(n.data).symbol), n);
    case NodeKind::place:
      return save(suite, name(std::get<Place>(n.data).symbol), n);
    case NodeKind::function_value:
      return save(suite, function_name(std::get<FunctionValue>(n.data).symbol),
                  n);
    case NodeKind::unary: {
      auto operand = expression(n.children[0], suite);
      const auto op = std::get<Unary>(n.data).op;
      auto text = op == UnaryOp::negate        ? "neg"
                  : op == UnaryOp::logical_not ? "not"
                                               : "inv";
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
      auto callee = call.kind == CalleeKind::direct
                        ? save(suite, function_name(*call.direct_symbol), n)
                        : expression(*call.callee, suite);
      std::vector<std::string> args(call.args.size());
      // Evaluate the published source-order vector before arranging formals.
      for (const auto &arg : call.args)
        args.at(arg.parameter_index) = expression(arg.expression, suite);
      return save(suite, callee + "(" + join(args) + ")", n);
    }
    default:
      throw std::logic_error("non-expression in verified expression position");
    }
  }
  void statement(HirNodeId id, Suite &suite) {
    check_cancelled(stop);
    const auto &n = node(id);
    switch (n.kind()) {
    case NodeKind::block:
      for (auto child : n.children)
        statement(child, suite);
      break;
    case NodeKind::binding: {
      const auto &b = std::get<Binding>(n.data);
      auto value = expression(n.children[0], suite);
      line(suite,
           name(b.symbol) + " = " +
               (b.writable ? "_rt.Cell(" + value + ")" : value),
           n);
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
      line(suite, place + ".value = " + value, n);
      break;
    }
    case NodeKind::expression_statement:
      (void)expression(n.children[0], suite);
      break;
    case NodeKind::if_: {
      auto condition = expression(n.children[0], suite);
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
      statement(n.children[0], suite);
      Statement loop{"while True", n.source.span, true, {}, {}};
      auto condition = expression(n.children[1], loop.body);
      loop.body.push_back({"if not " + condition,
                           n.source.span,
                           true,
                           {{"break", n.source.span, false, {}, {}}},
                           {}});
      loops.push_back({id, n.children[2]});
      statement(n.children[3], loop.body);
      loops.pop_back();
      statement(n.children[2], loop.body);
      suite.push_back(std::move(loop));
      break;
    }
    case NodeKind::return_: {
      auto value =
          n.children.empty() ? "None" : expression(n.children[0], suite);
      line(suite, "return " + std::string(value), n);
      break;
    }
    case NodeKind::break_:
      if (loops.empty() || loops.back().id != std::get<Break>(n.data).loop)
        throw std::logic_error(
            "verified break target is not the enclosing loop");
      line(suite, "break", n);
      break;
    case NodeKind::continue_: {
      if (loops.empty() || loops.back().id != std::get<Continue>(n.data).loop)
        throw std::logic_error(
            "verified continue target is not the enclosing loop");
      auto step = loops.back().step;
      statement(step, suite);
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
    output.code += std::string(indent * 4, ' ') + text + "\n";
    ++printed_lines;
    if (source)
      output.source_map.push_back({printed_lines, *source});
  }
  void print(const Suite &suite, unsigned indent) {
    if (suite.empty())
      print_line("pass", indent, {});
    for (const auto &s : suite) {
      print_line(s.text + (s.suite ? ":" : ""), indent, s.source);
      if (s.suite)
        print(s.body, indent + 1);
      if (!s.otherwise.empty()) {
        print_line("else:", indent, s.source);
        print(s.otherwise, indent + 1);
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
          "Python scalar emitter requires a Python/64 verified profile", {}};
    auto found = std::find_if(m.functions.begin(), m.functions.end(),
                              [&](const auto &f) { return f.symbol == entry; });
    if (found == m.functions.end())
      throw std::logic_error("entry SymbolId is not a verified function");
    const auto &signature = type(found->signature);
    const auto &result = type(signature.result);
    if (!signature.parameters.empty() || signature.throws_type ||
        !(result.kind == TypeKind::Unit ||
          (result.kind == TypeKind::SignedInt && result.bits == 32)))
      throw Rejected{
          "Python entry must have no parameters and return Unit or i32",
          found->source.span};
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      capability(f.signature, f.source.span);
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
        capability(n.type, n.source.span);
        if (const auto *b = std::get_if<Binding>(&n.data)) {
          named.insert(b->symbol);
          if (b->writable)
            cells.insert(b->symbol);
        }
        if (n.kind() == NodeKind::unsupported ||
            n.kind() == NodeKind::string_literal)
          throw Rejected{"Unsupported operation in Python scalar profile",
                         n.source.span};
        pending.insert(pending.end(), n.children.begin(), n.children.end());
      }
    }
    for (auto s : named)
      output.symbols.push_back({s, name(s)});
    Suite program{{"# Generated from verified HIR; compiler-dev/1; Python/64",
                   {},
                   false,
                   {},
                   {}},
                  {"import langrt as _rt", {}, false, {}, {}}};
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

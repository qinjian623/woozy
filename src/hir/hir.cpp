#include "lang/hir/hir.hpp"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace lang::hir {
namespace {
bool integer(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::UnsignedInt ||
         k == TypeKind::Isize || k == TypeKind::Usize;
}
bool signed_integer(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::Isize;
}
bool number(TypeKind k) { return integer(k) || k == TypeKind::Float; }
bool inout_scalar(TypeKind k) { return number(k) || k == TypeKind::Bool; }
bool mode_valid(ParamMode m) {
  return m == ParamMode::ordinary || m == ParamMode::inout ||
         m == ParamMode::move;
}
bool nominal(TypeKind k) {
  return k == TypeKind::NominalStruct || k == TypeKind::NominalEnum ||
         k == TypeKind::TypeParameter;
}
} // namespace
const Type *TypeContext::find(TypeId id) const {
  return id.value && id.value <= entries_.size() ? &entries_[id.value - 1]
                                                 : nullptr;
}
TypeId TypeContext::intern(Type t) {
  auto bad = [] {
    throw std::invalid_argument("invalid canonical type shape/reference");
  };
  switch (t.kind) {
  case TypeKind::SignedInt:
  case TypeKind::UnsignedInt:
    if (t.bits != 8 && t.bits != 16 && t.bits != 32 && t.bits != 64)
      bad();
    break;
  case TypeKind::Float:
    if (t.bits != 32 && t.bits != 64)
      bad();
    break;
  case TypeKind::Function:
    if (!find(t.result) || (t.throws_type && !find(*t.throws_type)))
      bad();
    for (auto p : t.parameters)
      if (!find(p.type) || !mode_valid(p.mode))
        bad();
    break;
  case TypeKind::Unit:
  case TypeKind::Bool:
  case TypeKind::String:
  case TypeKind::Isize:
  case TypeKind::Usize:
  case TypeKind::NominalStruct:
  case TypeKind::NominalEnum:
  case TypeKind::TypeParameter:
  case TypeKind::ForeignOpaque:
  case TypeKind::Error:
    break;
  default:
    bad();
  }
  if (t.kind != TypeKind::Function &&
      (!t.parameters.empty() || t.result.value || t.throws_type))
    bad();
  if (t.kind != TypeKind::SignedInt && t.kind != TypeKind::UnsignedInt &&
      t.kind != TypeKind::Float && t.bits)
    bad();
  if (!nominal(t.kind) && t.kind != TypeKind::ForeignOpaque &&
      (t.declaration.value || t.binding.value))
    bad();
  if (nominal(t.kind) && t.binding.value)
    bad();
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i] == t)
      return TypeId{i + 1};
  entries_.push_back(std::move(t));
  return TypeId{entries_.size()};
}
unsigned bit_width(const Type &t, const Config &c) {
  if (c.word_bits != 32 && c.word_bits != 64)
    throw std::invalid_argument("target word_bits");
  if (t.kind == TypeKind::Isize || t.kind == TypeKind::Usize)
    return c.word_bits;
  if (integer(t.kind) || t.kind == TypeKind::Float)
    return t.bits;
  throw std::invalid_argument("type has no numeric width");
}
namespace {
struct Invalid {
  std::string message;
  std::optional<SourceSpan> span;
};
class Verifier {
  const HirModule &m;
  std::stop_token stop;
  std::map<uint64_t, const HirFunction *> functions;
  struct Local {
    TypeId type;
    ScopeId scope;
    SymbolId function;
    bool writable;
    size_t ready;
  };
  std::map<uint64_t, Local> locals;
  std::vector<size_t> enter, leave;
  std::vector<std::optional<SymbolId>> owners;
  std::vector<bool> scope_used;
  size_t clock = 0;
  [[noreturn]] void fail(std::string message,
                         std::optional<SourceSpan> span = {}) const {
    throw Invalid{std::move(message), span};
  }
  void need(bool b, std::string_view message) const {
    if (!b)
      fail(std::string(message));
  }
  const Type &type(TypeId id) const {
    auto t = m.types.find(id);
    if (!t)
      fail("missing or out-of-range TypeId");
    return *t;
  }
  const HirNode &node(HirNodeId id) const {
    if (id.value >= m.nodes.size())
      fail("out-of-range HirNodeId");
    return m.nodes[id.value];
  }
  const Scope &scope(ScopeId id) const {
    if (id.value >= m.scopes.size())
      fail("out-of-range ScopeId");
    return m.scopes[id.value];
  }
  const modules::Symbol &symbol(SymbolId id) const {
    if (id.value >= m.resolved->symbols.size())
      fail("out-of-range SymbolId");
    const auto &s = m.resolved->symbols[id.value];
    need(s.id == id, "symbol index/identity mismatch");
    need(s.module.value < m.resolved->project->modules.size(),
         "symbol module index");
    return s;
  }
  void source(const Source &s) const {
    const auto &p = *m.resolved->project;
    need(s.node.module.value < p.modules.size(), "source module index");
    const auto &mod = p.modules[s.node.module.value];
    need(mod.id == s.node.module, "module index/identity mismatch");
    need(s.node.file < mod.files.size() && mod.files[s.node.file],
         "source file index");
    const auto &file = *mod.files[s.node.file];
    need(!file.recovered, "owning AST contains syntax recovery");
    need(file.source && file.source->valid(),
         "source snapshot missing/invalid");
    need(s.node.node.value < file.nodes.size(), "source AST node index");
    const auto &ast = file.nodes[s.node.node.value];
    need(ast.span.begin_byte <= ast.span.end_byte &&
             ast.span.end_byte <= file.source->original.size(),
         "invalid AST source span");
    need(!ast.recovered && ast.kind != AstKind::Error &&
             ast.kind != AstKind::NeutralBracket &&
             ast.kind != AstKind::BracketItem,
         "unresolved/recovered AST source");
    need(s.span.snapshot == file.source->id &&
             s.span.snapshot == ast.span.snapshot &&
             s.span.begin_byte <= s.span.end_byte &&
             s.span.end_byte <= file.source->original.size() &&
             s.span.begin_byte >= ast.span.begin_byte &&
             s.span.end_byte <= ast.span.end_byte,
         "invalid source span");
  }
  bool ancestor(ScopeId a, ScopeId b) const {
    for (size_t count = 0; count <= m.scopes.size(); ++count) {
      if (a == b)
        return true;
      auto parent = scope(b).parent;
      if (!parent)
        return false;
      b = *parent;
    }
    fail("scope cycle");
  }
  void unit(const HirNode &n) const {
    need(type(n.type).kind == TypeKind::Unit, "statement must have Unit type");
  }
  bool expression(const HirNode &n) const {
    return n.kind() <= NodeKind::call && n.kind() != NodeKind::place &&
           n.kind() != NodeKind::binding && n.kind() != NodeKind::assign;
  }
  const HirNode &expr(HirNodeId id) const {
    const auto &n = node(id);
    need(expression(n), "expected value expression");
    return n;
  }
  bool statement(const HirNode &n) const {
    return !expression(n) && n.kind() != NodeKind::place;
  }
  const HirFunction &function(SymbolId s) const {
    symbol(s);
    auto it = functions.find(s.value);
    if (it == functions.end())
      fail("function identity has no HIR definition");
    return *it->second;
  }
  void scalar(TypeId id) const {
    const auto &t = type(id);
    need(t.kind <= TypeKind::Function,
         "unsupported/error/unresolved type in scalar HIR");
    if (t.kind == TypeKind::Function) {
      need(!t.throws_type, "throws execution is unsupported");
      // Dependencies precede each canonical function type; no recursive type
      // graph.
      for (auto p : t.parameters) {
        need(p.mode == ParamMode::ordinary ||
                 (p.mode == ParamMode::inout &&
                  inout_scalar(type(p.type).kind)),
             "move or nontrivial inout execution is unsupported");
        need(p.type.value < id.value, "invalid function parameter type edge");
      }
      need(t.result.value < id.value, "invalid function result type edge");
    }
  }
  void operator_types(BinaryOp op, TypeId a, TypeId b, TypeId result) const {
    auto k = type(a).kind;
    switch (op) {
    case BinaryOp::shift_left:
    case BinaryOp::shift_right:
      need(integer(k) && integer(type(b).kind) && result == a, "shift types");
      break;
    case BinaryOp::add:
    case BinaryOp::subtract:
    case BinaryOp::multiply:
    case BinaryOp::divide:
      need(number(k) && a == b && result == a, "arithmetic types");
      break;
    case BinaryOp::remainder:
    case BinaryOp::bit_and:
    case BinaryOp::bit_or:
    case BinaryOp::bit_xor:
      need(integer(k) && a == b && result == a, "integer operation types");
      break;
    case BinaryOp::equal:
    case BinaryOp::not_equal:
      need((number(k) || k == TypeKind::Bool) && a == b &&
               type(result).kind == TypeKind::Bool,
           "equality types");
      break;
    case BinaryOp::less:
    case BinaryOp::less_equal:
    case BinaryOp::greater:
    case BinaryOp::greater_equal:
      need(number(k) && a == b && type(result).kind == TypeKind::Bool,
           "comparison types");
      break;
    default:
      fail("unknown binary operator");
    }
  }
  void integer_literal(const HirNode &n, const IntegerLiteral &l) const {
    const auto &t = type(n.type);
    need(integer(t.kind), "integer literal type");
    const auto &s = l.decimal;
    need(!s.empty(), "empty integer mathematical value");
    bool neg = s[0] == '-';
    size_t begin = neg ? 1 : 0;
    need(begin < s.size() &&
             !(s[begin] == '0' && (neg || s.size() - begin != 1)),
         "noncanonical decimal integer");
    uint64_t magnitude = 0;
    for (size_t i = begin; i < s.size(); ++i) {
      check_cancelled(stop);
      need(s[i] >= '0' && s[i] <= '9', "nondecimal integer value");
      unsigned d = unsigned(s[i] - '0');
      need(magnitude <= (std::numeric_limits<uint64_t>::max() - d) / 10,
           "integer literal exceeds u64 magnitude");
      magnitude = magnitude * 10 + d;
    }
    unsigned w = bit_width(t, m.config);
    uint64_t mask = w == 64 ? ~uint64_t{0} : (uint64_t{1} << w) - 1;
    uint64_t limit = signed_integer(t.kind) ? (uint64_t{1} << (w - 1)) : mask;
    need(!neg || signed_integer(t.kind), "negative unsigned literal");
    need(magnitude <= (signed_integer(t.kind) && !neg ? limit - 1 : limit),
         "integer literal range");
    need(l.bits == ((neg ? uint64_t{0} - magnitude : magnitude) & mask),
         "integer bit pattern mismatch");
  }
  void check_call(const HirNode &n, const Call &c) const {
    const auto &sig = type(c.signature);
    need(sig.kind == TypeKind::Function, "call signature kind");
    need(n.type == sig.result, "call result mismatch");
    const HirFunction *direct = nullptr;
    size_t offset = 0;
    if (c.kind == CalleeKind::direct) {
      need(c.direct_symbol.has_value() && !c.callee, "direct callee metadata");
      direct = &function(*c.direct_symbol);
      need(direct->signature == c.signature, "direct signature mismatch");
    } else if (c.kind == CalleeKind::function_value) {
      need(!c.direct_symbol && c.callee.has_value(),
           "function value callee metadata");
      need(expr(*c.callee).type == c.signature,
           "function value signature mismatch");
      offset = 1;
    } else
      fail("unsupported callee kind");
    need(c.args.size() == sig.parameters.size() &&
             n.children.size() == c.args.size() + offset,
         "call arity/children");
    if (offset)
      need(n.children[0] == *c.callee, "callee must be evaluated first");
    std::vector<bool> seen(sig.parameters.size());
    std::set<uint64_t> borrowed;
    bool named = false;
    for (size_t i = 0; i < c.args.size(); ++i) {
      check_cancelled(stop);
      const auto &a = c.args[i];
      need(a.source_index == i && a.parameter_index < seen.size(),
           "call source/parameter index");
      need(!seen[a.parameter_index], "duplicate call parameter");
      seen[a.parameter_index] = true;
      need(n.children[i + offset] == a.expression,
           "call children must preserve source order");
      auto p = sig.parameters[a.parameter_index];
      const auto &argument = node(a.expression);
      need(a.mode == p.mode && argument.type == p.type,
           "call argument mode/type");
      if (p.mode == ParamMode::inout) {
        need(argument.kind() == NodeKind::place &&
                 inout_scalar(type(p.type).kind),
             "inout requires a whole trivial scalar place");
        need(borrowed.insert(std::get<Place>(argument.data).symbol.value).second,
             "duplicate inout storage in call");
      } else {
        need(p.mode == ParamMode::ordinary, "unsupported argument mode");
        expr(a.expression);
      }
      if (a.label) {
        named = true;
        need(direct && !a.label->empty() &&
                 *a.label == direct->parameters[a.parameter_index].label,
             "call label mismatch or label on function value");
      } else {
        need(!named && a.parameter_index == i,
             "positional argument index/order");
      }
    }
    // Whole local scalar bindings cannot alias each other. An inout parameter
    // is also unique because every caller is checked by this same rule. Scan
    // nested argument expressions too: f(inout x, g(x)) must not read x while
    // f's exclusive access is active, and g(inout x) must not borrow it again.
    for (const auto &a : c.args) {
      if (a.mode == ParamMode::inout)
        continue;
      std::vector<HirNodeId> pending{a.expression};
      std::set<uint64_t> visited;
      while (!pending.empty()) {
        check_cancelled(stop);
        auto id = pending.back();
        pending.pop_back();
        need(visited.insert(id.value).second,
             "cycle/shared child in call argument");
        const auto &part = node(id);
        if (part.kind() == NodeKind::read || part.kind() == NodeKind::place) {
          auto storage = part.kind() == NodeKind::read
                             ? std::get<Read>(part.data).symbol
                             : std::get<Place>(part.data).symbol;
          need(!borrowed.contains(storage.value),
               "inout storage is also accessed by another argument");
        }
        pending.insert(pending.end(), part.children.begin(), part.children.end());
      }
    }
  }
  void shape(HirNodeId id) const {
    check_cancelled(stop);
    const auto &n = node(id);
    need(!n.data.valueless_by_exception(), "valueless HIR payload");
    source(n.source);
    scalar(n.type);
    const auto &ch = n.children;
    auto arity = [&](size_t count) {
      need(ch.size() == count, "node child count");
    };
    switch (n.kind()) {
    case NodeKind::unit_literal:
      arity(0);
      unit(n);
      break;
    case NodeKind::bool_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::Bool, "bool literal type");
      break;
    case NodeKind::integer_literal:
      arity(0);
      integer_literal(n, std::get<IntegerLiteral>(n.data));
      break;
    case NodeKind::float_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::Float, "float literal type");
      need(type(n.type).bits != 32 ||
               std::get<FloatLiteral>(n.data).bits <= UINT32_MAX,
           "f32 literal bit pattern");
      break;
    case NodeKind::string_literal:
      arity(0);
      need(type(n.type).kind == TypeKind::String, "string literal type");
      try {
        validate_utf8(std::get<StringLiteral>(n.data).utf8);
      } catch (const SourceError &) {
        fail("invalid UTF-8 string");
      }
      break;
    case NodeKind::read:
    case NodeKind::place:
      arity(0);
      break; // binding checks after tree traversal
    case NodeKind::function_value: {
      arity(0);
      const auto &f = function(std::get<FunctionValue>(n.data).symbol);
      need(n.type == f.signature, "function value type mismatch");
      break;
    }
    case NodeKind::binding: {
      arity(1);
      unit(n);
      const auto &s = symbol(std::get<Binding>(n.data).symbol);
      need(s.kind == modules::SymbolKind::value ||
               s.kind == modules::SymbolKind::constant,
           "binding symbol kind");
      expr(ch[0]);
      break;
    }
    case NodeKind::assign: {
      arity(2);
      unit(n);
      const auto &lhs = node(ch[0]);
      const auto &rhs = expr(ch[1]);
      need(lhs.kind() == NodeKind::place, "assignment requires writable place");
      auto op = std::get<Assign>(n.data).compound;
      if (op) {
        need(*op <= BinaryOp::bit_xor, "invalid compound assignment operator");
        operator_types(*op, lhs.type, rhs.type, lhs.type);
      } else
        need(lhs.type == rhs.type, "assignment type mismatch");
      break;
    }
    case NodeKind::unary: {
      arity(1);
      const auto &a = expr(ch[0]);
      auto k = type(a.type).kind;
      switch (std::get<Unary>(n.data).op) {
      case UnaryOp::negate:
        need(number(k) && n.type == a.type, "negation types");
        break;
      case UnaryOp::logical_not:
        need(k == TypeKind::Bool && n.type == a.type, "logical not types");
        break;
      case UnaryOp::bit_not:
        need(integer(k) && n.type == a.type, "bitwise not types");
        break;
      default:
        fail("unknown unary operator");
      }
      break;
    }
    case NodeKind::binary:
      arity(2);
      operator_types(std::get<Binary>(n.data).op, expr(ch[0]).type,
                     expr(ch[1]).type, n.type);
      break;
    case NodeKind::short_circuit: {
      arity(2);
      auto op = std::get<ShortCircuit>(n.data).op;
      need(op == LogicalOp::and_ || op == LogicalOp::or_,
           "unknown short circuit operator");
      need(type(n.type).kind == TypeKind::Bool && expr(ch[0]).type == n.type &&
               expr(ch[1]).type == n.type,
           "short circuit types");
      break;
    }
    case NodeKind::integer_convert:
      arity(1);
      need(integer(type(n.type).kind) && integer(type(expr(ch[0]).type).kind),
           "integer conversion types");
      break;
    case NodeKind::call:
      check_call(n, std::get<Call>(n.data));
      break;
    case NodeKind::block:
      unit(n);
      for (auto c : ch)
        need(statement(node(c)), "block child must be a statement");
      break;
    case NodeKind::expression_statement:
      arity(1);
      unit(n);
      expr(ch[0]);
      break;
    case NodeKind::if_:
      need(ch.size() == 2 || ch.size() == 3, "if child count");
      unit(n);
      need(type(expr(ch[0]).type).kind == TypeKind::Bool &&
               node(ch[1]).kind() == NodeKind::block &&
               (ch.size() == 2 || node(ch[2]).kind() == NodeKind::block),
           "if condition/branch shape");
      break;
    case NodeKind::for_: {
      arity(4);
      unit(n);
      auto init = node(ch[0]).kind(), step = node(ch[2]).kind();
      need((init == NodeKind::binding || init == NodeKind::assign ||
            init == NodeKind::expression_statement ||
            init == NodeKind::noop) &&
               (step == NodeKind::assign ||
                step == NodeKind::expression_statement ||
                step == NodeKind::noop) &&
               type(expr(ch[1]).type).kind == TypeKind::Bool &&
               node(ch[3]).kind() == NodeKind::block,
           "for children shape");
      break;
    }
    case NodeKind::noop:
    case NodeKind::break_:
    case NodeKind::continue_:
      arity(0);
      unit(n);
      break;
    case NodeKind::return_:
      need(ch.size() <= 1, "return child count");
      unit(n);
      if (!ch.empty())
        expr(ch[0]);
      break;
    case NodeKind::unsupported:
      fail("unsupported HIR node: " + std::get<Unsupported>(n.data).feature,
           n.source.span);
    }
  }
  static bool same_ref(const modules::NodeRef &a, const modules::NodeRef &b) {
    return a.module == b.module && a.file == b.file && a.node == b.node;
  }
  void declaration(SymbolId id, const Source &src, modules::SymbolKind kind,
                   AstKind ast_kind) const {
    const auto &s = symbol(id);
    source(src);
    need(s.kind == kind && s.definition && same_ref(*s.definition, src.node),
         "declaration source/symbol kind mismatch");
    const auto &file = *m.resolved->project->modules[src.node.module.value]
                            .files[src.node.file];
    need(file.nodes[src.node.node.value].kind == ast_kind,
         "declaration AST kind mismatch");
  }
  void tree(const HirFunction &f) {
    struct Frame {
      HirNodeId id;
      size_t next;
    };
    std::vector<Frame> stack;
    std::vector<HirNodeId> loops;
    auto push = [&](HirNodeId id, std::optional<HirNodeId> parent) {
      check_cancelled(stop);
      const auto &n = node(id);
      need(!owners[id.value], "HIR node cycle or shared executable child");
      owners[id.value] = f.symbol;
      enter[id.value] = ++clock;
      need(n.source.node.module == f.source.node.module &&
               n.source.node.file == f.source.node.file &&
               n.source.span.begin_byte >= f.source.span.begin_byte &&
               n.source.span.end_byte <= f.source.span.end_byte,
           "node source belongs to another function");
      const auto &s = scope(n.scope);
      need(s.function == f.symbol, "node scope belongs to another function");
      if (!parent) {
        need(n.kind() == NodeKind::block && n.scope == f.scope && !s.parent,
             "function root block/scope");
      } else {
        const auto &p = node(*parent);
        if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_)
          need(s.parent && *s.parent == p.scope,
               "nested scope must have structural parent");
        else
          need(n.scope == p.scope,
               "node scope differs from enclosing construct");
        if (n.kind() == NodeKind::place) {
          bool allowed = p.kind() == NodeKind::assign && p.children[0] == id;
          if (p.kind() == NodeKind::call)
            for (const auto &a : std::get<Call>(p.data).args)
              allowed = allowed ||
                        (a.mode == ParamMode::inout && a.expression == id);
          need(allowed, "place used outside assignment or inout argument");
        }
      }
      if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_) {
        need(!scope_used[n.scope.value],
             "scope has multiple owning constructs");
        scope_used[n.scope.value] = true;
      }
      if (n.kind() == NodeKind::for_)
        loops.push_back(id);
      if (n.kind() == NodeKind::return_) {
        need(std::get<Return>(n.data).function == f.symbol,
             "return target function mismatch");
        const auto &sig = type(f.signature);
        need(n.children.empty() ? type(sig.result).kind == TypeKind::Unit
                                : expr(n.children[0]).type == sig.result,
             "function return type mismatch");
      }
      if (n.kind() == NodeKind::break_ || n.kind() == NodeKind::continue_) {
        auto target = n.kind() == NodeKind::break_
                          ? std::get<Break>(n.data).loop
                          : std::get<Continue>(n.data).loop;
        need(node(target).kind() == NodeKind::for_ && !loops.empty() &&
                 loops.back() == target,
             "jump target must be nearest enclosing loop in same function");
        need(scope(node(target).scope).function == f.symbol &&
                 ancestor(node(target).scope, n.scope),
             "jump function/scope mismatch");
      }
      stack.push_back({id, 0});
    };
    push(f.body, {});
    while (!stack.empty()) {
      check_cancelled(stop);
      auto &frame = stack.back();
      const auto &n = node(frame.id);
      if (frame.next < n.children.size()) {
        auto child = n.children[frame.next++];
        auto parent = frame.id;
        push(child, parent);
        continue;
      }
      leave[frame.id.value] = ++clock;
      if (n.kind() == NodeKind::binding) {
        auto b = std::get<Binding>(n.data);
        const auto &s = symbol(b.symbol);
        declaration(b.symbol, n.source, s.kind, AstKind::Binding);
        const auto &file = *m.resolved->project->modules[n.source.node.module.value]
                                .files[n.source.node.file];
        const auto &ast = file.node(n.source.node.node);
        need(ast.token_begin < file.tokens.size(), "binding token range");
        auto keyword = file.tokens[ast.token_begin].spelling;
        need(keyword == "let" || keyword == "var" || keyword == "const",
             "local binding keyword");
        need(!b.writable || keyword == "var", "non-var binding marked writable");
        need(s.kind != modules::SymbolKind::constant || !b.writable,
             "constant marked writable");
        need(locals
                 .emplace(b.symbol.value,
                          Local{node(n.children[0]).type, n.scope, f.symbol,
                                b.writable, leave[frame.id.value]})
                 .second,
             "duplicate local/parameter identity");
      }
      if (n.kind() == NodeKind::for_)
        loops.pop_back();
      stack.pop_back();
    }
  }
  void uses_and_flow() const {
    std::vector<bool> falls(m.nodes.size(), true);
    std::vector<std::set<uint64_t>> reachable_breaks(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      if (n.kind() != NodeKind::read && n.kind() != NodeKind::place)
        continue;
      auto s = n.kind() == NodeKind::read ? std::get<Read>(n.data).symbol
                                          : std::get<Place>(n.data).symbol;
      symbol(s);
      auto it = locals.find(s.value);
      need(it != locals.end(), "read/place has no typed binding");
      const auto &l = it->second;
      need(owners[i] && *owners[i] == l.function &&
               ancestor(l.scope, n.scope) && l.ready < enter[i],
           "binding out of scope or used before initialization");
      need(n.type == l.type && (n.kind() != NodeKind::place || l.writable),
           "binding type/writability mismatch");
    }
    // Postorder without recursive C++ calls. The traversal already rejected
    // DAGs/cycles.
    std::vector<size_t> order(m.nodes.size());
    for (size_t i = 0; i < order.size(); ++i)
      order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return leave[a] < leave[b]; });
    for (auto i : order) {
      check_cancelled(stop);
      const auto &n = m.nodes[i];
      switch (n.kind()) {
      case NodeKind::break_:
        reachable_breaks[i].insert(std::get<Break>(n.data).loop.value);
        falls[i] = false;
        break;
      case NodeKind::return_:
      case NodeKind::continue_:
        falls[i] = false;
        break;
      case NodeKind::block:
        for (auto c : n.children) {
          if (!falls[i])
            break;
          const auto &exits = reachable_breaks[c.value];
          reachable_breaks[i].insert(exits.begin(), exits.end());
          falls[i] = falls[c.value];
        }
        break;
      case NodeKind::if_: {
        size_t first = 1, end = n.children.size();
        const auto &condition = node(n.children[0]);
        if (condition.kind() == NodeKind::bool_literal) {
          first = std::get<BoolLiteral>(condition.data).value ? 1 : 2;
          end = std::min(first + 1, n.children.size());
          falls[i] = first == end || falls[n.children[first].value];
        } else
          falls[i] = falls[n.children[1].value] || n.children.size() == 2 ||
                     falls[n.children[2].value];
        for (size_t branch = first; branch < end; ++branch) {
          const auto &exits = reachable_breaks[n.children[branch].value];
          reachable_breaks[i].insert(exits.begin(), exits.end());
        }
        break;
      }
      case NodeKind::for_: {
        const auto &cond = node(n.children[1]);
        falls[i] = !(cond.kind() == NodeKind::bool_literal &&
                     std::get<BoolLiteral>(cond.data).value &&
                     !reachable_breaks[n.children[3].value].contains(i));
        // Breaks from this body terminate this For. They cannot leave an
        // enclosing loop because tree() requires the nearest loop target.
        break;
      }
      default:
        break;
      }
    }
    for (const auto &f : m.functions)
      need(type(type(f.signature).result).kind == TypeKind::Unit ||
               !falls[f.body.value],
           "non-Unit function has a normal exit without return");
  }

public:
  Verifier(const HirModule &module, std::stop_token token)
      : m(module), stop(token), enter(m.nodes.size()), leave(m.nodes.size()),
        owners(m.nodes.size()), scope_used(m.scopes.size()) {}
  void run() {
    check_cancelled(stop);
    need(m.config.backend == Backend::native ||
             m.config.backend == Backend::python,
         "invalid backend");
    need(m.config.word_bits == 32 || m.config.word_bits == 64,
         "invalid target word width");
    need(m.resolved && m.resolved->project,
         "missing owning ResolvedProject/Project");
    const auto &project = *m.resolved->project;
    need(project.word_bits == m.config.word_bits &&
             project.backend ==
                 (m.config.backend == Backend::native ? "native" : "python"),
         "HIR target differs from project target");
    need(m.identity.value < project.modules.size() &&
             project.modules[m.identity.value].id == m.identity,
         "module identity index");
    for (size_t i = 1; i <= m.types.size(); ++i) {
      check_cancelled(stop);
      scalar(TypeId{i});
    }
    for (const auto &f : m.functions) {
      check_cancelled(stop);
      source(f.source);
      auto ast_kind = project.modules[f.source.node.module.value]
                          .files[f.source.node.file]->node(f.source.node.node).kind;
      need(ast_kind == AstKind::Function || ast_kind == AstKind::FunctionLiteral,
           "function source is not a declaration or literal");
      declaration(f.symbol, f.source, modules::SymbolKind::function,
                  ast_kind);
      need(functions.emplace(f.symbol.value, &f).second,
           "duplicate function identity");
      const auto &sig = type(f.signature);
      need(sig.kind == TypeKind::Function, "function signature kind");
      need(sig.parameters.size() == f.parameters.size(),
           "function parameter count");
      need(scope(f.scope).function == f.symbol && !scope(f.scope).parent,
           "function scope ownership");
      std::set<std::string> labels;
      for (size_t i = 0; i < f.parameters.size(); ++i) {
        const auto &p = f.parameters[i];
        const auto &s = symbol(p.symbol);
        need(s.kind == modules::SymbolKind::value && s.definition &&
                 s.declaration,
             "parameter symbol kind/source");
        Source src{*s.definition, *s.declaration};
        declaration(p.symbol, src, modules::SymbolKind::value,
                    AstKind::Parameter);
        const auto &file = *project.modules[src.node.module.value]
                                .files[src.node.file];
        const auto &ast = file.node(src.node.node);
        need(ast.token_begin + 1 < file.tokens.size(), "parameter token range");
        auto mode_token = file.tokens[ast.token_begin + 1].spelling;
        auto declared_mode = mode_token == "inout" ? ParamMode::inout
                             : mode_token == "move" ? ParamMode::move
                                                    : ParamMode::ordinary;
        need(sig.parameters[i].mode == declared_mode,
             "parameter mode differs from source declaration");
        need(src.node.module == f.source.node.module &&
                 src.node.file == f.source.node.file &&
                 src.span.begin_byte >= f.source.span.begin_byte &&
                 src.span.end_byte <= f.source.span.end_byte,
             "parameter belongs to another function");
        need((s.name == "_" && p.label.empty()) ||
                 (s.name != "_" && !p.label.empty() && p.label == s.name &&
                  labels.insert(p.label).second),
             "duplicate/invalid parameter label");
        need(locals
                 .emplace(p.symbol.value, Local{sig.parameters[i].type, f.scope,
                                                f.symbol,
                                                declared_mode == ParamMode::inout,
                                                0})
                 .second,
             "duplicate parameter symbol");
      }
    }
    // Validate all edges before traversing so bad indices cannot escape via
    // at/UB.
    for (size_t i = 0; i < m.scopes.size(); ++i) {
      check_cancelled(stop);
      const auto &s = m.scopes[i];
      const auto &owner = function(s.function);
      source(s.source);
      need(s.source.node.module == owner.source.node.module &&
               s.source.node.file == owner.source.node.file &&
               s.source.span.begin_byte >= owner.source.span.begin_byte &&
               s.source.span.end_byte <= owner.source.span.end_byte,
           "scope source belongs to another function");
      if (s.parent)
        need(scope(*s.parent).function == s.function && *s.parent != ScopeId{i},
             "scope parent ownership");
      auto current = s.parent;
      for (size_t count = 0; current; ++count) {
        need(count < m.scopes.size(), "scope parent cycle");
        check_cancelled(stop);
        current = scope(*current).parent;
      }
    }
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      scope(m.nodes[i].scope);
      for (auto c : m.nodes[i].children)
        node(c);
      shape(HirNodeId{i});
    }
    for (const auto &f : m.functions)
      tree(f);
    for (auto owner : owners)
      need(owner.has_value(), "orphan executable node");
    for (bool used : scope_used)
      need(used, "orphan scope");
    uses_and_flow();
  }
};
std::shared_ptr<const HirModule> freeze(const HirModule &input,
                                        std::stop_token stop) {
  check_cancelled(stop);
  auto result = std::make_shared<HirModule>(input);
  if (input.resolved) {
    auto resolved = std::make_shared<modules::ResolvedProject>(*input.resolved);
    if (input.resolved->project) {
      auto project =
          std::make_shared<modules::Project>(*input.resolved->project);
      for (auto &mod : project->modules)
        for (auto &file : mod.files) {
          check_cancelled(stop);
          if (file)
            file = std::make_shared<const AstFile>(*file);
        }
      resolved->project = std::move(project);
    }
    result->resolved = std::move(resolved);
  }
  return result;
}
} // namespace
PhaseResult<VerifiedHirModule> verify(const HirModule &input,
                                      std::stop_token stop) {
  PhaseResult<VerifiedHirModule> r;
  try {
    auto module = freeze(input, stop);
    Verifier(*module, stop).run();
    check_cancelled(stop);
    r.value = VerifiedHirModule(std::move(module));
    r.state = PhaseState::complete;
  } catch (const Cancelled &) {
    r.state = PhaseState::cancelled;
  } catch (const Invalid &e) {
    Diagnostic d;
    d.code = "E_IR_INVALID";
    d.phase = "lowering";
    d.severity = "error";
    d.message = e.message;
    d.primary = e.span;
    r.diagnostics.push_back(std::move(d));
  }
  return r;
}
namespace {
void print_source(std::ostream &s, const Source &src) {
  s << " src=" << src.node.module.value << ':' << src.node.file << ':'
    << src.node.node.value << '@' << src.span.snapshot.value << ':'
    << src.span.begin_byte << ':' << src.span.end_byte;
}
} // namespace
std::string dump(const TypeContext &c) {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  for (size_t i = 1; i <= c.size(); ++i) {
    const auto &t = *c.find(TypeId{i});
    s << "type " << i << " kind=" << int(t.kind) << " bits=" << t.bits
      << " declaration=" << t.declaration.value
      << " binding=" << t.binding.value << " result=" << t.result.value
      << " throws=";
    if (t.throws_type)
      s << t.throws_type->value;
    else
      s << '-';
    for (auto p : t.parameters)
      s << " p=" << int(p.mode) << ':' << p.type.value;
    s << '\n';
  }
  return s.str();
}
std::string dump(const HirModule &m) {
  std::ostringstream s;
  s.imbue(std::locale::classic());
  s << contract_version << " scalar-v1 module=" << m.identity.value
    << " backend=" << int(m.config.backend)
    << " word_bits=" << m.config.word_bits << '\n';
  s << dump(m.types);
  for (const auto &f : m.functions) {
    s << "function " << f.symbol.value << " signature=" << f.signature.value
      << " scope=" << f.scope.value << " body=" << f.body.value;
    print_source(s, f.source);
    for (const auto &p : f.parameters)
      s << " param=" << p.symbol.value << ':' << std::quoted(p.label);
    s << '\n';
  }
  for (size_t i = 0; i < m.scopes.size(); ++i) {
    const auto &scope = m.scopes[i];
    s << "scope " << i << " function=" << scope.function.value << " parent=";
    if (scope.parent)
      s << scope.parent->value;
    else
      s << '-';
    print_source(s, scope.source);
    s << '\n';
  }
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const auto &n = m.nodes[i];
    s << "node " << i << " kind=" << int(n.kind()) << " type=" << n.type.value
      << " scope=" << n.scope.value;
    print_source(s, n.source);
    s << " children=";
    for (auto c : n.children)
      s << c.value << ',';
    std::visit(
        [&](const auto &data) {
          using T = std::decay_t<decltype(data)>;
          if constexpr (std::is_same_v<T, BoolLiteral>)
            s << " bool=" << data.value;
          else if constexpr (std::is_same_v<T, IntegerLiteral>)
            s << " decimal=" << std::quoted(data.decimal)
              << " bits=" << data.bits;
          else if constexpr (std::is_same_v<T, FloatLiteral>)
            s << " bits=" << data.bits;
          else if constexpr (std::is_same_v<T, StringLiteral>)
            s << " utf8=" << std::quoted(data.utf8);
          else if constexpr (std::is_same_v<T, Read> ||
                             std::is_same_v<T, Place> ||
                             std::is_same_v<T, FunctionValue>)
            s << " symbol=" << data.symbol.value;
          else if constexpr (std::is_same_v<T, Binding>)
            s << " symbol=" << data.symbol.value
              << " writable=" << data.writable;
          else if constexpr (std::is_same_v<T, Assign>) {
            s << " compound=";
            if (data.compound)
              s << int(*data.compound);
            else
              s << '-';
          } else if constexpr (std::is_same_v<T, Unary> ||
                               std::is_same_v<T, Binary> ||
                               std::is_same_v<T, ShortCircuit>)
            s << " op=" << int(data.op);
          else if constexpr (std::is_same_v<T, Return>)
            s << " function=" << data.function.value;
          else if constexpr (std::is_same_v<T, Break> ||
                             std::is_same_v<T, Continue>)
            s << " loop=" << data.loop.value;
          else if constexpr (std::is_same_v<T, Unsupported>)
            s << " unsupported=" << std::quoted(data.feature);
          else if constexpr (std::is_same_v<T, Call>) {
            s << " callee_kind=" << int(data.kind)
              << " signature=" << data.signature.value << " direct=";
            if (data.direct_symbol)
              s << data.direct_symbol->value;
            else
              s << '-';
            s << " callee=";
            if (data.callee)
              s << data.callee->value;
            else
              s << '-';
            for (auto &a : data.args) {
              s << " arg=" << a.source_index << ':' << a.parameter_index << ':'
                << int(a.mode) << ':' << a.expression.value << ':';
              if (a.label)
                s << std::quoted(*a.label);
              else
                s << '-';
            }
          }
        },
        n.data);
    s << '\n';
  }
  return s.str();
}
std::string dump(const VerifiedHirModule &m) { return dump(m.module()); }
} // namespace lang::hir

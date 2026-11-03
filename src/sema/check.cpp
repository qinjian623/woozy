#include "lang/sema/check.hpp"
#include <algorithm>
#include <gmpxx.h>
#include <map>
#include <set>
#include <tuple>

namespace lang::sema {
namespace {
using namespace hir;
using Ref = modules::NodeRef;
using Key = std::tuple<uint64_t, size_t, uint64_t>;
Key key(Ref r) { return {r.module.value, r.file, r.node.value}; }
struct Rejected {
  Diagnostic diagnostic;
  FailureKind kind;
};
struct Value {
  TypeId type{0};
  std::optional<mpz_class> integer;
  std::optional<bool> boolean;
};
struct Expr {
  HirNodeId id;
  Value value;
};
struct Local {
  TypeId type;
  bool writable;
  bool ordinary_parameter = false;
};
struct Constant {
  Value value;
  std::optional<SymbolId> function;
};
class Checker {
  HirModule m;
  std::stop_token stop;
  std::map<Key, SymbolId> occurrences, definitions;
  std::map<uint64_t, TypeId> signatures;
  std::map<uint64_t, std::vector<Parameter>> parameters;
  std::map<uint64_t, Local> locals;
  std::map<uint64_t, Constant> constants;
  std::set<uint64_t> evaluating;
  SymbolId function{0};
  ScopeId scope{0};
  TypeId unit{0}, boolean{0}, i64{0};
  std::vector<HirNodeId> loops;

  const AstFile &file(Ref r) const {
    return *m.resolved->project->module(r.module).files.at(r.file);
  }
  const AstNode &ast(Ref r) const { return file(r).node(r.node); }
  Ref child(Ref r, size_t i) const {
    r.node = ast(r).children.at(i);
    return r;
  }
  Ref at(Ref r, AstNodeId id) const {
    r.node = id;
    return r;
  }
  std::string_view token(Ref r, size_t n = 0) const {
    return file(r).tokens.at(ast(r).token_begin + n).spelling;
  }
  std::string_view first(Ref r) const { return token(r, token(r) == "pub"); }
  Source source(Ref r) const { return {r, ast(r).span}; }
  [[noreturn]] void reject(Ref r, std::string message,
                           std::string code = "E_TYPE_MISMATCH") const {
    Diagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.phase = "type";
    d.severity = "error";
    d.primary = ast(r).span;
    const auto kind = d.code == "E_CAPABILITY_UNAVAILABLE"
                          ? FailureKind::unsupported
                          : FailureKind::source;
    throw Rejected{std::move(d), kind};
  }
  [[noreturn]] void unavailable(Ref r, std::string feature) const {
    reject(r, std::move(feature), "E_CAPABILITY_UNAVAILABLE");
  }
  SymbolId definition(Ref r) const { return definitions.at(key(r)); }
  std::optional<SymbolId> occurrence(Ref r) const {
    if (ast(r).kind == AstKind::Parenthesized)
      return occurrence(child(r, 0));
    auto it = occurrences.find(key(r));
    return it == occurrences.end() ? std::nullopt : std::optional(it->second);
  }
  const modules::Symbol &symbol(SymbolId s) const {
    return m.resolved->symbols.at(s.value);
  }
  const Type &type(TypeId t) const { return *m.types.find(t); }
  bool integer(TypeId t) const {
    auto k = type(t).kind;
    return k == TypeKind::SignedInt || k == TypeKind::UnsignedInt ||
           k == TypeKind::Isize || k == TypeKind::Usize;
  }
  bool signed_(TypeId t) const {
    return type(t).kind == TypeKind::SignedInt ||
           type(t).kind == TypeKind::Isize;
  }
  unsigned width(TypeId t) const { return bit_width(type(t), m.config); }
  mpz_class modulus(TypeId t) const { return mpz_class(1) << width(t); }
  mpz_class wrap(mpz_class v, TypeId t) const {
    mpz_class mod = modulus(t);
    mpz_mod(v.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    if (signed_(t) && v >= (mod >> 1))
      v -= mod;
    return v;
  }
  void range(Ref r, const mpz_class &v, TypeId t) const {
    mpz_class hi = signed_(t) ? mpz_class(modulus(t) >> 1) : modulus(t);
    mpz_class lo = signed_(t) ? mpz_class(-hi) : mpz_class(0);
    if (v < lo || v >= hi)
      reject(r, "integer constant is outside the target type range");
  }
  uint64_t bits(const mpz_class &v, TypeId t) const {
    mpz_class n;
    const auto mod = modulus(t);
    mpz_mod(n.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    uint64_t result = 0;
    size_t count = 0;
    mpz_export(&result, &count, -1, sizeof(result), 0, 0, n.get_mpz_t());
    return result;
  }
  HirNodeId node(Ref r, TypeId t, NodeData data,
                 std::vector<HirNodeId> children = {}) {
    check_cancelled(stop);
    HirNodeId id{m.nodes.size()};
    m.nodes.push_back(
        {t, source(r), scope, std::move(children), std::move(data)});
    return id;
  }
  Expr int_literal(Ref r, mpz_class v, TypeId t) {
    if (!integer(t))
      reject(r, "integer literal requires an integer context");
    range(r, v, t);
    auto id = node(r, t, IntegerLiteral{v.get_str(), bits(v, t)});
    return {id, {t, std::move(v), {}}};
  }
  TypeId scalar(TypeKind k, unsigned bits = 0) {
    Type t;
    t.kind = k;
    t.bits = bits;
    return m.types.intern(t);
  }
  ParamMode mode(Ref r) const {
    const auto &n = ast(r);
    // Parameters in FunctionType have no name. Declaration parameters do.
    for (size_t i = n.token_begin; i < file(r).node(n.children[0]).token_begin;
         ++i) {
      auto s = file(r).tokens[i].spelling;
      if (s == "inout")
        return ParamMode::inout;
      if (s == "move")
        return ParamMode::move;
    }
    return ParamMode::ordinary;
  }
  TypeId parse_type(Ref r) {
    check_cancelled(stop);
    const auto &n = ast(r);
    if (n.kind == AstKind::ParenthesizedType)
      return parse_type(child(r, 0));
    if (n.kind == AstKind::FunctionType)
      return signature(r, false);
    if ((n.kind != AstKind::NamedType && n.kind != AstKind::Name &&
         n.kind != AstKind::Member) ||
        (n.kind == AstKind::NamedType && !n.children.empty()))
      unavailable(r, "generic and aggregate types require T07/T09");
    auto s = occurrence(r);
    if (!s)
      reject(r, "type has no resolved symbol", "E_NAME_KIND");
    if (symbol(*s).kind != modules::SymbolKind::builtin_type)
      unavailable(r, "nominal and foreign types require T07/T10/T11");
    const auto &name = symbol(*s).name;
    if (name == "Unit")
      return unit;
    if (name == "bool")
      return boolean;
    if (name == "isize")
      return scalar(TypeKind::Isize);
    if (name == "usize")
      return scalar(TypeKind::Usize);
    if (name == "f32" || name == "f64")
      return scalar(TypeKind::Float, name == "f32" ? 32 : 64);
    if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u')) {
      unsigned w = name == "i8" || name == "u8"     ? 8
                   : name == "i16" || name == "u16" ? 16
                   : name == "i32" || name == "u32" ? 32
                                                    : 64;
      return scalar(
          name[0] == 'i' ? TypeKind::SignedInt : TypeKind::UnsignedInt, w);
    }
    unavailable(r, "this type is outside the scalar checker");
  }
  TypeId signature(Ref r, bool declaration) {
    Type t;
    t.kind = TypeKind::Function;
    t.result = unit;
    for (size_t i = ast(r).token_begin; i < ast(r).token_end; ++i)
      if (file(r).tokens[i].spelling == "throws") {
        // Only signature tokens: a nested body's throws is diagnosed in body.
        if (!declaration ||
            i < ast(child(r, ast(r).children.size() - 1)).token_begin)
          unavailable(r, "typed throws requires T08");
      }
    for (auto c : ast(r).children) {
      auto cr = at(r, c);
      const auto k = ast(cr).kind;
      if (k == AstKind::GenericParameter)
        unavailable(cr, "generics require T07");
      if (k == AstKind::Parameter) {
        auto pm = mode(cr);
        auto pt = parse_type(child(cr, 0));
        if (pm == ParamMode::move)
          unavailable(cr, "move parameters require T09");
        if (pm == ParamMode::inout && !integer(pt) &&
            type(pt).kind != TypeKind::Bool && type(pt).kind != TypeKind::Float)
          unavailable(cr,
                      "inout accepts only trivial numeric/bool whole bindings");
        t.parameters.push_back({pm, pt});
      } else if (k != AstKind::Block)
        t.result = parse_type(cr);
    }
    return m.types.intern(t);
  }
  std::optional<mpz_class> literal_integer(Ref r) const {
    while (ast(r).kind == AstKind::Parenthesized)
      r = child(r, 0);
    bool negative = false;
    if (ast(r).kind == AstKind::Unary && (token(r) == "+" || token(r) == "-")) {
      negative = token(r) == "-";
      r = child(r, 0);
      while (ast(r).kind == AstKind::Parenthesized)
        r = child(r, 0);
    }
    const auto &n = ast(r);
    if (n.kind != AstKind::Literal ||
        file(r).tokens[n.token_begin].kind != TokenKind::integer)
      return {};
    std::string s(token(r));
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    int base = 10;
    if (s.size() > 2 && s[0] == '0') {
      base = s[1] == 'x' || s[1] == 'X'   ? 16
             : s[1] == 'b' || s[1] == 'B' ? 2
             : s[1] == 'o' || s[1] == 'O' ? 8
                                          : 10;
      if (base != 10)
        s.erase(0, 2);
    }
    mpz_class v;
    if (v.set_str(s, base) != 0)
      throw std::invalid_argument("invalid parsed integer");
    return negative ? mpz_class(-v) : v;
  }
  // Correctly round the decimal rational directly to IEEE precision. No host
  // double intermediate (in particular for f32 halfway and subnormal values).
  uint64_t float_bits(Ref r, TypeId t) const {
    std::string s(token(r));
    s.erase(std::remove(s.begin(), s.end(), '_'), s.end());
    auto ep = s.find_first_of("eE");
    mpz_class decimal_exponent = 0;
    if (ep != std::string::npos) {
      const auto es = s.substr(ep + 1);
      if (decimal_exponent.set_str(es[0] == '+' ? es.substr(1) : es, 10) != 0)
        throw std::invalid_argument("invalid parsed float exponent");
      s.resize(ep);
    }
    auto dot = s.find('.');
    if (dot != std::string::npos) {
      decimal_exponent -= s.size() - dot - 1;
      s.erase(dot, 1);
    }
    auto first = s.find_first_not_of('0');
    if (first == std::string::npos)
      return 0;
    s.erase(0, first);
    auto last = s.find_last_not_of('0');
    decimal_exponent += s.size() - last - 1;
    s.resize(last + 1);
    mpz_class magnitude = decimal_exponent + s.size() - 1;
    // Beyond these decimal magnitudes no f32/f64 rounding boundary remains.
    if (magnitude > 400)
      return width(t) == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    if (magnitude < -400)
      return 0;
    const long exponent = decimal_exponent.get_si();
    mpz_class numerator;
    numerator.set_str(s, 10);
    if (numerator == 0)
      return 0;
    mpz_class denominator = 1, power;
    mpz_ui_pow_ui(
        power.get_mpz_t(), 10,
        static_cast<unsigned long>(exponent < 0 ? -exponent : exponent));
    if (exponent >= 0)
      numerator *= power;
    else
      denominator = power;
    int precision = width(t) == 32 ? 24 : 53;
    int bias = width(t) == 32 ? 127 : 1023;
    long e = long(mpz_sizeinbase(numerator.get_mpz_t(), 2)) -
             long(mpz_sizeinbase(denominator.get_mpz_t(), 2));
    if (e >= 0 ? numerator < (denominator << e)
               : (numerator << -e) < denominator)
      --e;
    if (e > bias)
      return width(t) == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    long scale = precision - 1 - std::max(e, long(1 - bias));
    mpz_class a = numerator, b = denominator, q, rem;
    if (scale >= 0)
      a <<= scale;
    else
      b <<= -scale;
    mpz_fdiv_qr(q.get_mpz_t(), rem.get_mpz_t(), a.get_mpz_t(), b.get_mpz_t());
    if (2 * rem > b || (2 * rem == b && mpz_odd_p(q.get_mpz_t())))
      ++q;
    if (q >= (mpz_class(1) << precision)) {
      q >>= 1;
      ++e;
    }
    if (e > bias)
      return width(t) == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    bool normal = q >= (mpz_class(1) << (precision - 1));
    uint64_t fraction = q.get_ui();
    if (normal)
      fraction -= uint64_t(1) << (precision - 1);
    return fraction | (normal ? uint64_t(std::max(e, long(1 - bias)) + bias)
                                    << (precision - 1)
                              : 0);
  }
  std::string_view binary_token(Ref r) const {
    return file(r).tokens.at(ast(child(r, 0)).token_end).spelling;
  }
  static BinaryOp binary_op(std::string_view s) {
    static const std::map<std::string_view, BinaryOp> ops{
        {"+", BinaryOp::add},          {"-", BinaryOp::subtract},
        {"*", BinaryOp::multiply},     {"/", BinaryOp::divide},
        {"%", BinaryOp::remainder},    {"<<", BinaryOp::shift_left},
        {">>", BinaryOp::shift_right}, {"&", BinaryOp::bit_and},
        {"|", BinaryOp::bit_or},       {"^", BinaryOp::bit_xor},
        {"==", BinaryOp::equal},       {"!=", BinaryOp::not_equal},
        {"<", BinaryOp::less},         {"<=", BinaryOp::less_equal},
        {">", BinaryOp::greater},      {">=", BinaryOp::greater_equal}};
    return ops.at(s);
  }
  void match(Ref r, TypeId actual, std::optional<TypeId> expected) const {
    if (expected && actual != *expected)
      reject(r, "value type does not match the expected type");
  }
  bool contextual(Ref r) const {
    if (literal_integer(r))
      return true;
    if (ast(r).kind == AstKind::Parenthesized)
      return contextual(child(r, 0));
    if (ast(r).kind == AstKind::Binary)
      return contextual(child(r, 0)) && contextual(child(r, 1));
    return false;
  }
  Value fold(BinaryOp op, Value a, const Value &b) const {
    Value result{a.type, {}, {}};
    if (!a.integer || !b.integer)
      return result;
    const mpz_class &x = *a.integer, &y = *b.integer;
    mpz_class z;
    switch (op) {
    case BinaryOp::add:
      z = x + y;
      break;
    case BinaryOp::subtract:
      z = x - y;
      break;
    case BinaryOp::multiply:
      z = x * y;
      break;
    case BinaryOp::divide:
    case BinaryOp::remainder:
      if (y == 0)
        return result; // retain the runtime Panic operation
      if (op == BinaryOp::divide)
        mpz_tdiv_q(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
      else
        mpz_tdiv_r(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
      break;
    case BinaryOp::shift_left:
    case BinaryOp::shift_right:
      if (y < 0 || y >= width(a.type))
        return result;
      if (op == BinaryOp::shift_left)
        z = x << y.get_ui();
      else
        z = x >> y.get_ui();
      break;
    case BinaryOp::bit_and:
      z = x & y;
      break;
    case BinaryOp::bit_or:
      z = x | y;
      break;
    case BinaryOp::bit_xor:
      z = x ^ y;
      break;
    default:
      result.type = boolean;
      result.boolean = op == BinaryOp::equal        ? x == y
                       : op == BinaryOp::not_equal  ? x != y
                       : op == BinaryOp::less       ? x < y
                       : op == BinaryOp::less_equal ? x <= y
                       : op == BinaryOp::greater    ? x > y
                                                    : x >= y;
      return result;
    }
    result.integer = wrap(std::move(z), a.type);
    return result;
  }
  Expr expression(Ref r, std::optional<TypeId> expected = {}) {
    check_cancelled(stop);
    if (auto v = literal_integer(r))
      return int_literal(r, *v, expected.value_or(i64));
    const auto &n = ast(r);
    Expr result{HirNodeId{0}, {unit, {}, {}}};
    switch (n.kind) {
    case AstKind::Parenthesized:
      return expression(child(r, 0), expected);
    case AstKind::Literal:
      if (token(r) == "true" || token(r) == "false") {
        bool b = token(r) == "true";
        result = {node(r, boolean, BoolLiteral{b}), {boolean, {}, b}};
      } else if (file(r).tokens[n.token_begin].kind == TokenKind::floating) {
        TypeId t = expected.value_or(scalar(TypeKind::Float, 64));
        if (type(t).kind != TypeKind::Float)
          reject(r, "float literal requires float context");
        result = {node(r, t, FloatLiteral{float_bits(r, t)}), {t, {}, {}}};
      } else
        unavailable(r, "string/owned literals require T09");
      break;
    case AstKind::Name:
    case AstKind::Member: {
      auto s = occurrence(r);
      if (!s)
        unavailable(r,
                    "field and external member expressions are not supported");
      auto k = symbol(*s).kind;
      if (k == modules::SymbolKind::function) {
        auto it = signatures.find(s->value);
        if (it == signatures.end())
          unavailable(r, "foreign function calls require T10/T11");
        result = {node(r, it->second, FunctionValue{*s}), {it->second, {}, {}}};
      } else if (k == modules::SymbolKind::constant &&
                 !locals.count(s->value)) {
        const auto c = constant(*s);
        if (c.value.integer) {
          match(r, c.value.type, expected);
          return int_literal(r, *c.value.integer, c.value.type);
        }
        if (c.value.boolean)
          result = {node(r, boolean, BoolLiteral{*c.value.boolean}), c.value};
        else if (c.function)
          result = {node(r, c.value.type, FunctionValue{*c.function}), c.value};
        else
          unavailable(r, "constant value cannot be materialized by scalar HIR");
      } else {
        auto l = locals.find(s->value);
        if (l == locals.end())
          unavailable(r, "binding is not a checked scalar local");
        result = {node(r, l->second.type, Read{*s}), {l->second.type, {}, {}}};
        if (auto c = constants.find(s->value); c != constants.end())
          result.value = c->second.value;
        if (result.value.boolean)
          m.nodes[result.id.value].data = BoolLiteral{*result.value.boolean};
      }
      break;
    }
    case AstKind::Unary: {
      auto op = token(r);
      if (op == "inout" || op == "move")
        reject(r, "argument mode marker outside its matching call",
               "E_CALL_ARGUMENT");
      auto a = expression(child(r, 0),
                          op == "!" ? std::optional(boolean) : expected);
      if (op == "+") {
        if (type(a.value.type).kind == TypeKind::Float)
          unavailable(r, "strict float unary arithmetic is not implemented");
        if (!integer(a.value.type))
          reject(r, "unary plus requires a numeric operand");
        return a;
      }
      UnaryOp u = op == "!"   ? UnaryOp::logical_not
                  : op == "~" ? UnaryOp::bit_not
                              : UnaryOp::negate;
      if (op == "!")
        match(r, a.value.type, boolean);
      else if (op == "~" && !integer(a.value.type))
        reject(r, "bitwise not requires an integer operand");
      else if (!integer(a.value.type)) {
        if (type(a.value.type).kind == TypeKind::Float)
          unavailable(r, "strict float unary arithmetic is not implemented");
        reject(r, "integer unary operator requires an integer operand");
      }
      result = {node(r, a.value.type, Unary{u}, {a.id}), a.value};
      if (a.value.integer)
        result.value.integer = wrap(op == "~" ? mpz_class(~*a.value.integer)
                                              : mpz_class(-*a.value.integer),
                                    a.value.type);
      if (a.value.boolean)
        result.value.boolean = !*a.value.boolean;
      break;
    }
    case AstKind::Binary: {
      auto op = binary_token(r);
      if (op == "&&" || op == "||") {
        auto a = expression(child(r, 0), boolean),
             b = expression(child(r, 1), boolean);
        Value v{boolean, {}, {}};
        if (a.value.boolean && ((op == "&&" && !*a.value.boolean) ||
                                (op == "||" && *a.value.boolean)))
          v.boolean = *a.value.boolean;
        else if (a.value.boolean && b.value.boolean)
          v.boolean = op == "&&" ? *a.value.boolean && *b.value.boolean
                                 : *a.value.boolean || *b.value.boolean;
        result = {
            node(r, boolean,
                 ShortCircuit{op == "&&" ? LogicalOp::and_ : LogicalOp::or_},
                 {a.id, b.id}),
            v};
        break;
      }
      const auto bop = binary_op(op);
      bool comparison = bop >= BinaryOp::equal;
      bool shift = bop == BinaryOp::shift_left || bop == BinaryOp::shift_right;
      std::optional<TypeId> context = comparison ? std::nullopt : expected;
      Expr a{HirNodeId{0}, {}}, b{HirNodeId{0}, {}};
      if (!shift && contextual(child(r, 0)) && !contextual(child(r, 1))) {
        b = expression(child(r, 1), context);
        a = expression(child(r, 0), b.value.type);
        // Construction order does not determine execution order; children do.
      } else {
        a = expression(child(r, 0), context);
        b = expression(child(r, 1),
                       shift ? std::nullopt : std::optional(a.value.type));
      }
      if (shift) {
        if (!integer(a.value.type) || !integer(b.value.type))
          reject(r, "shift operands must be integers");
      } else
        match(r, b.value.type, a.value.type);
      const auto k = type(a.value.type).kind;
      if ((bop == BinaryOp::remainder || bop == BinaryOp::bit_and ||
           bop == BinaryOp::bit_or || bop == BinaryOp::bit_xor) &&
          !integer(a.value.type))
        reject(r, "bitwise and remainder operators require integer operands");
      if (k == TypeKind::Float)
        unavailable(r, "strict float operations are not implemented");
      if (!integer(a.value.type) &&
          !(k == TypeKind::Bool &&
            (bop == BinaryOp::equal || bop == BinaryOp::not_equal)))
        reject(r, "invalid scalar operator operands");
      auto v = integer(a.value.type) ? fold(bop, a.value, b.value)
                                     : Value{boolean, {}, {}};
      if (k == TypeKind::Bool && a.value.boolean && b.value.boolean)
        v.boolean = bop == BinaryOp::equal
                        ? *a.value.boolean == *b.value.boolean
                        : *a.value.boolean != *b.value.boolean;
      v.type = comparison ? boolean : a.value.type;
      result = {node(r, v.type, Binary{bop}, {a.id, b.id}), v};
      break;
    }
    case AstKind::Call:
      result = call(r);
      break;
    case AstKind::FunctionLiteral: {
      auto s = definition(r);
      auto sig = signatures.at(s.value);
      result = {node(r, sig, FunctionValue{s}), {sig, {}, {}}};
      break;
    }
    default:
      unavailable(r, "this expression requires aggregate, ownership, or "
                     "external-call support");
    }
    match(r, result.value.type, expected);
    return result;
  }
  Expr place(Ref r, bool inout) {
    if (ast(r).kind == AstKind::Parenthesized)
      return place(child(r, 0), inout);
    if (ast(r).kind != AstKind::Name)
      unavailable(
          r, "only a whole writable scalar binding can be used as a place");
    auto s = occurrence(r);
    if (!s || !locals.count(s->value))
      reject(r, "place is not a local binding",
             inout ? "E_CALL_ARGUMENT" : "E_TYPE_MISMATCH");
    auto l = locals.at(s->value);
    if (l.ordinary_parameter) {
      if (inout)
        reject(
            r,
            "an ordinary parameter is not a var or an exclusive inout binding",
            "E_CALL_ARGUMENT");
      unavailable(r, "ordinary parameter reassignment has no specified "
                     "source-language rule");
    }
    if (!l.writable)
      reject(r, "let/const bindings are not writable",
             inout ? "E_CALL_ARGUMENT" : "E_TYPE_MISMATCH");
    if (inout && !integer(l.type) && type(l.type).kind != TypeKind::Bool &&
        type(l.type).kind != TypeKind::Float)
      unavailable(r, "inout requires a trivial numeric/bool binding");
    return {node(r, l.type, Place{*s}), {l.type, {}, {}}};
  }
  Expr call(Ref r) {
    auto callee_ref = child(r, 0);
    auto s = occurrence(callee_ref);
    if (s && symbol(*s).kind == modules::SymbolKind::builtin_type) {
      auto target = parse_type(callee_ref);
      if (!integer(target))
        unavailable(
            r, "only integer-to-integer scalar conversions are implemented");
      if (ast(r).children.size() != 2)
        reject(r, "conversion requires one argument", "E_CALL_ARGUMENT");
      auto arg = child(r, 1);
      if (ast(arg).token_begin + 1 < ast(arg).token_end && token(arg, 1) == ":")
        reject(arg, "conversion does not accept labels", "E_CALL_ARGUMENT");
      auto xr = child(arg, 0);
      auto x = expression(xr, literal_integer(xr) ? std::optional(target)
                                                  : std::nullopt);
      if (!integer(x.value.type))
        unavailable(r, "float/integer conversions are not implemented");
      if (x.value.integer)
        range(arg, *x.value.integer, target);
      auto v = x.value;
      v.type = target;
      return {node(r, target, IntegerConvert{}, {x.id}), v};
    }
    Call c;
    std::vector<HirNodeId> children;
    if (s && symbol(*s).kind == modules::SymbolKind::function &&
        signatures.count(s->value)) {
      c.kind = CalleeKind::direct;
      c.direct_symbol = s;
      c.signature = signatures.at(s->value);
    } else {
      auto callee = expression(callee_ref);
      if (type(callee.value.type).kind != TypeKind::Function)
        reject(callee_ref, "callee is not a function", "E_CALL_ARGUMENT");
      c.kind = CalleeKind::function_value;
      c.callee = callee.id;
      c.signature = callee.value.type;
      children.push_back(callee.id);
    }
    const auto sig = type(c.signature);
    std::vector<bool> seen(sig.parameters.size());
    if (ast(r).children.size() - 1 != seen.size())
      reject(r, "wrong number of call arguments", "E_CALL_ARGUMENT");
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto ar = child(r, i);
      auto er = child(ar, 0);
      size_t pi = i - 1;
      std::optional<std::string> label;
      if (token(ar, 1) == ":") {
        if (!c.direct_symbol)
          reject(ar, "function values do not preserve parameter labels",
                 "E_CALL_ARGUMENT");
        label = std::string(token(ar));
        const auto &ps = parameters.at(c.direct_symbol->value);
        auto found = std::find_if(ps.begin(), ps.end(), [&](const auto &p) {
          return p.label == *label;
        });
        if (found == ps.end())
          reject(ar, "unknown argument label", "E_CALL_ARGUMENT");
        pi = size_t(found - ps.begin());
      }
      if (pi >= seen.size() || seen[pi])
        reject(ar, "parameter provided more than once", "E_CALL_ARGUMENT");
      seen[pi] = true;
      ParamMode pm = ParamMode::ordinary;
      if (ast(er).kind == AstKind::Unary &&
          (token(er) == "inout" || token(er) == "move")) {
        pm = token(er) == "inout" ? ParamMode::inout : ParamMode::move;
        er = child(er, 0);
      }
      if (pm != sig.parameters[pi].mode)
        reject(ar, "argument mode differs from parameter mode",
               "E_CALL_ARGUMENT");
      if (pm == ParamMode::move)
        unavailable(ar, "move arguments require T09");
      auto e = pm == ParamMode::inout ? place(er, true)
                                      : expression(er, sig.parameters[pi].type);
      match(ar, e.value.type, sig.parameters[pi].type);
      c.args.push_back({uint32_t(i - 1), uint32_t(pi), label, pm, e.id});
      children.push_back(e.id);
    }
    // In the supported scalar subset each binding identifies its entire
    // storage. Include nested reads/places in arguments and the callee.
    std::set<uint64_t> borrowed;
    for (const auto &a : c.args)
      if (a.mode == ParamMode::inout) {
        auto id =
            std::get<Place>(m.nodes[a.expression.value].data).symbol.value;
        if (!borrowed.insert(id).second)
          reject(r, "same binding borrowed by inout twice", "E_CALL_ARGUMENT");
      }
    for (auto root : children) {
      std::vector<HirNodeId> todo{root};
      while (!todo.empty()) {
        auto id = todo.back();
        todo.pop_back();
        const auto &n = m.nodes[id.value];
        if (auto p = std::get_if<Read>(&n.data);
            p && borrowed.count(p->symbol.value))
          reject(r, "inout binding is also read by this call",
                 "E_CALL_ARGUMENT");
        if (auto p = std::get_if<Place>(&n.data);
            p && borrowed.count(p->symbol.value)) {
          bool top = false;
          for (auto &a : c.args)
            if (a.mode == ParamMode::inout && a.expression == id)
              top = true;
          if (!top)
            reject(r, "nested call aliases an inout argument",
                   "E_CALL_ARGUMENT");
        }
        todo.insert(todo.end(), n.children.begin(), n.children.end());
      }
    }
    return {node(r, sig.result, std::move(c), std::move(children)),
            {sig.result, {}, {}}};
  }
  Constant constant(SymbolId s) {
    if (auto it = constants.find(s.value); it != constants.end())
      return it->second;
    if (!evaluating.insert(s.value).second)
      reject(*symbol(s).definition, "constant cycle", "E_CONSTANT_CYCLE");
    auto r = *symbol(s).definition;
    // Temporarily emitted evaluation nodes are discarded; constants are
    // materialized at their use sites with the use's Source/Scope.
    auto begin = m.nodes.size();
    std::optional<TypeId> expected;
    if (ast(r).children.size() > 1)
      expected = parse_type(child(r, 0));
    auto e = expression(child(r, ast(r).children.size() - 1), expected);
    Constant c{e.value, {}};
    if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data))
      c.function = f->symbol;
    if (!c.value.integer && !c.value.boolean && !c.function)
      unavailable(
          r, "const initializer must be a non-Panic scalar compile-time value");
    m.nodes.resize(begin);
    evaluating.erase(s.value);
    constants[s.value] = c;
    return c;
  }
  ScopeId new_scope(Ref r, std::optional<ScopeId> parent) {
    ScopeId id{m.scopes.size()};
    m.scopes.push_back({function, parent, source(r)});
    return id;
  }
  struct Statement {
    HirNodeId id;
    bool falls = true;
    bool breaks = false;
  };
  Statement block(Ref r, bool outer = false) {
    auto saved = scope;
    if (!outer)
      scope = new_scope(r, saved);
    std::vector<HirNodeId> children;
    bool falls = true, breaks = false;
    for (auto c : ast(r).children) {
      auto s = statement(at(r, c));
      children.push_back(s.id);
      if (falls) {
        breaks = breaks || s.breaks;
        falls = s.falls;
      }
    }
    auto id = node(r, unit, Block{}, std::move(children));
    scope = saved;
    return {id, falls, breaks};
  }
  Statement statement(Ref r) {
    check_cancelled(stop);
    const auto &n = ast(r);
    switch (n.kind) {
    case AstKind::Block:
      return block(r);
    case AstKind::Binding: {
      auto s = definition(r);
      std::optional<TypeId> expected;
      if (n.children.size() > 1)
        expected = parse_type(child(r, 0));
      auto e = expression(child(r, n.children.size() - 1), expected);
      bool writable = first(r) == "var";
      if (first(r) == "const" && !e.value.integer && !e.value.boolean &&
          !std::holds_alternative<FunctionValue>(m.nodes[e.id.value].data))
        unavailable(
            r,
            "const initializer must be a non-Panic scalar compile-time value");
      locals[s.value] = {e.value.type, writable};
      if (first(r) == "const") {
        Constant c{e.value, {}};
        if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data))
          c.function = f->symbol;
        constants[s.value] = c;
      }
      return {node(r, unit, Binding{s, writable}, {e.id})};
    }
    case AstKind::Assign: {
      auto p = place(child(r, 0), false);
      auto op = binary_token(r);
      bool shift = op == "<<=" || op == ">>=";
      auto rhs = expression(child(r, 1),
                            shift ? std::nullopt : std::optional(p.value.type));
      std::optional<BinaryOp> compound;
      if (op != "=") {
        compound = binary_op(op.substr(0, op.size() - 1));
        if (type(p.value.type).kind == TypeKind::Float &&
            (*compound == BinaryOp::add || *compound == BinaryOp::subtract ||
             *compound == BinaryOp::multiply || *compound == BinaryOp::divide))
          unavailable(r, "strict float compound arithmetic is not implemented");
        if (!integer(p.value.type) || !integer(rhs.value.type))
          reject(r, "compound assignment requires integer operands");
        if (!shift)
          match(r, rhs.value.type, p.value.type);
      }
      return {node(r, unit, Assign{compound}, {p.id, rhs.id})};
    }
    case AstKind::ExpressionStatement: {
      auto e = expression(child(r, 0));
      return {node(r, unit, ExpressionStatement{}, {e.id})};
    }
    case AstKind::Return: {
      auto result = type(signatures.at(function.value)).result;
      std::vector<HirNodeId> children;
      if (n.children.empty())
        match(r, unit, result);
      else
        children.push_back(expression(child(r, 0), result).id);
      return {node(r, unit, Return{function}, std::move(children)), false};
    }
    case AstKind::If: {
      auto cond = expression(child(r, 0), boolean);
      auto yes = block(child(r, 1));
      std::vector<HirNodeId> children{cond.id, yes.id};
      bool falls = true, breaks = yes.breaks;
      bool else_falls = true, else_breaks = false;
      if (n.children.size() > 2) {
        auto er = child(r, 2);
        Statement no{HirNodeId{0}};
        if (ast(er).kind == AstKind::Block)
          no = block(er);
        else {
          auto saved = scope;
          scope = new_scope(er, saved);
          auto branch = statement(er);
          no = {node(er, unit, Block{}, {branch.id}), branch.falls,
                branch.breaks};
          scope = saved;
        }
        children.push_back(no.id);
        falls = yes.falls || no.falls;
        breaks = breaks || no.breaks;
        else_falls = no.falls;
        else_breaks = no.breaks;
      }
      if (auto b = std::get_if<BoolLiteral>(&m.nodes[cond.id.value].data)) {
        if (b->value) {
          falls = yes.falls;
          breaks = yes.breaks;
        } else if (n.children.size() == 2) {
          falls = true;
          breaks = false;
        } else {
          // Branch summaries are needed after their scope has been restored.
          // The selected else summary is the combined result minus the then
          // arm; keep it directly rather than inferring from bool algebra.
          falls = else_falls;
          breaks = else_breaks;
        }
      }
      return {node(r, unit, If{}, std::move(children)), falls, breaks};
    }
    case AstKind::For: {
      auto saved = scope;
      scope = new_scope(r, saved);
      auto id = node(r, unit, For{});
      loops.push_back(id);
      auto bodyref = child(r, n.children.size() - 1);
      size_t first_semicolon = ast(bodyref).token_begin,
             second_semicolon = first_semicolon;
      // Separators at nesting depth zero, since calls/function types may have
      // punctuation in their own token ranges.
      unsigned nesting = 0;
      for (size_t i = n.token_begin + 1; i < ast(bodyref).token_begin; ++i) {
        auto t = file(r).tokens[i].spelling;
        if (t == "(" || t == "[")
          ++nesting;
        if (t == ")" || t == "]")
          --nesting;
        if (t == ";" && !nesting) {
          if (first_semicolon == ast(bodyref).token_begin)
            first_semicolon = i;
          else
            second_semicolon = i;
        }
      }
      std::optional<Ref> init, condition, step;
      for (size_t i = 0; i + 1 < n.children.size(); ++i) {
        auto cr = child(r, i);
        auto start = ast(cr).token_begin;
        if (first_semicolon == ast(bodyref).token_begin) {
          if (ast(cr).kind != AstKind::ExpressionStatement)
            reject(cr, "invalid loop condition");
          condition = child(cr, 0);
        } else if (start < first_semicolon)
          init = cr;
        else if (start < second_semicolon)
          condition = cr;
        else
          step = cr;
      }
      auto ini = init ? statement(*init).id : node(r, unit, Noop{});
      auto cond = condition ? expression(*condition, boolean)
                            : Expr{node(r, boolean, BoolLiteral{true}),
                                   {boolean, {}, true}};
      auto st = step ? statement(*step).id : node(r, unit, Noop{});
      auto body = block(bodyref);
      m.nodes[id.value].children = {ini, cond.id, st, body.id};
      loops.pop_back();
      scope = saved;
      bool forever =
          std::holds_alternative<BoolLiteral>(m.nodes[cond.id.value].data) &&
          std::get<BoolLiteral>(m.nodes[cond.id.value].data).value;
      return {id, !forever || body.breaks, false};
    }
    case AstKind::Break:
    case AstKind::Continue:
      if (loops.empty())
        reject(r, "jump has no enclosing loop");
      if (n.kind == AstKind::Break)
        return {node(r, unit, Break{loops.back()}), false, true};
      return {node(r, unit, Continue{loops.back()}), false, false};
    default:
      unavailable(r, "switch/aggregate, typed errors, cleanup, or ownership "
                     "statement requires a later task");
    }
  }

public:
  Checker(std::shared_ptr<const modules::ResolvedProject> r, Config config,
          std::stop_token s)
      : stop(s) {
    m.resolved = std::move(r);
    m.config = config;
    m.identity = m.resolved->project->entry;
    unit = scalar(TypeKind::Unit);
    boolean = scalar(TypeKind::Bool);
    i64 = scalar(TypeKind::SignedInt, 64);
    for (const auto &s : m.resolved->symbols)
      if (s.definition)
        definitions.emplace(key(*s.definition), s.id);
    for (const auto &o : m.resolved->occurrences)
      occurrences[key(o.source)] = o.symbol;
  }
  HirModule run() {
    // Predeclare every signature, then check bodies: recursion and cross-file
    // calls use resolved IDs without inferring an interface from its body.
    for (const auto &mod : m.resolved->project->modules) {
      if (!mod.foreign_exports.empty() || mod.files.empty()) {
        for (const auto &owner : m.resolved->project->modules)
          for (const auto &import : owner.imports)
            if (import.target == mod.id)
              unavailable({owner.id, import.file, import.node},
                          "foreign declarations require T10/T11");
        Diagnostic d;
        d.code = "E_CAPABILITY_UNAVAILABLE";
        d.phase = "type";
        d.severity = "error";
        d.message = "foreign declarations require T10/T11";
        throw Rejected{std::move(d), FailureKind::unsupported};
      }
      for (size_t f = 0; f < mod.files.size(); ++f) {
        const auto &a = *mod.files[f];
        for (auto id : a.node(a.root).children) {
          Ref r{mod.id, f, id};
          auto k = ast(r).kind;
          if (k == AstKind::Package || k == AstKind::Import)
            continue;
          if (k == AstKind::Binding)
            continue;
          if (k != AstKind::Function)
            unavailable(r, "aggregate/trait/impl declarations require T07");
        }
      }
    }
    for (const auto &s : m.resolved->symbols) {
      if (s.kind != modules::SymbolKind::function || !s.definition)
        continue;
      auto r = *s.definition;
      if (ast(r).kind != AstKind::Function &&
          ast(r).kind != AstKind::FunctionLiteral)
        continue;
      signatures[s.id.value] = signature(r, true);
      for (auto c : ast(r).children)
        if (ast(at(r, c)).kind == AstKind::Parameter) {
          auto p = definition(at(r, c));
          auto name = symbol(p).name;
          parameters[s.id.value].push_back({p, name == "_" ? "" : name});
        }
      parameters.try_emplace(s.id.value);
    }
    // Validate unused top-level constants too; they cannot hide bad types.
    for (const auto &mod : m.resolved->project->modules)
      for (size_t f = 0; f < mod.files.size(); ++f)
        for (auto id : mod.files[f]->node(mod.files[f]->root).children) {
          Ref r{mod.id, f, id};
          if (ast(r).kind == AstKind::Binding)
            constant(definition(r));
        }
    for (const auto &[sid, sig] : signatures) {
      check_cancelled(stop);
      function = SymbolId{sid};
      auto r = *symbol(function).definition;
      locals.clear();
      loops.clear();
      scope = new_scope(r, {});
      const auto ps = parameters.at(sid);
      const auto ft = type(sig);
      for (size_t i = 0; i < ps.size(); ++i)
        locals[ps[i].symbol.value] = {
            ft.parameters[i].type, ft.parameters[i].mode == ParamMode::inout,
            ft.parameters[i].mode == ParamMode::ordinary};
      auto body = block(child(r, ast(r).children.size() - 1), true);
      if (ft.result != unit && body.falls)
        reject(r,
               "non-Unit function has a reachable normal exit without return");
      m.functions.push_back({function, sig, ps, body.id, scope, source(r)});
    }
    return std::move(m);
  }
};
PhaseResult<VerifiedHirModule> internal(std::string message) {
  PhaseResult<VerifiedHirModule> r;
  Diagnostic d;
  d.code = "E_INTERNAL_ERROR";
  d.phase = "type";
  d.severity = "error";
  d.message = std::move(message);
  r.diagnostics.push_back(std::move(d));
  return r;
}
} // namespace
PhaseResult<VerifiedHirModule>
check_project(std::shared_ptr<const modules::ResolvedProject> resolved,
              std::optional<CheckConfig> config, std::stop_token stop) {
  try {
    check_cancelled(stop);
    if (!resolved || !resolved->project)
      return internal(
          "checker requires an owning successfully resolved project");
    const auto &p = *resolved->project;
    if (p.backend != "native" && p.backend != "python")
      return internal("invalid project backend");
    Config c{p.backend == "native" ? Backend::native : Backend::python,
             p.word_bits};
    if (config && *config != c)
      return internal("CheckConfig does not match Project target");
    if (c.word_bits != 32 && c.word_bits != 64)
      return internal("target word_bits must be 32 or 64");
    Checker checker(std::move(resolved), c, stop);
    auto m = checker.run();
    return hir::verify(m, stop); // the sole constructor of VerifiedHirModule
  } catch (const Cancelled &) {
    PhaseResult<VerifiedHirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  } catch (const Rejected &e) {
    PhaseResult<VerifiedHirModule> r;
    r.failure = e.kind;
    r.diagnostics = {e.diagnostic};
    return r;
  } catch (const std::exception &e) {
    return internal(e.what());
  }
}
PhaseResult<VerifiedHirModule>
check_project(const PhaseResult<modules::ResolvedProject> &resolved,
              std::optional<CheckConfig> config, std::stop_token stop) {
  if (stop.stop_requested()) {
    PhaseResult<VerifiedHirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  }
  if (!resolved.success()) {
    PhaseResult<VerifiedHirModule> r;
    r.state = resolved.state;
    r.failure = resolved.failure;
    r.diagnostics = resolved.diagnostics;
    if (r.state == PhaseState::complete)
      r.state = PhaseState::failed;
    if (r.diagnostics.empty() && r.state != PhaseState::cancelled)
      return internal("resolve did not successfully complete");
    return r;
  }
  return check_project(
      std::make_shared<const modules::ResolvedProject>(*resolved.value), config,
      stop);
}
} // namespace lang::sema

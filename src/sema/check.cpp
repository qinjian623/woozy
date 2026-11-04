#include "lang/sema/check.hpp"
#include <algorithm>
#include <functional>
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
  std::vector<TypeId> type_args;
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
  std::vector<HirNodeId> loops, break_targets;
  std::shared_ptr<modules::ResolvedProject> owned;
  std::map<uint64_t, size_t> nominal_ids;
  std::map<uint64_t, TypeId> self_types;
  std::map<uint64_t, std::vector<GenericParam>> generic_params;
  std::map<uint64_t, std::optional<SymbolId>> bounds;
  std::map<uint64_t, Ref> bodies;
  std::map<uint64_t, size_t> template_ids;
  std::map<std::pair<uint64_t, std::vector<uint64_t>>, SymbolId> instance_cache;
  size_t instantiation_depth = 0;
  size_t constraint_depth = 0;
  std::map<std::pair<uint64_t, uint64_t>, bool> trait_results;
  std::set<std::pair<uint64_t, uint64_t>> active_constraints;
  size_t instantiated_nodes = 0;
  std::vector<GenericParam> active_params;

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
  TypeId parameter_type(SymbolId s) {
    Type t;
    t.kind = TypeKind::TypeParameter;
    t.declaration = s;
    return m.types.intern(t);
  }
  std::vector<GenericParam> generics(Ref r) {
    std::vector<GenericParam> ps;
    for (auto c : ast(r).children) {
      auto cr = at(r, c);
      if (ast(cr).kind != AstKind::GenericParameter)
        continue;
      auto id = definition(cr);
      std::optional<SymbolId> trait;
      if (!ast(cr).children.empty()) {
        trait = occurrence(child(cr, 0));
        if (!trait || symbol(*trait).kind != modules::SymbolKind::trait)
          reject(cr, "generic constraint must name a trait");
        if (!ast(child(cr, 0)).children.empty())
          unavailable(cr, "generic traits are not implemented");
      }
      bounds[id.value] = trait;
      ps.push_back({id, trait, source(cr)});
      parameter_type(id);
    }
    return ps;
  }
  const NominalDecl &nominal(TypeId id) const {
    return m.nominals.at(nominal_ids.at(type(id).declaration.value));
  }
  Substitutions nominal_subs(TypeId id) const {
    Substitutions result;
    const auto &d = nominal(id);
    for (size_t i = 0; i < d.params.size(); ++i)
      result.push_back({d.params[i].symbol, type(id).type_args.at(i)});
    return result;
  }
  TypeId field_type(TypeId id, const FieldDecl &f) {
    return substitute(f.type, nominal_subs(id), m.types);
  }
  TypeId parse_type(Ref r) {
    check_cancelled(stop);
    const auto &n = ast(r);
    if (n.kind == AstKind::ParenthesizedType ||
        n.kind == AstKind::Parenthesized || n.kind == AstKind::BracketItem)
      return parse_type(child(r, 0));
    if (n.kind == AstKind::FunctionType)
      return signature(r, false);
    std::optional<SymbolId> s;
    std::vector<TypeId> args;
    if (n.kind == AstKind::NeutralBracket) {
      s = occurrence(child(r, 0));
      for (size_t i = 1; i < n.children.size(); ++i)
        args.push_back(parse_type(child(r, i)));
    } else if (n.kind == AstKind::NamedType || n.kind == AstKind::Name ||
               n.kind == AstKind::Member) {
      s = occurrence(r);
      for (auto c : n.children)
        if (n.kind == AstKind::NamedType)
          args.push_back(parse_type(at(r, c)));
    } else
      reject(r, "generic arguments must be types", "E_NAME_KIND");
    if (!s)
      reject(r, "type has no resolved symbol", "E_NAME_KIND");
    auto kind = symbol(*s).kind;
    if (kind == modules::SymbolKind::type_parameter) {
      if (!args.empty())
        reject(r, "type parameter does not accept arguments");
      auto it = self_types.find(s->value);
      return it == self_types.end() ? parameter_type(*s) : it->second;
    }
    if (kind == modules::SymbolKind::type) {
      auto it = nominal_ids.find(s->value);
      if (it == nominal_ids.end())
        unavailable(r, "foreign types require T10/T11");
      const auto &d = m.nominals[it->second];
      if (args.size() != d.params.size())
        reject(r, "wrong number of type arguments");
      Type t;
      t.kind = d.kind;
      t.declaration = *s;
      t.type_args = std::move(args);
      return m.types.intern(t);
    }
    if (kind != modules::SymbolKind::builtin_type)
      reject(r, "name does not denote a value type", "E_NAME_KIND");
    const auto name = symbol(*s).name;
    if (name == "Vec" || name == "Result")
      unavailable(
          r, "container, owned string and Result support requires T08/T09");
    if (!args.empty())
      reject(r, "scalar type does not accept type arguments");
    if (name == "Unit")
      return unit;
    if (name == "bool")
      return boolean;
    if (name == "string")
      return scalar(TypeKind::String);
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
    unavailable(r, "type is not implemented");
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
        continue;
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
      } else if (file(r).tokens[n.token_begin].kind == TokenKind::string_literal) {
        auto t = scalar(TypeKind::String);
        // The lexer already decoded escapes and normalized physical newlines.
        // Keep its complete byte sequence, including embedded NULs.
        result = {node(r, t, StringLiteral{
                      file(r).tokens[n.token_begin].string_value.value()}),
                  {t, {}, {}}};
      } else
        unavailable(r, "string/owned literals require T09");
      break;
    case AstKind::Name:
    case AstKind::Member: {
      auto s = occurrence(r);
      if (!s) {
        if (n.kind != AstKind::Member)
          reject(r, "unresolved value");
        auto base_ref = child(r, 0);
        if (auto bt = type_expression(base_ref)) {
          if (type(*bt).kind == TypeKind::NominalEnum) {
            const auto &d = nominal(*bt);
            auto name = token(r, n.token_end - n.token_begin - 1);
            for (const auto &v : d.variants)
              if (symbol(v.symbol).name == name) {
                if (!v.payload.empty())
                  reject(r, "variant payload requires a constructor call");
                result = {node(r, *bt, EnumInit{v.symbol, {}}), {*bt, {}, {}}};
                break;
              }
            if (result.value.type == unit)
              reject(r, "unknown enum variant");
            break;
          }
          reject(r, "type member is not a value");
        }
        auto base = expression(base_ref);
        const auto f = find_field(r, base.value.type);
        auto t = field_type(base.value.type, f);
        result = {node(r, t, FieldRead{f.symbol}, {base.id}), {t, {}, {}}};
        break;
      }
      auto k = symbol(*s).kind;
      if (k == modules::SymbolKind::function) {
        auto it = signatures.find(s->value);
        if (it == signatures.end())
          unavailable(r, "foreign function calls require T10/T11");
        std::vector<TypeId> args;
        auto sig = it->second;
        if (!generic_params[s->value].empty()) {
          Substitutions sub;
          if (expected)
            infer(r, sig, *expected, sub);
          args = finish_arguments(r, generic_params[s->value], sub);
          sig = substitute(sig, sub, m.types);
        }
        result = {node(r, sig, FunctionValue{*s, args}), {sig, {}, {}}};
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
          result = {
              node(r, c.value.type, FunctionValue{*c.function, c.type_args}),
              c.value};
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
        if (!integer(a.value.type) &&
            type(a.value.type).kind != TypeKind::Float)
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
      else if (!integer(a.value.type) &&
               type(a.value.type).kind != TypeKind::Float) {
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
      if (!integer(a.value.type) && k != TypeKind::Float &&
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
      result = call(r, expected);
      break;
    case AstKind::NeutralBracket: {
      auto base = child(r, 0);
      auto s = occurrence(base);
      if (!s || symbol(*s).kind != modules::SymbolKind::function)
        unavailable(
            r, "index operations require T09 or declared external interfaces");
      auto ps = generic_params.at(s->value);
      Substitutions sub;
      if (n.children.size() - 1 != ps.size())
        reject(r, "wrong number of generic arguments");
      std::vector<TypeId> args;
      for (size_t i = 0; i < ps.size(); ++i) {
        auto t = parse_type(child(r, i + 1));
        args.push_back(t);
        sub.push_back({ps[i].symbol, t});
      }
      check_constraints(r, ps, args);
      auto sig = substitute(signatures.at(s->value), sub, m.types);
      result = {node(r, sig, FunctionValue{*s, args}), {sig, {}, {}}};
      break;
    }
    case AstKind::FunctionLiteral: {
      auto s = definition(r);
      auto sig = signatures.at(s.value);
      std::vector<TypeId> args;
      for (const auto &p : generic_params[s.value])
        args.push_back(parameter_type(p.symbol));
      result = {node(r, sig, FunctionValue{s, args}), {sig, {}, {}}};
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
    if (ast(r).kind == AstKind::Member) {
      if (inout)
        unavailable(r, "field inout is not implemented");
      auto base = place(child(r, 0), false);
      auto f = find_field(r, base.value.type);
      auto t = field_type(base.value.type, f);
      return {node(r, t, FieldPlace{f.symbol}, {base.id}), {t, {}, {}}};
    }
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
  bool contains_parameter(TypeId id) const {
    std::vector<TypeId> pending{id};
    std::set<uint64_t> visited;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      if (!visited.insert(current.value).second)
        continue;
      const auto &t = type(current);
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
  std::optional<TypeId> type_expression(Ref r) {
    if (ast(r).kind == AstKind::Parenthesized)
      return type_expression(child(r, 0));
    auto base = r;
    if (ast(r).kind == AstKind::NeutralBracket)
      base = child(r, 0);
    auto s = occurrence(base);
    if (s && (symbol(*s).kind == modules::SymbolKind::type ||
              symbol(*s).kind == modules::SymbolKind::type_parameter ||
              symbol(*s).kind == modules::SymbolKind::builtin_type))
      return parse_type(r);
    return {};
  }
  FieldDecl find_field(Ref r, TypeId t) {
    if (type(t).kind != TypeKind::NominalStruct)
      reject(r, "field receiver is not a struct");
    auto name = file(r).tokens[ast(r).token_end - 1].spelling;
    for (const auto &f : nominal(t).fields)
      if (symbol(f.symbol).name == name) {
        if (symbol(f.symbol).module != r.module && !f.exported)
          reject(r, "field is private", "E_PRIVATE_ACCESS");
        return f;
      }
    reject(r, "unknown struct field");
  }
  void infer(Ref r, TypeId pattern, TypeId actual, Substitutions &sub) {
    auto p = type(pattern), a = type(actual);
    if (p.kind == TypeKind::TypeParameter) {
      for (auto [id, t] : sub)
        if (id == p.declaration) {
          if (t != actual)
            reject(r, "conflicting generic type inference");
          return;
        }
      sub.push_back({p.declaration, actual});
      return;
    }
    if (p.kind != a.kind || p.bits != a.bits ||
        p.declaration != a.declaration ||
        p.parameters.size() != a.parameters.size() ||
        p.type_args.size() != a.type_args.size() ||
        p.throws_type.has_value() != a.throws_type.has_value())
      reject(r, "type inference mismatch");
    for (size_t i = 0; i < p.type_args.size(); ++i)
      infer(r, p.type_args[i], a.type_args[i], sub);
    if (p.kind == TypeKind::Function) {
      for (size_t i = 0; i < p.parameters.size(); ++i) {
        if (p.parameters[i].mode != a.parameters[i].mode)
          reject(r, "function argument mode mismatch");
        infer(r, p.parameters[i].type, a.parameters[i].type, sub);
      }
      infer(r, p.result, a.result, sub);
      if (p.throws_type)
        infer(r, *p.throws_type, *a.throws_type, sub);
    }
  }
  bool try_infer(Ref r, TypeId pattern, TypeId actual, Substitutions &sub) {
    try {
      infer(r, pattern, actual, sub);
      return true;
    } catch (const Rejected &) {
      return false;
    }
  }
  std::vector<TypeId> finish_arguments(Ref r,
                                       const std::vector<GenericParam> &ps,
                                       Substitutions &sub) {
    std::vector<TypeId> args;
    for (const auto &p : ps) {
      auto it = std::find_if(sub.begin(), sub.end(),
                             [&](auto x) { return x.first == p.symbol; });
      if (it == sub.end())
        reject(r,
               "cannot infer type argument; provide explicit type arguments");
      args.push_back(it->second);
    }
    check_constraints(r, ps, args);
    return args;
  }
  bool satisfies(Ref r, TypeId actual, SymbolId trait) {
    auto key = std::make_pair(actual.value, trait.value);
    if (auto it = trait_results.find(key); it != trait_results.end())
      return it->second;
    if (constraint_depth >= 48 || !active_constraints.insert(key).second)
      return false;
    ++constraint_depth;
    const auto t = type(actual);
    size_t found = 0;
    if (t.kind == TypeKind::TypeParameter) {
      found = bounds[t.declaration.value] == trait ? 1 : 0;
    } else
      for (const auto &im : m.impls)
        if (im.trait == trait) {
          Substitutions sub;
          if (!try_infer(r, im.self_type, actual, sub))
            continue;
          bool valid = true;
          for (auto p : im.params) {
            auto it = std::find_if(sub.begin(), sub.end(), [&](auto entry) {
              return entry.first == p.symbol;
            });
            if (it == sub.end() ||
                (p.trait && !satisfies(r, it->second, *p.trait))) {
              valid = false;
              break;
            }
          }
          if (valid)
            ++found;
        }
    --constraint_depth;
    active_constraints.erase(key);
    return trait_results[key] = found == 1;
  }
  void check_constraints(Ref r, const std::vector<GenericParam> &ps,
                         const std::vector<TypeId> &args) {
    for (size_t i = 0; i < ps.size(); ++i)
      if (ps[i].trait && !satisfies(r, args.at(i), *ps[i].trait))
        reject(r, "generic constraint requires one applicable explicit trait "
                  "implementation");
  }
  struct Selection {
    SymbolId method;
    Substitutions substitutions;
  };
  Selection select_method(Ref r, TypeId receiver, std::string_view name,
                          std::optional<SymbolId> required = {}) {
    std::optional<Selection> result;
    for (const auto &im : m.impls) {
      if (required && im.trait != required)
        continue;
      Substitutions sub;
      if (!try_infer(r, im.self_type, receiver, sub))
        continue;
      const auto saved_depth = constraint_depth;
      try {
        finish_arguments(r, im.params, sub);
      } catch (const Rejected &) {
        constraint_depth = saved_depth;
        continue;
      }
      for (auto method : im.methods)
        if (symbol(method).name == name) {
          // Trait methods use the trait's public interface. Inherent
          // methods have their own pub declaration.
          if (symbol(method).module != r.module &&
              !(im.trait ? symbol(*im.trait).exported
                         : symbol(method).exported))
            reject(r, "method is private", "E_PRIVATE_ACCESS");
          if (result)
            reject(r, "ambiguous method implementation");
          result = Selection{method, sub};
        }
    }
    if (!result)
      reject(r, "no matching explicit method implementation");
    return *result;
  }
  Expr constructor(Ref r, TypeId target, std::optional<SymbolId> variant = {}) {
    const auto &d = nominal(target);
    std::vector<FieldDecl> fields = d.fields;
    if (variant) {
      for (const auto &v : d.variants)
        if (v.symbol == *variant)
          fields = v.payload;
    }
    if (ast(r).children.size() - 1 != fields.size())
      reject(r, "constructor requires every field exactly once",
             "E_CALL_ARGUMENT");
    std::vector<CallArg> args;
    std::vector<HirNodeId> children;
    std::vector<bool> seen(fields.size());
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto ar = child(r, i);
      if (token(ar, 1) != ":")
        reject(ar, "constructor requires named fields", "E_CALL_ARGUMENT");
      std::string label(token(ar));
      size_t pi = 0;
      while (pi < fields.size() && symbol(fields[pi].symbol).name != label)
        ++pi;
      if (pi == fields.size() || seen[pi])
        reject(ar, "unknown or duplicate constructor field", "E_CALL_ARGUMENT");
      if (!variant && symbol(fields[pi].symbol).module != r.module &&
          !fields[pi].exported)
        reject(ar, "field is private", "E_PRIVATE_ACCESS");
      seen[pi] = true;
      auto e = expression(child(ar, 0), field_type(target, fields[pi]));
      args.push_back(
          {uint32_t(i - 1), uint32_t(pi), label, ParamMode::ordinary, e.id});
      children.push_back(e.id);
    }
    NodeData data = variant ? NodeData(EnumInit{*variant, args})
                            : NodeData(StructInit{args});
    return {node(r, target, std::move(data), std::move(children)),
            {target, {}, {}}};
  }
  Expr call(Ref r, std::optional<TypeId> expected) {
    auto callee_ref = child(r, 0);
    if (auto t = type_expression(callee_ref)) {
      if (type(*t).kind == TypeKind::NominalStruct)
        return constructor(r, *t);
      return scalar_call(r);
    }
    auto method_ref = callee_ref;
    std::vector<TypeId> method_args;
    bool method_explicit = false;
    if (ast(method_ref).kind == AstKind::NeutralBracket &&
        ast(child(method_ref, 0)).kind == AstKind::Member &&
        !occurrence(child(method_ref, 0))) {
      method_explicit = true;
      for (size_t i = 1; i < ast(method_ref).children.size(); ++i)
        method_args.push_back(parse_type(child(method_ref, i)));
      method_ref = child(method_ref, 0);
    }
    if (ast(method_ref).kind == AstKind::Member && !occurrence(method_ref)) {
      auto base = child(method_ref, 0);
      auto name = file(r).tokens[ast(method_ref).token_end - 1].spelling;
      auto method_subs = [&](Selection selected) {
        if (method_explicit) {
          std::vector<GenericParam> unbound;
          for (auto p : generic_params[selected.method.value]) {
            bool bound = false;
            for (auto [s, t] : selected.substitutions)
              if (s == p.symbol)
                bound = true;
            if (!bound)
              unbound.push_back(p);
          }
          if (unbound.size() != method_args.size())
            reject(r, "wrong number of method type arguments");
          for (size_t i = 0; i < unbound.size(); ++i)
            selected.substitutions.push_back(
                {unbound[i].symbol, method_args[i]});
        }
        return selected;
      };
      if (auto t = type_expression(base)) {
        if (type(*t).kind == TypeKind::NominalEnum) {
          for (const auto &v : nominal(*t).variants)
            if (symbol(v.symbol).name == name)
              return constructor(r, *t, v.symbol);
          reject(r, "unknown enum variant");
        }
        auto selected = method_subs(select_method(r, *t, name));
        return typed_call(r, selected.method, {}, selected.substitutions, {},
                          expected);
      }
      auto receiver = expression(base);
      if (type(receiver.value.type).kind == TypeKind::NominalStruct)
        for (const auto &f : nominal(receiver.value.type).fields)
          if (symbol(f.symbol).name == name) {
            if (method_explicit)
              reject(r, "function fields do not take type arguments",
                     "E_CALL_ARGUMENT");
            auto field = find_field(method_ref, receiver.value.type);
            auto t = field_type(receiver.value.type, field);
            Expr callee{node(method_ref, t, FieldRead{field.symbol},
                             {receiver.id}), {t, {}, {}}};
            return scalar_call(r, callee);
          }
      if (type(receiver.value.type).kind == TypeKind::TypeParameter) {
        if (method_explicit)
          unavailable(r, "generic trait methods are not implemented");
        auto bound = bounds[type(receiver.value.type).declaration.value];
        if (!bound)
          reject(r, "method requires a declared trait constraint");
        for (const auto &tr : m.traits)
          if (tr.symbol == *bound)
            for (const auto &method : tr.methods)
              if (symbol(method.symbol).name == name) {
                auto sig =
                    substitute(method.signature,
                               {{tr.self, receiver.value.type}}, m.types);
                return typed_call(r, method.symbol, sig, {}, receiver, expected,
                                  *bound);
              }
        reject(r, "method is not declared by the constraint");
      }
      auto selected = method_subs(select_method(r, receiver.value.type, name));
      return typed_call(r, selected.method, {}, selected.substitutions,
                        receiver, expected);
    }
    auto base = callee_ref;
    std::vector<TypeId> explicit_args;
    if (ast(base).kind == AstKind::NeutralBracket) {
      for (size_t i = 1; i < ast(base).children.size(); ++i)
        explicit_args.push_back(parse_type(child(base, i)));
      base = child(base, 0);
    }
    auto s = occurrence(base);
    if (s && symbol(*s).kind == modules::SymbolKind::function &&
        signatures.count(s->value)) {
      if (generic_params[s->value].empty() &&
          ast(callee_ref).kind != AstKind::NeutralBracket)
        return scalar_call(r);
      Substitutions sub;
      auto ps = generic_params[s->value];
      if (ast(callee_ref).kind == AstKind::NeutralBracket) {
        if (explicit_args.size() != ps.size())
          reject(r, "wrong number of generic arguments");
        for (size_t i = 0; i < ps.size(); ++i)
          sub.push_back({ps[i].symbol, explicit_args[i]});
      }
      return typed_call(r, *s, {}, sub, {}, expected);
    }
    return scalar_call(r);
  }
  Expr typed_call(Ref r, SymbolId target,
                  std::optional<TypeId> signature_override, Substitutions sub,
                  std::optional<Expr> receiver, std::optional<TypeId> expected,
                  std::optional<SymbolId> constraint = {}) {
    auto original = signature_override.value_or(signatures.at(target.value));
    auto original_type = type(original);
    if (expected)
      infer(r, original_type.result, *expected, sub);
    const auto ps = parameters.at(target.value);
    size_t offset = receiver ? 1 : 0;
    if (ast(r).children.size() - 1 + offset != ps.size())
      reject(r, "wrong number of arguments", "E_CALL_ARGUMENT");
    std::vector<CallArg> args;
    std::vector<HirNodeId> children;
    std::vector<bool> seen(ps.size());
    std::vector<std::optional<Expr>> values(ast(r).children.size() - 1);
    std::vector<size_t> indices(values.size());
    std::vector<std::optional<std::string>> labels(values.size());
    if (receiver) {
      infer(r, original_type.parameters.at(0).type, receiver->value.type, sub);
      args.push_back({0, 0, {}, ParamMode::ordinary, receiver->id});
      children.push_back(receiver->id);
      seen[0] = true;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      auto ar = child(r, i + 1);
      size_t pi = i + offset;
      if (token(ar, 1) == ":") {
        labels[i] = std::string(token(ar));
        pi = 0;
        while (pi < ps.size() && ps[pi].label != *labels[i])
          ++pi;
      }
      if (pi >= ps.size() || seen[pi])
        reject(ar, "unknown or duplicate argument", "E_CALL_ARGUMENT");
      seen[pi] = true;
      indices[i] = pi;
    }
    for (size_t pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i < values.size(); ++i) {
        auto ar = child(r, i + 1), er = child(ar, 0);
        if (values[i] || (pass == 0 && contextual(er)))
          continue;
        auto pt =
            substitute(original_type.parameters[indices[i]].type, sub, m.types);
        auto pm = original_type.parameters[indices[i]].mode;
        if (pm != ParamMode::ordinary)
          unavailable(
              ar, "generic/method inout and move calls are not implemented");
        auto e = expression(er, contains_parameter(pt) ? std::nullopt
                                                       : std::optional(pt));
        infer(ar, original_type.parameters[indices[i]].type, e.value.type, sub);
        values[i] = e;
      }
    std::vector<TypeId> type_args;
    if (!constraint)
      type_args = finish_arguments(r, generic_params[target.value], sub);
    auto sig = substitute(original, sub, m.types);
    auto ft = type(sig);
    for (size_t i = 0; i < values.size(); ++i) {
      auto e = *values[i];
      match(r, e.value.type, ft.parameters[indices[i]].type);
      args.push_back({uint32_t(i + offset), uint32_t(indices[i]), labels[i],
                      ParamMode::ordinary, e.id});
      children.push_back(e.id);
    }
    if (constraint)
      return {node(r, ft.result,
                   ConstraintCall{*constraint, target, receiver->value.type,
                                  sig, args},
                   children),
              {ft.result, {}, {}}};
    Call c;
    c.kind = CalleeKind::direct;
    c.signature = sig;
    c.direct_symbol = target;
    c.args = std::move(args);
    c.type_args = std::move(type_args);
    return {node(r, ft.result, std::move(c), children), {ft.result, {}, {}}};
  }

  Expr scalar_call(Ref r, std::optional<Expr> checked_callee = {}) {
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
      auto callee = checked_callee ? *checked_callee : expression(callee_ref);
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
    if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data)) {
      c.function = f->symbol;
      c.type_args = f->type_args;
    }
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
  Pattern pattern(Ref r, TypeId t) {
    Pattern p{t, source(r), {}, {}, {}};
    if (ast(r).kind == AstKind::BindPattern) {
      if (token(r) != "_") {
        auto s = definition(r);
        p.binding = s;
        locals[s.value] = {t, false};
      }
      return p;
    }
    if (ast(r).kind != AstKind::VariantPattern)
      unavailable(
          r,
          "only enum variant, binding and wildcard patterns are implemented");
    if (type(t).kind != TypeKind::NominalEnum)
      reject(r, "variant pattern requires an enum");
    auto base = occurrence(r);
    if (!base || *base != type(t).declaration)
      reject(r, "pattern belongs to a different enum");
    std::string_view name;
    unsigned depth = 0;
    for (size_t i = ast(r).token_begin; i < ast(r).token_end; ++i) {
      auto tok = file(r).tokens[i].spelling;
      if (tok == "(" && !depth)
        break;
      if (tok == "[")
        ++depth;
      if (tok == "]")
        --depth;
      if (tok == "." && !depth)
        name = file(r).tokens[i + 1].spelling;
    }
    const VariantDecl *v = nullptr;
    for (const auto &candidate : nominal(t).variants)
      if (symbol(candidate.symbol).name == name)
        v = &candidate;
    if (!v)
      reject(r, "unknown enum pattern variant");
    p.variant = v->symbol;
    size_t pi = 0, ti = 0;
    for (auto id : ast(r).children) {
      auto cr = at(r, id);
      if (ast(cr).kind == AstKind::NamedType ||
          ast(cr).kind == AstKind::FunctionType ||
          ast(cr).kind == AstKind::ParenthesizedType) {
        if (ti >= type(t).type_args.size() ||
            parse_type(cr) != type(t).type_args[ti++])
          reject(cr, "enum pattern type arguments mismatch");
        continue;
      }
      if (pi >= v->payload.size())
        reject(r, "wrong variant pattern payload count");
      p.payload.push_back(pattern(cr, field_type(t, v->payload[pi++])));
    }
    if (pi != v->payload.size())
      reject(r, "wrong variant pattern payload count");
    return p;
  }
  Statement switch_statement(Ref r) {
    auto value = expression(child(r, 0));
    if (type(value.value.type).kind != TypeKind::NominalEnum)
      unavailable(r, "only enum switch is implemented in T07");
    auto id = node(r, unit, Switch{});
    break_targets.push_back(id);
    std::vector<HirNodeId> children{value.id};
    std::vector<Arm> arms;
    std::set<uint64_t> covered;
    bool wildcard = false, falls = false;
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto cr = child(r, i);
      auto saved = scope;
      scope = new_scope(cr, saved);
      if (wildcard)
        reject(cr, "unreachable switch arm after exhaustive pattern");
      bool def = token(cr) == "default";
      Pattern p = def ? Pattern{value.value.type, source(cr), {}, {}, {}}
                      : pattern(child(cr, 0), value.value.type);
      for (auto c : ast(cr).children) {
        if (ast(at(cr, c)).kind == AstKind::VariantPattern)
          for (auto payload : ast(at(cr, c)).children)
            if (ast(at(cr, payload)).kind == AstKind::VariantPattern)
              unavailable(at(cr, payload),
                          "nested enum variant patterns are not implemented");
      }
      bool total_payload = true;
      std::function<void(const Pattern &)> total = [&](const Pattern &pat) {
        if (pat.variant)
          total_payload = false;
        for (const auto &c : pat.payload)
          total(c);
      };
      for (const auto &c : p.payload)
        total(c);
      if (p.variant) {
        if (covered.count(p.variant->value))
          reject(cr, "unreachable repeated enum variant");
        if (total_payload)
          covered.insert(p.variant->value);
      } else
        wildcard = true;
      std::vector<HirNodeId> statements;
      bool arm_falls = true, arm_breaks = false;
      for (size_t j = def ? 0 : 1; j < ast(cr).children.size(); ++j) {
        auto st = statement(child(cr, j));
        statements.push_back(st.id);
        if (arm_falls) {
          arm_breaks |= st.breaks;
          arm_falls = st.falls;
        }
      }
      auto body = node(cr, unit, Block{}, statements);
      arms.push_back({std::move(p), scope, body, source(cr)});
      children.push_back(body);
      scope = saved;
      falls |= arm_falls || arm_breaks;
    }
    bool exhaustive =
        wildcard || covered.size() == nominal(value.value.type).variants.size();
    if (!exhaustive)
      reject(r, "enum switch is not exhaustive");
    m.nodes[id.value].children = std::move(children);
    m.nodes[id.value].data = Switch{std::move(arms), exhaustive};
    break_targets.pop_back();
    return {id, falls, false};
  }
  Statement statement(Ref r) {
    check_cancelled(stop);
    const auto &n = ast(r);
    switch (n.kind) {
    case AstKind::Block:
      return block(r);
    case AstKind::Switch:
      return switch_statement(r);
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
        if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data)) {
          c.function = f->symbol;
          c.type_args = f->type_args;
        }
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
        bool floating =
            type(p.value.type).kind == TypeKind::Float &&
            (*compound == BinaryOp::add || *compound == BinaryOp::subtract ||
             *compound == BinaryOp::multiply || *compound == BinaryOp::divide);
        if (!floating && (!integer(p.value.type) || !integer(rhs.value.type)))
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
      break_targets.push_back(id);
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
      break_targets.pop_back();
      scope = saved;
      bool forever =
          std::holds_alternative<BoolLiteral>(m.nodes[cond.id.value].data) &&
          std::get<BoolLiteral>(m.nodes[cond.id.value].data).value;
      return {id, !forever || body.breaks, false};
    }
    case AstKind::Break:
    case AstKind::Continue:
      if (n.kind == AstKind::Break) {
        if (break_targets.empty())
          reject(r, "break has no enclosing loop or switch");
        return {node(r, unit, Break{break_targets.back()}), false, true};
      }
      if (loops.empty())
        reject(r, "continue has no enclosing loop");
      return {node(r, unit, Continue{loops.back()}), false, false};
    default:
      unavailable(r, "switch/aggregate, typed errors, cleanup, or ownership "
                     "statement requires a later task");
    }
  }

  SymbolId self_parameter(Ref r) const {
    for (auto [owner, s] : m.resolved->self_parameters)
      if (key(owner) == key(r))
        return s;
    throw std::invalid_argument("missing resolver Self owner");
  }
  void register_function(Ref r, std::vector<GenericParam> inherited,
                         bool prototype = false) {
    auto id = definition(r);
    auto own = generics(r);
    inherited.insert(inherited.end(), own.begin(), own.end());
    generic_params[id.value] = inherited;
    signatures[id.value] = signature(r, true);
    for (auto c : ast(r).children)
      if (ast(at(r, c)).kind == AstKind::Parameter) {
        auto p = definition(at(r, c));
        parameters[id.value].push_back(
            {p, symbol(p).name == "_" ? "" : symbol(p).name});
      }
    parameters.try_emplace(id.value);
    if (prototype)
      return;
    bodies[id.value] = r;
    std::function<void(Ref)> nested = [&](Ref cr) {
      if (ast(cr).kind == AstKind::FunctionLiteral) {
        register_function(cr, inherited);
        return;
      }
      for (auto c : ast(cr).children)
        nested(at(cr, c));
    };
    for (auto c : ast(r).children)
      if (ast(at(r, c)).kind == AstKind::Block)
        nested(at(r, c));
  }
  bool overlap(TypeId a, TypeId b) {
    return type_patterns_overlap(a, b, m.types);
  }
  void declarations() {
    std::vector<Ref> top;
    for (const auto &mod : m.resolved->project->modules) {
      if (!mod.foreign_exports.empty() || mod.files.empty()) {
        for (const auto &owner : m.resolved->project->modules)
          for (const auto &im : owner.imports)
            if (im.target == mod.id)
              unavailable({owner.id, im.file, im.node},
                          "foreign declarations require T10/T11");
        throw std::invalid_argument("foreign module has no importing source");
      }
      for (size_t f = 0; f < mod.files.size(); ++f)
        for (auto id : mod.files[f]->node(mod.files[f]->root).children) {
          Ref r{mod.id, f, id};
          top.push_back(r);
          if (ast(r).kind == AstKind::Struct || ast(r).kind == AstKind::Enum) {
            auto s = definition(r);
            nominal_ids[s.value] = m.nominals.size();
            m.nominals.push_back({s,
                                  ast(r).kind == AstKind::Struct
                                      ? TypeKind::NominalStruct
                                      : TypeKind::NominalEnum,
                                  generics(r),
                                  {},
                                  {},
                                  source(r)});
          }
        }
    }
    for (auto r : top)
      if (ast(r).kind == AstKind::Trait) {
        if (!generics(r).empty())
          unavailable(r, "generic traits are not implemented");
        auto self = self_parameter(r);
        self_types[self.value] = parameter_type(self);
        bounds[self.value] = definition(r);
        TraitDecl tr{definition(r), self, {}, source(r)};
        for (auto id : ast(r).children)
          if (ast(at(r, id)).kind == AstKind::Function) {
            auto cr = at(r, id);
            register_function(cr, {}, true);
            auto method = definition(cr);
            if (!generic_params[method.value].empty())
              unavailable(cr, "generic trait methods are not implemented");
            tr.methods.push_back({method, signatures.at(method.value),
                                  parameters.at(method.value), source(cr)});
          }
        m.traits.push_back(std::move(tr));
      }
    auto field_value_type = [&](Ref field) {
      auto t = parse_type(child(field, 0));
      if (type(t).kind == TypeKind::String)
        unavailable(field, "string aggregate fields require owned-value support");
      return t;
    };
    for (auto &d : m.nominals) {
      auto r = d.source.node;
      for (auto c : ast(r).children) {
        auto cr = at(r, c);
        if (ast(cr).kind == AstKind::Field)
          d.fields.push_back({definition(cr), field_value_type(cr),
                              token(cr) == "pub", source(cr)});
        if (ast(cr).kind == AstKind::Variant) {
          VariantDecl v{definition(cr), {}, source(cr)};
          for (auto f : ast(cr).children) {
            auto fr = at(cr, f);
            v.payload.push_back(
                {definition(fr), field_value_type(fr), true, source(fr)});
          }
          d.variants.push_back(std::move(v));
        }
      }
    }
    for (auto r : top)
      if (ast(r).kind == AstKind::Impl) {
        auto ps = generics(r);
        std::vector<Ref> ts;
        for (auto c : ast(r).children)
          if (ast(at(r, c)).kind == AstKind::NamedType)
            ts.push_back(at(r, c));
        std::optional<SymbolId> trait;
        if (ts.size() == 2) {
          trait = occurrence(ts[0]);
          if (!trait || symbol(*trait).kind != modules::SymbolKind::trait)
            reject(ts[0], "impl requires a nominal trait");
        }
        auto self = parse_type(ts.back());
        auto sp = self_parameter(r);
        self_types[sp.value] = self;
        if (type(self).kind != TypeKind::NominalStruct &&
            type(self).kind != TypeKind::NominalEnum && !integer(self) &&
            type(self).kind != TypeKind::Float &&
            type(self).kind != TypeKind::Bool)
          unavailable(r, "impl target is not an implemented value type");
        ImplDecl im{ps, trait, sp, self, {}, source(r)};
        for (auto c : ast(r).children)
          if (ast(at(r, c)).kind == AstKind::Function) {
            auto cr = at(r, c);
            register_function(cr, ps);
            im.methods.push_back(definition(cr));
          }
        for (const auto &previous : m.impls)
          if (previous.trait == im.trait &&
              overlap(previous.self_type, im.self_type))
            reject(r, "duplicate or overlapping impl");
        m.impls.push_back(std::move(im));
      }
    for (const auto &im : m.impls)
      if (im.trait) {
        const TraitDecl *tr = nullptr;
        for (const auto &t : m.traits)
          if (t.symbol == *im.trait)
            tr = &t;
        if (!tr)
          reject(im.source.node, "unknown trait implementation");
        if (im.methods.size() != tr->methods.size())
          reject(im.source.node,
                 "impl must define every trait method exactly once");
        for (const auto &proto : tr->methods) {
          std::optional<SymbolId> method;
          for (auto id : im.methods)
            if (symbol(id).name == symbol(proto.symbol).name)
              method = id;
          if (!method)
            reject(im.source.node, "impl is missing a trait method");
          auto expected =
              substitute(proto.signature, {{tr->self, im.self_type}}, m.types);
          if (signatures.at(method->value) != expected)
            reject(im.source.node,
                   "impl method signature does not match trait");
          auto ps = parameters.at(method->value);
          for (size_t i = 0; i < ps.size(); ++i)
            if (ps[i].label != proto.parameters[i].label)
              reject(im.source.node, "impl method labels do not match trait");
        }
      }
    for (auto r : top)
      if (ast(r).kind == AstKind::Function)
        register_function(r, {});
    std::function<void(Ref)> const_functions = [&](Ref r) {
      if (ast(r).kind == AstKind::FunctionLiteral) {
        register_function(r, {});
        return;
      }
      for (auto c : ast(r).children)
        const_functions(at(r, c));
    };
    for (auto r : top)
      if (ast(r).kind == AstKind::Binding)
        const_functions(r);
  }
  void check_body(SymbolId id) {
    function = id;
    auto r = bodies.at(id.value);
    auto sig = signatures.at(id.value);
    locals.clear();
    loops.clear();
    break_targets.clear();
    active_params = generic_params[id.value];
    scope = new_scope(r, {});
    auto ps = parameters.at(id.value);
    auto ft = type(sig);
    for (size_t i = 0; i < ps.size(); ++i)
      locals[ps[i].symbol.value] = {
          ft.parameters[i].type, ft.parameters[i].mode == ParamMode::inout,
          ft.parameters[i].mode == ParamMode::ordinary};
    auto body = block(child(r, ast(r).children.size() - 1), true);
    if (ft.result != unit && body.falls)
      reject(r, "non-Unit function has a reachable normal exit without return");
    HirFunction f{id, sig, ps, body.id, scope, source(r)};
    if (active_params.empty())
      m.functions.push_back(f);
    else {
      template_ids[id.value] = m.templates.size();
      m.templates.push_back({f, active_params});
    }
  }
  SymbolId duplicate_symbol(SymbolId origin) {
    auto s = symbol(origin);
    s.id = SymbolId{owned->symbols.size()};
    owned->symbols.push_back(s);
    return s.id;
  }
  void transform_node(HirNodeId id, size_t depth) {
    if (depth > 512)
      throw std::invalid_argument("HIR transform nesting limit");
    auto n = m.nodes.at(id.value);
    if (auto c = std::get_if<Call>(&n.data);
        c && c->direct_symbol && template_ids.count(c->direct_symbol->value)) {
      auto original = *c->direct_symbol;
      auto target = instantiate(original, c->type_args, n.source.node);
      c->direct_symbol = target;
      c->type_args.clear();
    }
    if (auto v = std::get_if<FunctionValue>(&n.data);
        v && template_ids.count(v->symbol.value)) {
      v->symbol = instantiate(v->symbol, v->type_args, n.source.node);
      v->type_args.clear();
    }
    if (auto c = std::get_if<ConstraintCall>(&n.data)) {
      auto name = symbol(c->method).name;
      auto selected = select_method(n.source.node, c->receiver, name, c->trait);
      auto args =
          finish_arguments(n.source.node, generic_params[selected.method.value],
                           selected.substitutions);
      auto target = selected.method;
      if (template_ids.count(target.value))
        target = instantiate(target, args, n.source.node);
      Call concrete;
      concrete.kind = CalleeKind::direct;
      concrete.signature = c->signature;
      concrete.direct_symbol = target;
      concrete.args = c->args;
      n.data = std::move(concrete);
    }
    m.nodes.at(id.value) = n;
    for (auto ch : n.children)
      transform_node(ch, depth + 1);
  }
  SymbolId instantiate(SymbolId origin, const std::vector<TypeId> &args,
                       Ref use) {
    const auto temp = m.templates.at(template_ids.at(origin.value));
    if (args.size() != temp.params.size())
      throw std::invalid_argument("instance type argument count");
    check_constraints(use, temp.params, args);
    std::vector<uint64_t> ids;
    for (auto t : args) {
      if (contains_parameter(t))
        throw std::invalid_argument("nonconcrete instance request");
      ids.push_back(t.value);
    }
    auto key = std::make_pair(origin.value, ids);
    if (auto it = instance_cache.find(key); it != instance_cache.end())
      return it->second;
    if (instance_cache.size() >= 128 || instantiation_depth >= 48)
      reject(use, "generic instantiation budget exceeded",
             "E_INSTANTIATION_LIMIT");
    ++instantiation_depth;
    Substitutions sub;
    for (size_t i = 0; i < args.size(); ++i)
      sub.push_back({temp.params[i].symbol, args[i]});
    auto f = temp.function;
    const auto result = duplicate_symbol(origin);
    instance_cache.emplace(key, result);
    std::map<uint64_t, SymbolId> renamed;
    renamed[origin.value] = result;
    auto rename = [&](SymbolId s) {
      auto it = renamed.find(s.value);
      if (it != renamed.end())
        return it->second;
      return renamed[s.value] = duplicate_symbol(s);
    };
    f.symbol = result;
    f.signature = substitute(f.signature, sub, m.types);
    for (auto &p : f.parameters)
      p.symbol = rename(p.symbol);
    std::map<uint64_t, ScopeId> scopes;
    std::function<ScopeId(ScopeId)> clone_scope = [&](ScopeId old) {
      if (auto it = scopes.find(old.value); it != scopes.end())
        return it->second;
      auto sc = m.scopes.at(old.value);
      sc.function = result;
      if (sc.parent)
        sc.parent = clone_scope(*sc.parent);
      ScopeId sid{m.scopes.size()};
      m.scopes.push_back(sc);
      scopes[old.value] = sid;
      return sid;
    };
    std::map<uint64_t, HirNodeId> nodes;
    std::function<void(Pattern &)> clone_pattern = [&](Pattern &p) {
      p.type = substitute(p.type, sub, m.types);
      if (p.binding)
        p.binding = rename(*p.binding);
      for (auto &c : p.payload)
        clone_pattern(c);
    };
    std::function<HirNodeId(HirNodeId)> clone = [&](HirNodeId old) {
      if (++instantiated_nodes > 200000)
        reject(use, "generic instantiated node budget exceeded",
               "E_INSTANTIATION_LIMIT");
      auto n = m.nodes.at(old.value);
      n.type = substitute(n.type, sub, m.types);
      n.scope = clone_scope(n.scope);
      if (auto b = std::get_if<Binding>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Read>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Place>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Return>(&n.data))
        b->function = result;
      HirNodeId id{m.nodes.size()};
      m.nodes.push_back(n);
      nodes[old.value] = id;
      for (auto &c : n.children)
        c = clone(c);
      auto update_args = [&](auto &as) {
        for (auto &a : as)
          a.expression = nodes.at(a.expression.value);
      };
      if (auto c = std::get_if<Call>(&n.data)) {
        c->signature = substitute(c->signature, sub, m.types);
        for (auto &t : c->type_args)
          t = substitute(t, sub, m.types);
        if (c->callee)
          c->callee = nodes.at(c->callee->value);
        update_args(c->args);
      }
      if (auto v = std::get_if<FunctionValue>(&n.data))
        for (auto &t : v->type_args)
          t = substitute(t, sub, m.types);
      if (auto c = std::get_if<ConstraintCall>(&n.data)) {
        c->signature = substitute(c->signature, sub, m.types);
        c->receiver = substitute(c->receiver, sub, m.types);
        update_args(c->args);
      }
      if (auto c = std::get_if<StructInit>(&n.data))
        update_args(c->fields);
      if (auto c = std::get_if<EnumInit>(&n.data))
        update_args(c->payload);
      if (auto c = std::get_if<Switch>(&n.data))
        for (auto &a : c->arms) {
          clone_pattern(a.pattern);
          a.scope = scopes.at(a.scope.value);
          a.body = nodes.at(a.body.value);
        }
      if (auto b = std::get_if<Break>(&n.data))
        b->loop = nodes.at(b->loop.value);
      if (auto b = std::get_if<Continue>(&n.data))
        b->loop = nodes.at(b->loop.value);
      m.nodes[id.value] = std::move(n);
      return id;
    };
    f.scope = clone_scope(f.scope);
    f.body = clone(f.body);
    FunctionInstance instance{result, origin, args, {}, m.config};
    for (auto [old, s] : renamed)
      instance.renamed.push_back({SymbolId{old}, s});
    m.instances.push_back(std::move(instance));
    m.functions.push_back(f);
    // Identity and signature are present before resolving recursive uses.
    transform_node(f.body, 0);
    --instantiation_depth;
    return result;
  }

public:
  Checker(std::shared_ptr<const modules::ResolvedProject> r, Config config,
          std::stop_token s)
      : stop(s) {
    owned = std::make_shared<modules::ResolvedProject>(*r);
    m.resolved = owned;
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
    declarations();
    for (const auto &mod : m.resolved->project->modules)
      for (size_t f = 0; f < mod.files.size(); ++f)
        for (auto id : mod.files[f]->node(mod.files[f]->root).children) {
          Ref r{mod.id, f, id};
          if (ast(r).kind == AstKind::Binding)
            constant(definition(r));
        }
    for (auto [id, r] : bodies) {
      check_cancelled(stop);
      check_body(SymbolId{id});
    }
    auto concrete = m.functions;
    for (const auto &f : concrete)
      transform_node(f.body, 0);
    const auto type_roots = m.types.size();
    for (size_t i = 1; i <= type_roots; ++i) {
      auto t = TypeId{i};
      if (type(t).kind == TypeKind::NominalStruct ||
          type(t).kind == TypeKind::NominalEnum) {
        auto d = nominal(t);
        check_constraints(d.source.node, d.params, type(t).type_args);
      }
    }
    if (auto infinite = infinite_value_layout(m, stop))
      reject(m.nominals.at(nominal_ids.at(infinite->value)).source.node,
             "value type has an infinite recursive layout");
    return std::move(m);
  }
};
template <class T> PhaseResult<T> internal(std::string message) {
  PhaseResult<T> r;
  Diagnostic d;
  d.code = "E_INTERNAL_ERROR";
  d.phase = "type";
  d.severity = "error";
  d.message = std::move(message);
  r.diagnostics.push_back(std::move(d));
  return r;
}
} // namespace
PhaseResult<HirModule>
check_types(std::shared_ptr<const modules::ResolvedProject> resolved,
            std::optional<CheckConfig> config, std::stop_token stop) {
  try {
    check_cancelled(stop);
    if (!resolved || !resolved->project)
      return internal<HirModule>(
          "checker requires an owning successfully resolved project");
    const auto &p = *resolved->project;
    if (p.backend != "native" && p.backend != "python")
      return internal<HirModule>("invalid project backend");
    Config c{p.backend == "native" ? Backend::native : Backend::python,
             p.word_bits};
    if (config && *config != c)
      return internal<HirModule>("CheckConfig does not match Project target");
    if (c.word_bits != 32 && c.word_bits != 64)
      return internal<HirModule>("target word_bits must be 32 or 64");
    Checker checker(std::move(resolved), c, stop);
    auto m = checker.run();
    auto verified = hir::verify_types(m, stop);
    PhaseResult<HirModule> result;
    result.state = verified.state;
    result.failure = verified.failure;
    result.diagnostics = std::move(verified.diagnostics);
    if (verified.success())
      result.value = std::move(m);
    return result;
  } catch (const Cancelled &) {
    PhaseResult<HirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  } catch (const Rejected &e) {
    PhaseResult<HirModule> r;
    r.failure = e.kind;
    r.diagnostics = {e.diagnostic};
    return r;
  } catch (const std::exception &e) {
    return internal<HirModule>(e.what());
  }
}
PhaseResult<HirModule>
check_types(const PhaseResult<modules::ResolvedProject> &resolved,
            std::optional<CheckConfig> config, std::stop_token stop) {
  if (stop.stop_requested()) {
    PhaseResult<HirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  }
  if (!resolved.success()) {
    PhaseResult<HirModule> r;
    r.state = resolved.state;
    r.failure = resolved.failure;
    r.diagnostics = resolved.diagnostics;
    if (r.state == PhaseState::complete)
      r.state = PhaseState::failed;
    if (r.diagnostics.empty() && r.state != PhaseState::cancelled)
      return internal<HirModule>("resolve did not successfully complete");
    return r;
  }
  return check_types(
      std::make_shared<const modules::ResolvedProject>(*resolved.value), config,
      stop);
}
PhaseResult<VerifiedHirModule>
check_project(std::shared_ptr<const modules::ResolvedProject> r,
              std::optional<CheckConfig> c, std::stop_token stop) {
  auto typed = check_types(std::move(r), c, stop);
  if (typed.success())
    return hir::verify(*typed.value, stop);
  PhaseResult<VerifiedHirModule> result;
  result.state = typed.state;
  result.failure = typed.failure;
  result.diagnostics = std::move(typed.diagnostics);
  return result;
}
PhaseResult<VerifiedHirModule>
check_project(const PhaseResult<modules::ResolvedProject> &r,
              std::optional<CheckConfig> c, std::stop_token stop) {
  auto typed = check_types(r, c, stop);
  if (typed.success())
    return hir::verify(*typed.value, stop);
  PhaseResult<VerifiedHirModule> result;
  result.state = typed.state;
  result.failure = typed.failure;
  result.diagnostics = std::move(typed.diagnostics);
  return result;
}
} // namespace lang::sema

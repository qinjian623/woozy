#include "constant_value.hpp"
#include "literal_value.hpp"
#include <algorithm>
#include <set>
#include <tuple>
namespace lang::hir {
bool same_constant_value(const ConstantValue &a, const ConstantValue &b) {
  if (a.type != b.type || a.data.index() != b.data.index())
    return false;
  return std::visit(
      [&](const auto &x) {
        using T = std::decay_t<decltype(x)>;
        const auto &y = std::get<T>(b.data);
        if constexpr (std::is_same_v<T, UnitLiteral>)
          return true;
        else if constexpr (std::is_same_v<T, BoolLiteral>)
          return x.value == y.value;
        else if constexpr (std::is_same_v<T, IntegerLiteral>)
          return x.bits == y.bits && x.decimal == y.decimal;
        else if constexpr (std::is_same_v<T, FloatLiteral>)
          return x.bits == y.bits;
        else if constexpr (std::is_same_v<T, StringLiteral>)
          return x.utf8 == y.utf8;
        else if constexpr (std::is_same_v<T, FunctionValue>)
          return x.symbol == y.symbol && x.type_args == y.type_args;
        else {
          if (x.variant != y.variant || x.fields.size() != y.fields.size())
            return false;
          for (size_t i = 0; i < x.fields.size(); ++i)
            if (!same_constant_value(x.fields[i], y.fields[i]))
              return false;
          return true;
        }
      },
      a.data);
}
void transform_constant(ConstantValue &v, const Substitutions &s,
                        TypeContext &types,
                        const std::function<void(FunctionValue &)> &f) {
  v.type = substitute(v.type, s, types);
  if (auto p = std::get_if<FunctionValue>(&v.data)) {
    for (auto &t : p->type_args)
      t = substitute(t, s, types);
    f(*p);
  }
  if (auto p = std::get_if<ConstantAggregate>(&v.data))
    for (auto &x : p->fields)
      transform_constant(x, s, types, f);
}
namespace {
using Ref = modules::NodeRef;
struct Number {
  mpq_class q;
  bool negative = false;
  int special = 0;
}; // 1=Inf,2=NaN
Number number(uint64_t bits, unsigned w) {
  const unsigned frac = w == 32 ? 23 : 52, exp = w == 32 ? 8 : 11;
  const int bias = w == 32 ? 127 : 1023;
  Number n;
  n.negative = bits >> (w - 1);
  auto e = (bits >> frac) & ((1ULL << exp) - 1),
       m = bits & ((1ULL << frac) - 1);
  if (e == ((1ULL << exp) - 1)) {
    n.special = m ? 2 : 1;
    return n;
  }
  mpz_class sig(std::to_string(m));
  if (e)
    sig += mpz_class(1) << frac;
  int shift = (e ? int(e) - bias : 1 - bias) - int(frac);
  n.q = shift >= 0 ? mpq_class(sig << shift)
                   : mpq_class(sig, mpz_class(1) << -shift);
  n.q.canonicalize();
  if (n.negative)
    n.q = -n.q;
  return n;
}
bool integral(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::UnsignedInt ||
         k == TypeKind::Isize || k == TypeKind::Usize;
}
bool signed_(TypeKind k) {
  return k == TypeKind::SignedInt || k == TypeKind::Isize;
}
class SourceConstant {
  const HirModule &m;
  TypeContext &types;
  const std::function<TypeId(Ref)> &parse;
  const std::function<FunctionValue(Ref, TypeId)> &function;
  std::stop_token stop;
  size_t work = 0;
  std::set<std::tuple<uint64_t, size_t, uint64_t>> active;
  [[noreturn]] void bad(const char *s) const { throw LiteralError(s); }
  const AstFile &file(Ref r) const {
    return *m.resolved->project->module(r.module).files.at(r.file);
  }
  const AstNode &ast(Ref r) const { return file(r).node(r.node); }
  Ref child(Ref r, size_t i) const {
    r.node = ast(r).children.at(i);
    return r;
  }
  std::optional<SymbolId> symbol(Ref r) const {
    std::optional<SymbolId> out;
    for (auto &o : m.resolved->occurrences)
      if (o.source.module == r.module && o.source.file == r.file &&
          o.source.node == r.node)
        out = o.symbol;
    return out;
  }
  TypeId scalar(TypeKind k, unsigned bits = 0) {
    Type t;
    t.kind = k;
    t.bits = bits;
    return types.intern(t);
  }
  const Type &type(TypeId t) const { return *types.find(t); }
  const NominalDecl &nominal(TypeId t) const {
    for (auto &d : m.nominals)
      if (d.symbol == type(t).declaration)
        return d;
    bad("constant nominal declaration missing");
  }
  TypeId field_type(TypeId t, const FieldDecl &f) {
    Substitutions sub;
    auto args = type(t).type_args;
    auto &d = nominal(t);
    for (size_t i = 0; i < args.size(); ++i)
      sub.push_back({d.params.at(i).symbol, args[i]});
    return substitute(f.type, sub, types);
  }
  ConstantValue integer(mpz_class z, TypeId t, bool wrap = false) {
    if (!integral(type(t).kind))
      bad("constant integer type");
    unsigned w = bit_width(type(t), m.config);
    mpz_class mod = mpz_class(1) << w;
    if (wrap) {
      z %= mod;
      if (z < 0)
        z += mod;
      if (signed_(type(t).kind) && z >= (mod >> 1))
        z -= mod;
    }
    mpz_class low =
                  signed_(type(t).kind) ? mpz_class(-(mod >> 1)) : mpz_class(0),
              high = signed_(type(t).kind) ? mpz_class(mod >> 1) : mod;
    if (z < low || z >= high)
      bad("constant integer out of range");
    auto u = z;
    if (u < 0)
      u += mod;
    return {t, IntegerLiteral{z.get_str(), u.get_ui()}};
  }
  uint64_t round(const Number &n, TypeId t) {
    unsigned w = type(t).bits;
    uint64_t sign = uint64_t(n.negative) << (w - 1),
             inf = w == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    if (n.special == 2)
      return inf | (1ULL << (w == 32 ? 22 : 51));
    if (n.special)
      return sign | inf;
    mpq_class q = n.q < 0 ? -n.q : n.q;
    return exact_ratio_to_ieee(n.q < 0 || (n.q == 0 && n.negative), q.get_num(),
                               q.get_den(), w);
  }

public:
  ConstantValue convert(ConstantValue a, TypeId to) {
    auto from = type(a.type), dest = type(to);
    if (integral(dest.kind)) {
      if (auto p = std::get_if<IntegerLiteral>(&a.data))
        return integer(mpz_class(p->decimal), to);
      if (auto p = std::get_if<FloatLiteral>(&a.data)) {
        auto q = number(p->bits, from.bits);
        if (q.special)
          bad("non-finite constant integer conversion");
        mpz_class z;
        mpz_tdiv_q(z.get_mpz_t(), q.q.get_num_mpz_t(), q.q.get_den_mpz_t());
        return integer(z, to);
      }
    }
    if (dest.kind == TypeKind::Float) {
      if (auto p = std::get_if<IntegerLiteral>(&a.data)) {
        Number n;
        n.q = mpq_class(mpz_class(p->decimal));
        return {to, FloatLiteral{round(n, to)}};
      }
      if (auto p = std::get_if<FloatLiteral>(&a.data))
        return {to, FloatLiteral{from.bits == dest.bits
                                     ? p->bits
                                     : round(number(p->bits, from.bits), to)}};
    }
    bad("unsupported constant conversion");
  }
  bool contextual(Ref r) const {
    const auto &n = ast(r);
    if (n.kind == AstKind::Parenthesized)
      return contextual(child(r, 0));
    if (n.kind == AstKind::Unary) {
      auto op = file(r).tokens.at(n.token_begin).spelling;
      return (op == "+" || op == "-") && contextual(child(r, 0));
    }
    if (n.kind == AstKind::Literal) {
      auto k = file(r).tokens.at(n.token_begin).kind;
      return k == TokenKind::integer || k == TokenKind::floating;
    }
    return n.kind == AstKind::Binary && contextual(child(r, 0)) &&
           contextual(child(r, 1));
  }
  ConstantValue binary(std::string_view op, ConstantValue a, ConstantValue b) {
    auto boolean = scalar(TypeKind::Bool);
    bool cmp = op == "==" || op == "!=" || op == "<" || op == "<=" ||
               op == ">" || op == ">=";
    if (a.type != b.type && op != "<<" && op != ">>")
      bad("constant operand type mismatch");
    auto compare = [&](int c) {
      return op == "=="   ? c == 0
             : op == "!=" ? c != 0
             : op == "<"  ? c < 0
             : op == "<=" ? c <= 0
             : op == ">"  ? c > 0
                          : c >= 0;
    };
    if (auto p = std::get_if<BoolLiteral>(&a.data)) {
      bool q = std::get<BoolLiteral>(b.data).value;
      if (op == "&&" || op == "||")
        return {boolean,
                BoolLiteral{op == "&&" ? p->value && q : p->value || q}};
      if (op == "==" || op == "!=")
        return {boolean,
                BoolLiteral{op == "==" ? p->value == q : p->value != q}};
      bad("constant bool operator");
    }
    if (auto p = std::get_if<IntegerLiteral>(&a.data)) {
      mpz_class x(p->decimal), y(std::get<IntegerLiteral>(b.data).decimal), z;
      if (cmp)
        return {boolean,
                BoolLiteral{compare(mpz_cmp(x.get_mpz_t(), y.get_mpz_t()))}};
      if (op == "+")
        z = x + y;
      else if (op == "-")
        z = x - y;
      else if (op == "*")
        z = x * y;
      else if (op == "/" || op == "%") {
        if (y == 0)
          bad("constant operation would Panic");
        if (op == "/")
          mpz_tdiv_q(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
        else
          mpz_tdiv_r(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
      } else if (op == "&")
        z = x & y;
      else if (op == "|")
        z = x | y;
      else if (op == "^")
        z = x ^ y;
      else if (op == "<<" || op == ">>") {
        if (y < 0 || y >= bit_width(type(a.type), m.config))
          bad("constant shift would Panic");
        if (op == "<<")
          z = x << y.get_ui();
        else
          z = x >> y.get_ui();
      } else
        bad("unsupported constant integer operator");
      return integer(z, a.type, true);
    }
    if (auto p = std::get_if<FloatLiteral>(&a.data)) {
      auto x = number(p->bits, type(a.type).bits),
           y = number(std::get<FloatLiteral>(b.data).bits, type(b.type).bits);
      if (cmp) {
        if (x.special == 2 || y.special == 2)
          return {boolean, BoolLiteral{op == "!="}};
        int c = x.special == 1 ? (y.special == 1 ? (x.negative == y.negative ? 0
                                                    : x.negative ? -1
                                                                 : 1)
                                  : x.negative   ? -1
                                                 : 1)
                : y.special == 1 ? (y.negative ? 1 : -1)
                                 : mpq_cmp(x.q.get_mpq_t(), y.q.get_mpq_t());
        return {boolean, BoolLiteral{compare(c)}};
      }
      Number z;
      if (op == "-") {
        y.q = -y.q;
        y.negative = !y.negative;
        op = "+";
      }
      if (x.special == 2 || y.special == 2)
        z.special = 2;
      else if (op == "+") {
        if (x.special || y.special) {
          z = x.special ? x : y;
          if (x.special && y.special && x.negative != y.negative)
            z.special = 2;
        } else {
          z.q = x.q + y.q;
          z.negative = z.q < 0 || (z.q == 0 && x.negative && y.negative);
        }
      } else if (op == "*" || op == "/") {
        z.negative = x.negative != y.negative;
        if (op == "*") {
          if ((x.special && y.q == 0 && !y.special) ||
              (y.special && x.q == 0 && !x.special))
            z.special = 2;
          else if (x.special || y.special)
            z.special = 1;
          else
            z.q = x.q * y.q;
        } else {
          if ((x.special && y.special) ||
              (!x.special && !y.special && x.q == 0 && y.q == 0))
            z.special = 2;
          else if (x.special || (!y.special && y.q == 0))
            z.special = 1;
          else if (!y.special)
            z.q = x.q / y.q;
        }
      } else
        bad("unsupported constant float operator");
      return {a.type, FloatLiteral{round(z, a.type)}};
    }
    bad("constant operands are not scalar");
  }

public:
  SourceConstant(const HirModule &module, TypeContext &tc,
                 const std::function<TypeId(Ref)> &p,
                 const std::function<FunctionValue(Ref, TypeId)> &f,
                 std::stop_token s)
      : m(module), types(tc), parse(p), function(f), stop(s) {}
  ConstantValue eval(Ref r, std::optional<TypeId> expected = {},
                     size_t depth = 0) {
    check_cancelled(stop);
    if (depth > 128 || ++work > 100000)
      bad("constant evaluation exceeds budget");
    auto key = std::tuple{r.module.value, r.file, r.node.value};
    if (!active.insert(key).second)
      bad("constant source cycle");
    struct Pop {
      decltype(active) &a;
      decltype(key) k;
      ~Pop() { a.erase(k); }
    } pop{active, key};
    auto finish = [&](ConstantValue v) {
      if (expected && v.type != *expected)
        bad("constant source type mismatch");
      return v;
    };
    const auto n = ast(r);
    auto cr = [&](size_t i) { return child(r, i); };
    if (n.kind == AstKind::Parenthesized || n.kind == AstKind::BracketItem)
      return eval(cr(0), expected, depth + 1);
    if (n.kind == AstKind::Literal) {
      const auto &tok = file(r).tokens.at(n.token_begin);
      if (tok.kind == TokenKind::integer)
        return finish(
            integer(parse_integer_spelling(tok.spelling),
                    expected.value_or(scalar(TypeKind::SignedInt, 64))));
      if (tok.kind == TokenKind::floating) {
        auto t = expected.value_or(scalar(TypeKind::Float, 64));
        if (type(t).kind != TypeKind::Float)
          bad("constant float context");
        return finish({t, FloatLiteral{ieee_float_bits(tok.spelling, false,
                                                       type(t).bits)}});
      }
      if (tok.kind == TokenKind::string_literal)
        return finish(
            {scalar(TypeKind::String), StringLiteral{*tok.string_value}});
      if (tok.spelling == "true" || tok.spelling == "false")
        return finish(
            {scalar(TypeKind::Bool), BoolLiteral{tok.spelling == "true"}});
    }
    if (n.kind == AstKind::Name || n.kind == AstKind::Member ||
        n.kind == AstKind::NeutralBracket ||
        n.kind == AstKind::FunctionLiteral) {
      auto sid = symbol(r);
      if (sid && m.resolved->symbols.at(sid->value).kind ==
                     modules::SymbolKind::constant) {
        const auto &decl = m.resolved->symbols.at(sid->value);
        if (!decl.definition)
          bad("constant has no definition");
        auto def = *decl.definition;
        const auto &written = ast(def);
        std::optional<TypeId> annotation;
        if (written.children.size() > 1)
          annotation = parse(child(def, 0));
        // Function identities need a signature for generic specialization;
        // the callback independently checks it against the real declaration.
        if (!annotation && expected &&
            type(*expected).kind == TypeKind::Function)
          annotation = expected;
        return finish(eval(child(def, written.children.size() - 1), annotation,
                           depth + 1));
      }
      if (n.kind == AstKind::Member && !sid) {
        auto base_symbol = symbol(cr(0));
        if (ast(cr(0)).kind == AstKind::NeutralBracket)
          base_symbol = symbol(child(cr(0), 0));
        if (base_symbol && (m.resolved->symbols.at(base_symbol->value).kind ==
                                modules::SymbolKind::type ||
                            m.resolved->symbols.at(base_symbol->value).kind ==
                                modules::SymbolKind::type_parameter ||
                            m.resolved->symbols.at(base_symbol->value).kind ==
                                modules::SymbolKind::builtin_type)) {
          auto t = parse(cr(0));
          if (type(t).kind == TypeKind::NominalEnum) {
            auto name = file(r).tokens.at(n.token_end - 1).spelling;
            for (auto &v : nominal(t).variants)
              if (m.resolved->symbols.at(v.symbol.value).name == name &&
                  v.payload.empty())
                return finish({t, ConstantAggregate{v.symbol, {}}});
          }
        } else {
          auto base = eval(cr(0), {}, depth + 1);
          auto agg = std::get_if<ConstantAggregate>(&base.data);
          if (!agg || type(base.type).kind != TypeKind::NominalStruct)
            bad("constant field requires struct");
          auto name = file(r).tokens.at(n.token_end - 1).spelling;
          auto &fields = nominal(base.type).fields;
          for (size_t i = 0; i < fields.size(); ++i)
            if (m.resolved->symbols.at(fields[i].symbol.value).name == name) {
              if (m.resolved->symbols.at(fields[i].symbol.value).module !=
                      r.module &&
                  !fields[i].exported)
                bad("constant field is private");
              return finish(agg->fields.at(i));
            }
          bad("constant field does not exist");
        }
      }
      if (expected && type(*expected).kind == TypeKind::Function) {
        auto ref = r;
        while (ast(ref).kind == AstKind::Parenthesized ||
               ast(ref).kind == AstKind::NeutralBracket)
          ref = child(ref, 0);
        auto target = symbol(ref);
        bool callable = ast(ref).kind == AstKind::FunctionLiteral ||
                        (target && m.resolved->symbols.at(target->value).kind ==
                                       modules::SymbolKind::function);
        if (ast(ref).kind == AstKind::Member && !target) {
          auto base = child(ref, 0);
          while (ast(base).kind == AstKind::NeutralBracket)
            base = child(base, 0);
          if (auto s = symbol(base)) {
            auto kind = m.resolved->symbols.at(s->value).kind;
            callable = kind == modules::SymbolKind::type ||
                       kind == modules::SymbolKind::type_parameter ||
                       kind == modules::SymbolKind::builtin_type;
          }
        }
        if (callable)
          return finish({*expected, function(r, *expected)});
      }
      bad("constant source is a runtime value");
    }
    if (n.kind == AstKind::Unary) {
      auto op = file(r).tokens.at(n.token_begin).spelling;
      // Apply the sign before checking the integer range, including MIN.
      auto inner = cr(0);
      while (ast(inner).kind == AstKind::Parenthesized)
        inner = child(inner, 0);
      if (op == "-" && ast(inner).kind == AstKind::Literal &&
          file(inner).tokens.at(ast(inner).token_begin).kind ==
              TokenKind::integer)
        return finish(
            integer(-parse_integer_spelling(
                        file(inner).tokens.at(ast(inner).token_begin).spelling),
                    expected.value_or(scalar(TypeKind::SignedInt, 64))));
      auto a = eval(
          cr(0), op == "!" ? std::optional(scalar(TypeKind::Bool)) : expected,
          depth + 1);
      if (op == "+")
        return finish(a);
      if (auto p = std::get_if<IntegerLiteral>(&a.data)) {
        mpz_class z(p->decimal);
        if (op == "-")
          return finish(integer(-z, a.type, true));
        if (op == "~")
          return finish(integer(mpz_class(~z), a.type, true));
      }
      if (auto p = std::get_if<FloatLiteral>(&a.data); p && op == "-") {
        p->bits ^= 1ULL << (type(a.type).bits - 1);
        return finish(a);
      }
      if (auto p = std::get_if<BoolLiteral>(&a.data); p && op == "!") {
        p->value = !p->value;
        return finish(a);
      }
      bad("unsupported constant unary operator");
    }
    if (n.kind == AstKind::Binary) {
      auto op = file(r).tokens.at(ast(cr(0)).token_end).spelling;
      bool cmp = op == "==" || op == "!=" || op == "<" || op == "<=" ||
                 op == ">" || op == ">=";
      auto context = (cmp || op == "&&" || op == "||") ? std::optional<TypeId>{}
                                                       : expected;
      bool shift = op == "<<" || op == ">>";
      if (!shift && op != "&&" && op != "||" && contextual(cr(0)) &&
          !contextual(cr(1))) {
        auto b = eval(cr(1), context, depth + 1);
        auto a = eval(cr(0), b.type, depth + 1);
        return finish(binary(op, a, b));
      }
      auto a = eval(cr(0), context, depth + 1);
      if ((op == "&&" || op == "||") &&
          std::holds_alternative<BoolLiteral>(a.data)) {
        bool v = std::get<BoolLiteral>(a.data).value;
        if ((op == "&&" && !v) || (op == "||" && v))
          return finish(a);
        auto b = eval(cr(1), a.type, depth + 1);
        return finish(binary(op, a, b));
      }
      auto b =
          eval(cr(1), shift ? std::optional<TypeId>{} : std::optional(a.type),
               depth + 1);
      return finish(binary(op, a, b));
    }
    if (n.kind == AstKind::Call) {
      auto callee = cr(0);
      auto sid = symbol(callee);
      if (ast(callee).kind == AstKind::NeutralBracket)
        sid = symbol(child(callee, 0));
      std::optional<SymbolId> variant;
      if (ast(callee).kind == AstKind::Member && !sid) {
        auto name = file(callee).tokens.at(ast(callee).token_end - 1).spelling;
        auto t = parse(child(callee, 0));
        if (type(t).kind != TypeKind::NominalEnum)
          bad("ordinary calls are not constant");
        for (auto &v : nominal(t).variants)
          if (m.resolved->symbols.at(v.symbol.value).name == name)
            variant = v.symbol;
        if (!variant)
          bad("constant enum variant missing");
        callee = child(callee, 0);
      } else if (!sid || (m.resolved->symbols.at(sid->value).kind !=
                              modules::SymbolKind::type &&
                          m.resolved->symbols.at(sid->value).kind !=
                              modules::SymbolKind::builtin_type))
        bad("ordinary calls are not constant");
      auto t = parse(callee);
      if (integral(type(t).kind) || type(t).kind == TypeKind::Float) {
        if (n.children.size() != 2)
          bad("constant conversion arity");
        auto argument = child(cr(1), 0), literal = argument;
        while (ast(literal).kind == AstKind::Parenthesized)
          literal = child(literal, 0);
        if (ast(literal).kind == AstKind::Unary) {
          auto op = file(literal).tokens.at(ast(literal).token_begin).spelling;
          if (op == "+" || op == "-") {
            literal = child(literal, 0);
            while (ast(literal).kind == AstKind::Parenthesized)
              literal = child(literal, 0);
          }
        }
        bool integer_literal =
            ast(literal).kind == AstKind::Literal &&
            file(literal).tokens.at(ast(literal).token_begin).kind ==
                TokenKind::integer;
        return finish(convert(eval(argument,
                                   integral(type(t).kind) && integer_literal
                                       ? std::optional(t)
                                       : std::optional<TypeId>{},
                                   depth + 1),
                              t));
      }
      if (type(t).kind != TypeKind::NominalStruct &&
          type(t).kind != TypeKind::NominalEnum)
        bad("constant aggregate type unsupported");
      auto &d = nominal(t);
      const auto *fields = &d.fields;
      if (variant) {
        fields = nullptr;
        for (auto &v : d.variants)
          if (v.symbol == *variant)
            fields = &v.payload;
      }
      if (!fields || n.children.size() != fields->size() + 1)
        bad("constant aggregate arity");
      ConstantAggregate agg{variant, {}};
      agg.fields.resize(fields->size());
      std::set<size_t> seen;
      bool named = false;
      for (size_t i = 1; i < n.children.size(); ++i) {
        auto arg = cr(i);
        auto &an = ast(arg);
        size_t index = i - 1;
        if (file(arg).tokens.at(an.token_begin + 1).spelling == ":") {
          named = true;
          auto name = file(arg).tokens.at(an.token_begin).spelling;
          index = 0;
          while (index < fields->size() &&
                 m.resolved->symbols.at((*fields)[index].symbol.value).name !=
                     name)
            ++index;
        } else if (!variant || named)
          bad("constant aggregate argument labels/order");
        if (index >= fields->size() || !seen.insert(index).second)
          bad("constant aggregate field mapping");
        if (!variant &&
            m.resolved->symbols.at((*fields)[index].symbol.value).module !=
                r.module &&
            !(*fields)[index].exported)
          bad("constant constructor field is private");
        agg.fields[index] =
            eval(child(arg, 0), field_type(t, (*fields)[index]), depth + 1);
      }
      return finish({t, std::move(agg)});
    }
    bad("unsupported constant source expression");
  }
};
} // namespace
ConstantValue source_constant_value(
    const HirModule &m, TypeContext &types, modules::NodeRef r, TypeId expected,
    const std::function<TypeId(modules::NodeRef)> &p,
    const std::function<FunctionValue(modules::NodeRef, TypeId)> &f,
    std::stop_token stop) {
  return SourceConstant(m, types, p, f, stop).eval(r, expected);
}
namespace {
class HirConstant {
  const HirModule &m;
  std::stop_token stop;
  std::set<uint64_t> active;
  size_t work = 0;
  TypeContext types;
  std::function<TypeId(Ref)> parse;
  std::function<FunctionValue(Ref, TypeId)> function;
  SourceConstant math;

public:
  HirConstant(const HirModule &x, std::stop_token s)
      : m(x), stop(s), types(x.types), math(x, types, parse, function, s) {}
  ConstantValue eval(HirNodeId id, size_t depth = 0) {
    check_cancelled(stop);
    if (depth > 128 || ++work > 100000 || !active.insert(id.value).second)
      throw LiteralError("constant HIR evaluation budget/cycle");
    struct Pop {
      std::set<uint64_t> &s;
      uint64_t k;
      ~Pop() { s.erase(k); }
    } pop{active, id.value};
    const auto &n = m.nodes.at(id.value);
    if (auto p = std::get_if<ConstantLiteral>(&n.data))
      return p->value;
    ConstantValue value{n.type, UnitLiteral{}};
    bool literal = true;
    std::visit(
        [&](const auto &v) {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, UnitLiteral> ||
                        std::is_same_v<T, BoolLiteral> ||
                        std::is_same_v<T, IntegerLiteral> ||
                        std::is_same_v<T, FloatLiteral> ||
                        std::is_same_v<T, StringLiteral> ||
                        std::is_same_v<T, FunctionValue>)
            value.data = v;
          else
            literal = false;
        },
        n.data);
    if (literal)
      return value;
    auto child = [&](size_t i) { return eval(n.children.at(i), depth + 1); };
    if (auto p = std::get_if<Read>(&n.data)) {
      if (m.resolved->symbols.at(p->symbol.value).kind !=
          modules::SymbolKind::constant)
        throw LiteralError("runtime read is not constant");
      for (auto &binding : m.nodes)
        if (auto b = std::get_if<Binding>(&binding.data);
            b && b->symbol == p->symbol)
          return eval(binding.children.at(0), depth + 1);
      throw LiteralError("constant binding is missing");
    }
    if (n.kind() == NodeKind::struct_init || n.kind() == NodeKind::enum_init) {
      ConstantAggregate result;
      auto p = std::get_if<EnumInit>(&n.data);
      if (p)
        result.variant = p->variant;
      const auto &args = p ? p->payload : std::get<StructInit>(n.data).fields;
      result.fields.resize(args.size());
      for (auto &a : args)
        result.fields.at(a.parameter_index) = eval(a.expression, depth + 1);
      value.data = std::move(result);
      return value;
    }
    if (auto p = std::get_if<FieldRead>(&n.data)) {
      auto base = child(0);
      auto &a = std::get<ConstantAggregate>(base.data);
      const auto &t = *types.find(base.type);
      for (auto &d : m.nominals)
        if (d.symbol == t.declaration)
          for (size_t i = 0; i < d.fields.size(); ++i)
            if (d.fields[i].symbol == p->field)
              return a.fields.at(i);
      throw LiteralError("constant field missing");
    }
    if (auto p = std::get_if<Binary>(&n.data)) {
      static const char *ops[] = {"+", "-", "*",  "/",  "%", "<<", ">>", "&",
                                  "|", "^", "==", "!=", "<", "<=", ">",  ">="};
      return math.binary(ops[unsigned(p->op)], child(0), child(1));
    }
    if (auto p = std::get_if<ShortCircuit>(&n.data)) {
      auto a = child(0);
      bool v = std::get<BoolLiteral>(a.data).value;
      if ((p->op == LogicalOp::and_ && !v) || (p->op == LogicalOp::or_ && v))
        return a;
      return child(1);
    }
    if (n.kind() == NodeKind::integer_convert)
      return math.convert(child(0), n.type);
    if (auto p = std::get_if<Unary>(&n.data)) {
      auto a = child(0);
      if (p->op == UnaryOp::logical_not) {
        std::get<BoolLiteral>(a.data).value =
            !std::get<BoolLiteral>(a.data).value;
        return a;
      }
      if (auto f = std::get_if<FloatLiteral>(&a.data)) {
        f->bits ^= 1ULL << (types.find(a.type)->bits - 1);
        return a;
      }
      auto zero = ConstantValue{a.type, IntegerLiteral{"0", 0}};
      if (p->op == UnaryOp::negate)
        return math.binary("-", zero, a);
      return math.binary(
          "^", a,
          ConstantValue{
              a.type,
              IntegerLiteral{"-1", ~0ULL >> (64 - bit_width(*types.find(a.type),
                                                            m.config))}});
    }
    throw LiteralError("initializer is not a pure compile-time value");
  }
};
} // namespace
std::optional<ConstantValue>
evaluate_hir_constant(const HirModule &m, HirNodeId id, std::stop_token stop) {
  try {
    return HirConstant(m, stop).eval(id);
  } catch (const LiteralError &) {
    return {};
  }
}
bool aggregate_constant_expression(const HirModule &m, HirNodeId root) {
  std::set<uint64_t> seen;
  std::vector<HirNodeId> pending{root};
  while (!pending.empty()) {
    auto id = pending.back();
    pending.pop_back();
    if (!seen.insert(id.value).second)
      continue;
    const auto &n = m.nodes.at(id.value);
    if (n.kind() == NodeKind::struct_init || n.kind() == NodeKind::enum_init ||
        n.kind() == NodeKind::field_read ||
        n.kind() == NodeKind::constant_literal)
      return true;
    if (auto r = std::get_if<Read>(&n.data);
        r && m.resolved->symbols.at(r->symbol.value).kind ==
                 modules::SymbolKind::constant)
      for (auto &b : m.nodes)
        if (auto p = std::get_if<Binding>(&b.data); p && p->symbol == r->symbol)
          pending.push_back(b.children.at(0));
    pending.insert(pending.end(), n.children.begin(), n.children.end());
  }
  return false;
}
} // namespace lang::hir

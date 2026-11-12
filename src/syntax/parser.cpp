#include "lang/syntax/parser.hpp"
#include <algorithm>
namespace lang {
namespace {
struct SyntaxError {};
class Parser {
  const TokenBuffer &b;
  std::stop_token stop;
  size_t p = 0, depth = 0;
  AstFile a;
  std::vector<Diagnostic> diagnostics;
  struct Guard {
    Parser &p;
    Guard(Parser &x) : p(x) {
      if (++p.depth > 256) {
        --p.depth;
        p.fail("nesting limit exceeded");
      }
    }
    ~Guard() { --p.depth; }
  };
  std::string_view text(size_t n = 0) const {
    return p + n < b.tokens.size() ? b.tokens[p + n].spelling
                                   : std::string_view{};
  }
  bool end() const {
    return p >= b.tokens.size() || b.tokens[p].kind == TokenKind::end;
  }
  bool at(std::string_view s) const { return text() == s; }
  void advance() {
    check_cancelled(stop);
    if (!end())
      ++p;
  }
  bool eat(std::string_view s) {
    if (at(s)) {
      advance();
      return true;
    }
    return false;
  }
  [[noreturn]] void fail(const char *message) {
    Diagnostic d;
    d.code = "E_PARSE_EXPECTED_TOKEN";
    d.phase = "parse";
    d.severity = "error";
    d.message = message;
    if (p < b.tokens.size())
      d.primary = b.tokens[p].span;
    diagnostics.push_back(std::move(d));
    throw SyntaxError{};
  }
  void expect(std::string_view s) {
    if (!eat(s))
      fail(("expected '" + std::string(s) + "'").c_str());
  }
  void ident() {
    if (end() || b.tokens[p].kind != TokenKind::identifier)
      fail("expected identifier");
    advance();
  }
  AstNodeId node(AstKind k, size_t start, std::vector<AstNodeId> c = {}) {
    SourceSpan span = a.source->raw_span(0, 0);
    if (start < b.tokens.size())
      span = b.tokens[start].span;
    if (p > start)
      span.end_byte = b.tokens[p - 1].span.end_byte;
    AstNodeId id(a.nodes.size());
    a.nodes.push_back({k, span, start, p, std::move(c), k == AstKind::Error});
    return id;
  }
  bool type_start() const {
    return !end() && (b.tokens[p].kind == TokenKind::identifier || at("("));
  }
  template <class F>
  void comma(std::string_view close, F f, bool empty = true) {
    if (at(close)) {
      if (!empty)
        fail("expected list item");
      advance();
      return;
    }
    do {
      f();
    } while (eat(",") && !at(close));
    expect(close);
  }
  AstNodeId type(bool named = false) {
    Guard g(*this);
    size_t s = p;
    std::vector<AstNodeId> c;
    if (!named && eat("(")) {
      c.push_back(type());
      expect(")");
      return node(AstKind::ParenthesizedType, s, c);
    }
    if (!named && eat("func")) {
      expect("(");
      comma(")", [&] {
        size_t q = p;
        eat("move") || eat("inout");
        c.push_back(node(AstKind::Parameter, q, {type()}));
      });
      signature(c);
      return node(AstKind::FunctionType, s, c);
    }
    if (!named && at("dyn")) {
      advance();
      size_t q = p;
      ident();
      while (eat("."))
        ident();
      std::vector<AstNodeId> args;
      if (eat("["))
        comma("]", [&] { args.push_back(type()); }, false);
      auto named_type = node(AstKind::NamedType, q, std::move(args));
      c.push_back(named_type);
      return node(AstKind::TraitObjectType, s, c);
    }
    do {
      ident();
      if (eat("["))
        comma("]", [&] { c.push_back(type()); }, false);
    } while (eat("."));
    return node(AstKind::NamedType, s, c);
  }
  void signature(std::vector<AstNodeId> &c) {
    if (type_start())
      c.push_back(type());
    if (eat("throws"))
      c.push_back(type());
  }
  void generics(std::vector<AstNodeId> &c) {
    if (eat("["))
      comma(
          "]",
          [&] {
            size_t s = p;
            ident();
            std::vector<AstNodeId> t;
            if (!at(",") && !at("]"))
              t.push_back(type(true));
            c.push_back(node(AstKind::GenericParameter, s, t));
          },
          false);
  }
  void params(std::vector<AstNodeId> &c) {
    expect("(");
    comma(")", [&] {
      size_t s = p;
      ident();
      eat("inout") || eat("move");
      c.push_back(node(AstKind::Parameter, s, {type()}));
    });
    signature(c);
  }
  int precedence() const {
    auto t = text();
    if (t == "||")
      return 1;
    if (t == "&&")
      return 2;
    if (t == "|")
      return 3;
    if (t == "^")
      return 4;
    if (t == "&")
      return 5;
    if (t == "==" || t == "!=")
      return 6;
    if (t == "<" || t == ">" || t == "<=" || t == ">=")
      return 7;
    if (t == "<<" || t == ">>")
      return 8;
    if (t == "+" || t == "-")
      return 9;
    if (t == "*" || t == "/" || t == "%")
      return 10;
    return 0;
  }
  bool literal() const {
    return !end() && (b.tokens[p].kind == TokenKind::integer ||
                      b.tokens[p].kind == TokenKind::floating ||
                      b.tokens[p].kind == TokenKind::string_literal ||
                      at("true") || at("false"));
  }
  AstNodeId expr(int min = 1) {
    Guard g(*this);
    size_t s = p;
    AstNodeId left;
    if (eat("await")) {
      // await binds to the complete postfix expression to its right, so
      // `await f(x).field` awaits one value and `await a + b` is `(await a) +
      // b`. Binary operators keep their ordinary precedence relative to it.
      left = node(AstKind::Await, s, {expr(11)});
    } else if (at("+") || at("-") || at("!") || at("~") || at("move") ||
               at("inout")) {
      advance();
      left = node(AstKind::Unary, s, {expr(11)});
    } else {
      if (eat("func")) {
        std::vector<AstNodeId> c;
        params(c);
        c.push_back(block());
        left = node(AstKind::FunctionLiteral, s, c);
      } else if (eat("(")) {
        auto x = expr();
        expect(")");
        left = node(AstKind::Parenthesized, s, {x});
      } else if (eat("[")) {
        std::vector<AstNodeId> c;
        comma("]", [&] { c.push_back(expr()); });
        left = node(AstKind::List, s, c);
      } else if (literal()) {
        advance();
        left = node(AstKind::Literal, s);
      } else {
        ident();
        left = node(AstKind::Name, s);
      }
      for (;;) {
        if (eat(".")) {
          ident();
          left = node(AstKind::Member, s, {left});
        } else if (eat("(")) {
          std::vector<AstNodeId> c{left};
          bool named = false;
          comma(")", [&] {
            size_t q = p;
            bool label = text(1) == ":";
            if (label) {
              ident();
              expect(":");
              named = true;
            } else if (named)
              fail("positional argument follows named argument");
            c.push_back(node(AstKind::Argument, q, {expr()}));
          });
          left = node(AstKind::Call, s, c);
        } else if (eat("[")) {
          std::vector<AstNodeId> c{left};
          bool slice = false;
          if (!at(":")) {
            do {
              size_t q = p;
              AstNodeId x;
              if (at("func") || at("(")) {
                size_t save = p;
                size_t count = a.nodes.size();
                size_t ds = diagnostics.size();
                try {
                  x = type();
                  // A candidate must consume the whole item. In particular,
                  // (index) + 1 is an expression, not a parenthesized type.
                  if (!at(",") && !at("]"))
                    fail("expected end of bracket type candidate");
                } catch (SyntaxError &) {
                  p = save;
                  a.nodes.resize(count);
                  diagnostics.resize(ds);
                  x = expr();
                }
              } else
                x = expr();
              c.push_back(node(AstKind::BracketItem, q, {x}));
            } while (eat(",") && !at("]"));
          }
          if (eat(":")) {
            slice = true;
            if (!at("]"))
              c.push_back(expr());
          }
          expect("]");
          left = node(slice ? AstKind::Slice : AstKind::NeutralBracket, s, c);
        } else
          break;
      }
    }
    while (precedence() >= min && precedence() != 0) {
      int prec = precedence();
      advance();
      auto right = expr(prec + 1);
      left = node(AstKind::Binary, s, {left, right});
    }
    return left;
  }
  AstNodeId binding() {
    size_t s = p;
    advance();
    ident();
    std::vector<AstNodeId> c;
    if (!at("="))
      c.push_back(type());
    expect("=");
    c.push_back(expr());
    return node(AstKind::Binding, s, c);
  }
  bool binding_start() const { return at("let") || at("var") || at("const"); }
  AstNodeId simple() {
    size_t s = p;
    auto x = expr();
    auto t = text();
    if (t == "=" || t == "+=" || t == "-=" || t == "*=" || t == "/=" ||
        t == "%=" || t == "<<=" || t == ">>=" || t == "&=" || t == "^=" ||
        t == "|=") {
      advance();
      return node(AstKind::Assign, s, {x, expr()});
    }
    return node(AstKind::ExpressionStatement, s, {x});
  }
  AstNodeId pattern() {
    Guard g(*this);
    size_t s = p;
    std::vector<AstNodeId> c;
    if (eat("-")) {
      if (end() || (b.tokens[p].kind != TokenKind::integer &&
                    b.tokens[p].kind != TokenKind::floating))
        fail("expected number");
      advance();
      return node(AstKind::LiteralPattern, s);
    }
    if (literal()) {
      advance();
      return node(AstKind::LiteralPattern, s);
    }
    ident();
    bool qualified = false;
    while (at("[") || at(".")) {
      if (eat("[")) {
        comma("]", [&] { c.push_back(type()); }, false);
        if (!at("."))
          fail("expected qualified variant");
      }
      expect(".");
      ident();
      qualified = true;
    }
    if (qualified && eat("("))
      comma(")", [&] { c.push_back(pattern()); });
    return node(qualified ? AstKind::VariantPattern : AstKind::BindPattern, s,
                c);
  }
  AstNodeId statement() {
    Guard g(*this);
    size_t s = p;
    std::vector<AstNodeId> c;
    AstKind k;
    if (at("{"))
      return block();
    if (binding_start()) {
      auto x = binding();
      expect(";");
      return x;
    }
    if (eat("if")) {
      c.push_back(expr());
      c.push_back(block());
      if (eat("else"))
        c.push_back(at("if") ? statement() : block());
      return node(AstKind::If, s, c);
    }
    if (eat("for")) {
      k = AstKind::For;
      if (!at("{")) {
        if (text(1) == "in") {
          ident();
          expect("in");
          c.push_back(expr());
          k = AstKind::ForIn;
        } else {
          if (!at(";"))
            c.push_back(binding_start() ? binding() : simple());
          if (eat(";")) {
            if (!at(";"))
              c.push_back(expr());
            expect(";");
            if (!at("{"))
              c.push_back(simple());
          } else if (!c.empty() &&
                     a.node(c.back()).kind != AstKind::ExpressionStatement)
            fail("expected loop separator");
        }
      }
      c.push_back(block());
      return node(k, s, c);
    }
    if (eat("switch")) {
      c.push_back(expr());
      expect("{");
      while (!at("}") && !end()) {
        size_t q = p;
        std::vector<AstNodeId> cc;
        if (eat("case"))
          cc.push_back(pattern());
        else
          expect("default");
        expect(":");
        while (!at("case") && !at("default") && !at("}") && !end())
          cc.push_back(statement());
        c.push_back(node(AstKind::Case, q, cc));
      }
      expect("}");
      return node(AstKind::Switch, s, c);
    }
    if (eat("try")) {
      c.push_back(block());
      if (!at("catch"))
        fail("expected catch");
      while (eat("catch")) {
        size_t q = p - 1;
        ident();
        auto t = type();
        auto body = block();
        c.push_back(node(AstKind::Catch, q, {t, body}));
      }
      return node(AstKind::Try, s, c);
    }
    if (at("defer") || at("unsafe")) {
      k = at("defer") ? AstKind::Defer : AstKind::Unsafe;
      advance();
      c.push_back(block());
      return node(k, s, c);
    }
    if (at("return") || at("throw") || at("break") || at("continue")) {
      k = at("return")  ? AstKind::Return
          : at("throw") ? AstKind::Throw
          : at("break") ? AstKind::Break
                        : AstKind::Continue;
      advance();
      if (k == AstKind::Throw || (k == AstKind::Return && !at(";")))
        c.push_back(expr());
      expect(";");
      return node(k, s, c);
    }
    auto x = simple();
    expect(";");
    return x;
  }
  bool top_start() const {
    return at("pub") || at("async") || at("func") || at("struct") ||
           at("enum") || at("trait") || at("impl") || binding_start();
  }
  // Reads an optional `async` marker before a function declaration. Returns
  // true when the keyword was present and records the marker in children.
  bool async_marker(std::vector<AstNodeId> &c) {
    if (!at("async"))
      return false;
    size_t s = p;
    advance();
    c.push_back(node(AstKind::Async, s));
    return true;
  }
  AstNodeId block() {
    Guard g(*this);
    size_t s = p;
    expect("{");
    std::vector<AstNodeId> c;
    while (!end() && !at("}")) {
      if ((at("func") || at("async")) && text(1) != "(")
        fail("nested declaration or missing closing brace");
      size_t q = p;
      try {
        c.push_back(statement());
      } catch (SyntaxError &) {
        while (!end() && !at(";") && !at("}") && !top_start())
          advance();
        c.push_back(node(AstKind::Error, q));
        if (top_start())
          throw;
        eat(";");
      }
      if (p == q && !end())
        advance();
    }
    expect("}");
    return node(AstKind::Block, s, c);
  }
  AstNodeId declaration() {
    size_t s = p;
    eat("pub");
    std::vector<AstNodeId> c;
    const bool async = async_marker(c);
    if (binding_start()) {
      auto x = binding();
      expect(";");
      auto &declaration = a.nodes[x.value];
      declaration.token_begin = s;
      declaration.token_end = p;
      declaration.span.begin_byte = b.tokens[s].span.begin_byte;
      declaration.span.end_byte = b.tokens[p - 1].span.end_byte;
      return x;
    }
    AstKind k;
    if (eat("func")) {
      ident();
      generics(c);
      params(c);
      c.push_back(block());
      return node(AstKind::Function, s, c);
    }
    if (async)
      fail("expected 'func' after 'async'");
    if (eat("struct"))
      k = AstKind::Struct;
    else if (eat("enum"))
      k = AstKind::Enum;
    else if (eat("trait"))
      k = AstKind::Trait;
    else if (eat("impl"))
      k = AstKind::Impl;
    else
      fail("expected declaration");
    if (k == AstKind::Impl) {
      generics(c);
      c.push_back(type(true));
      if (eat("for"))
        c.push_back(type(true));
    } else {
      ident();
      generics(c);
    }
    expect("{");
    while (!at("}") && !end()) {
      size_t q = p;
      if (k == AstKind::Enum && at("pub"))
        fail("enum variants do not accept pub");
      eat("pub");
      if (k == AstKind::Struct) {
        ident();
        auto t = type();
        expect(";");
        c.push_back(node(AstKind::Field, q, {t}));
      } else if (k == AstKind::Enum) {
        ident();
        std::vector<AstNodeId> cc;
        if (eat("("))
          comma(")", [&] {
            size_t z = p;
            ident();
            cc.push_back(node(AstKind::Field, z, {type()}));
          });
        expect(";");
        c.push_back(node(AstKind::Variant, q, cc));
      } else {
        std::vector<AstNodeId> cc;
        async_marker(cc);
        expect("func");
        ident();
        generics(cc);
        params(cc);
        if (k == AstKind::Trait)
          expect(";");
        else
          cc.push_back(block());
        c.push_back(node(AstKind::Function, q, cc));
      }
    }
    expect("}");
    return node(k, s, c);
  }

public:
  Parser(const TokenBuffer &buffer, std::stop_token st) : b(buffer), stop(st) {
    a.source = b.source;
    a.tokens = b.tokens;
  }
  PhaseResult<AstFile> run() {
    PhaseResult<AstFile> r;
    try {
      check_cancelled(stop);
      if (!a.source || !a.source->valid()) {
        r.failure = FailureKind::internal;
        return r;
      }
      std::vector<AstNodeId> c;
      try {
        size_t s = p;
        expect("package");
        ident();
        expect(";");
        c.push_back(node(AstKind::Package, s));
        while (eat("import")) {
          s = p - 1;
          if (end() || b.tokens[p].kind != TokenKind::string_literal ||
              (!text().empty() && text().front() == '`'))
            fail("expected text import string");
          advance();
          if (eat("as"))
            ident();
          expect(";");
          c.push_back(node(AstKind::Import, s));
        }
      } catch (SyntaxError &) {
        c.push_back(node(AstKind::Error, 0));
      }
      while (!end()) {
        size_t s = p;
        try {
          c.push_back(declaration());
        } catch (SyntaxError &) {
          if (p == s)
            advance();
          while (!end() && !top_start())
            advance();
          c.push_back(node(AstKind::Error, s));
        }
      }
      a.root = node(AstKind::File, 0, c);
      a.recovered = !diagnostics.empty();
      r.state = a.recovered ? PhaseState::recovered : PhaseState::complete;
      r.failure = FailureKind::source;
      r.value = std::move(a);
      r.diagnostics = std::move(diagnostics);
    } catch (Cancelled &) {
      r.state = PhaseState::cancelled;
      r.failure = FailureKind::source;
    }
    return r;
  }
};
} // namespace
PhaseResult<AstFile> parse(const TokenBuffer &b, std::stop_token stop) {
  return Parser(b, stop).run();
}
SignatureSyntax split_signature(const AstFile &file, AstNodeId id) {
  const auto &n = file.node(id);
  if (n.kind != AstKind::Function && n.kind != AstKind::FunctionLiteral &&
      n.kind != AstKind::FunctionType)
    throw std::invalid_argument("signature owner is not a function");
  SignatureSyntax result;
  for (auto child : n.children) {
    const auto &c = file.node(child);
    if (c.kind == AstKind::Parameter)
      result.parameters.push_back(child);
    else if (c.kind == AstKind::NamedType || c.kind == AstKind::FunctionType ||
             c.kind == AstKind::ParenthesizedType ||
             c.kind == AstKind::TraitObjectType) {
      if (!c.token_begin || c.token_end > file.tokens.size())
        throw std::invalid_argument("invalid signature type token range");
      auto &slot = file.tokens[c.token_begin - 1].spelling == "throws"
                       ? result.throws_type
                       : result.result;
      if (slot)
        throw std::invalid_argument("duplicate signature type component");
      slot = child;
    }
  }
  return result;
}
} // namespace lang

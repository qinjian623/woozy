#include "lang/syntax/parser.hpp"
#include <iostream>
#include <stdexcept>
using namespace lang;
void require(bool v) {
  if (!v)
    throw std::runtime_error("parser test failed");
}
TokenBuffer
buffer(std::initializer_list<std::pair<TokenKind, std::string_view>> items) {
  std::string source;
  for (auto [k, t] : items) {
    (void)k;
    source += t;
    source += ' ';
  }
  auto snapshot = std::make_shared<SourceSnapshot>(
      FileId(1), RevisionId(1), SourceSnapshotId(1), "test", source);
  TokenBuffer b{snapshot, {}};
  size_t pos = 0;
  for (auto [k, t] : items) {
    b.tokens.push_back(
        {k,
         snapshot->span(pos, pos + t.size()),
         std::string_view(snapshot->normalized()).substr(pos, t.size()),
         {},
         {}});
    pos += t.size() + 1;
  }
  b.tokens.push_back({TokenKind::end, snapshot->span(pos, pos), {}, {}, {}});
  return b;
}
namespace {
void integration_tests();
}
int main() {
  integration_tests();
  using K = TokenKind;
  auto b = buffer({{K::keyword, "package"},
                   {K::identifier, "p"},
                   {K::punctuation, ";"},
                   {K::keyword, "func"},
                   {K::identifier, "f"},
                   {K::punctuation, "("},
                   {K::punctuation, ")"},
                   {K::punctuation, "{"},
                   {K::keyword, "return"},
                   {K::punctuation, ";"},
                   {K::punctuation, "}"}});
  auto r = parse(b);
  require(r.success());
  require(r.value->nodes.size() > 3);
  b.source.reset();
  require(r.value->source->valid());
  auto bad = b;
  bad.source = r.value->source;
  bad.tokens.erase(bad.tokens.begin() + 9);
  auto e = parse(bad);
  require(e.state == PhaseState::recovered);
  require(!e.success());
  require(e.failure == FailureKind::source);
  std::stop_source stop;
  stop.request_stop();
  require(parse(bad, stop.get_token()).state == PhaseState::cancelled);
  std::cout << "parser local tests passed\n";
}

namespace {
PhaseResult<AstFile> real_parse(std::string source) {
  auto snapshot = std::make_shared<SourceSnapshot>(
      FileId(2), RevisionId(1), SourceSnapshotId(2), "real.lang",
      std::move(source));
  auto tokens = lex(snapshot);
  require(tokens.success());
  return parse(*tokens.value);
}
void validate_ast(const AstFile &a) {
  require(a.node(a.root).kind == AstKind::File);
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    const auto &n = a.nodes[i];
    require(n.token_begin <= n.token_end && n.token_end <= a.tokens.size());
    require(n.span.snapshot == a.source->id);
    require(n.span.begin_byte <= n.span.end_byte);
    require(n.span.end_byte <= a.source->original.size());
    if (n.kind == AstKind::Error)
      require(n.recovered);
    for (auto child : n.children) {
      require(child.value < i); // arena is acyclic and children exist
      const auto &c = a.node(child);
      require(n.token_begin <= c.token_begin && c.token_end <= n.token_end);
    }
  }
}
void integration_tests() {
  require(!real_parse("package p; enum E { pub Value; }").success());
  auto exported = real_parse("package p; pub const exported i32 = 1;");
  require(exported.success());
  const auto &binding = exported.value->node(
      exported.value->node(exported.value->root).children.back());
  require(binding.kind == AstKind::Binding);
  require(exported.value->tokens[binding.token_begin].spelling == "pub");
  validate_ast(*exported.value);
  auto r =
      real_parse("package sample; func check() { let x = factory[func(inout "
                 "i32) Unit](value); let y = factory[(func(i32) i32 throws "
                 "E)](value); let z = table[(index) + 1]; }");
  require(r.success());
  validate_ast(*r.value);
  size_t neutral = 0, parenthesized_types = 0;
  for (const auto &n : r.value->nodes) {
    neutral += n.kind == AstKind::NeutralBracket;
    parenthesized_types += n.kind == AstKind::ParenthesizedType;
    if (n.kind == AstKind::BracketItem)
      require(n.token_begin < n.token_end);
  }
  require(neutral == 3 && parenthesized_types == 1);
  for (const auto &source :
       {"package p; func broken() { let x = 1 } func later() { return; }",
        "package p; func broken() { let x = 1; func later() { return; }",
        "package p; func broken() { let x = 1 func later() { return; }"}) {
    auto e = real_parse(source);
    require(e.state == PhaseState::recovered && !e.success());
    require(e.failure == FailureKind::source && e.value->recovered);
    validate_ast(*e.value);
    bool later = false, error = false;
    for (auto id : e.value->node(e.value->root).children) {
      const auto &n = e.value->node(id);
      error |= n.kind == AstKind::Error;
      if (n.kind == AstKind::Function) {
        auto t = n.token_begin;
        later |= e.value->tokens.at(t).spelling == "func" &&
                 e.value->tokens.at(t + 1).spelling == "later" &&
                 e.value->tokens.at(n.token_end - 1).spelling == "}";
      }
    }
    // A locally recovered function can contain its Error below the root.
    for (const auto &n : e.value->nodes)
      error |= n.kind == AstKind::Error;
    require(later && error);
  }
  auto deep =
      real_parse("package p; func f() { let x = " + std::string(400, '(') +
                 "1" + std::string(400, ')') + "; } func later() {}");
  require(!deep.success() && deep.state == PhaseState::recovered);
  validate_ast(*deep.value);
  bool limit = false;
  for (const auto &d : deep.diagnostics)
    limit |= d.message == "nesting limit exceeded";
  require(limit);
  auto snapshot = std::make_shared<SourceSnapshot>(FileId(3), RevisionId(1),
                                                   SourceSnapshotId(3),
                                                   "cancel.lang", "package p;");
  auto tokens = lex(snapshot);
  require(tokens.success());
  std::stop_source stop;
  stop.request_stop();
  auto cancelled = parse(*tokens.value, stop.get_token());
  require(cancelled.state == PhaseState::cancelled && !cancelled.value);
  std::cout << "real lexer AST/recovery/depth/cancel tests passed\n";
}
} // namespace

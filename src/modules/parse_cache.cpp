#include "lang/modules/parse_cache.hpp"
#include "lang/support/cache_io.hpp"
#include <algorithm>

namespace lang::modules {
namespace {
std::string filename(const ParseCache &cache, const SourceSnapshot &source) {
  cache::Writer key;
  key.string(cache.fingerprint); key.string(source.path); key.string(sha256(source.original));
  return sha256(key.bytes) + ".ast";
}
void span(cache::Writer &out, SourceSpan value) {
  out.number(value.begin_byte); out.number(value.end_byte);
}
SourceSpan span(cache::Reader &in, const SourceSnapshot &source) {
  auto begin = in.number(), end = in.number();
  if (begin > end || end > source.original.size()) throw std::invalid_argument("invalid cached span");
  // Byte boundaries must be valid in the current immutable source snapshot.
  source.position(begin); source.position(end);
  return {source.id, begin, end};
}
}
std::shared_ptr<const AstFile> ParseCache::load(std::shared_ptr<const SourceSnapshot> source) {
  ++misses;
  if (!source->valid()) return {};
  try {
    auto bytes = cache::load(directory / filename(*this, *source));
    if (!bytes) return {};
    cache::Reader in{*bytes};
    if (in.string() != "wz-ast/1" || in.string() != source->path ||
        in.string() != sha256(source->original) || in.string() != fingerprint) return {};
    auto file = std::make_shared<AstFile>(); file->source = source;
    const auto token_count = in.count();
    if (token_count > source->normalized().size() + 1) return {};
    size_t normalized_cursor = 0;
    for (size_t i = 0; i < token_count; ++i) {
      const auto kind = in.number();
      if (kind > static_cast<unsigned>(TokenKind::punctuation)) {
        if (kind != static_cast<unsigned>(TokenKind::end)) return {};
      }
      auto where = span(in, *source);
      auto normalized_begin = in.number(), normalized_end = in.number();
      if (normalized_begin > normalized_end || normalized_end > source->normalized().size() ||
          source->span(normalized_begin, normalized_end).begin_byte != where.begin_byte ||
          source->span(normalized_begin, normalized_end).end_byte != where.end_byte) return {};
      Token token{static_cast<TokenKind>(kind), where,
          std::string_view(source->normalized()).substr(normalized_begin, normalized_end - normalized_begin), {}, {}};
      if (in.flag()) token.string_value = in.string();
      if (token.kind == TokenKind::string_literal) {
        ++string_tokens_checked;
        // Reuse the lexer on the real source token. The cached decoded value
        // must never redirect an import or change a string literal.
        auto fragment = std::make_shared<const SourceSnapshot>(FileId{0}, RevisionId{0},
            SourceSnapshotId{0}, "<cached-string-token>", std::string(token.spelling));
        auto decoded = lex(fragment);
        if (!decoded.success() || decoded.value->tokens.size() != 2 ||
            decoded.value->tokens.front().kind != TokenKind::string_literal ||
            decoded.value->tokens.front().spelling.size() != token.spelling.size() ||
            !decoded.value->tokens.front().leading_trivia.empty() ||
            decoded.value->tokens.front().string_value != token.string_value) return {};
      } else if (token.string_value) return {};
      if (normalized_begin < normalized_cursor ||
          (token.kind == TokenKind::end ?
            (i + 1 != token_count || normalized_begin != source->normalized().size() || normalized_end != normalized_begin) :
            normalized_begin == normalized_end)) return {};
      size_t trivia_cursor = normalized_cursor;
      const auto trivia_count = in.count();
      for (size_t j = 0; j < trivia_count; ++j) {
        auto trivia_kind = in.number();
        if (trivia_kind > static_cast<unsigned>(TriviaKind::block_comment)) return {};
        auto location = span(in, *source);
        auto begin = in.number(), end = in.number();
        if (begin != trivia_cursor || begin >= end || end > normalized_begin ||
            source->span(begin, end).begin_byte != location.begin_byte ||
            source->span(begin, end).end_byte != location.end_byte) return {};
        trivia_cursor = end;
        token.leading_trivia.push_back({static_cast<TriviaKind>(trivia_kind), location,
            std::string_view(source->normalized()).substr(begin, end - begin)});
      }
      if (trivia_cursor != normalized_begin) return {};
      normalized_cursor = normalized_end;
      file->tokens.push_back(std::move(token));
    }
    const auto node_count = in.count();
    if (node_count > source->normalized().size() * 4 + 16) return {};
    file->root = AstNodeId{in.number()};
    if (!token_count || !node_count || file->root.value >= node_count ||
        file->tokens.back().kind != TokenKind::end) return {};
    std::vector<size_t> parents(node_count);
    for (size_t i = 0; i < node_count; ++i) {
      auto kind = in.number();
      if (kind > static_cast<unsigned>(AstKind::TraitObjectType) || kind == static_cast<unsigned>(AstKind::Error)) return {};
      AstNode node{static_cast<AstKind>(kind), span(in, *source), static_cast<size_t>(in.number()), static_cast<size_t>(in.number()), {}};
      if (node.token_begin >= token_count || node.token_begin > node.token_end || node.token_end > token_count) return {};
      auto expected = file->tokens[node.token_begin].span;
      if (node.token_end > node.token_begin) expected.end_byte = file->tokens[node.token_end - 1].span.end_byte;
      if (node.span.begin_byte != expected.begin_byte || node.span.end_byte != expected.end_byte) return {};
      const auto children = in.count();
      if (children > node_count) return {};
      for (size_t j = 0; j < children; ++j) {
        auto child = in.number();
        if (child >= node_count || child == i || ++parents[child] > 1) return {};
        node.children.push_back(AstNodeId{child});
      }
      file->nodes.push_back(std::move(node));
    }
    in.done();
    if (parents[file->root.value] || file->node(file->root).kind != AstKind::File) return {};
    std::vector<bool> visited(node_count);
    std::vector<AstNodeId> pending{file->root};
    size_t reached = 0;
    while (!pending.empty()) {
      auto id = pending.back(); pending.pop_back();
      if (visited[id.value]) return {};
      visited[id.value] = true; ++reached;
      const auto &parent = file->node(id);
      for (auto child : parent.children) {
        const auto &node = file->node(child);
        if (node.span.begin_byte < parent.span.begin_byte || node.span.end_byte > parent.span.end_byte ||
            node.token_begin < parent.token_begin || node.token_end > parent.token_end) return {};
        pending.push_back(child);
      }
    }
    // Parser arenas can contain speculative unused syntax nodes. They must not
    // be referenced by the rooted tree; preserve them for current source IDs.
    (void)reached;
    if (file->node(file->root).children.empty() ||
        file->node(file->node(file->root).children.front()).kind != AstKind::Package) return {};
    --misses; ++hits; return file;
  } catch (const std::exception &) { return {}; }
}
void ParseCache::store(const AstFile &file) {
  cache::Writer out;
  out.string("wz-ast/1"); out.string(file.source->path);
  out.string(sha256(file.source->original)); out.string(fingerprint);
  out.number(file.tokens.size());
  auto normalized_offset = [&](std::string_view spelling) {
    if (spelling.empty()) return file.source->normalized().size();
    return static_cast<size_t>(spelling.data() - file.source->normalized().data());
  };
  for (const auto &token : file.tokens) {
    out.number(static_cast<unsigned>(token.kind)); span(out, token.span);
    auto begin = normalized_offset(token.spelling); out.number(begin); out.number(begin + token.spelling.size());
    out.number(token.string_value.has_value()); if (token.string_value) out.string(*token.string_value);
    out.number(token.leading_trivia.size());
    for (const auto &trivia : token.leading_trivia) {
      out.number(static_cast<unsigned>(trivia.kind)); span(out, trivia.span);
      begin = normalized_offset(trivia.spelling); out.number(begin); out.number(begin + trivia.spelling.size());
    }
  }
  out.number(file.nodes.size()); out.number(file.root.value);
  for (const auto &node : file.nodes) {
    out.number(static_cast<unsigned>(node.kind)); span(out, node.span);
    out.number(node.token_begin); out.number(node.token_end); out.number(node.children.size());
    for (auto child : node.children) out.number(child.value);
  }
  cache::store(directory, filename(*this, *file.source), out.bytes);
}
}

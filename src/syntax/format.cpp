#include "lang/syntax/format.hpp"
#include "lang/syntax/parser.hpp"

#include <algorithm>

namespace lang {
namespace {
struct Layout {
  bool new_line = false;
  bool group = false;
  bool prefix = false;
  bool slice_colon = false;
  int branch_indent = 0;
};

std::vector<Layout> layout_for(const AstFile &file) {
  std::vector<Layout> layout(file.tokens.size());
  for (const auto &node : file.nodes) {
    auto &first = layout[node.token_begin];
    switch (node.kind) {
    case AstKind::Parenthesized:
    case AstKind::ParenthesizedType:
    case AstKind::List:
      first.group = true;
      break;
    case AstKind::Unary:
    case AstKind::LiteralPattern: {
      const auto spelling = file.tokens[node.token_begin].spelling;
      first.prefix = spelling == "+" || spelling == "-" || spelling == "!" ||
                     spelling == "~";
      break;
    }
    case AstKind::Slice: {
      auto colon = file.node(node.children.front()).token_end + 1;
      for (auto id : node.children) {
        const auto &child = file.node(id);
        if (child.kind == AstKind::BracketItem)
          colon = child.token_end;
      }
      layout[colon].slice_colon = true;
      break;
    }
    case AstKind::Case: {
      const auto start = file.tokens[node.token_begin].spelling == "case" ? 1U : 0U;
      if (start == node.children.size())
        break;
      ++layout[file.node(node.children[start]).token_begin].branch_indent;
      --layout[node.token_end].branch_indent;
      for (size_t i = start; i < node.children.size(); ++i)
        layout[file.node(node.children[i]).token_begin].new_line = true;
      break;
    }
    case AstKind::File:
    case AstKind::Block:
    case AstKind::Struct:
    case AstKind::Enum:
    case AstKind::Trait:
    case AstKind::Impl:
    case AstKind::Switch:
      for (auto id : node.children) {
        const auto &child = file.node(id);
        if ((node.kind == AstKind::Struct && child.kind != AstKind::Field) ||
            (node.kind == AstKind::Enum && child.kind != AstKind::Variant) ||
            ((node.kind == AstKind::Trait || node.kind == AstKind::Impl) &&
             child.kind != AstKind::Function) ||
            (node.kind == AstKind::Switch && child.kind != AstKind::Case))
          continue;
        layout[child.token_begin].new_line = true;
        if (node.kind != AstKind::File)
          layout[node.token_end - 1].new_line = true;
      }
      break;
    default:
      break;
    }
  }
  return layout;
}

bool opening(std::string_view spelling) {
  return spelling == "(" || spelling == "[" || spelling == "{";
}

bool closing(std::string_view spelling) {
  return spelling == ")" || spelling == "]" || spelling == "}";
}

bool space_between(std::string_view left, const Layout &previous,
                   std::string_view right, const Layout &next) {
  if (right == "." || right == "," || right == ";" || right == ":" ||
      closing(right) || left == "." || left == "(" || left == "[" ||
      previous.slice_colon)
    return false;
  if (previous.prefix)
    return (left == "+" || left == "-") && left == right;
  if (right == "(" || right == "[")
    return next.group;
  return true;
}

std::string_view original(const SourceSnapshot &source, SourceSpan span) {
  return std::string_view(source.original)
      .substr(span.begin_byte, span.end_byte - span.begin_byte);
}

std::string format(const AstFile &file) {
  const auto &source = *file.source;
  const auto layout = layout_for(file);
  const auto &first = file.tokens.front();
  auto start = first.span.begin_byte;
  for (const auto &trivia : first.leading_trivia)
    if (trivia.kind != TriviaKind::whitespace) {
      start = trivia.span.begin_byte;
      break;
    }
  const auto first_newline = source.original.find('\n', start);
  const std::string_view eol =
      first_newline != source.original.npos && first_newline > 0 &&
              source.original[first_newline - 1] == '\r'
          ? "\r\n"
          : "\n";
  std::string result;
  result.reserve(source.original.size());
  if (source.original.starts_with("\xef\xbb\xbf"))
    result += "\xef\xbb\xbf";

  int indent = 0;
  size_t breaks = 0;
  bool has_content = false;
  bool previous_comment = false;
  auto emit = [&](std::string_view bytes, bool space) {
    if (has_content)
      for (size_t i = 0; i < breaks; ++i)
        result += eol;
    if (!has_content || breaks != 0)
      result.append(static_cast<size_t>(indent) * 4, ' ');
    else if (space)
      result += ' ';
    result += bytes;
    has_content = true;
    breaks = 0;
  };

  for (size_t i = 0; i < file.tokens.size(); ++i) {
    const auto &token = file.tokens[i];
    const auto &style = layout[i];
    indent += std::max(style.branch_indent, 0);
    for (const auto &trivia : token.leading_trivia) {
      if (trivia.kind == TriviaKind::whitespace) {
        breaks += std::count(trivia.spelling.begin(), trivia.spelling.end(), '\n');
      } else {
        emit(original(source, trivia.span), true);
        previous_comment = true;
      }
    }
    if (token.kind == TokenKind::end)
      break;

    indent += std::min(style.branch_indent, 0);
    if (token.kind == TokenKind::punctuation && closing(token.spelling))
      --indent;
    if (style.new_line && breaks == 0)
      breaks = 1;
    emit(original(source, token.span),
         previous_comment ||
             (i != 0 && space_between(file.tokens[i - 1].spelling, layout[i - 1],
                                      token.spelling, style)));
    previous_comment = false;
    if (token.kind == TokenKind::punctuation && opening(token.spelling))
      ++indent;
  }
  result += eol;
  return result;
}

bool same_tokens(const TokenBuffer &before, const TokenBuffer &after) {
  return std::ranges::equal(before.tokens, after.tokens,
      [](const Token &left, const Token &right) {
        return left.kind == right.kind && left.spelling == right.spelling;
      });
}
} // namespace

PhaseResult<std::string>
format_source(std::shared_ptr<const SourceSnapshot> source) {
  auto tokens = lex(source);
  if (!tokens.success())
    return {tokens.state, {}, std::move(tokens.diagnostics), tokens.failure};
  auto parsed = parse(*tokens.value);
  if (!parsed.success())
    return {parsed.state, {}, std::move(parsed.diagnostics), parsed.failure};

  std::string formatted = format(*parsed.value);
  if (formatted != source->original) {
    auto rewritten = lex(std::make_shared<const SourceSnapshot>(
        source->file, source->revision, source->id, source->path, formatted));
    if (!rewritten.success() || !same_tokens(*tokens.value, *rewritten.value)) {
      Diagnostic diagnostic;
      diagnostic.code = "E_INTERNAL_ERROR";
      diagnostic.phase = "toolchain";
      diagnostic.severity = "error";
      diagnostic.message = "Formatting changed the source tokens";
      return {PhaseState::failed, {}, {std::move(diagnostic)}, FailureKind::internal};
    }
  }
  return {PhaseState::complete, std::move(formatted), {}, FailureKind::source};
}
} // namespace lang

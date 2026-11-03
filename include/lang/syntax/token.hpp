#pragma once
#include "lang/support/phase.hpp"
#include <string_view>

namespace lang {
enum class TokenKind {
  end,
  identifier,
  integer,
  floating,
  string_literal,
  keyword,
  punctuation,
  error
};

enum class TriviaKind { whitespace, line_comment, block_comment };
struct Trivia {
  TriviaKind kind;
  SourceSpan span;
  std::string_view spelling;
};

struct Token {
  TokenKind kind;
  SourceSpan span;
  std::string_view spelling;
  std::vector<Trivia> leading_trivia;
  std::optional<std::string> string_value;
};

struct TokenBuffer {
  // Views in tokens and trivia refer to this immutable normalized text.
  std::shared_ptr<const SourceSnapshot> source;
  std::vector<Token> tokens;
};

PhaseResult<TokenBuffer>
lex(std::shared_ptr<const SourceSnapshot>, std::stop_token = {});
} // namespace lang

#include "lang/syntax/token.hpp"
#include <algorithm>
#include <regex>

namespace lang {
namespace {
bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool member(std::string_view words, std::string_view word) {
  return words.find(" " + std::string(word) + " ") != words.npos;
}
void utf8(std::string &s, unsigned c) {
  if (c < 128)
    s += char(c);
  else if (c < 2048) {
    s += char(192 | (c >> 6));
    s += char(128 | (c & 63));
  } else if (c < 65536) {
    s += char(224 | (c >> 12));
    s += char(128 | ((c >> 6) & 63));
    s += char(128 | (c & 63));
  } else {
    s += char(240 | (c >> 18));
    s += char(128 | ((c >> 12) & 63));
    s += char(128 | ((c >> 6) & 63));
    s += char(128 | (c & 63));
  }
}
} // namespace
PhaseResult<TokenBuffer> lex(std::shared_ptr<const SourceSnapshot> source,
                             std::stop_token stop) {
  PhaseResult<TokenBuffer> result;
  result.failure = FailureKind::source;
  auto error = [&](SourceSpan span, bool literal, const char *message) {
    Diagnostic d;
    d.code = literal ? "E_LEX_INVALID_LITERAL" : "E_LEX_INVALID_TOKEN";
    d.phase = "lex";
    d.severity = "error";
    d.message = message;
    d.primary = span;
    result.diagnostics.push_back(std::move(d));
  };
  try {
    check_cancelled(stop);
    if (!source) {
      result.failure = FailureKind::internal;
      return result;
    }
    if (!source->valid()) {
      const auto &e = *source->error();
      error(source->raw_span(e.byte, e.byte + e.length), false, e.what());
      return result;
    }
    TokenBuffer buffer{source, {}};
    std::string_view text = source->normalized();
    size_t p = 0;
    auto peek = [&](size_t n = 0) {
      return p + n < text.size() ? text[p + n] : '\0';
    };
    auto starts = [&](std::string_view s) {
      return text.substr(p, s.size()) == s;
    };
    std::vector<Trivia> trivia;
    const std::regex number(
        R"((0b[01](_?[01])*|0o[0-7](_?[0-7])*|0x[0-9a-fA-F](_?[0-9a-fA-F])*|(0|[1-9](_?[0-9])*)(\.[0-9](_?[0-9])*)?([eE][+-]?[0-9](_?[0-9])*)?))");
    while (true) {
      check_cancelled(stop);
      size_t begin = p;
      if (peek() == ' ' || peek() == '\t' || peek() == '\n' || starts("//") ||
          starts("/*")) {
        TriviaKind kind = TriviaKind::whitespace;
        if (starts("//")) {
          kind = TriviaKind::line_comment;
          while (p < text.size() && peek() != '\n') {
            check_cancelled(stop);
            ++p;
          }
        } else if (starts("/*")) {
          kind = TriviaKind::block_comment;
          p += 2;
          while (p < text.size() && !starts("*/")) {
            check_cancelled(stop);
            ++p;
          }
          if (p == text.size())
            error(source->span(begin, p), false, "unterminated block comment");
          else
            p += 2;
        } else
          while (peek() == ' ' || peek() == '\t' || peek() == '\n') {
            check_cancelled(stop);
            ++p;
          }
        trivia.push_back(
            {kind, source->span(begin, p), text.substr(begin, p - begin)});
        continue;
      }
      TokenKind kind = TokenKind::end;
      std::optional<std::string> value;
      bool bad = false, literal = false;
      if (p == text.size()) {
        buffer.tokens.push_back(
            {kind, source->span(p, p), text.substr(p), std::move(trivia), {}});
        break;
      }
      if (letter(peek())) {
        while (letter(peek()) || digit(peek())) {
          check_cancelled(stop);
          ++p;
        }
        auto word = text.substr(begin, p - begin);
        kind = member(" as  break  case  catch  const  continue  default  "
                      "defer  else  enum  false  for  func  if  impl  import  "
                      "in  inout  let  move  package  pub  return  struct  "
                      "switch  throw  throws  trait  true  try  unsafe  var  "
                      "async  await ",
                      word)
                   ? TokenKind::keyword
                   : TokenKind::identifier;
        bad = member(" class  extends  extern  operator ", word);
      } else if (digit(peek())) {
        literal = true;
        while (letter(peek()) || digit(peek()) ||
               (peek() == '.' && digit(peek(1))) ||
               ((peek() == '+' || peek() == '-') && p > begin &&
                (text[p - 1] == 'e' || text[p - 1] == 'E') &&
                !(p > begin + 1 && text[begin] == '0' &&
                  text[begin + 1] == 'x'))) {
          check_cancelled(stop);
          ++p;
        }
        auto spelling = text.substr(begin, p - begin);
        bad = !std::regex_match(spelling.begin(), spelling.end(), number);
        kind = spelling.find_first_of(".eE") != spelling.npos &&
                       !(spelling.size() > 1 && spelling[0] == '0' &&
                         spelling[1] == 'x')
                   ? TokenKind::floating
                   : TokenKind::integer;
      } else if (peek() == '"' || peek() == '`') {
        literal = true;
        kind = TokenKind::string_literal;
        char quote = text[p++];
        std::string decoded;
        while (p < text.size() && peek() != quote) {
          check_cancelled(stop);
          unsigned char c = text[p++];
          if (quote == '`') {
            decoded += char(c);
            continue;
          }
          if (c < 32 || c == 127) {
            bad = true;
            if (c == '\n')
              break;
            continue;
          }
          if (c != '\\') {
            decoded += char(c);
            continue;
          }
          if (p == text.size()) {
            bad = true;
            break;
          }
          char e = text[p++];
          switch (e) {
          case 'n':
            decoded += '\n';
            break;
          case 'r':
            decoded += '\r';
            break;
          case 't':
            decoded += '\t';
            break;
          case '0':
            decoded += '\0';
            break;
          case '\\':
          case '"':
            decoded += e;
            break;
          case 'u': {
            if (peek() != '{') {
              bad = true;
              break;
            }
            ++p;
            unsigned scalar = 0;
            size_t count = 0;
            while ((peek() >= '0' && peek() <= '9') ||
                   (peek() >= 'a' && peek() <= 'f') ||
                   (peek() >= 'A' && peek() <= 'F')) {
              check_cancelled(stop);
              char h = text[p++];
              if (count < 6)
                scalar = scalar * 16 +
                         (digit(h) ? h - '0'
                                   : (h >= 'a' ? h - 'a' + 10 : h - 'A' + 10));
              ++count;
            }
            if (peek() == '}')
              ++p;
            else
              bad = true;
            if (!count || count > 6 || scalar > 0x10ffff ||
                (scalar >= 0xd800 && scalar <= 0xdfff))
              bad = true;
            else
              utf8(decoded, scalar);
            break;
          }
          default:
            bad = true;
          }
        }
        if (p < text.size() && peek() == quote)
          ++p;
        else
          bad = true;
        if (!bad)
          value = std::move(decoded);
      } else {
        kind = TokenKind::punctuation;
        size_t length = 0;
        for (auto op : {"<<=", ">>=", "++", "--", "->", "=>", "==", "!=",
                        "<=",  ">=",  "&&", "||", "<<", ">>", "+=", "-=",
                        "*=",  "/=",  "%=", "&=", "|=", "^="}) {
          if (starts(op)) {
            length = std::string_view(op).size();
            bad = member(" ++  --  ->  => ", op);
            break;
          }
        }
        if (length)
          p += length;
        else if (std::string_view("+-*/%!~&|^<>=()[]{}.,:;").find(peek()) !=
                 text.npos)
          ++p;
        else {
          bad = true;
          ++p;
          while (p < text.size() &&
                 (static_cast<unsigned char>(peek()) & 192) == 128)
            ++p;
        }
      }
      if (bad) {
        kind = TokenKind::error;
        error(source->span(begin, p), literal,
              literal ? "invalid literal" : "invalid or reserved token");
      }
      buffer.tokens.push_back({kind, source->span(begin, p),
                               text.substr(begin, p - begin), std::move(trivia),
                               std::move(value)});
      trivia.clear();
    }
    result.state = result.diagnostics.empty() ? PhaseState::complete
                                              : PhaseState::recovered;
    result.value = std::move(buffer);
  } catch (const Cancelled &) {
    result.state = PhaseState::cancelled;
  }
  return result;
}
} // namespace lang

#include "lang/syntax/token.hpp"
#include <iostream>
#include <stdexcept>
using namespace lang;
void check(bool b) {
  if (!b)
    throw std::runtime_error("lexer check failed");
}
auto scan(std::string s) {
  return lex(std::make_shared<SourceSnapshot>(
      FileId(1), RevisionId(1), SourceSnapshotId(1), "test", std::move(s)));
}
int main() {
  try {
    for (auto s :
         {"0", "42", "1_000", "0b10_01", "0o755", "0xdead_BEEF", "1.0", "1e-3",
          "1_000.25", "9223372036854775808", "fn while auto null i32 share",
          "a>>=2;", "/* a /* b */ x", "`中文\n😀`", "\"\\u{10FFFF}\\0\\n\""})
      check(scan(s).success());
    for (auto s : {"007", "0x_FF", "1__0", "10_", "123i32", "0b2", "1e+",
                   "async", "++", "?", "/*", "\"\\x20\"", "\"\\u{D800}\"",
                   "\"\\u{110000}\"", "\"\\u{}\"", "\"a\n", "`a", "中文"}) {
      auto r = scan(s);
      check(!r.success());
      check(r.state == PhaseState::recovered);
      check(r.value->tokens.back().kind == TokenKind::end);
    }
    auto r = scan("\xef\xbb\xbf //中文😀\r\n\"\\u{1F600}\\0\"\r\n");
    check(r.success());
    const auto &t = r.value->tokens[0];
    check(t.span.begin_byte == 18);
    check(t.string_value == std::string("😀\0", 5));
    check(t.leading_trivia.size() == 3);
    check(r.value->tokens.back().leading_trivia.size() == 1);
    check(t.spelling.data() >= r.value->source->normalized().data());
    for (auto s :
         {std::string("a\0b", 3), std::string("\xff"), std::string("a\rb")}) {
      auto invalid = scan(s);
      check(invalid.state == PhaseState::failed);
      check(!invalid.diagnostics.empty());
    }
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = lex(r.value->source, stop.get_token());
    check(cancelled.state == PhaseState::cancelled);
    check(int(cancelled.exit_code()) == 130);
    std::cout << "lexer tests passed\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

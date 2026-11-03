#include "lang/support/diagnostic.hpp"
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace lang;
void check(bool condition) {
  if (!condition)
    throw std::runtime_error("test check failed");
}
template <class F> void fails(F f) {
  bool failed = false;
  try {
    f();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed);
}
int main() {
  static_assert(!std::is_convertible_v<FileId, TypeId>);
  static_assert(!std::is_convertible_v<int, SymbolId>);
  SourceManager m;
  auto f = m.add_file("test.lang");
  auto s = m.update(f, "\xef\xbb\xbf中😀\t\r\nx\xef\xbb\xbf");
  check(s->normalized() == "中😀\t\nx\xef\xbb\xbf");
  check((s->position(10) == Position{0, 3}));
  check(s->byte({0, 4}) == 11);
  check(s->span(8, 9).begin_byte == 11);
  check(s->span(8, 9).end_byte == 13);
  fails([&] { s->byte({0, 2}); });
  fails([&] { s->position(12); });
  fails([&] { s->span(1, 2); });
  fails([&] { m.get(SourceSnapshotId(999)); });
  fails([&] { m.update(FileId{}, ""); });
  for (auto b : {std::string("\xc0\xaf"), std::string("\x80"),
                 std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80"),
                 std::string("\0", 1), std::string("\r")}) {
    auto bad = m.update(f, "a" + b);
    check(!bad->valid());
    check(bad->original == "a" + b);
    check(m.get(bad->id) == bad);
    check(bad->error()->byte == 1);
    fails([&] { bad->normalized(); });
    fails([&] { bad->position(0); });
    fails([&] { bad->byte({0, 0}); });
    fails([&] { bad->span(0, 0); });
    Diagnostic error{"E_INVALID_SOURCE",
                     "lex",
                     "error",
                     bad->error()->what(),
                     bad->raw_span(1, 1 + bad->error()->length),
                     {},
                     {},
                     {},
                     {},
                     {}};
    auto json = diagnostic_json(m, "v", std::to_string(bad->revision.value),
                                Status::error, {error});
    check(json.find("\"start_byte\":1,\"end_byte\":" +
                    std::to_string(1 + bad->error()->length)) != std::string::npos);
    if (b == std::string("\x80"))
      std::cout << json << '\n';
  }
  auto t = m.update(f, "a\nb");
  for (const auto &bytes : {std::string("\xe4\xb8x"),
                            std::string("\xe4\xb8"),
                            std::string("\xe0\x80\x80")}) {
    auto bad = m.update(f, "a" + bytes);
    check(!bad->valid());
    check(bad->error()->byte == 1);
    check(bad->error()->length == (bytes.back() == 'x' ? 2 : bytes.size()));
  }
  auto empty = m.update(f, "");
  check(empty->span(0, 0).begin_byte == 0);
  check(empty->byte({0, 0}) == 0);
  auto bom_only = m.update(f, "\xef\xbb\xbf");
  check(bom_only->span(0, 0).begin_byte == 3);
  check(bom_only->byte({0, 0}) == 3);
  check(t->valid());
  check(t->normalized() == "a\nb");
  check((t->position(2) == Position{1, 0}));
  check(t->byte({1, 0}) == 2);
  check(s->raw_span(4, 5).begin_byte == 4);
  fails([&] { s->raw_span(0, s->original.size() + 1); });
  fails([&] { s->raw_span(2, 1); });
  check(s->original.starts_with("\xef\xbb\xbf"));
  check(t->span(1, 2).end_byte == 2);
  check(m.get(s->id) == s);
  Diagnostic d{"E_TEST",
               "lex",
               "error",
               "quote\" slash\\\n\t\x01",
               s->span(0, 3),
               {},
               {},
               {},
               {},
               {}};
  d.related.push_back({t->span(0, 1), "related"});
  d.fixits.push_back({s->span(0, 3), "replacement", s->revision});
  auto json = diagnostic_json(m, "中文😀", "test", Status::error, {d});
  fails([&] { diagnostic_json(m, "v", "r", Status::ok, {d}); });
  check(json.find("中文😀") != std::string::npos);
  check(json.find("quote\\\" slash\\\\\\u000a\\u0009\\u0001") !=
        std::string::npos);
  const std::string invalid("\x80");
  fails([&] { diagnostic_json(m, invalid, "r", Status::error, {d}); });
  fails([&] { diagnostic_json(m, "v", invalid, Status::error, {d}); });
  for (int field = 0; field < 7; ++field) {
    auto broken = d;
    switch (field) {
    case 0:
      broken.message = invalid;
      break;
    case 1:
      broken.notes = {invalid};
      break;
    case 2:
      broken.fixits[0].replacement = invalid;
      break;
    case 3:
      broken.related[0].message = invalid;
      break;
    case 4:
      broken.expected_type = invalid;
      break;
    case 5:
      broken.actual_type = invalid;
      break;
    case 6: {
      auto file = m.add_file(invalid);
      broken.primary = m.update(file, "a")->raw_span(0, 1);
      break;
    }
    }
    fails([&] { diagnostic_json(m, "v", "r", Status::error, {broken}); });
  }
  d.message = std::string("a\0b", 3);
  check(diagnostic_json(m, "v", "r", Status::error, {d}).find("a\\u0000b") !=
        std::string::npos);
  d.fixits[0].revision = t->revision;
  fails([&] { diagnostic_json(m, "v", "r", Status::error, {d}); });
  check(static_cast<int>(ExitCode::cancelled) == 130);
}

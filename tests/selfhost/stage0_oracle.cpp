// Stage-0 frontend oracle for SH04.
//
// This executable is a test-only adapter over the existing C++ source,
// lexer and parser.  The Wz frontend is never allowed to execute it.  It
// reads one UTF-8 (or deliberately invalid) source file and writes a single
// canonical JSON document containing every fact compared by front_diff.py.
#include "lang/support/source.hpp"
#include "lang/syntax/parser.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace lang;

constexpr uint64_t kFileId = 7;
constexpr uint64_t kRevisionId = 11;
constexpr uint64_t kSourceId = 13;

const char *phase_name(PhaseState state) {
  switch (state) {
  case PhaseState::complete:
    return "complete";
  case PhaseState::recovered:
    return "recovered";
  case PhaseState::failed:
    return "failed";
  case PhaseState::cancelled:
    return "cancelled";
  }
  return "unknown";
}

int phase_id(PhaseState state) {
  switch (state) {
  case PhaseState::complete:
    return 0;
  case PhaseState::recovered:
    return 1;
  case PhaseState::failed:
    return 2;
  case PhaseState::cancelled:
    return 3;
  }
  return -1;
}

const char *failure_name(FailureKind failure) {
  switch (failure) {
  case FailureKind::source:
    return "source";
  case FailureKind::unsupported:
    return "unsupported";
  case FailureKind::environment:
    return "environment";
  case FailureKind::internal:
    return "internal";
  }
  return "unknown";
}

int failure_id(FailureKind failure) {
  switch (failure) {
  case FailureKind::source:
    return 0;
  case FailureKind::unsupported:
    return 1;
  case FailureKind::environment:
    return 2;
  case FailureKind::internal:
    return 3;
  }
  return -1;
}

const char *token_name(TokenKind kind) {
  switch (kind) {
  case TokenKind::end:
    return "end";
  case TokenKind::identifier:
    return "identifier";
  case TokenKind::integer:
    return "integer";
  case TokenKind::floating:
    return "floating";
  case TokenKind::string_literal:
    return "string_literal";
  case TokenKind::keyword:
    return "keyword";
  case TokenKind::punctuation:
    return "punctuation";
  case TokenKind::error:
    return "error";
  }
  return "unknown";
}

const char *trivia_name(TriviaKind kind) {
  switch (kind) {
  case TriviaKind::whitespace:
    return "whitespace";
  case TriviaKind::line_comment:
    return "line_comment";
  case TriviaKind::block_comment:
    return "block_comment";
  }
  return "unknown";
}

const char *ast_name(AstKind kind) {
  static const char *names[] = {
      "File",          "Package",       "Import",          "Binding",
      "Function",      "Struct",        "Enum",            "Trait",
      "Impl",          "Field",         "Variant",         "GenericParameter",
      "Parameter",     "Async",         "Await",           "NamedType",
      "FunctionType",  "ParenthesizedType", "Block",       "ExpressionStatement",
      "Assign",        "If",            "For",             "ForIn",
      "Switch",        "Case",          "Try",             "Catch",
      "Defer",         "Unsafe",        "Return",          "Throw",
      "Break",         "Continue",      "Name",            "Literal",
      "Unary",         "Binary",        "Call",            "Argument",
      "Member",        "NeutralBracket", "BracketItem",    "Slice",
      "List",          "FunctionLiteral", "Parenthesized", "BindPattern",
      "LiteralPattern", "VariantPattern", "Error",         "TraitObjectType"};
  const auto value = static_cast<size_t>(kind);
  return value < sizeof(names) / sizeof(names[0]) ? names[value] : "unknown";
}

int ast_id(AstKind kind) { return static_cast<int>(kind); }

void number(std::string &out, uint64_t value) {
  out += std::to_string(value);
}

void boolean(std::string &out, bool value) { out += value ? "true" : "false"; }

void json_string(std::string &out, std::string_view value) {
  out += '"';
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        char buffer[7];
        std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
        out += buffer;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += '"';
}

void hex(std::string &out, std::string_view value) {
  static const char digits[] = "0123456789abcdef";
  out += '"';
  for (unsigned char c : value) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  out += '"';
}

void span(std::string &out, SourceSpan value) {
  out += "{\"snapshot\":";
  number(out, value.snapshot.value);
  out += ",\"begin\":";
  number(out, value.begin_byte);
  out += ",\"end\":";
  number(out, value.end_byte);
  out += '}';
}

void diagnostics(std::string &out, const std::vector<Diagnostic> &values) {
  out += '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      out += ',';
    const auto &d = values[i];
    out += "{\"code\":";
    json_string(out, d.code);
    out += ",\"phase\":";
    json_string(out, d.phase);
    out += ",\"severity\":";
    json_string(out, d.severity);
    out += ",\"message\":";
    json_string(out, d.message);
    out += ",\"primary\":";
    if (d.primary)
      span(out, *d.primary);
    else
      out += "null";
    out += ",\"related\":[";
    for (size_t r = 0; r < d.related.size(); ++r) {
      if (r)
        out += ',';
      out += "{\"span\":";
      span(out, d.related[r].span);
      out += ",\"message\":";
      json_string(out, d.related[r].message);
      out += '}';
    }
    out += "],\"fixits\":[";
    for (size_t f = 0; f < d.fixits.size(); ++f) {
      if (f)
        out += ',';
      out += "{\"span\":";
      span(out, d.fixits[f].span);
      out += ",\"replacement\":";
      json_string(out, d.fixits[f].replacement);
      out += ",\"revision\":";
      number(out, d.fixits[f].revision.value);
      out += '}';
    }
    out += "],\"notes\":[";
    for (size_t n = 0; n < d.notes.size(); ++n) {
      if (n)
        out += ',';
      json_string(out, d.notes[n]);
    }
    out += "],\"expected_type\":";
    if (d.expected_type)
      json_string(out, *d.expected_type);
    else
      out += "null";
    out += ",\"actual_type\":";
    if (d.actual_type)
      json_string(out, *d.actual_type);
    else
      out += "null";
    out += '}';
  }
  out += ']';
}

struct DerivedBoundary {
  uint64_t byte;
  uint64_t original;
  Position position;
};

struct Derived {
  std::vector<uint64_t> offsets;
  std::vector<DerivedBoundary> boundaries;
};

size_t utf8_length(unsigned char c) {
  if (c < 0x80)
    return 1;
  if ((c & 0xe0) == 0xc0)
    return 2;
  if ((c & 0xf0) == 0xe0)
    return 3;
  if ((c & 0xf8) == 0xf0)
    return 4;
  throw std::runtime_error("invalid normalized UTF-8");
}

// SourceSnapshot intentionally keeps the byte map private.  Rebuild it from
// the public span/position operations while checking that every mapping is a
// real character boundary, not an interior byte of a UTF-8 scalar.
Derived derive(const SourceSnapshot &source) {
  Derived result;
  const auto &normalized = source.normalized();
  const auto first = source.span(0, 0);
  result.offsets.push_back(first.begin_byte);
  result.boundaries.push_back(
      {0, first.begin_byte, source.position(first.begin_byte)});
  size_t begin = 0;
  while (begin < normalized.size()) {
    const size_t length =
        utf8_length(static_cast<unsigned char>(normalized[begin]));
    if (length > normalized.size() - begin)
      throw std::runtime_error("truncated normalized UTF-8");
    const auto mapped = source.span(begin, begin + length);
    const uint64_t raw_length = mapped.end_byte - mapped.begin_byte;
    if (raw_length == length) {
      for (size_t j = 1; j < length; ++j)
        result.offsets.push_back(mapped.begin_byte + j);
    } else if (!(length == 1 && raw_length == 2)) {
      throw std::runtime_error("unexpected normalized byte mapping");
    }
    result.offsets.push_back(mapped.end_byte);
    begin += length;
    result.boundaries.push_back(
        {begin, mapped.end_byte, source.position(mapped.end_byte)});
  }
  if (result.offsets.size() != normalized.size() + 1 ||
      result.boundaries.empty())
    throw std::runtime_error("incomplete source byte map");
  return result;
}

void write_source(std::string &out, const SourceSnapshot &source) {
  out += "{\"file_id\":";
  number(out, source.file.value);
  out += ",\"revision_id\":";
  number(out, source.revision.value);
  out += ",\"id\":";
  number(out, source.id.value);
  out += ",\"path\":";
  json_string(out, source.path);
  out += ",\"valid\":";
  boolean(out, source.valid());
  out += ",\"error_byte\":";
  number(out, source.error() ? source.error()->byte : 0);
  out += ",\"error_length\":";
  number(out, source.error() ? source.error()->length : 0);
  out += ",\"error_message\":";
  if (source.error())
    json_string(out, source.error()->what());
  else
    out += "null";
  out += ",\"original_hex\":";
  hex(out, source.original);
  if (!source.valid()) {
    out += ",\"normalized_hex\":\"\",\"offsets\":[],\"boundaries\":[]}";
    return;
  }
  const auto map = derive(source);
  out += ",\"normalized_hex\":";
  hex(out, source.normalized());
  out += ",\"offsets\":[";
  for (size_t i = 0; i < map.offsets.size(); ++i) {
    if (i)
      out += ',';
    number(out, map.offsets[i]);
  }
  out += "],\"boundaries\":[";
  for (size_t i = 0; i < map.boundaries.size(); ++i) {
    if (i)
      out += ',';
    const auto &boundary = map.boundaries[i];
    // Boundary.byte is the original (raw) byte offset, matching the existing
    // SourceSnapshot positions_ map and source.position(original_byte).
    out += "{\"byte\":";
    number(out, boundary.original);
    out += ",\"original_byte\":";
    number(out, boundary.original);
    out += ",\"line\":";
    number(out, boundary.position.line);
    out += ",\"character\":";
    number(out, boundary.position.character);
    out += ",\"byte_at\":";
    number(out, source.byte(boundary.position));
    out += '}';
  }
  out += "]}";
}

void write_lex(std::string &out, const PhaseResult<TokenBuffer> &lexed) {
  out += "{\"state\":";
  json_string(out, phase_name(lexed.state));
  out += ",\"state_id\":";
  number(out, static_cast<uint64_t>(phase_id(lexed.state)));
  out += ",\"failure\":";
  json_string(out, failure_name(lexed.failure));
  out += ",\"failure_id\":";
  number(out, static_cast<uint64_t>(failure_id(lexed.failure)));
  out += ",\"has_value\":";
  boolean(out, lexed.value.has_value());
  out += ",\"diagnostics\":";
  diagnostics(out, lexed.diagnostics);
  out += ",\"tokens\":[";
  if (lexed.value) {
    const auto &tokens = lexed.value->tokens;
    for (size_t i = 0; i < tokens.size(); ++i) {
      if (i)
        out += ',';
      const auto &token = tokens[i];
      out += "{\"kind\":";
      number(out, static_cast<uint64_t>(token.kind));
      out += ",\"kind_name\":";
      json_string(out, token_name(token.kind));
      out += ",\"span\":";
      span(out, token.span);
      out += ",\"spelling\":";
      json_string(out, token.spelling);
      out += ",\"leading_trivia\":[";
      for (size_t t = 0; t < token.leading_trivia.size(); ++t) {
        if (t)
          out += ',';
        const auto &trivia = token.leading_trivia[t];
        out += "{\"kind\":";
        number(out, static_cast<uint64_t>(trivia.kind));
        out += ",\"kind_name\":";
        json_string(out, trivia_name(trivia.kind));
        out += ",\"span\":";
        span(out, trivia.span);
        out += ",\"spelling\":";
        json_string(out, trivia.spelling);
        out += '}';
      }
      out += "],\"has_string_value\":";
      boolean(out, token.string_value.has_value());
      out += ",\"string_value\":";
      if (token.string_value)
        json_string(out, *token.string_value);
      else
        out += "null";
      out += '}';
    }
  }
  out += "]}";
}

void write_parse_not_run(std::string &out) {
  out += "{\"state\":\"not_run\",\"state_id\":null,\"failure\":null,"
         "\"failure_id\":null,\"has_value\":false,\"root\":null,"
         "\"recovered\":false,\"diagnostics\":[],\"nodes\":[]}";
}

void write_parse(std::string &out, const PhaseResult<AstFile> &parsed,
                 const std::vector<Diagnostic> &lex_diagnostics) {
  out += "{\"state\":";
  json_string(out, phase_name(parsed.state));
  out += ",\"state_id\":";
  number(out, static_cast<uint64_t>(phase_id(parsed.state)));
  out += ",\"failure\":";
  json_string(out, failure_name(parsed.failure));
  out += ",\"failure_id\":";
  number(out, static_cast<uint64_t>(failure_id(parsed.failure)));
  out += ",\"has_value\":";
  boolean(out, parsed.value.has_value());
  out += ",\"root\":";
  if (parsed.value)
    number(out, parsed.value->root.value);
  else
    out += "null";
  out += ",\"recovered\":";
  boolean(out, parsed.value && parsed.value->recovered);
  out += ",\"diagnostics\":";
  // The frozen selfhost contract makes parser.parse(LexResult) preserve the
  // input lexical diagnostics exactly once.  The C++ oracle calls
  // parse(TokenBuffer), which only produces parser diagnostics, so project the
  // two phases together here.  Do not use this projection to alter the raw
  // parser state or AstFile.recovered: a lexical diagnostic alone does not
  // mean the parser recovered.
  std::vector<Diagnostic> projected = lex_diagnostics;
  projected.insert(projected.end(), parsed.diagnostics.begin(),
                   parsed.diagnostics.end());
  diagnostics(out, projected);
  out += ",\"nodes\":[";
  if (parsed.value) {
    const auto &nodes = parsed.value->nodes;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i)
        out += ',';
      const auto &node = nodes[i];
      out += "{\"kind\":";
      number(out, static_cast<uint64_t>(ast_id(node.kind)));
      out += ",\"kind_name\":";
      json_string(out, ast_name(node.kind));
      out += ",\"span\":";
      span(out, node.span);
      out += ",\"token_begin\":";
      number(out, node.token_begin);
      out += ",\"token_end\":";
      number(out, node.token_end);
      out += ",\"children\":[";
      for (size_t c = 0; c < node.children.size(); ++c) {
        if (c)
          out += ',';
        number(out, node.children[c].value);
      }
      out += "],\"recovered\":";
      boolean(out, node.recovered);
      out += '}';
    }
  }
  out += "]}";
}

std::string read_file(const char *path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot open source file");
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: stage0_oracle SOURCE\n";
    return 2;
  }
  try {
    auto source = std::make_shared<SourceSnapshot>(
        FileId(kFileId), RevisionId(kRevisionId), SourceSnapshotId(kSourceId),
        argv[1], read_file(argv[1]));
    auto lexed = lex(source);

    std::string out;
    out.reserve(65536);
    out += "{\"protocol\":\"woozy-selfhost-front/1\",";
    out += "\"operation_id\":\"woozy-SH04-front-diff-one-correction-20261113\",";
    out += "\"source\":";
    write_source(out, *source);
    out += ",\"lex\":";
    write_lex(out, lexed);
    out += ",\"parse\":";
    if (lexed.value) {
      auto parsed = parse(*lexed.value);
      write_parse(out, parsed, lexed.diagnostics);
    } else {
      write_parse_not_run(out);
    }
    out += "}\n";
    std::cout << out;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "stage0_oracle: " << error.what() << '\n';
    return 3;
  }
}

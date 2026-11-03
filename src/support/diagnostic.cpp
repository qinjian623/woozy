#include "lang/support/diagnostic.hpp"
#include <algorithm>
namespace lang {
namespace {
std::string q(const std::string &s) {
  validate_utf8(s);
  std::string o = "\"";
  const char *hex = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (c < 32) {
      o += "\\u00";
      o += hex[c >> 4];
      o += hex[c & 15];
    } else
      o += c;
  }
  return o + '"';
}
std::string span(const SourceManager &m, SourceSpan s) {
  auto p = m.get(s.snapshot);
  if (s.begin_byte > s.end_byte)
    throw std::invalid_argument("reversed span");
  p->raw_span(s.begin_byte, s.end_byte);
  return "{\"path\":" + q(p->path) +
         ",\"start_byte\":" + std::to_string(s.begin_byte) +
         ",\"end_byte\":" + std::to_string(s.end_byte) + "}";
}
bool one(const std::string &s, std::initializer_list<const char *> values) {
  return std::any_of(values.begin(), values.end(),
                     [&](auto v) { return s == v; });
}
} // namespace
std::string diagnostic_json(const SourceManager &m, std::string version,
                            std::string revision, Status status,
                            const std::vector<Diagnostic> &ds) {
  if (status == Status::ok &&
      std::any_of(ds.begin(), ds.end(),
                  [](const Diagnostic &d) { return d.severity == "error"; }))
    throw std::invalid_argument("successful diagnostic response contains errors");
  const char *name = nullptr;
  switch (status) {
  case Status::ok:
    name = "ok";
    break;
  case Status::error:
    name = "error";
    break;
  case Status::cancelled:
    name = "cancelled";
    break;
  case Status::internal_error:
    name = "internal_error";
    break;
  }
  if (!name)
    throw std::invalid_argument("status");
  std::string o = "{\"schema_version\":1,\"compiler_version\":" + q(version) +
                  ",\"source_revision\":" + q(revision) +
                  ",\"status\":" + q(name) + ",\"diagnostics\":[";
  bool first = true;
  for (const auto &d : ds) {
    if (d.code.size() < 3 || (d.code[0] != 'E' && d.code[0] != 'W') ||
        d.code[1] != '_' ||
        d.code.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", 2) !=
            std::string::npos ||
        !one(d.phase,
             {"lex", "parse", "resolve", "type", "effect", "ownership",
              "lowering", "codegen", "link", "runtime", "toolchain"}) ||
        !one(d.severity, {"error", "warning", "note"}))
      throw std::invalid_argument("diagnostic fields");
    if (!first) {
      o += ',';
    }
    first = false;
    o += "{\"code\":" + q(d.code) + ",\"phase\":" + q(d.phase) +
         ",\"severity\":" + q(d.severity) + ",\"message\":" + q(d.message) +
         ",\"primary\":" + (d.primary ? span(m, *d.primary) : "null") +
         ",\"related\":[";
    bool f = true;
    for (auto &r : d.related) {
      if (!f)
        o += ',';
      f = false;
      o +=
          "{\"span\":" + span(m, r.span) + ",\"message\":" + q(r.message) + "}";
    }
    o += "],\"fixits\":[";
    f = true;
    for (auto &r : d.fixits) {
      if (m.get(r.span.snapshot)->revision != r.revision)
        throw std::invalid_argument("stale fixit");
      if (!f)
        o += ',';
      f = false;
      o += "{\"span\":" + span(m, r.span) +
           ",\"replacement\":" + q(r.replacement) +
           ",\"source_revision\":" + q(std::to_string(r.revision.value)) + "}";
    }
    o += "],\"notes\":[";
    f = true;
    for (auto &n : d.notes) {
      if (!f)
        o += ',';
      f = false;
      o += q(n);
    }
    o += ']';
    if (d.expected_type)
      o += ",\"expected_type\":" + q(*d.expected_type);
    if (d.actual_type)
      o += ",\"actual_type\":" + q(*d.actual_type);
    o += '}';
  }
  return o + "]}";
}
} // namespace lang

#include "lang/driver/lsp.hpp"
#include "lang/syntax/format.hpp"
#include "lang/syntax/parser.hpp"
#include "lang/version.hpp"

#include <cctype>
#include <charconv>
#include <istream>
#include <json-c/json.h>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace lang {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

Json owned(json_object *object) { return {object, json_object_put}; }

bool is_type(json_object *value, json_type type) {
  return value && json_object_is_type(value, type);
}

json_object *field(json_object *object, const char *name) {
  return is_type(object, json_type_object)
             ? json_object_object_get(object, name)
             : nullptr;
}

std::optional<std::string_view> string_value(json_object *value) {
  if (!is_type(value, json_type_string))
    return std::nullopt;
  return std::string_view(json_object_get_string(value),
                          json_object_get_string_len(value));
}

json_object *json_string(std::string_view text) {
  return json_object_new_string_len(text.data(), static_cast<int>(text.size()));
}

std::string_view trim(std::string_view text) {
  auto begin = text.find_first_not_of(" \t");
  if (begin == text.npos)
    return {};
  return text.substr(begin, text.find_last_not_of(" \t") - begin + 1);
}

constexpr size_t max_header_bytes = 8192;
constexpr size_t max_body_bytes = 8 * 1024 * 1024;

bool read_message(std::istream &input, std::string &body) {
  size_t header_bytes = 0;
  std::optional<size_t> length;
  for (;;) {
    std::string line;
    for (;;) {
      auto c = input.get();
      if (c == std::istream::traits_type::eof() ||
          ++header_bytes > max_header_bytes)
        return false;
      if (c == '\n')
        break;
      line.push_back(static_cast<char>(c));
    }
    if (line.ends_with('\r'))
      line.pop_back();
    if (line.empty())
      break;
    auto colon = line.find(':');
    if (colon == std::string::npos || colon == 0)
      return false;
    auto name = line.substr(0, colon);
    for (char &c : name)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name != "content-length")
      continue;
    auto value = trim(std::string_view(line).substr(colon + 1));
    if (length || value.empty())
      return false;
    size_t size = 0;
    auto result =
        std::from_chars(value.data(), value.data() + value.size(), size);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        size > max_body_bytes)
      return false;
    length = size;
  }
  if (!length)
    return false;
  body.resize(*length);
  return bool(input.read(body.data(), static_cast<std::streamsize>(*length)));
}

Json parse_message(const std::string &body, bool &valid) {
  std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser(
      json_tokener_new(), json_tokener_free);
  json_tokener_set_flags(parser.get(),
                         JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  auto message = owned(json_tokener_parse_ex(
      parser.get(), body.c_str(), static_cast<int>(body.size() + 1)));
  valid = json_tokener_get_error(parser.get()) == json_tokener_success;
  auto end = json_tokener_get_parse_end(parser.get());
  valid = valid && end <= body.size() &&
          body.find_first_not_of(" \t\r\n", end) == std::string::npos;
  return message;
}

Json envelope() {
  auto message = owned(json_object_new_object());
  json_object_object_add(message.get(), "jsonrpc", json_string("2.0"));
  return message;
}

void send(std::ostream &output, const Json &message) {
  size_t size = 0;
  auto text = json_object_to_json_string_length(message.get(),
                                                JSON_C_TO_STRING_PLAIN, &size);
  output << "Content-Length: " << size << "\r\n\r\n";
  output.write(text, static_cast<std::streamsize>(size));
  output.flush();
}

void respond(std::ostream &output, json_object *id, const char *key,
             json_object *value) {
  auto message = envelope();
  json_object_object_add(message.get(), "id", json_object_get(id));
  json_object_object_add(message.get(), key, value);
  send(output, message);
}

void respond_error(std::ostream &output, json_object *id, int code,
                   std::string_view message) {
  auto *error = json_object_new_object();
  json_object_object_add(error, "code", json_object_new_int(code));
  json_object_object_add(error, "message", json_string(message));
  respond(output, id, "error", error);
}

json_object *initialize_result() {
  auto *sync = json_object_new_object();
  json_object_object_add(sync, "openClose", json_object_new_boolean(true));
  json_object_object_add(sync, "change", json_object_new_int(1));
  auto *capabilities = json_object_new_object();
  json_object_object_add(capabilities, "textDocumentSync", sync);
  json_object_object_add(capabilities, "positionEncoding",
                         json_string("utf-16"));
  json_object_object_add(capabilities, "documentFormattingProvider",
                         json_object_new_boolean(true));
  auto *info = json_object_new_object();
  json_object_object_add(info, "name", json_string("woozyc"));
  json_object_object_add(info, "version", json_string(version()));
  auto *result = json_object_new_object();
  json_object_object_add(result, "capabilities", capabilities);
  json_object_object_add(result, "serverInfo", info);
  return result;
}

struct Document {
  int64_t version;
  std::string text;
};

using Documents = std::map<std::string, Document, std::less<>>;

std::shared_ptr<const SourceSnapshot> snapshot(std::string_view uri,
                                               const Document &document) {
  // SourceManager owns all its revisions, so use it only for one analysis.
  SourceManager sources;
  auto file = sources.add_file(std::string(uri));
  return sources.update(file, document.text);
}

Position buffer_position(const SourceSnapshot &source, uint64_t byte) {
  auto position = source.position(byte);
  if (position.line == 0 && source.original.starts_with("\xef\xbb\xbf"))
    ++position.character;
  return position;
}

json_object *json_position(Position position) {
  auto *object = json_object_new_object();
  json_object_object_add(object, "line", json_object_new_uint64(position.line));
  json_object_object_add(object, "character",
                         json_object_new_uint64(position.character));
  return object;
}

json_object *json_range(Position start, Position end) {
  auto *object = json_object_new_object();
  json_object_object_add(object, "start", json_position(start));
  json_object_object_add(object, "end", json_position(end));
  return object;
}

void publish(std::ostream &output, std::string_view uri,
             const Document *document) {
  auto message = envelope();
  json_object_object_add(message.get(), "method",
                         json_string("textDocument/publishDiagnostics"));
  auto *params = json_object_new_object();
  json_object_object_add(message.get(), "params", params);
  auto *diagnostics = json_object_new_array();
  json_object_object_add(params, "uri", json_string(uri));
  json_object_object_add(params, "diagnostics", diagnostics);
  if (document) {
    json_object_object_add(params, "version",
                           json_object_new_int64(document->version));
    auto source = snapshot(uri, *document);
    auto lexed = lex(source);
    auto errors = lexed.success() ? parse(*lexed.value).diagnostics
                                  : std::move(lexed.diagnostics);
    for (const auto &diagnostic : errors) {
      Position start{0, 0}, end{0, 0};
      if (source->valid() && diagnostic.primary) {
        start = buffer_position(*source, diagnostic.primary->begin_byte);
        end = buffer_position(*source, diagnostic.primary->end_byte);
      } else if (!source->valid()) {
        SourceSnapshot prefix(
            source->file, source->revision, source->id, source->path,
            source->original.substr(0, source->error()->byte));
        start = end = buffer_position(prefix, prefix.original.size());
      }
      auto *entry = json_object_new_object();
      json_object_array_add(diagnostics, entry);
      json_object_object_add(entry, "range", json_range(start, end));
      json_object_object_add(
          entry, "severity",
          json_object_new_int(diagnostic.severity == "warning" ? 2 : 1));
      json_object_object_add(entry, "code", json_string(diagnostic.code));
      json_object_object_add(entry, "source", json_string("woozyc"));
      json_object_object_add(entry, "message", json_string(diagnostic.message));
    }
  }
  send(output, message);
}

std::optional<std::string_view> changed_text(json_object *params) {
  auto *changes = field(params, "contentChanges");
  if (!is_type(changes, json_type_array))
    return std::nullopt;
  std::optional<std::string_view> text;
  for (size_t i = 0; i < json_object_array_length(changes); ++i) {
    auto *change = json_object_array_get_idx(changes, i);
    if (!is_type(change, json_type_object) ||
        json_object_object_get_ex(change, "range", nullptr))
      return std::nullopt;
    text = string_value(field(change, "text"));
    if (!text)
      return std::nullopt;
  }
  return text;
}

void update_document(std::ostream &output, Documents &documents,
                     std::string_view method, json_object *params) {
  auto *identifier = field(params, "textDocument");
  auto uri = string_value(field(identifier, "uri"));
  if (!uri || uri->empty() || uri->find('\0') != uri->npos)
    return;
  auto existing = documents.find(*uri);
  if (method == "textDocument/didClose") {
    if (existing != documents.end()) {
      documents.erase(existing);
      publish(output, *uri, nullptr);
    }
    return;
  }
  auto *number = field(identifier, "version");
  if (!is_type(number, json_type_int))
    return;
  auto version = json_object_get_int64(number);
  auto opening = method == "textDocument/didOpen";
  if (opening != (existing == documents.end()) ||
      (!opening && version <= existing->second.version))
    return;
  auto text =
      opening ? string_value(field(identifier, "text")) : changed_text(params);
  if (!text)
    return;
  auto &document = documents
                       .insert_or_assign(std::string(*uri),
                                         Document{version, std::string(*text)})
                       .first->second;
  publish(output, *uri, &document);
}

void format_document(std::ostream &output, json_object *id,
                     const Documents &documents, json_object *params) {
  auto uri = string_value(field(field(params, "textDocument"), "uri"));
  if (!uri || uri->empty() || uri->find('\0') != uri->npos) {
    respond_error(output, id, -32602, "Expected a text document URI");
    return;
  }
  auto edits = owned(json_object_new_array());
  if (auto it = documents.find(*uri); it != documents.end()) {
    auto source = snapshot(*uri, it->second);
    auto formatted = format_source(source);
    if (formatted.success() && *formatted.value != source->original) {
      if (formatted.value->size() > max_body_bytes) {
        respond_error(output, id, -32603, "Formatted document is too large");
        return;
      }
      auto *edit = json_object_new_object();
      json_object_array_add(edits.get(), edit);
      json_object_object_add(
          edit, "range",
          json_range({0, 0},
                     buffer_position(*source, source->original.size())));
      json_object_object_add(edit, "newText", json_string(*formatted.value));
    }
  }
  respond(output, id, "result", edits.release());
}
} // namespace

int run_lsp(std::istream &input, std::ostream &output) {
  enum class State { initial, running, shutdown };
  auto state = State::initial;
  Documents documents;
  std::string body;
  while (output && read_message(input, body)) {
    bool valid = false;
    auto message = parse_message(body, valid);
    if (!valid) {
      respond_error(output, nullptr, -32700, "Parse error");
      continue;
    }
    auto *object = message.get();
    auto method = string_value(field(object, "method"));
    auto protocol = string_value(field(object, "jsonrpc"));
    json_object *id = nullptr;
    auto request = is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, "id", &id);
    if (protocol != "2.0" || !method || method->empty() ||
        (request && !is_type(id, json_type_int) &&
         !is_type(id, json_type_string))) {
      respond_error(output, nullptr, -32600, "Invalid request");
      continue;
    }
    auto *params = field(object, "params");
    if (*method == "exit" && !request)
      return state == State::shutdown ? 0 : 1;
    if (state == State::initial) {
      if (request && *method == "initialize") {
        state = State::running;
        respond(output, id, "result", initialize_result());
      } else if (request) {
        respond_error(output, id, -32002, "Server not initialized");
      }
      continue;
    }
    if (state == State::shutdown) {
      if (request)
        respond_error(output, id, -32600, "Server has shut down");
      continue;
    }
    if (request) {
      if (*method == "shutdown") {
        state = State::shutdown;
        documents.clear();
        respond(output, id, "result", nullptr);
      } else if (*method == "initialize") {
        respond_error(output, id, -32600, "Server already initialized");
      } else if (*method == "textDocument/formatting") {
        format_document(output, id, documents, params);
      } else {
        respond_error(output, id, -32601, "Method not found");
      }
    } else if (*method == "textDocument/didOpen" ||
               *method == "textDocument/didChange" ||
               *method == "textDocument/didClose") {
      update_document(output, documents, *method, params);
    }
  }
  return 1;
}
} // namespace lang

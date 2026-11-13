// Test-only json-c 0.19 oracle. It mirrors the strict tokenizer flags and the
// actual-root-nonnull condition used by src/modules/project.cpp. It is built
// only by run.py; the Woozy compiler and module never link or execute it.
#include <json-c/json.h>

#include <iostream>
#include <iterator>
#include <string>

int main() {
  std::string text((std::istreambuf_iterator<char>(std::cin)), {});
  json_tokener *tok = json_tokener_new();
  json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  json_object *value =
      json_tokener_parse_ex(tok, text.data(), static_cast<int>(text.size()));
  enum json_tokener_error error = json_tokener_get_error(tok);
  int end = json_tokener_get_parse_end(tok);
  const char *classification = "ok";
  if (error != json_tokener_success || !value) {
    classification = "invalid JSON binding bundle";
  } else {
    for (size_t i = end; i < text.size(); ++i) {
      if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' &&
          text[i] != '\n') {
        classification = "trailing JSON content";
        break;
      }
    }
  }
  std::cout << "{\"parse_error\":\"" << json_tokener_error_desc(error)
            << "\",\"parse_end\":" << end << ",\"nonnull\":"
            << (value ? "true" : "false") << ",\"classification\":\""
            << classification << "\"}\n";
  if (value) json_object_put(value);
  json_tokener_free(tok);
  return 0;
}

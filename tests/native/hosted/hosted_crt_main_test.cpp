// Generated language executables define an ordinary no-argument main.  This
// fixture has that exact shape and does not reference any startup helper in
// its own code, so it only works when the hosted link force-extracts
// lang_hosted_startup_v1 from the archive.
#include "../../../runtime/native/process.hpp"

#include <cstdint>
#include <cstring>

namespace {
LangVec *bytes(const char *text) {
  const uint64_t length = std::strlen(text);
  LangVec *value = lang_vec_new_v1(length, 1, nullptr, nullptr);
  if (!value) return nullptr;
  std::memcpy(value->data, text, length);
  value->initialized = length;
  return value;
}
} // namespace

int main() {
  // argc is only available through the constructor-published process state.
  LangVec *arguments = nullptr;
  if (lang_process_args_bytes_v1(&arguments) != 0 || !arguments ||
      arguments->length == 0)
    return 1;
  lang_vec_release_v1(arguments);

  LangVec *key = bytes("HOSTED_NOARG_ENV");
  LangVec *value = nullptr;
  const int64_t status = lang_process_getenv_bytes_v1(&value, key);
  lang_vec_release_v1(key);
  if (status != 1 || !value) return 2;
  const bool matches = value->length == 18 &&
                       std::memcmp(value->data, "hosted-noarg-value", 18) == 0;
  lang_vec_release_v1(value);
  return matches ? 0 : 3;
}

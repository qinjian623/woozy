// Standalone C++ harness for the hosted startup helpers.  It is linked with a
// normal CRT main and the hosted runtime archive; it is not a source-language
// program fixture.
#include "../../../runtime/hosted/start.hpp"
#include "../../../runtime/native/process.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" void *__libc_stack_end;

namespace {
void need(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "hosted_start_test: %s\n", message);
    std::exit(1);
  }
}

std::vector<std::string> args() {
  LangVec *value = nullptr;
  const int64_t status = lang_process_args_bytes_v1(&value);
  need(status == 0 && value, "lang_process_args_bytes_v1 failed");
  std::vector<std::string> result;
  auto **rows = static_cast<LangVec **>(value->data);
  for (uint64_t i = 0; i < value->length; ++i) {
    const LangVec *row = rows[i];
    result.emplace_back(reinterpret_cast<const char *>(row->data), row->length);
  }
  lang_vec_release_v1(value);
  return result;
}

std::vector<std::string> environment() {
  LangVec *value = nullptr;
  const int64_t status = lang_process_environment_bytes_v1(&value);
  need(status == 0 && value, "lang_process_environment_bytes_v1 failed");
  std::vector<std::string> result;
  auto **rows = static_cast<LangVec **>(value->data);
  for (uint64_t i = 0; i < value->length; ++i) {
    const LangVec *row = rows[i];
    result.emplace_back(reinterpret_cast<const char *>(row->data), row->length);
  }
  lang_vec_release_v1(value);
  return result;
}

bool contains(const std::vector<std::string> &rows, const std::string &item) {
  for (const auto &row : rows)
    if (row == item) return true;
  return false;
}

LangVec *bytes(const char *text) {
  const uint64_t length = std::strlen(text);
  LangVec *value = lang_vec_new_v1(length, 1, nullptr, nullptr);
  need(value != nullptr, "lang_vec_new_v1 failed");
  std::memcpy(value->data, text, length);
  value->initialized = length;
  return value;
}

std::string getenv_bytes(const char *name) {
  LangVec *key = bytes(name);
  LangVec *value = nullptr;
  const int64_t status = lang_process_getenv_bytes_v1(&value, key);
  lang_vec_release_v1(key);
  need(status == 1 && value, "language getenv did not find startup variable");
  std::string result(reinterpret_cast<const char *>(value->data), value->length);
  lang_vec_release_v1(value);
  return result;
}

uint64_t fs_base() {
  uint64_t value = 0;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(value));
  return value;
}

uint64_t canary() {
  uint64_t value = 0;
  __asm__ volatile("movq %%fs:0x28, %0" : "=r"(value));
  return value;
}

void verify_process_state(int argc, char **argv, char **envp) {
  const auto language_args = args();
  need(language_args.size() == static_cast<size_t>(argc),
       "language argc differs from CRT argc");
  for (int i = 0; i < argc; ++i)
    need(language_args[static_cast<size_t>(i)] == argv[i],
         "language argv differs from CRT argv");

  const char *libc_value = std::getenv("HOSTED_START_ENV");
  need(libc_value != nullptr, "CRT getenv did not see HOSTED_START_ENV");
  need(std::string(libc_value) == "hosted-start-value",
       "CRT getenv returned the wrong value");
  need(getenv_bytes("HOSTED_START_ENV") == "hosted-start-value",
       "language getenv returned the wrong value");

  const auto rows = environment();
  need(contains(rows, "HOSTED_START_ENV=hosted-start-value"),
       "language environment snapshot is missing HOSTED_START_ENV");

  bool found_envp = false;
  for (char **row = envp; row && *row; ++row)
    if (std::strcmp(*row, "HOSTED_START_ENV=hosted-start-value") == 0)
      found_envp = true;
  need(found_envp, "CRT envp is missing HOSTED_START_ENV");
}
} // namespace

int main(int argc, char **argv, char **envp) {
  need(__libc_stack_end != nullptr, "glibc __libc_stack_end is unavailable");
  const auto *initial_stack =
      static_cast<const char *const *>(__libc_stack_end);
  need(*reinterpret_cast<const uint64_t *>(initial_stack) ==
           static_cast<uint64_t>(argc),
       "initial stack argc differs from CRT argc");

  verify_process_state(argc, argv, envp);

  const uint64_t fs_before = fs_base();
  const uint64_t canary_before = canary();
  need(fs_before != 0 && canary_before != 0, "host TLS is not initialized");

  // Calling the compatibility helper with the real initial stack must only
  // republish argv/envp; it must not install FS or change the canary.
  lang_init_tcb(reinterpret_cast<const uintptr_t *>(__libc_stack_end));
  need(fs_base() == fs_before, "lang_init_tcb changed the host FS base");
  need(canary() == canary_before, "lang_init_tcb changed the host canary");
  verify_process_state(argc, argv, envp);

  // The explicit helper has the same process-vector behavior as the
  // constructor and does not depend on private TCB state.
  lang_hosted_init_v1(argc, argv, envp);
  need(fs_base() == fs_before, "lang_hosted_init_v1 changed the host FS base");
  need(canary() == canary_before, "lang_hosted_init_v1 changed the host canary");
  verify_process_state(argc, argv, envp);

  char buffer[512];
  const int length = std::snprintf(buffer, sizeof(buffer), "%d/%s", argc, argv[0]);
  need(length > 0 && static_cast<size_t>(length) < sizeof(buffer),
       "hosted main snprintf failed");
  void *allocation = std::malloc(257);
  need(allocation != nullptr, "hosted main malloc failed");
  std::free(allocation);

  std::printf("passed: hosted CRT startup published argc/argv/envp without TCB or FS changes\n");
  return 0;
}

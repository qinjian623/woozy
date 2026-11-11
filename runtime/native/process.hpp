#pragma once
#include "text.hpp"
#include <cstdint>

// Private Linux x86_64 ABI. Status functions return 0 or negative errno;
// fork/spawn/waitpid return a nonnegative pid or negative errno.
struct LangSignalInfo {
  int32_t number, code, pid;
  uint32_t uid;
  int32_t status;
  uint32_t reserved;
  int64_t value;
};
static_assert(sizeof(LangSignalInfo) == 32);
extern "C" {
// Startup saves borrowed kernel argument/environment storage, without allocation.
void lang_process_init_v1(uint64_t argc, const char *const *argv,
                           const char *const *environment) noexcept;
int64_t lang_process_args_v1(LangVec **out) noexcept; // Vec[string], EILSEQ on invalid UTF-8
int64_t lang_process_args_bytes_v1(LangVec **out) noexcept; // Vec[Vec[u8]], deep copy
int64_t lang_process_environment_v1(LangVec **out) noexcept;
int64_t lang_process_environment_bytes_v1(LangVec **out) noexcept;
// getenv returns 1 when present, 0 when absent; empty output on absence/error.
int64_t lang_process_getenv_v1(LangText *out, const LangText *name) noexcept;
int64_t lang_process_getenv_bytes_v1(LangVec **out, const LangVec *name) noexcept;
int64_t lang_process_setenv_v1(const LangText *name, const LangText *value,
                               int32_t overwrite) noexcept;
int64_t lang_process_setenv_bytes_v1(const LangVec *name, const LangVec *value,
                                     int32_t overwrite) noexcept;
int64_t lang_process_unsetenv_v1(const LangText *name) noexcept;
int64_t lang_process_unsetenv_bytes_v1(const LangVec *name) noexcept;
int64_t lang_process_fork_v1() noexcept;
int64_t lang_process_waitpid_v1(int32_t pid, int32_t options,
                                int32_t *status) noexcept;
int64_t lang_process_execve_v1(const LangText *path, const LangVec *arguments,
                               const LangVec *environment) noexcept;
int64_t lang_process_execve_bytes_v1(const LangVec *path, const LangVec *arguments,
                                     const LangVec *environment) noexcept;
// -1 inherits a standard descriptor. All redirects are duplicated in the
// parent, so swaps/collisions are safe. Failed exec is reported and reaped.
int64_t lang_process_spawn_v1(const LangText *path, const LangVec *arguments,
                              const LangVec *environment, int32_t input,
                              int32_t output, int32_t error) noexcept;
int64_t lang_process_spawn_bytes_v1(const LangVec *path, const LangVec *arguments,
                                    const LangVec *environment, int32_t input,
                                    int32_t output, int32_t error) noexcept;
int64_t lang_signal_mask_v1(uint64_t *old, int32_t how, uint64_t mask) noexcept;
int64_t lang_signal_pending_v1(uint64_t *out) noexcept;
// Relative timeout in ns; -1 waits indefinitely. Signals must be blocked by caller.
int64_t lang_signal_wait_v1(LangSignalInfo *out, uint64_t mask,
                            int64_t timeout_ns) noexcept;
// action 0 = default, 1 = ignore. No asynchronous language callback.
int64_t lang_signal_disposition_v1(int32_t number, int32_t action) noexcept;
}

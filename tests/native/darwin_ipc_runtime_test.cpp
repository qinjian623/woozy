#include "../../runtime/native/ipc.hpp"
#include "../../runtime/native/process.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
int main() {
  char name[32];
  auto count = snprintf(name, sizeof(name), "/woozy-io-%d", getpid());
  const auto length = static_cast<uint64_t>(count);
  LangText text{reinterpret_cast<const uint8_t *>(name), length, length, nullptr};
  const auto file = lang_ipc_shm_open_v1(&text, 2|64|128, 0600);
  assert(file >= 0);
  assert(lang_ipc_shm_unlink_v1(&text) == 0);
  assert(fcntl(file, F_GETFD) & FD_CLOEXEC);
  assert(ftruncate(file, 8192) == 0);
  LangHandle *view = nullptr;
  assert(lang_ipc_map_file_v1(&view, file, 4096, 4096, 3) == 0);
  assert(lang_ipc_mutex_init_v1(view, 4) == 0);
  assert(lang_ipc_sem_init_v1(view, 20, 0) == 0);
  assert(lang_ipc_cond_init_v1(view, 36) == 0);
  assert(lang_ipc_mutex_lock_v1(view, 4) == 0);
  auto child = lang_process_fork_v1();
  assert(child >= 0);
  if (!child) {
    LangHandle *other = nullptr;
    if (lang_ipc_map_file_v1(&other, file, 4096, 4096, 3) != 0) _exit(80);
    if (lang_ipc_sem_post_v1(other, 20) != 0 || lang_ipc_mutex_lock_v1(other, 4) != 0) _exit(81);
    int64_t value;
    if (lang_ipc_read_i64_v1(&value, other, 72) != 0 || value != 42) _exit(82);
    if (lang_ipc_write_i64_v1(other, 72, 43) != 0 || lang_ipc_cond_signal_v1(other, 36) != 0) _exit(83);
    if (lang_ipc_mutex_unlock_v1(other, 4) != 0) _exit(84);
    lang_handle_release_v1(other);
    _exit(0);
  }
  assert(lang_ipc_sem_wait_v1(view, 20, 1000000000) == 0);
  assert(lang_ipc_write_i64_v1(view, 72, 42) == 0);
  int64_t value;
  do {
    assert(lang_ipc_cond_wait_v1(view, 36, view, 4, 1000000000) == 0);
    assert(lang_ipc_read_i64_v1(&value, view, 72) == 0);
  } while (value == 42);
  assert(value == 43);
  assert(lang_ipc_mutex_unlock_v1(view, 4) == 0);
  int32_t status;
  assert(lang_process_waitpid_v1(child, 0, &status) == child && status == 0);
  assert(lang_ipc_mutex_destroy_v1(view, 4) == 0);
  assert(lang_ipc_sem_destroy_v1(view, 20) == 0);
  assert(lang_ipc_cond_destroy_v1(view, 36) == 0);
  close(file);
  lang_handle_release_v1(view);
  std::printf("Darwin IPC: 4096 offset on %d-byte host page, independently mapped process-shared sync passed\n", getpagesize());
}

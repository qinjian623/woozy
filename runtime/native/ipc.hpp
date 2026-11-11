#pragma once
#include "handle.hpp"
#include "text.hpp"
#include <cstdint>

// Private Linux x86_64 ABI. Mapping metadata is process-private ARC; mapped
// bytes and futex objects are MAP_SHARED. Only numeric/byte values may be stored.
inline constexpr uint64_t LANG_IPC_MUTEX_BYTES = 16, LANG_IPC_COND_BYTES = 16,
                           LANG_IPC_SEM_BYTES = 16, LANG_IPC_SYNC_ALIGNMENT = 4;
extern "C" {
// All status results are 0 or negative errno; out is initialized only on success.
int64_t lang_ipc_map_anon_v1(LangHandle **out, uint64_t bytes, int32_t protection) noexcept;
int64_t lang_ipc_map_file_v1(LangHandle **out, int32_t fd, uint64_t bytes,
                             uint64_t offset, int32_t protection) noexcept;
uint64_t lang_ipc_length_v1(LangHandle *) noexcept;
int64_t lang_ipc_shm_open_v1(const LangText *name, int32_t flags, uint32_t mode) noexcept;
int64_t lang_ipc_shm_unlink_v1(const LangText *name) noexcept;
int64_t lang_ipc_read_bytes_v1(LangVec **out, LangHandle *, uint64_t offset,
                               uint64_t count) noexcept;
int64_t lang_ipc_write_bytes_v1(LangHandle *, uint64_t offset, const LangVec *,
                                uint64_t start, uint64_t count) noexcept;
int64_t lang_ipc_read_i32_v1(int32_t *out, LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_write_i32_v1(LangHandle *, uint64_t offset, int32_t value) noexcept;
int64_t lang_ipc_read_i64_v1(int64_t *out, LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_write_i64_v1(LangHandle *, uint64_t offset, int64_t value) noexcept;
int64_t lang_ipc_atomic_load_v1(int64_t *out, LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_atomic_store_v1(LangHandle *, uint64_t offset, int64_t value) noexcept;
int64_t lang_ipc_atomic_add_v1(int64_t *old, LangHandle *, uint64_t offset, int64_t delta) noexcept;
int64_t lang_ipc_atomic_compare_v1(bool *changed, LangHandle *, uint64_t offset,
                                   int64_t expected, int64_t desired) noexcept;
// Fixed-size process-shared objects are explicitly initialized/destroyed in
// mapped storage; they contain no process-relative pointers. Misuse Panics.
int64_t lang_ipc_mutex_init_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_mutex_lock_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_mutex_unlock_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_mutex_destroy_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_cond_init_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_cond_wait_v1(LangHandle *condition, uint64_t condition_offset,
                              LangHandle *mutex, uint64_t mutex_offset,
                              int64_t timeout_ns) noexcept;
int64_t lang_ipc_cond_signal_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_cond_broadcast_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_cond_destroy_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_sem_init_v1(LangHandle *, uint64_t offset, uint32_t value) noexcept;
int64_t lang_ipc_sem_wait_v1(LangHandle *, uint64_t offset, int64_t timeout_ns) noexcept;
int64_t lang_ipc_sem_trywait_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_sem_post_v1(LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_sem_value_v1(uint32_t *out, LangHandle *, uint64_t offset) noexcept;
int64_t lang_ipc_sem_destroy_v1(LangHandle *, uint64_t offset) noexcept;
}

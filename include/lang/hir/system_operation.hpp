#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace lang::hir {
// Compiler-private typed operations restricted to their source std module.
// No operation accepts a syscall number or an untyped address.
enum class SystemOperation : uint16_t {
  read, write, write_text, open, close, seek, unlink, mkdir, rmdir,
  getpid, exit, vec_length, text_length,
  thread_spawn, thread_error, thread_wait, thread_result,
  mutex_new, mutex_lock, mutex_unlock,
  condvar_new, condvar_wait, condvar_notify_one, condvar_notify_all,
  atomic_i64_new, atomic_i64_load, atomic_i64_store, atomic_i64_fetch_add,
  atomic_i64_compare_exchange,
  text_len, text_byte_len, text_from_bytes, text_from_rune, text_bytes,
  vec_empty, vec_make, vec_len, vec_capacity, vec_reserve, vec_push,
  vec_pop, vec_resize, vec_clear,
  fs_stat, fs_lstat, fs_fstat, fs_rename, fs_chmod, fs_fchmod, fs_chdir,
  fs_getcwd, fs_getdents, fs_readlink, fs_symlink, fs_link, fs_truncate,
  fs_ftruncate, fs_fsync, fs_fdatasync, fs_dup, fs_dup2, fs_pipe, fs_mkfifo,
  fs_access, os_getuid, os_geteuid, os_getgid, os_getegid, os_getppid,
  os_gettid, os_setpgid, os_getpgid, os_setsid, os_kill, clock_gettime,
  nanosleep, getrandom,
  num_parse_i64, num_parse_u64, num_parse_f32, num_parse_f64,
  num_format_i64, num_format_u64, num_format_f32, num_format_f64,
  net_socket, net_bind, net_connect, net_listen, net_accept, net_send, net_recv,
  net_sendto, net_recvfrom, net_shutdown, net_getsockname, net_getpeername,
  net_setsockopt_int, net_getsockopt_int, net_setsockopt_bytes, net_getsockopt_bytes,
  net_socketpair, net_poll, net_inet_pton, net_inet_ntop, net_lookup,
  math_fabs, math_floor, math_ceil, math_trunc, math_round, math_sqrt, math_sin, math_cos, math_tan, math_asin, math_acos, math_atan, math_exp, math_exp2, math_expm1, math_log, math_log2, math_log10, math_log1p, math_copysign, math_hypot, math_atan2, math_pow, math_fmod, math_remainder, math_fma, math_ldexp, math_frexp, math_isnan, math_isinf, math_isfinite, math_signbit,
  process_args, process_args_bytes, process_environment, process_environment_bytes, process_getenv, process_getenv_bytes, process_setenv, process_unsetenv, process_execve, process_spawn, process_setenv_bytes, process_unsetenv_bytes, process_execve_bytes, process_spawn_bytes, process_fork, process_waitpid, signal_mask, signal_pending, signal_wait, signal_disposition, ipc_map_anon, ipc_map_file, ipc_shm_open, ipc_shm_unlink, ipc_read_bytes, ipc_write_bytes, ipc_read_i32, ipc_write_i32, ipc_read_i64, ipc_write_i64, ipc_atomic_load, ipc_atomic_store, ipc_atomic_add, ipc_atomic_compare, ipc_mutex_init, ipc_cond_init, ipc_sem_init, ipc_length, ipc_mutex_lock, ipc_mutex_unlock, ipc_mutex_destroy, ipc_cond_signal, ipc_cond_broadcast, ipc_cond_destroy, ipc_sem_post, ipc_sem_destroy, ipc_cond_wait, ipc_sem_wait, ipc_sem_trywait, ipc_sem_value,
  unicode_normalize, unicode_casefold, unicode_boundaries,
  thread_wait_for, condvar_wait_for,
  ffi_buffer, ffi_buffer_mut, ffi_buffer_len, ffi_buffer_len_mut,
  slice_view, slice_len, slice_sub, slice_to_vec,
  // Source-level Weak. These are builtins owned by std/weak; there is no new
  // Native IR opcode. upgrade returns the canonical std/option.Option[A].
  weak_empty, weak_downgrade, weak_upgrade,
  // std/async executor operations. Task[A,B] is the compiler-private owning
  // handle; sleep/readable/writable return an already-pending Task that an
  // async function awaits.
  async_spawn, async_cancel, async_finished, async_sleep,
  async_wait_readable, async_wait_writable, async_run, async_run_until,
  async_shutdown,
  // Raw byte-path stat. Appending after async_shutdown keeps every existing
  // operation number stable; the operation is owned by std/os.
  fs_stat_bytes,
  fs_flock,
  fs_executable_path,
  // Package-private code-point cursor slicing for std/unicode.graphemes.
  // 216-227 are reserved for independent pending operations; the explicit
  // value keeps this operation stable when those entries are appended.
  unicode_text_take = 228,
  vec_try_move_trivial = 229,
  text_load8 = 230,
  // Private std/heap active-iteration pin. The explicit IDs follow the
  // existing entries so they stay stable; the owner module restriction keeps
  // these out of public APIs.
  heap_pin_begin = 231,
  heap_pin_end = 232
};
struct SystemOperationSpec {
  SystemOperation operation;
  std::string_view name, result;
  std::array<std::string_view, 8> parameters;
  uint32_t count;
  // Logical source module allowed to call this operation. The default keeps
  // the original std/os entries unchanged.
  std::string_view owner = "std/os";
  uint32_t type_arity = 0;
};
inline constexpr std::array<SystemOperationSpec, 13> system_operations{{
  {SystemOperation::read, "__woozy_os_read", "i64", {"i32", "Vec[u8]", "usize", ""}, 3},
  {SystemOperation::write, "__woozy_os_write", "i64", {"i32", "Vec[u8]", "usize", "usize"}, 4},
  {SystemOperation::write_text, "__woozy_os_write_text", "i64", {"i32", "string", "usize", "usize"}, 4},
  {SystemOperation::open, "__woozy_os_open", "i64", {"string", "i32", "u32", ""}, 3},
  {SystemOperation::close, "__woozy_os_close", "i64", {"i32", "", "", ""}, 1},
  {SystemOperation::seek, "__woozy_os_seek", "i64", {"i32", "i64", "i32", ""}, 3},
  {SystemOperation::unlink, "__woozy_os_unlink", "i64", {"string", "", "", ""}, 1},
  {SystemOperation::mkdir, "__woozy_os_mkdir", "i64", {"string", "u32", "", ""}, 2},
  {SystemOperation::rmdir, "__woozy_os_rmdir", "i64", {"string", "", "", ""}, 1},
  {SystemOperation::getpid, "__woozy_os_getpid", "i64", {"", "", "", ""}, 0},
  {SystemOperation::exit, "__woozy_os_exit", "Unit", {"i32", "", "", ""}, 1},
  {SystemOperation::vec_length, "__woozy_os_vec_length", "usize", {"Vec[u8]", "", "", ""}, 1},
  {SystemOperation::text_length, "__woozy_os_text_length", "usize", {"string", "", "", ""}, 1},
}};
inline constexpr std::array<SystemOperationSpec, 9> async_system_operations{{
  {SystemOperation::async_spawn, "__woozy_async_spawn", "Unit",
   {"Task[A,B]", "", "", ""}, 1, "std/async", 2},
  {SystemOperation::async_cancel, "__woozy_async_cancel", "Unit",
   {"Task[A,B]", "", "", ""}, 1, "std/async", 2},
  {SystemOperation::async_finished, "__woozy_async_finished", "bool",
   {"Task[A,B]", "", "", ""}, 1, "std/async", 2},
  {SystemOperation::async_sleep, "__woozy_async_sleep", "Task[Unit,Unit]",
   {"i64", "", "", ""}, 1, "std/async"},
  {SystemOperation::async_wait_readable, "__woozy_async_wait_readable",
   "Task[Unit,Unit]", {"i32", "", "", ""}, 1, "std/async"},
  {SystemOperation::async_wait_writable, "__woozy_async_wait_writable",
   "Task[Unit,Unit]", {"i32", "", "", ""}, 1, "std/async"},
  {SystemOperation::async_run, "__woozy_async_run", "Unit",
   {"", "", "", ""}, 0, "std/async"},
  {SystemOperation::async_run_until, "__woozy_async_run_until", "Unit",
   {"Task[A,B]", "", "", ""}, 1, "std/async", 2},
  {SystemOperation::async_shutdown, "__woozy_async_shutdown", "Unit",
   {"", "", "", ""}, 0, "std/async"},
}};
// New owning runtime operations are kept in a separate table so the original
// std/os operation inventory (and its source tests) remains exact. Both tables
// are searched by system_operation_spec and registered in the prelude.
inline constexpr std::array<SystemOperationSpec, 18> concurrency_system_operations{{
  {SystemOperation::thread_spawn, "__woozy_thread_spawn", "__woozy_Thread",
   {"func(A) i64", "A", "", ""}, 2, "std/thread", 1},
  {SystemOperation::thread_error, "__woozy_thread_error", "i64",
   {"__woozy_Thread", "", "", ""}, 1, "std/thread"},
  {SystemOperation::thread_wait, "__woozy_thread_wait", "i64",
   {"__woozy_Thread", "", "", ""}, 1, "std/thread"},
  {SystemOperation::thread_wait_for, "__woozy_thread_wait_for", "i64",
   {"__woozy_Thread", "i64", "", ""}, 2, "std/thread"},
  {SystemOperation::thread_result, "__woozy_thread_result", "i64",
   {"__woozy_Thread", "", "", ""}, 1, "std/thread"},
  {SystemOperation::mutex_new, "__woozy_mutex_new", "__woozy_Mutex",
   {"", "", "", ""}, 0, "std/sync"},
  {SystemOperation::mutex_lock, "__woozy_mutex_lock", "Unit",
   {"__woozy_Mutex", "", "", ""}, 1, "std/sync"},
  {SystemOperation::mutex_unlock, "__woozy_mutex_unlock", "Unit",
   {"__woozy_Mutex", "", "", ""}, 1, "std/sync"},
  {SystemOperation::condvar_new, "__woozy_condvar_new", "__woozy_Condvar",
   {"", "", "", ""}, 0, "std/sync"},
  {SystemOperation::condvar_wait, "__woozy_condvar_wait", "Unit",
   {"__woozy_Condvar", "__woozy_Mutex", "", ""}, 2, "std/sync"},
  {SystemOperation::condvar_wait_for, "__woozy_condvar_wait_for", "bool",
   {"__woozy_Condvar", "__woozy_Mutex", "i64", ""}, 3, "std/sync"},
  {SystemOperation::condvar_notify_one, "__woozy_condvar_notify_one", "Unit",
   {"__woozy_Condvar", "", "", ""}, 1, "std/sync"},
  {SystemOperation::condvar_notify_all, "__woozy_condvar_notify_all", "Unit",
   {"__woozy_Condvar", "", "", ""}, 1, "std/sync"},
  {SystemOperation::atomic_i64_new, "__woozy_atomic_i64_new",
   "__woozy_AtomicI64", {"i64", "", "", ""}, 1, "std/sync"},
  {SystemOperation::atomic_i64_load, "__woozy_atomic_i64_load", "i64",
   {"__woozy_AtomicI64", "", "", ""}, 1, "std/sync"},
  {SystemOperation::atomic_i64_store, "__woozy_atomic_i64_store", "Unit",
   {"__woozy_AtomicI64", "i64", "", ""}, 2, "std/sync"},
  {SystemOperation::atomic_i64_fetch_add, "__woozy_atomic_i64_fetch_add", "i64",
   {"__woozy_AtomicI64", "i64", "", ""}, 2, "std/sync"},
  {SystemOperation::atomic_i64_compare_exchange,
   "__woozy_atomic_i64_compare_exchange", "bool",
   {"__woozy_AtomicI64", "i64", "i64", ""}, 3, "std/sync"},
}};
inline constexpr std::array<SystemOperationSpec, 62> value_system_operations{{
  {SystemOperation::unicode_normalize, "__woozy_unicode_normalize", "string", {"string", "i32"}, 2, "std/unicode"},
  {SystemOperation::unicode_casefold, "__woozy_unicode_casefold", "string", {"string"}, 1, "std/unicode"},
  {SystemOperation::unicode_boundaries, "__woozy_unicode_boundaries", "Vec[usize]", {"string"}, 1, "std/unicode"},
  {SystemOperation::unicode_text_take, "__woozy_unicode_text_take", "string", {"string", "Vec[u64]", "usize"}, 3, "std/unicode"},
  {SystemOperation::text_len, "__woozy_text_len", "usize", {"string"}, 1, "std/text"},
  {SystemOperation::text_byte_len, "__woozy_text_byte_len", "usize", {"string"}, 1, "std/text"},
  {SystemOperation::text_from_bytes, "__woozy_text_from_bytes", "Result[string,i32]", {"Vec[u8]"}, 1, "std/text"},
  {SystemOperation::text_from_rune, "__woozy_text_from_rune", "Result[string,i32]", {"u32"}, 1, "std/text"},
  {SystemOperation::text_bytes, "__woozy_text_bytes", "Vec[u8]", {"string"}, 1, "std/text"},
  {SystemOperation::text_load8, "__woozy_text_load8", "Option[u64]", {"string", "usize"}, 2, "std/text"},
  {SystemOperation::vec_empty, "__woozy_vec_empty", "Vec[A]", {}, 0, "std/vec", 1},
  {SystemOperation::vec_make, "__woozy_vec_make", "Vec[A]", {"usize", "A"}, 2, "std/vec", 1},
  {SystemOperation::vec_len, "__woozy_vec_len", "usize", {"Vec[A]"}, 1, "std/vec", 1},
  {SystemOperation::vec_capacity, "__woozy_vec_capacity", "usize", {"Vec[A]"}, 1, "std/vec", 1},
  {SystemOperation::vec_reserve, "__woozy_vec_reserve", "Unit", {"Vec[A]", "usize"}, 2, "std/vec", 1},
  {SystemOperation::vec_push, "__woozy_vec_push", "Unit", {"Vec[A]", "A"}, 2, "std/vec", 1},
  {SystemOperation::vec_pop, "__woozy_vec_pop", "Option[A]", {"Vec[A]"}, 1, "std/vec", 1},
  {SystemOperation::vec_resize, "__woozy_vec_resize", "Unit", {"Vec[A]", "usize", "A"}, 3, "std/vec", 1},
  {SystemOperation::vec_clear, "__woozy_vec_clear", "Unit", {"Vec[A]"}, 1, "std/vec", 1},
  {SystemOperation::vec_try_move_trivial, "__woozy_vec_try_move_trivial", "bool", {"Vec[A]", "usize", "usize", "usize"}, 4, "std/vec", 1},
  {SystemOperation::heap_pin_begin, "__woozy_heap_pin_begin", "Unit", {"Vec[A]"}, 1, "std/heap", 1},
  {SystemOperation::heap_pin_end, "__woozy_heap_pin_end", "Unit", {"Vec[A]"}, 1, "std/heap", 1},
  {SystemOperation::num_parse_i64, "__woozy_num_parse_i64", "Result[i64,i32]", {"string"}, 1, "std/strconv"},
  {SystemOperation::num_format_i64, "__woozy_num_format_i64", "string", {"i64"}, 1, "std/strconv"},
  {SystemOperation::num_parse_u64, "__woozy_num_parse_u64", "Result[u64,i32]", {"string"}, 1, "std/strconv"},
  {SystemOperation::num_format_u64, "__woozy_num_format_u64", "string", {"u64"}, 1, "std/strconv"},
  {SystemOperation::num_parse_f32, "__woozy_num_parse_f32", "Result[f32,i32]", {"string"}, 1, "std/strconv"},
  {SystemOperation::num_format_f32, "__woozy_num_format_f32", "string", {"f32"}, 1, "std/strconv"},
  {SystemOperation::num_parse_f64, "__woozy_num_parse_f64", "Result[f64,i32]", {"string"}, 1, "std/strconv"},
  {SystemOperation::num_format_f64, "__woozy_num_format_f64", "string", {"f64"}, 1, "std/strconv"},
  {SystemOperation::math_fabs, "__woozy_math_fabs", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_floor, "__woozy_math_floor", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_ceil, "__woozy_math_ceil", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_trunc, "__woozy_math_trunc", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_round, "__woozy_math_round", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_sqrt, "__woozy_math_sqrt", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_sin, "__woozy_math_sin", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_cos, "__woozy_math_cos", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_tan, "__woozy_math_tan", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_asin, "__woozy_math_asin", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_acos, "__woozy_math_acos", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_atan, "__woozy_math_atan", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_exp, "__woozy_math_exp", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_exp2, "__woozy_math_exp2", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_expm1, "__woozy_math_expm1", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_log, "__woozy_math_log", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_log2, "__woozy_math_log2", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_log10, "__woozy_math_log10", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_log1p, "__woozy_math_log1p", "f64", {"f64"}, 1, "std/math"},
  {SystemOperation::math_copysign, "__woozy_math_copysign", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_hypot, "__woozy_math_hypot", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_atan2, "__woozy_math_atan2", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_pow, "__woozy_math_pow", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_fmod, "__woozy_math_fmod", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_remainder, "__woozy_math_remainder", "f64", {"f64", "f64"}, 2, "std/math"},
  {SystemOperation::math_fma, "__woozy_math_fma", "f64", {"f64", "f64", "f64"}, 3, "std/math"},
  {SystemOperation::math_ldexp, "__woozy_math_ldexp", "f64", {"f64", "i32"}, 2, "std/math"},
  {SystemOperation::math_frexp, "__woozy_math_frexp", "std/math.Fraction", {"f64"}, 1, "std/math"},
  {SystemOperation::math_isnan, "__woozy_math_isnan", "bool", {"f64"}, 1, "std/math"},
  {SystemOperation::math_isinf, "__woozy_math_isinf", "bool", {"f64"}, 1, "std/math"},
  {SystemOperation::math_isfinite, "__woozy_math_isfinite", "bool", {"f64"}, 1, "std/math"},
  {SystemOperation::math_signbit, "__woozy_math_signbit", "bool", {"f64"}, 1, "std/math"},
}};
inline constexpr std::array<SystemOperationSpec, 37> os_system_operations{{
  {SystemOperation::fs_stat, "__woozy_os_stat", "Result[std/os.FileInfo,i32]", {"string"}, 1},
  {SystemOperation::fs_lstat, "__woozy_os_lstat", "Result[std/os.FileInfo,i32]", {"string"}, 1},
  {SystemOperation::fs_fstat, "__woozy_os_fstat", "Result[std/os.FileInfo,i32]", {"i32"}, 1},
  {SystemOperation::fs_rename, "__woozy_os_rename", "Result[Unit,i32]", {"string", "string"}, 2},
  {SystemOperation::fs_chmod, "__woozy_os_chmod", "Result[Unit,i32]", {"string", "u32"}, 2},
  {SystemOperation::fs_fchmod, "__woozy_os_fchmod", "Result[Unit,i32]", {"i32", "u32"}, 2},
  {SystemOperation::fs_chdir, "__woozy_os_chdir", "Result[Unit,i32]", {"string"}, 1},
  {SystemOperation::fs_getcwd, "__woozy_os_getcwd", "i64", {"Vec[u8]", "usize"}, 2},
  {SystemOperation::fs_getdents, "__woozy_os_getdents", "i64", {"i32", "Vec[u8]", "usize"}, 3},
  {SystemOperation::fs_readlink, "__woozy_os_readlink", "i64", {"string", "Vec[u8]", "usize"}, 3},
  {SystemOperation::fs_symlink, "__woozy_os_symlink", "Result[Unit,i32]", {"string", "string"}, 2},
  {SystemOperation::fs_link, "__woozy_os_link", "Result[Unit,i32]", {"string", "string"}, 2},
  {SystemOperation::fs_truncate, "__woozy_os_truncate", "Result[Unit,i32]", {"string", "i64"}, 2},
  {SystemOperation::fs_ftruncate, "__woozy_os_ftruncate", "Result[Unit,i32]", {"i32", "i64"}, 2},
  {SystemOperation::fs_fsync, "__woozy_os_fsync", "Result[Unit,i32]", {"i32"}, 1},
  {SystemOperation::fs_fdatasync, "__woozy_os_fdatasync", "Result[Unit,i32]", {"i32"}, 1},
  {SystemOperation::fs_dup, "__woozy_os_dup", "Result[i32,i32]", {"i32"}, 1},
  {SystemOperation::fs_dup2, "__woozy_os_dup2", "Result[i32,i32]", {"i32", "i32"}, 2},
  {SystemOperation::fs_pipe, "__woozy_os_pipe", "Result[std/os.Pipe,i32]", {"u32"}, 1},
  {SystemOperation::fs_mkfifo, "__woozy_os_mkfifo", "Result[Unit,i32]", {"string", "u32"}, 2},
  {SystemOperation::fs_access, "__woozy_os_access", "Result[Unit,i32]", {"string", "i32"}, 2},
  {SystemOperation::os_getuid, "__woozy_os_getuid", "Result[u32,i32]", {}, 0},
  {SystemOperation::os_geteuid, "__woozy_os_geteuid", "Result[u32,i32]", {}, 0},
  {SystemOperation::os_getgid, "__woozy_os_getgid", "Result[u32,i32]", {}, 0},
  {SystemOperation::os_getegid, "__woozy_os_getegid", "Result[u32,i32]", {}, 0},
  {SystemOperation::os_getppid, "__woozy_os_getppid", "Result[i32,i32]", {}, 0},
  {SystemOperation::os_gettid, "__woozy_os_gettid", "Result[i32,i32]", {}, 0},
  {SystemOperation::os_setpgid, "__woozy_os_setpgid", "Result[Unit,i32]", {"i32", "i32"}, 2},
  {SystemOperation::os_getpgid, "__woozy_os_getpgid", "Result[i32,i32]", {"i32"}, 1},
  {SystemOperation::os_setsid, "__woozy_os_setsid", "Result[i32,i32]", {}, 0},
  {SystemOperation::os_kill, "__woozy_os_kill", "Result[Unit,i32]", {"i32", "i32"}, 2},
  {SystemOperation::clock_gettime, "__woozy_os_clock_gettime", "Result[std/os.Timespec,i32]", {"i32"}, 1},
  {SystemOperation::nanosleep, "__woozy_os_nanosleep", "i64", {"std/os.Timespec", "Vec[std/os.Timespec]"}, 2},
  {SystemOperation::getrandom, "__woozy_os_getrandom", "i64", {"Vec[u8]", "usize", "u32"}, 3},
  // Linux paths are bytes. The runtime already accepts (data, byte_length),
  // so this operation reuses lang_sys_stat_v1 instead of widening the ABI.
  {SystemOperation::fs_stat_bytes, "__woozy_os_stat_bytes", "Result[std/os.FileInfo,i32]", {"Vec[u8]"}, 1},
  {SystemOperation::fs_flock, "__woozy_os_flock", "Result[Unit,i32]", {"i32", "i32"}, 2},
  {SystemOperation::fs_executable_path, "__woozy_os_executable_path", "i64", {"Vec[u8]", "usize"}, 2},
}};
inline constexpr std::array<SystemOperationSpec, 21> network_system_operations{{
  {SystemOperation::net_socket, "__woozy_net_socket", "Result[i32,i32]", {"i32", "i32", "i32"}, 3, "std/net"},
  {SystemOperation::net_bind, "__woozy_net_bind", "Result[Unit,i32]", {"i32", "std/net.Address"}, 2, "std/net"},
  {SystemOperation::net_connect, "__woozy_net_connect", "Result[Unit,i32]", {"i32", "std/net.Address"}, 2, "std/net"},
  {SystemOperation::net_listen, "__woozy_net_listen", "Result[Unit,i32]", {"i32", "i32"}, 2, "std/net"},
  {SystemOperation::net_accept, "__woozy_net_accept", "Result[std/net.Accepted,i32]", {"i32", "u32"}, 2, "std/net"},
  {SystemOperation::net_send, "__woozy_net_send", "Result[usize,i32]", {"i32", "Vec[u8]", "usize", "usize", "i32"}, 5, "std/net"},
  {SystemOperation::net_recv, "__woozy_net_recv", "Result[usize,i32]", {"i32", "Vec[u8]", "usize", "i32"}, 4, "std/net"},
  {SystemOperation::net_sendto, "__woozy_net_sendto", "Result[usize,i32]", {"i32", "Vec[u8]", "usize", "i32", "std/net.Address"}, 5, "std/net"},
  {SystemOperation::net_recvfrom, "__woozy_net_recvfrom", "Result[std/net.Received,i32]", {"i32", "Vec[u8]", "usize", "i32"}, 4, "std/net"},
  {SystemOperation::net_shutdown, "__woozy_net_shutdown", "Result[Unit,i32]", {"i32", "i32"}, 2, "std/net"},
  {SystemOperation::net_getsockname, "__woozy_net_getsockname", "Result[std/net.Address,i32]", {"i32"}, 1, "std/net"},
  {SystemOperation::net_getpeername, "__woozy_net_getpeername", "Result[std/net.Address,i32]", {"i32"}, 1, "std/net"},
  {SystemOperation::net_setsockopt_int, "__woozy_net_setsockopt_int", "Result[Unit,i32]", {"i32", "i32", "i32", "i32"}, 4, "std/net"},
  {SystemOperation::net_getsockopt_int, "__woozy_net_getsockopt_int", "Result[i32,i32]", {"i32", "i32", "i32"}, 3, "std/net"},
  {SystemOperation::net_setsockopt_bytes, "__woozy_net_setsockopt_bytes", "Result[Unit,i32]", {"i32", "i32", "i32", "Vec[u8]", "usize"}, 5, "std/net"},
  {SystemOperation::net_getsockopt_bytes, "__woozy_net_getsockopt_bytes", "Result[usize,i32]", {"i32", "i32", "i32", "Vec[u8]", "usize"}, 5, "std/net"},
  {SystemOperation::net_socketpair, "__woozy_net_socketpair", "Result[std/net.Pair,i32]", {"i32", "i32", "i32"}, 3, "std/net"},
  {SystemOperation::net_poll, "__woozy_net_poll", "Result[i32,i32]", {"Vec[std/net.PollFd]", "usize", "i32"}, 3, "std/net"},
  {SystemOperation::net_inet_pton, "__woozy_net_inet_pton", "Result[std/net.Address,i32]", {"i32", "string"}, 2, "std/net"},
  {SystemOperation::net_inet_ntop, "__woozy_net_inet_ntop", "Result[string,i32]", {"std/net.Address"}, 1, "std/net"},
  {SystemOperation::net_lookup, "__woozy_net_lookup", "Result[Vec[std/net.Address],i32]", {"string", "u16", "i32"}, 3, "std/net"},
}};
inline constexpr std::array<SystemOperationSpec, 50> process_system_operations{{
  {SystemOperation::process_args, "__woozy_process_args", "Result[Vec[string],i32]", {}, 0, "std/process"},
  {SystemOperation::process_args_bytes, "__woozy_process_args_bytes", "Result[Vec[Vec[u8]],i32]", {}, 0, "std/process"},
  {SystemOperation::process_environment, "__woozy_process_environment", "Result[Vec[string],i32]", {}, 0, "std/process"},
  {SystemOperation::process_environment_bytes, "__woozy_process_environment_bytes", "Result[Vec[Vec[u8]],i32]", {}, 0, "std/process"},
  {SystemOperation::process_getenv, "__woozy_process_getenv", "Result[Option[string],i32]", {"string"}, 1, "std/process"},
  {SystemOperation::process_getenv_bytes, "__woozy_process_getenv_bytes", "Result[Option[Vec[u8]],i32]", {"Vec[u8]"}, 1, "std/process"},
  {SystemOperation::process_setenv, "__woozy_process_setenv", "Result[Unit,i32]", {"string", "string", "i32"}, 3, "std/process"},
  {SystemOperation::process_unsetenv, "__woozy_process_unsetenv", "Result[Unit,i32]", {"string"}, 1, "std/process"},
  {SystemOperation::process_execve, "__woozy_process_execve", "Result[Unit,i32]", {"string", "Vec[string]", "Vec[string]"}, 3, "std/process"},
  {SystemOperation::process_spawn, "__woozy_process_spawn", "Result[i32,i32]", {"string", "Vec[string]", "Vec[string]", "i32", "i32", "i32"}, 6, "std/process"},
  {SystemOperation::process_setenv_bytes, "__woozy_process_setenv_bytes", "Result[Unit,i32]", {"Vec[u8]", "Vec[u8]", "i32"}, 3, "std/process"},
  {SystemOperation::process_unsetenv_bytes, "__woozy_process_unsetenv_bytes", "Result[Unit,i32]", {"Vec[u8]"}, 1, "std/process"},
  {SystemOperation::process_execve_bytes, "__woozy_process_execve_bytes", "Result[Unit,i32]", {"Vec[u8]", "Vec[Vec[u8]]", "Vec[Vec[u8]]"}, 3, "std/process"},
  {SystemOperation::process_spawn_bytes, "__woozy_process_spawn_bytes", "Result[i32,i32]", {"Vec[u8]", "Vec[Vec[u8]]", "Vec[Vec[u8]]", "i32", "i32", "i32"}, 6, "std/process"},
  {SystemOperation::process_fork, "__woozy_process_fork", "Result[i32,i32]", {}, 0, "std/process"},
  {SystemOperation::process_waitpid, "__woozy_process_waitpid", "Result[std/process.WaitStatus,i32]", {"i32", "i32"}, 2, "std/process"},
  {SystemOperation::signal_mask, "__woozy_signal_mask", "Result[u64,i32]", {"i32", "u64"}, 2, "std/signal"},
  {SystemOperation::signal_pending, "__woozy_signal_pending", "Result[u64,i32]", {}, 0, "std/signal"},
  {SystemOperation::signal_wait, "__woozy_signal_wait", "Result[std/signal.Info,i32]", {"u64", "i64"}, 2, "std/signal"},
  {SystemOperation::signal_disposition, "__woozy_signal_disposition", "Result[Unit,i32]", {"i32", "i32"}, 2, "std/signal"},
  {SystemOperation::ipc_map_anon, "__woozy_ipc_map_anon", "Result[__woozy_IpcHandle,i32]", {"usize", "i32"}, 2, "std/ipc"},
  {SystemOperation::ipc_map_file, "__woozy_ipc_map_file", "Result[__woozy_IpcHandle,i32]", {"i32", "usize", "u64", "i32"}, 4, "std/ipc"},
  {SystemOperation::ipc_shm_open, "__woozy_ipc_shm_open", "Result[i32,i32]", {"string", "i32", "u32"}, 3, "std/ipc"},
  {SystemOperation::ipc_shm_unlink, "__woozy_ipc_shm_unlink", "Result[Unit,i32]", {"string"}, 1, "std/ipc"},
  {SystemOperation::ipc_read_bytes, "__woozy_ipc_read_bytes", "Result[Vec[u8],i32]", {"__woozy_IpcHandle", "usize", "usize"}, 3, "std/ipc"},
  {SystemOperation::ipc_write_bytes, "__woozy_ipc_write_bytes", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize", "Vec[u8]", "usize", "usize"}, 5, "std/ipc"},
  {SystemOperation::ipc_read_i32, "__woozy_ipc_read_i32", "Result[i32,i32]", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_write_i32, "__woozy_ipc_write_i32", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize", "i32"}, 3, "std/ipc"},
  {SystemOperation::ipc_read_i64, "__woozy_ipc_read_i64", "Result[i64,i32]", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_write_i64, "__woozy_ipc_write_i64", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize", "i64"}, 3, "std/ipc"},
  {SystemOperation::ipc_atomic_load, "__woozy_ipc_atomic_load", "Result[i64,i32]", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_atomic_store, "__woozy_ipc_atomic_store", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize", "i64"}, 3, "std/ipc"},
  {SystemOperation::ipc_atomic_add, "__woozy_ipc_atomic_add", "Result[i64,i32]", {"__woozy_IpcHandle", "usize", "i64"}, 3, "std/ipc"},
  {SystemOperation::ipc_atomic_compare, "__woozy_ipc_atomic_compare", "Result[bool,i32]", {"__woozy_IpcHandle", "usize", "i64", "i64"}, 4, "std/ipc"},
  {SystemOperation::ipc_mutex_init, "__woozy_ipc_mutex_init", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_cond_init, "__woozy_ipc_cond_init", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_sem_init, "__woozy_ipc_sem_init", "Result[Unit,i32]", {"__woozy_IpcHandle", "usize", "u32"}, 3, "std/ipc"},
  {SystemOperation::ipc_length, "__woozy_ipc_length", "usize", {"__woozy_IpcHandle"}, 1, "std/ipc"},
  {SystemOperation::ipc_mutex_lock, "__woozy_ipc_mutex_lock", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_mutex_unlock, "__woozy_ipc_mutex_unlock", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_mutex_destroy, "__woozy_ipc_mutex_destroy", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_cond_signal, "__woozy_ipc_cond_signal", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_cond_broadcast, "__woozy_ipc_cond_broadcast", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_cond_destroy, "__woozy_ipc_cond_destroy", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_sem_post, "__woozy_ipc_sem_post", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_sem_destroy, "__woozy_ipc_sem_destroy", "Unit", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_cond_wait, "__woozy_ipc_cond_wait", "bool", {"__woozy_IpcHandle", "usize", "__woozy_IpcHandle", "usize", "i64"}, 5, "std/ipc"},
  {SystemOperation::ipc_sem_wait, "__woozy_ipc_sem_wait", "bool", {"__woozy_IpcHandle", "usize", "i64"}, 3, "std/ipc"},
  {SystemOperation::ipc_sem_trywait, "__woozy_ipc_sem_trywait", "bool", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
  {SystemOperation::ipc_sem_value, "__woozy_ipc_sem_value", "u32", {"__woozy_IpcHandle", "usize"}, 2, "std/ipc"},
}};
// std/ffi pointer/buffer plumbing. These operations never fabricate a raw
// address from an integer and never accept an arbitrary pointer spelling;
// their types are the same frozen C ABI pointer/buffer types used by schema
// v2 bindings.
inline constexpr std::array<SystemOperationSpec, 4> ffi_system_operations{{
  {SystemOperation::ffi_buffer, "__woozy_ffi_buffer", "Buffer[A]",
   {"Ptr[A]", "usize"}, 2, "std/ffi", 1},
  {SystemOperation::ffi_buffer_mut, "__woozy_ffi_buffer_mut", "BufferMut[A]",
   {"PtrMut[A]", "usize"}, 2, "std/ffi", 1},
  {SystemOperation::ffi_buffer_len, "__woozy_ffi_buffer_len", "usize",
   {"Buffer[A]"}, 1, "std/ffi", 1},
  {SystemOperation::ffi_buffer_len_mut, "__woozy_ffi_buffer_len_mut", "usize",
   {"BufferMut[A]"}, 1, "std/ffi", 1},
}};
// Source-level borrowed Slice operations. They are intentionally separate from
// value_system_operations: the Python backend must reject std/slice explicitly
// rather than silently copying a Vec.
inline constexpr std::array<SystemOperationSpec, 4> slice_system_operations{{
  {SystemOperation::slice_view, "__woozy_slice_view", "Slice[A]",
   {"Vec[A]", "usize", "usize"}, 3, "std/slice", 1},
  {SystemOperation::slice_len, "__woozy_slice_len", "usize",
   {"Slice[A]"}, 1, "std/slice", 1},
  {SystemOperation::slice_sub, "__woozy_slice_sub", "Slice[A]",
   {"Slice[A]", "usize", "usize"}, 3, "std/slice", 1},
  {SystemOperation::slice_to_vec, "__woozy_slice_to_vec", "Vec[A]",
   {"Slice[A]"}, 1, "std/slice", 1},
}};
// Weak[A] only accepts the canonical source targets checked by the frontend.
// downgrade/upgrade/empty never manufacture an address-keyed registry.
inline constexpr std::array<SystemOperationSpec, 3> weak_system_operations{{
  {SystemOperation::weak_empty, "__woozy_weak_empty", "Weak[A]", {}, 0, "std/weak", 1},
  {SystemOperation::weak_downgrade, "__woozy_weak_downgrade", "Weak[A]", {"A"}, 1, "std/weak", 1},
  {SystemOperation::weak_upgrade, "__woozy_weak_upgrade", "Option[A]", {"Weak[A]"}, 1, "std/weak", 1},
}};
inline constexpr auto all_system_operations = [] {
  std::array<SystemOperationSpec, system_operations.size() +
      concurrency_system_operations.size() + value_system_operations.size() + os_system_operations.size() + network_system_operations.size() + process_system_operations.size() + ffi_system_operations.size() + slice_system_operations.size() + weak_system_operations.size() + async_system_operations.size()> all{};
  std::size_t index = 0;
  for (const auto &spec : system_operations) all[index++] = spec;
  for (const auto &spec : concurrency_system_operations) all[index++] = spec;
  for (const auto &spec : value_system_operations) all[index++] = spec;
  for (const auto &spec : os_system_operations) all[index++] = spec;
  for (const auto &spec : network_system_operations) all[index++] = spec;
  for (const auto &spec : process_system_operations) all[index++] = spec;
  for (const auto &spec : ffi_system_operations) all[index++] = spec;
  for (const auto &spec : slice_system_operations) all[index++] = spec;
  for (const auto &spec : weak_system_operations) all[index++] = spec;
  for (const auto &spec : async_system_operations) all[index++] = spec;
  return all;
}();
inline const SystemOperationSpec *system_operation_spec(SystemOperation operation) {
  for (const auto &spec : all_system_operations)
    if (spec.operation == operation) return &spec;
  return nullptr;
}
inline const SystemOperationSpec *system_operation_spec(std::string_view name) {
  for (const auto &spec : all_system_operations)
    if (spec.name == name) return &spec;
  return nullptr;
}
// Private runtime handle type spellings. Only the owner std modules may use
// them in source type syntax; ordinary generic values may contain or pass the
// resulting handles without naming these spellings.
inline bool private_runtime_type_name(std::string_view name) {
  return name == "__woozy_Thread" || name == "__woozy_Mutex" ||
         name == "__woozy_Condvar" || name == "__woozy_AtomicI64" || name == "__woozy_IpcHandle";
}
inline bool private_runtime_type_module(std::string_view logical_path) {
  return logical_path == "std/thread" || logical_path == "std/sync" || logical_path == "std/ipc";
}
inline bool system_operation_has_type_arguments(SystemOperation operation) {
  const auto *spec = system_operation_spec(operation);
  return spec && spec->type_arity != 0;
}
} // namespace lang::hir

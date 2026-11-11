#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace lang::hir {
// Compiler-private typed operations restricted to their source std module.
// No operation accepts a syscall number or an untyped address.
enum class SystemOperation : uint8_t {
  read, write, write_text, open, close, seek, unlink, mkdir, rmdir,
  getpid, exit, vec_length, text_length,
  thread_spawn, thread_error, thread_wait, thread_result,
  mutex_new, mutex_lock, mutex_unlock,
  condvar_new, condvar_wait, condvar_notify_one, condvar_notify_all,
  atomic_i64_new, atomic_i64_load, atomic_i64_store, atomic_i64_fetch_add,
  atomic_i64_compare_exchange
};
struct SystemOperationSpec {
  SystemOperation operation;
  std::string_view name, result;
  std::array<std::string_view, 4> parameters;
  uint32_t count;
  // Logical source module allowed to call this operation. The default keeps
  // the original std/os entries unchanged.
  std::string_view owner = "std/os";
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
// New owning runtime operations are kept in a separate table so the original
// std/os operation inventory (and its source tests) remains exact. Both tables
// are searched by system_operation_spec and registered in the prelude.
inline constexpr std::array<SystemOperationSpec, 16> concurrency_system_operations{{
  {SystemOperation::thread_spawn, "__woozy_thread_spawn", "__woozy_Thread",
   {"func(A) i64", "A", "", ""}, 2, "std/thread"},
  {SystemOperation::thread_error, "__woozy_thread_error", "i64",
   {"__woozy_Thread", "", "", ""}, 1, "std/thread"},
  {SystemOperation::thread_wait, "__woozy_thread_wait", "i64",
   {"__woozy_Thread", "", "", ""}, 1, "std/thread"},
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
inline const SystemOperationSpec *system_operation_spec(SystemOperation operation) {
  for (const auto &spec : system_operations)
    if (spec.operation == operation) return &spec;
  for (const auto &spec : concurrency_system_operations)
    if (spec.operation == operation) return &spec;
  return nullptr;
}
inline const SystemOperationSpec *system_operation_spec(std::string_view name) {
  for (const auto &spec : system_operations)
    if (spec.name == name) return &spec;
  for (const auto &spec : concurrency_system_operations)
    if (spec.name == name) return &spec;
  return nullptr;
}
// Private runtime handle type spellings. Only the owner std modules may use
// them in source type syntax; ordinary generic values may contain or pass the
// resulting handles without naming these spellings.
inline bool private_runtime_type_name(std::string_view name) {
  return name == "__woozy_Thread" || name == "__woozy_Mutex" ||
         name == "__woozy_Condvar" || name == "__woozy_AtomicI64";
}
inline bool private_runtime_type_module(std::string_view logical_path) {
  return logical_path == "std/thread" || logical_path == "std/sync";
}
inline bool system_operation_has_type_arguments(SystemOperation operation) {
  return operation == SystemOperation::thread_spawn;
}
} // namespace lang::hir

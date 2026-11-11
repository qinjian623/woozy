#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace lang::hir {
// Compiler-private, typed operations used only by the source std/os module.
// No operation accepts a syscall number or an untyped address.
enum class SystemOperation : uint8_t {
  read, write, write_text, open, close, seek, unlink, mkdir, rmdir,
  getpid, exit, vec_length, text_length
};
struct SystemOperationSpec {
  SystemOperation operation;
  std::string_view name, result;
  std::array<std::string_view, 4> parameters;
  uint32_t count;
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
inline const SystemOperationSpec *system_operation_spec(SystemOperation operation) {
  for (const auto &spec : system_operations)
    if (spec.operation == operation) return &spec;
  return nullptr;
}
inline const SystemOperationSpec *system_operation_spec(std::string_view name) {
  for (const auto &spec : system_operations)
    if (spec.name == name) return &spec;
  return nullptr;
}
} // namespace lang::hir

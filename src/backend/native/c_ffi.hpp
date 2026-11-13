#pragma once
#include "lang/modules/project.hpp"
#include <set>
#include <string>
#include <utility>

namespace lang::backend::native::cffi {
std::string adapter_symbol(uint32_t callable);
std::string release_symbol(uint32_t c_type);
std::string retain_symbol(uint32_t c_type);
std::string callback_bridge_symbol(uint64_t function, uint32_t c_type);
std::string callback_trampoline_symbol(uint64_t function, uint32_t c_type);
// Managed capturing registration metadata (development/docs/
// C_RETAINED_CAPTURING_CALLBACKS.md). The mapping is one generic C trampoline
// and one typed LLVM dispatcher per callback type/userdata position; no
// per-instance code is generated.
struct RegistrationCallback {
  uint32_t resource_type = 0;
  uint32_t callback_type = 0;
  size_t callback_parameter = 0;
  size_t userdata_parameter = 0;
  size_t callback_userdata_parameter = 0;
};
std::optional<RegistrationCallback>
registration_callback(const modules::Project &project,
                      const modules::CCallableDecl &callable);
bool registration_close(const modules::Project &project,
                        const modules::CCallableDecl &callable);
std::string registration_callback_trampoline_symbol(uint32_t c_type,
                                                    size_t userdata);
std::string registration_callback_dispatch_symbol(uint32_t c_type,
                                                  size_t userdata);
std::string registration_unregister_symbol(uint32_t c_type);
// Generates self-contained C11 adapter declarations for the frozen binding.
// Only callables actually referenced by IR become exported adapter functions;
// every declared record/callback/buffer spelling is available to them.
std::string adapter_source(
    const modules::Project &project, const std::set<uint32_t> &callables,
    const std::set<uint32_t> &resource_types,
    const std::set<std::pair<uint64_t, uint32_t>> &callbacks);
} // namespace lang::backend::native::cffi

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
// Generates self-contained C11 adapter declarations for the frozen binding.
// Only callables actually referenced by IR become exported adapter functions;
// every declared record/callback/buffer spelling is available to them.
std::string adapter_source(
    const modules::Project &project, const std::set<uint32_t> &callables,
    const std::set<uint32_t> &resource_types,
    const std::set<std::pair<uint64_t, uint32_t>> &callbacks);
} // namespace lang::backend::native::cffi

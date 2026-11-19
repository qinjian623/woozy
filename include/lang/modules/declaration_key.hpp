#pragma once
#include <string>
#include <string_view>

namespace lang::modules {
// Cache identities must never depend on allocation order of compiler IDs.
// Length framing makes directory names and declaration spellings unambiguous.
inline void append_key_component(std::string &key, std::string_view component) {
  key += std::to_string(component.size());
  key += ':';
  key.append(component);
}
inline std::string declaration_key(std::string_view package_identity,
                                   std::string_view declaration_kind,
                                   std::string_view owner_key,
                                   std::string_view name) {
  std::string key = "wz-decl/1:";
  for (auto part : {package_identity, declaration_kind, owner_key, name})
    append_key_component(key, part);
  return key;
}
} // namespace lang::modules

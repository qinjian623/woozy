#pragma once
#include "lang/syntax/ast.hpp"
#include <filesystem>

namespace lang::modules {
struct ParseCache {
  std::filesystem::path directory;
  std::string fingerprint;
  size_t hits = 0, misses = 0, string_tokens_checked = 0;
  std::shared_ptr<const AstFile> load(std::shared_ptr<const SourceSnapshot>);
  void store(const AstFile &);
};
}

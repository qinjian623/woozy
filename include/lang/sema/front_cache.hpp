#pragma once
#include "lang/hir/hir.hpp"
#include <filesystem>
#include <set>

namespace lang::sema {
// First persistent frontend format: scalar, non-generic package bodies. Other
// packages keep the ordinary checker; no partial record grants verification.
struct FrontCache {
  std::filesystem::path directory;
  std::string fingerprint;
  size_t hits = 0, misses = 0, rejected = 0, checked_bodies = 0;
  bool disabled = false;
  std::string package_key(const hir::HirModule &, ModuleId) const;
  std::set<uint64_t> restore(hir::HirModule &);
  void store(const hir::HirModule &);
};
PhaseResult<hir::VerifiedHirModule> check_project_cached(
    const PhaseResult<modules::ResolvedProject> &, FrontCache &);
}

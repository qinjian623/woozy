#pragma once
#include "lang/support/source.hpp"
#include <optional>
namespace lang {
enum class Status { ok, error, cancelled, internal_error };
enum class ExitCode {
  success = 0,
  source_error = 1,
  environment_error = 2,
  internal_error = 3,
  cancelled = 130
};
struct Related {
  SourceSpan span;
  std::string message;
};
struct Fixit {
  SourceSpan span;
  std::string replacement;
  RevisionId revision;
};
struct Diagnostic {
  std::string code, phase, severity, message;
  std::optional<SourceSpan> primary;
  std::vector<Related> related;
  std::vector<Fixit> fixits;
  std::vector<std::string> notes;
  std::optional<std::string> expected_type, actual_type;
};
std::string diagnostic_json(const SourceManager &, std::string compiler_version,
                            std::string source_revision, Status,
                            const std::vector<Diagnostic> &);
} // namespace lang

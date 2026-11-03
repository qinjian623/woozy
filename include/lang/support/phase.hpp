#pragma once
#include "lang/support/diagnostic.hpp"
namespace lang {
enum class PhaseState { complete, recovered, failed, cancelled };
enum class FailureKind { source, unsupported, environment, internal };
template <class T> struct PhaseResult {
  PhaseState state = PhaseState::failed;
  std::optional<T> value;
  std::vector<Diagnostic> diagnostics;
  FailureKind failure = FailureKind::internal;
  bool success() const {
    if (state != PhaseState::complete || !value)
      return false;
    for (const auto &d : diagnostics)
      if (d.severity == "error")
        return false;
    return true;
  }
  ExitCode exit_code() const {
    if (state == PhaseState::cancelled)
      return ExitCode::cancelled;
    if (success())
      return ExitCode::success;
    switch (failure) {
    case FailureKind::environment:
      return ExitCode::environment_error;
    case FailureKind::internal:
      return ExitCode::internal_error;
    default:
      return ExitCode::source_error;
    }
  }
  Status status() const {
    if (state == PhaseState::cancelled)
      return Status::cancelled;
    if (success())
      return Status::ok;
    return failure == FailureKind::internal ? Status::internal_error
                                            : Status::error;
  }
};
} // namespace lang

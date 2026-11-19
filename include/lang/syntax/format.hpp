#pragma once
#include "lang/support/phase.hpp"

namespace lang {
PhaseResult<std::string>
format_source(std::shared_ptr<const SourceSnapshot> source);
}

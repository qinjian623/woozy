#pragma once
#include "lang/support/phase.hpp"
#include <filesystem>
namespace lang {
PhaseResult<std::shared_ptr<const SourceSnapshot>>
read_source(const std::filesystem::path &, SourceManager &,
            std::stop_token = {});
}

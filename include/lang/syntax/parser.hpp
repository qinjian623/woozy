#pragma once
#include "lang/syntax/ast.hpp"
namespace lang {
PhaseResult<AstFile> parse(const TokenBuffer &, std::stop_token = {});
}

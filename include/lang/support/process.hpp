#pragma once
#include <string>
#include <vector>
namespace lang {
struct ProcessResult { int exit_code; bool signalled; std::string output; };
ProcessResult run_process(const std::vector<std::string>& argv);
}

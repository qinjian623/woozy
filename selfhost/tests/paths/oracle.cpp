// Test-only canonical oracle. It is std::filesystem::canonical itself, so both
// the successful spelling and the exact filesystem_error text (operation,
// generic_category wording and the argument spelling) come from the real C++
// implementation instead of a Python approximation.
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  // The errno text table in wzcc/paths is checked against the local
  // generic_category, including a code it cannot name.
  for (int code : {2, 5, 12, 13, 17, 20, 21, 22, 23, 24, 36, 40, 9999})
    std::cout << "ERRNO\t" << code << '\t'
              << std::generic_category().message(code) << '\n';
  for (int index = 1; index < argc; ++index) {
    const std::string spelling = argv[index];
    try {
      // Evaluate before streaming so a throw cannot leave a partial OK prefix.
      const std::string resolved = fs::canonical(spelling).string();
      std::cout << "CANON\t" << spelling << "\tOK\t" << resolved << '\n';
    } catch (const fs::filesystem_error &e) {
      std::cout << "CANON\t" << spelling << "\tERR\t" << e.code().value()
                << '\t' << e.what() << '\n';
    }
  }
  return 0;
}

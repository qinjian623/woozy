// Test-only canonical oracle. It is std::filesystem::canonical itself, so both
// the successful spelling and the exact filesystem_error text (operation,
// generic_category wording and the argument spelling) come from the real C++
// implementation instead of a Python approximation. The errno table is checked
// under LC_ALL=C against this target's glibc generic_category text.
#include <climits>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

int main(int argc, char **argv) {
  // The errno text table in wzcc/paths is checked against the local
  // generic_category for every value in 0..255 plus signed and unknown edges.
  for (int code = 0; code <= 255; ++code)
    std::cout << "ERRNO\t" << code << '\t'
              << std::generic_category().message(code) << '\n';
  for (int code : {-1, 9999, INT_MIN, INT_MAX})
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

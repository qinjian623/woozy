#include "lang/support/file_digest.hpp"
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  try {
    lang::FileDigests cache{std::filesystem::path(argv[1]) / "file-digests"};
    const auto digest = cache.get(argv[2]);
    std::cout << "{\"sha256\":\"" << digest << "\",\"hits\":" << cache.hits
        << ",\"misses\":" << cache.misses << ",\"bytes_read\":" << cache.bytes_read << "}\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

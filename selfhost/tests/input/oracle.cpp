// Test-only oracle for the input I/O wording: this is the read loop of
// src/support/input.cpp, so the expected message comes from the real C++
// std::ifstream rather than a copied string. It prints one line per fixture:
//   IO<TAB>MESSAGE   for a failure (the compiler prefixes the path itself)
//   OK<TAB>PATH      for a successful read
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void probe(const std::string &path) {
  try {
    std::ifstream file(path, std::ios::binary);
    std::string bytes;
    std::array<char, 65536> buffer;
    if (!file.is_open())
      throw std::ios_base::failure("cannot open input");
    for (;;) {
      file.read(buffer.data(), buffer.size());
      bytes.append(buffer.data(), static_cast<size_t>(file.gcount()));
      if (file.bad() || (file.fail() && !file.eof()))
        throw std::ios_base::failure("cannot read input");
      if (file.eof())
        break;
    }
    std::cout << "OK\t" << path << '\n';
  } catch (const std::ios_base::failure &e) {
    std::cout << "IO\t" << path << ": " << e.what() << '\n';
  }
}

int main(int argc, char **argv) {
  try {
    if (argc != 2)
      throw std::runtime_error("usage: oracle BASE_DIRECTORY");
    const std::string base = argv[1];
    for (const std::string &path :
         {base + "/does-not-exist.bin", base + "/directory", std::string(),
          base + "/unreadable.bin", base + "/unreadable-directory",
          base + "/loop.bin", base + "/dangling.bin", base + "/file.bin/x"})
      probe(path);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 99;
  }
}

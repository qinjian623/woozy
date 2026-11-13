// Host-side reference for the raw byte-path stat ABI.
//
// It calls the same lang_sys_stat_v1 entry point the Wz program uses; this
// oracle is compiled against the real runtime archive, not a second
// implementation. The Wz and oracle stdout must match exactly.
#include "system.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
void report(const char *name, const void *data, uint64_t length) {
  LangFileStat status{};
  const int64_t result = lang_sys_stat_v1(
      static_cast<const char *>(data), length, &status);
  if (result < 0) {
    std::printf("%s: err %lld\n", name, static_cast<long long>(-result));
  } else {
    std::printf("%s: ok mode=%llu size=%lld\n", name,
                static_cast<unsigned long long>(status.mode),
                static_cast<long long>(status.size));
  }
}
uint64_t length_of(const char *path) {
  return static_cast<uint64_t>(std::strlen(path));
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 5) {
    std::fprintf(stderr, "usage: oracle INVALID UNICODE MISSING NOTDIR\n");
    return 2;
  }
  const std::vector<unsigned char> nul{'.', 0, 'x'};
  const std::vector<unsigned char> long_path(4096, 'A');
  report("empty", nullptr, 0);
  report("nul", nul.data(), nul.size());
  report("long", long_path.data(), long_path.size());
  report("invalid", argv[1], length_of(argv[1]));
  report("unicode", argv[2], length_of(argv[2]));
  report("missing", argv[3], length_of(argv[3]));
  report("notdir", argv[4], length_of(argv[4]));
  return 0;
}

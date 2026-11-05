#include "../../runtime/native/start.hpp"

int main() {
  unsigned char random[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  auto address = reinterpret_cast<uintptr_t>(random + 1);
  uintptr_t empty[] = {0, 0, 0, 25, address, 0, 0};
  if (lang_runtime::startup_canary(empty) != UINT64_C(0x0807060504030201)) return 1;
  uintptr_t populated[] = {2, 1, 2, 0, 3, 4, 0, 6, 4096, 25, address, 0, 0};
  if (lang_runtime::startup_canary(populated) != UINT64_C(0x0807060504030201)) return 2;
  uintptr_t absent[] = {0, 0, 0, 0, 0};
  if (lang_runtime::startup_canary(absent) != 0) return 3;
  uintptr_t null_random[] = {0, 0, 0, 25, 0, 0, 0};
  if (lang_runtime::startup_canary(null_random) != 0) return 4;
  if (lang_runtime::startup_canary(nullptr) != 0) return 5;
  return 0;
}

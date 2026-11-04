#include <cstdint>
#include <cstdio>
// Private link-time observer. The Python runner checks this stream by object
// identity; retain/release counts are deliberately not prescribed.
extern "C" void lang_vec_observe_v1(uint32_t event, const void *object, uint64_t detail) {
  std::fprintf(stderr, "VEC:%u:%p:%llu\n", event, object,
               static_cast<unsigned long long>(detail));
}
extern "C" void lang_test_value_v1(int64_t kind, int64_t value, int64_t log) {
  std::fprintf(stderr, "OBS:%lld:%lld:%lld\n", (long long)kind, (long long)value, (long long)log);
}

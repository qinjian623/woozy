#include "tls_source.hpp"

int main(void) {
  LangTlsError error = {0};
  WtsError *storage = wts_error_new();
  if (storage == 0) return 1;
  if (sizeof(error) != 40) return 2;
  wts_error_release(storage);
  return 0;
}

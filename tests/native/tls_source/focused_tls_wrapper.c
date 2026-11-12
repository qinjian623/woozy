#include "tls_source.hpp"

#include <stdint.h>
#include <stdio.h>

static int32_t previous_ssl_error = 0;

int32_t __real_wts_transport_read(uint64_t token, uint8_t *data,
                                  uint64_t capacity, WtsCount *count,
                                  WtsError *error);
int32_t __wrap_wts_transport_read(uint64_t token, uint8_t *data,
                                  uint64_t capacity, WtsCount *count,
                                  WtsError *error) {
  int32_t result =
      __real_wts_transport_read(token, data, capacity, count, error);
  if (result == 4) {
    previous_ssl_error = wts_error_ssl_error(error);
    dprintf(2, "TLS_READ_ERROR category=%d ssl_error=%d\n",
            wts_error_category(error), previous_ssl_error);
  }
  return result;
}

int32_t __real_wts_shutdown(WtsStream *stream, WtsError *error);
int32_t __wrap_wts_shutdown(WtsStream *stream, WtsError *error) {
  dprintf(2, "TLS_SHUTDOWN previous_ssl_error=%d\n", previous_ssl_error);
  return __real_wts_shutdown(stream, error);
}

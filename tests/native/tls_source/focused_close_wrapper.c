#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int calls = 0;
static int replacement = -1;

static void report_final(void) {
  if (replacement >= 0) {
    int alive = fcntl(replacement, F_GETFD);
    dprintf(2, "FINAL replacement_alive=%d\n", alive);
  }
}

__attribute__((constructor)) static void install_report(void) {
  atexit(report_final);
}

int64_t __real_lang_os_close_v1(int32_t fd);

int64_t __wrap_lang_os_close_v1(int32_t fd) {
  int before = (replacement >= 0) ? fcntl(replacement, F_GETFD) : -2;
  int is_replacement = (replacement >= 0 && fd == replacement);
  int64_t result = __real_lang_os_close_v1(fd);
  ++calls;
  dprintf(2, "CLOSE:%d fd=%d result=%lld replacement_alive_before=%d\n",
          calls, fd, (long long)result, before);
  if (is_replacement) dprintf(2, "REPLACEMENT_CLOSED\n");
  if (calls == 1 && result == 0) {
    replacement = open("/dev/null", O_RDONLY);
    dprintf(2, "OPEN_UNRELATED:%d\n", replacement);
  }
  return result;
}

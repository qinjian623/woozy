/* Deterministic diagnostic transport only; never counted as LLVM execution. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  const char *mode = getenv("WOOZY_NATIVEBUILD_TEST_TOOL");
  if (!mode) return 90;
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    puts(strcmp(mode, "wrong-version") == 0 ? "LLVM version 22.1.80" : "LLVM version 22.1.8");
    return 0;
  }
  if (strcmp(mode, "invalid-bytes") == 0) {
    const unsigned char bytes[] = {'A', 0, 255, 'B'};
    fwrite(bytes, 1, sizeof bytes, stderr);
    return 9;
  }
  fputs("diagnostic tool fixture\n", stderr);
  fflush(stderr);
  if (strcmp(mode, "signal") == 0) { raise(SIGTERM); return 91; }
  if (strcmp(mode, "exit126") == 0) return 126;
  if (strcmp(mode, "exit127") == 0) return 127;
  return 92;
}

// Ptrace-based deterministic rename harness for fs.remove_dir_all.
//
// The child runs the Woozy race fixture. Once the fixture writes "ARM", this
// parent stops it at a chosen newfstatat boundary, mutates the real tree with
// ordinary filesystem calls, and resumes. No test-only code exists in the
// production remove_dir_all path.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

static int read_string(pid_t pid, unsigned long address, char *buffer,
                       size_t capacity) {
  // Read one byte at a time: a whole-range process_vm_readv can fail when the
  // NUL-terminated string is at the end of a mapped page.
  for (size_t index = 0; index + 1 < capacity; ++index) {
    struct iovec local = {&buffer[index], 1};
    struct iovec remote = {(void *)(address + index), 1};
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) != 1) return -1;
    if (buffer[index] == '\0') return 0;
  }
  buffer[capacity - 1] = '\0';
  return -1;
}

static int write_sentinel(const char *path) {
  const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return -1;
  const char byte = 'r';
  const ssize_t wrote = write(fd, &byte, 1);
  const int close_result = close(fd);
  return wrote == 1 && close_result == 0 ? 0 : -1;
}

static int replace_child(void) {
  if (rename("race-root/child", "race-root/moved") != 0) return -1;
  if (mkdir("race-root/child", 0700) != 0) return -1;
  return write_sentinel("race-root/child/replacement");
}

static int replace_root(void) {
  if (rename("race-root", "moved-root") != 0) return -1;
  if (mkdir("race-root", 0700) != 0) return -1;
  return write_sentinel("race-root/replacement");
}

static int move_root_away(void) {
  return rename("race-root", "moved-root");
}

static int path_is(const char *path, const char *name) {
  if (strcmp(path, name) == 0) return 1;
  const size_t path_length = strlen(path);
  const size_t name_length = strlen(name);
  return path_length > name_length &&
         path[path_length - name_length - 1] == '/' &&
         strcmp(path + path_length - name_length, name) == 0;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s PROGRAM SCENARIO\n", argv[0]);
    return 2;
  }
  const char *scenario = argv[2];
  const int debug = getenv("FS_RACE_DEBUG") != NULL;
  if (debug) fprintf(stderr, "harness start %s\n", scenario);
  if (strcmp(scenario, "child-open") != 0 &&
      strcmp(scenario, "child-rmdir") != 0 &&
      strcmp(scenario, "root-replace") != 0 &&
      strcmp(scenario, "root-away") != 0) {
    fprintf(stderr, "unknown scenario: %s\n", scenario);
    return 2;
  }

  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return 2;
  }
  if (child == 0) {
    const int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      close(null_fd);
    }
    if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(126);
    raise(SIGSTOP);
    char *const child_argv[] = {argv[1], (char *)scenario, NULL};
    execv(argv[1], child_argv);
    _exit(127);
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFSTOPPED(status)) {
    fprintf(stderr, "initial stop failed\n");
    return 2;
  }
  if (ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACESYSGOOD) != 0) {
    perror("PTRACE_SETOPTIONS");
    return 2;
  }

  int entering = 1;
  int exec_seen = 0;
  int armed = 0;
  int done = 0;
  int pending_exit = 0;
  int matches = 0;
  for (;;) {
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) != 0) {
      perror("PTRACE_SYSCALL");
      return 2;
    }
    if (waitpid(child, &status, 0) != child) {
      perror("waitpid");
      return 2;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
      fprintf(stderr, "child signalled: %d\n", WTERMSIG(status));
      return 128 + WTERMSIG(status);
    }
    if (!WIFSTOPPED(status)) continue;
    const int signal = WSTOPSIG(status);
    if (signal != (SIGTRAP | 0x80)) {
      if (signal == SIGTRAP && !exec_seen) {
        exec_seen = 1;
        continue;
      }
      if (ptrace(PTRACE_SYSCALL, child, 0, signal) != 0) return 2;
      continue;
    }

    struct user_regs_struct registers;
    if (ptrace(PTRACE_GETREGS, child, 0, &registers) != 0) {
      perror("PTRACE_GETREGS");
      return 2;
    }
    const long syscall_number = (long)registers.orig_rax;
    if (debug && !armed) fprintf(stderr, "syscall %ld %s\n", syscall_number,
                                 entering ? "enter" : "exit");

    if (entering) {
      if (syscall_number == SYS_write && registers.rdi == STDOUT_FILENO) {
        char marker[16];
        const int read_result =
            read_string(child, registers.rsi, marker, sizeof marker);
        if (debug) {
          fprintf(stderr, "stdout write count=%llu read=%d data=%s\n",
                  (unsigned long long)registers.rdx, read_result,
                  read_result == 0 ? marker : "<unreadable>");
        }
        if (read_result == 0 && registers.rdx == 4 &&
            strncmp(marker, "ARM\n", 4) == 0) {
          armed = 1;
          if (debug) fprintf(stderr, "armed\n");
        }
      }
      if (armed && !done && syscall_number == SYS_newfstatat) {
        char path[256];
        const char *target =
            (strcmp(scenario, "child-open") == 0 ||
             strcmp(scenario, "child-rmdir") == 0)
                ? "child"
                : "race-root";
        if (read_string(child, registers.rsi, path, sizeof path) == 0) {
          if (debug) fprintf(stderr, "fstatat %s\n", path);
        }
        if (read_string(child, registers.rsi, path, sizeof path) == 0 &&
            path_is(path, target)) {
          ++matches;
          if (debug) fprintf(stderr, "match %s #%d\n", target, matches);
          if (strcmp(scenario, "child-open") == 0 && matches == 1) {
            pending_exit = 1;
          } else if (matches == 2) {
            int result = 0;
            if (strcmp(scenario, "child-open") == 0 ||
                strcmp(scenario, "child-rmdir") == 0) {
              result = replace_child();
            } else if (strcmp(scenario, "root-replace") == 0) {
              result = replace_root();
            } else {
              result = move_root_away();
            }
            if (result != 0) {
              fprintf(stderr, "rename point failed: %s\n", strerror(errno));
              return 2;
            }
            done = 1;
          }
        }
      }
      entering = 0;
    } else {
      if (pending_exit && !done && syscall_number == SYS_newfstatat) {
        if (replace_child() != 0) {
          fprintf(stderr, "child-open rename failed: %s\n", strerror(errno));
          return 2;
        }
        pending_exit = 0;
        done = 1;
      }
      entering = 1;
    }
  }
}

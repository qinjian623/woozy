#include "lang/support/process.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
namespace lang {
namespace {
[[noreturn]] void fail(int e) { throw std::system_error(e, std::generic_category()); }
struct File { int fd; ~File() { if (fd >= 0) ::close(fd); } };
struct Actions {
    posix_spawn_file_actions_t value;
    Actions() { int e = posix_spawn_file_actions_init(&value); if (e) fail(e); }
    ~Actions() { posix_spawn_file_actions_destroy(&value); }
};
}
ProcessResult run_process(const std::vector<std::string>& argv) {
    if (argv.empty()) throw std::invalid_argument("empty argv");
    for (const auto& s : argv) if (s.find('\0') != std::string::npos) throw std::invalid_argument("NUL in argv");
    std::vector<char*> args;
    for (const auto& s : argv) args.push_back(const_cast<char*>(s.c_str()));
    args.push_back(nullptr);
    char name[] = "/tmp/woozyc-process-XXXXXX";
    File file{mkostemp(name, O_CLOEXEC)};
    if (file.fd < 0) fail(errno);
    if (unlink(name) < 0) { int e = errno; ::unlink(name); fail(e); }
    // Keep the capture descriptor distinct from stdin/stdout/stderr.
    if (file.fd < 3) {
        int fd = fcntl(file.fd, F_DUPFD_CLOEXEC, 3);
        if (fd < 0) fail(errno);
        close(file.fd); file.fd = fd;
    }
    Actions actions;
    for (int target : {STDOUT_FILENO, STDERR_FILENO}) {
        int e = posix_spawn_file_actions_adddup2(&actions.value, file.fd, target);
        if (e) fail(e);
    }
    int e = posix_spawn_file_actions_addclose(&actions.value, file.fd);
    if (e) fail(e);
    pid_t pid;
    e = posix_spawn(&pid, argv[0].c_str(), &actions.value, nullptr, args.data(), environ);
    if (e) fail(e);
    int status;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) fail(errno); }
    if (lseek(file.fd, 0, SEEK_SET) < 0) fail(errno);
    ProcessResult result{WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status), WIFSIGNALED(status), {}};
    char buffer[16384];
    for (;;) {
        ssize_t n = read(file.fd, buffer, sizeof buffer);
        if (n < 0) { if (errno == EINTR) continue; fail(errno); }
        if (!n) break;
        result.output.append(buffer, static_cast<std::size_t>(n));
    }
    return result;
}
}

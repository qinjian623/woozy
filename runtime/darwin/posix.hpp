#ifndef LANG_RUNTIME_DARWIN_POSIX_HPP
#define LANG_RUNTIME_DARWIN_POSIX_HPP

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <signal.h>

namespace lang::native::darwin {

// The runtime ABI uses negative Linux errno, including in os.Error.System.
inline int64_t error(int value) noexcept {
  if (value > 0 && value <= ERANGE && value != EDEADLK) return -value;
  switch (value) {
    case EDEADLK: return -35;
    case ENAMETOOLONG: return -36;
    case ENOLCK: return -37;
    case ENOSYS: return -38;
    case ENOTEMPTY: return -39;
    case ELOOP: return -40;
    case ENOMSG: return -42;
    case EIDRM: return -43;
    case ENOSTR: return -60;
    case ENODATA:
    case ENOATTR: return -61;
    case ETIME: return -62;
    case ENOSR: return -63;
    case EREMOTE: return -66;
    case ENOLINK: return -67;
    case EPROTO: return -71;
    case EMULTIHOP: return -72;
    case EBADMSG: return -74;
    case EOVERFLOW: return -75;
    case EILSEQ: return -84;
    case EUSERS: return -87;
    case ENOTSOCK: return -88;
    case EDESTADDRREQ: return -89;
    case EMSGSIZE: return -90;
    case EPROTOTYPE: return -91;
    case ENOPROTOOPT: return -92;
    case EPROTONOSUPPORT: return -93;
    case ESOCKTNOSUPPORT: return -94;
    case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
      return -95;
    case EPFNOSUPPORT: return -96;
    case EAFNOSUPPORT: return -97;
    case EADDRINUSE: return -98;
    case EADDRNOTAVAIL: return -99;
    case ENETDOWN: return -100;
    case ENETUNREACH: return -101;
    case ENETRESET: return -102;
    case ECONNABORTED: return -103;
    case ECONNRESET: return -104;
    case ENOBUFS: return -105;
    case EISCONN: return -106;
    case ENOTCONN: return -107;
    case ESHUTDOWN: return -108;
    case ETOOMANYREFS: return -109;
    case ETIMEDOUT: return -110;
    case ECONNREFUSED: return -111;
    case EHOSTDOWN: return -112;
    case EHOSTUNREACH: return -113;
    case EALREADY: return -114;
    case EINPROGRESS: return -115;
    case ESTALE: return -116;
    case EDQUOT: return -122;
    case ECANCELED: return -125;
    case EOWNERDEAD: return -130;
    case ENOTRECOVERABLE: return -131;
    case EAGAIN:
    case EPROCLIM: return -11;
    case EBADEXEC:
    case EBADARCH:
    case ESHLIBVERS:
    case EBADMACHO: return -8;
    default: return -5;  // A Darwin-only failure without a Linux equivalent.
  }
}

inline int64_t result(int64_t value) noexcept {
  return value < 0 ? error(errno) : value;
}

struct Path {
  char bytes[4096];
  int64_t status = 0;

  Path(const char *data, uint64_t length) noexcept {
    if (length >= sizeof(bytes)) status = -36;
    else if (length != 0) {
      if (data == nullptr) status = -14;
      else if (std::memchr(data, 0, length)) status = -22;
      else std::memcpy(bytes, data, length);
    }
    if (status == 0) bytes[length] = '\0';
  }
};

// One directory-entry name for descriptor-relative operations. The validation
// matches the Linux runtime: non-empty, no NUL, no '/', and not "." or "..".
struct Component {
  char bytes[4096];
  int64_t status = 0;

  Component(const char *data, uint64_t length) noexcept {
    if (length == 0) {
      status = -22;
    } else if (length >= sizeof(bytes)) {
      status = -36;
    } else if (data == nullptr) {
      status = -14;
    } else if ((length == 1 && data[0] == '.') ||
               (length == 2 && data[0] == '.' && data[1] == '.')) {
      status = -22;
    } else {
      for (uint64_t index = 0; index < length; ++index) {
        if (data[index] == '\0' || data[index] == '/') {
          status = -22;
          return;
        }
        bytes[index] = data[index];
      }
      bytes[length] = '\0';
    }
  }
};

constexpr int32_t kNonblock = 2048;
constexpr int32_t kCloexec = 524288;

inline constexpr int linux_signal_to_darwin[] = {
  0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE,
  SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2, SIGPIPE, SIGALRM, SIGTERM, 0,
  SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGXCPU,
  SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0, SIGSYS,
};

// Darwin creates close-on-exec pipes in two POSIX calls. Any Darwin Woozy
// fork, spawn, or exec must take this lock while opening the inheritance
// window, and release it in both branches of fork. Never hold it while waiting
// for a child or entering SAFE.
inline uint32_t launch_lock = 0;

class LaunchGuard {
 public:
  LaunchGuard() noexcept {
    while (__atomic_exchange_n(&launch_lock, 1u, __ATOMIC_ACQUIRE)) {
      while (__atomic_load_n(&launch_lock, __ATOMIC_RELAXED))
        __asm__ volatile("yield" ::: "memory");
    }
  }
  ~LaunchGuard() noexcept { release(); }
  void release() noexcept {
    if (!held_) return;
    __atomic_store_n(&launch_lock, 0u, __ATOMIC_RELEASE);
    held_ = false;
  }
  LaunchGuard(const LaunchGuard &) = delete;
  LaunchGuard &operator=(const LaunchGuard &) = delete;

 private:
  bool held_ = true;
};

} // namespace lang::native::darwin

#endif

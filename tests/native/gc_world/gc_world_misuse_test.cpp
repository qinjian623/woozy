// Misuse cases for lang-gc-world/1. Each mode runs in a fresh process and is
// expected to Panic with LANG_NATIVE_PANIC:v1:gc-world:<reason> and exit 101.
// run_all.sh checks both the status and the reason for every mode.

#include "gc_world.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <unistd.h>

namespace {

void sleep_ms(int milliseconds) {
  std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

bool wait_for(const std::atomic<bool> &flag, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; ++waited) {
    if (flag.load(std::memory_order_acquire))
      return true;
    sleep_ms(1);
  }
  return flag.load(std::memory_order_acquire);
}

[[noreturn]] void unexpected_return(const char *mode) {
  std::fprintf(stderr, "MISUSE_MODE_DID_NOT_PANIC: %s\n", mode);
  std::_Exit(3);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <mode>\n", argv[0]);
    return 2;
  }
  const std::string mode = argv[1];

  if (mode == "poll-unregistered-pending") {
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<bool> stopped{false};
    std::thread collector([&] {
      if (!lang_gc_world_register_v1())
        std::_Exit(4);
      ready.store(true, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        sleep_ms(1);
      if (lang_gc_world_stop_v1(2000000000LL) != 0)
        std::_Exit(5);
      stopped.store(true, std::memory_order_release);
      // The main thread is expected to panic while this stop is owned.
      sleep_ms(1000);
      lang_gc_world_resume_v1();
      if (!lang_gc_world_unregister_v1())
        std::_Exit(6);
    });
    if (!wait_for(ready, 5000)) {
      go.store(true, std::memory_order_release);
      collector.join();
      return 7;
    }
    go.store(true, std::memory_order_release);
    if (!wait_for(stopped, 5000)) {
      collector.join();
      return 7;
    }
    lang_gc_world_poll_v1();
    collector.join();
    unexpected_return(argv[1]);
  }

  if (mode == "enter-safe-unregistered") {
    lang_gc_world_enter_safe_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "leave-safe-unregistered") {
    lang_gc_world_leave_safe_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "leave-without-enter") {
    if (!lang_gc_world_register_v1())
      std::_Exit(8);
    lang_gc_world_leave_safe_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "stop-unregistered") {
    (void)lang_gc_world_stop_v1(0);
    unexpected_return(argv[1]);
  }
  if (mode == "stop-in-safe") {
    if (!lang_gc_world_register_v1())
      std::_Exit(9);
    lang_gc_world_enter_safe_v1();
    (void)lang_gc_world_stop_v1(0);
    unexpected_return(argv[1]);
  }
  if (mode == "resume-unregistered") {
    lang_gc_world_resume_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "resume-not-owner") {
    if (!lang_gc_world_register_v1())
      std::_Exit(10);
    lang_gc_world_resume_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "unregister-in-safe") {
    if (!lang_gc_world_register_v1())
      std::_Exit(11);
    lang_gc_world_enter_safe_v1();
    (void)lang_gc_world_unregister_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "unregister-collector") {
    if (!lang_gc_world_register_v1())
      std::_Exit(12);
    if (lang_gc_world_stop_v1(0) != 0)
      std::_Exit(13);
    (void)lang_gc_world_unregister_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "critical-in-safe") {
    if (!lang_gc_world_register_v1())
      std::_Exit(17);
    lang_gc_world_enter_safe_v1();
    lang_gc_world_enter_critical_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "safe-in-critical") {
    if (!lang_gc_world_register_v1())
      std::_Exit(18);
    lang_gc_world_enter_critical_v1();
    lang_gc_world_enter_safe_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "enter-critical-unregistered") {
    lang_gc_world_enter_critical_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "leave-critical-unregistered") {
    lang_gc_world_leave_critical_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "unregister-in-critical") {
    if (!lang_gc_world_register_v1())
      std::_Exit(19);
    lang_gc_world_enter_critical_v1();
    (void)lang_gc_world_unregister_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "leave-critical-without-enter") {
    if (!lang_gc_world_register_v1())
      std::_Exit(20);
    lang_gc_world_leave_critical_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "fork-unregistered") {
    lang_gc_world_fork_prepare_v1();
    unexpected_return(argv[1]);
  }
  if (mode == "fork-active-stop") {
    std::atomic<bool> worker_ready{false};
    std::atomic<bool> try_fork{false};
    std::thread worker([&] {
      if (!lang_gc_world_register_v1())
        std::_Exit(14);
      lang_gc_world_enter_safe_v1();
      worker_ready.store(true, std::memory_order_release);
      while (!try_fork.load(std::memory_order_acquire))
        sleep_ms(1);
      lang_gc_world_fork_prepare_v1();
      unexpected_return("fork-active-stop-worker");
    });

    if (!lang_gc_world_register_v1())
      std::_Exit(15);
    while (!worker_ready.load(std::memory_order_acquire))
      sleep_ms(1);
    if (lang_gc_world_stop_v1(2000000000LL) != 0)
      std::_Exit(16);
    try_fork.store(true, std::memory_order_release);
    // The worker must Panic while trying to fork as a non-collector during an
    // active stop. If it returns instead, it exits the process with status 3.
    worker.join();
    lang_gc_world_resume_v1();
    (void)lang_gc_world_unregister_v1();
    unexpected_return(argv[1]);
  }

  std::fprintf(stderr, "UNKNOWN_MISUSE_MODE: %s\n", argv[1]);
  return 2;
}

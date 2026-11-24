#include "../../runtime/native/alloc.hpp"

#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <string_view>

#if defined(__x86_64__) && defined(__linux__)
#include <cstdlib>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr uint64_t kBoundarySizes[] = {
    0,     1,     7,     8,     15,    16,    17,    31,    32,    33,
    47,    48,    49,    63,    64,    65,    79,    80,    81,    95,
    96,    97,    127,   128,   129,   191,   192,   193,   255,   256,
    257,   383,   384,   385,   511,   512,   513,   767,   768,   769,
    1023,  1024,  1025,  1535,  1536,  1537,  2047,  2048,  2049,  3071,
    3072,  3073,  4080,  4081,  4096,  4097,  8192,  16368, 16369,
    16384, 32752, 32753, 65535, 98304, 131071, 131072};

constexpr uint64_t kAlignments[] = {1, 2, 4, 8, 16, 32, 64, 128,
                                    256, 1024, 4096, 65536, 1u << 20};

static inline bool aligned(const void *pointer, uint64_t align) {
  return (reinterpret_cast<uintptr_t>(pointer) & (align - 1)) == 0;
}

static void fill(void *pointer, uint64_t size, uint8_t seed) {
  auto *bytes = static_cast<uint8_t *>(pointer);
  for (uint64_t index = 0; index < size; ++index)
    bytes[index] = static_cast<uint8_t>(seed + index * 37u + (index >> 3));
}

static bool matches(const void *pointer, uint64_t size, uint8_t seed) {
  auto *bytes = static_cast<const uint8_t *>(pointer);
  for (uint64_t index = 0; index < size; ++index)
    if (bytes[index] != static_cast<uint8_t>(seed + index * 37u + (index >> 3)))
      return false;
  return true;
}

static int test_invalid_requests() {
  if (lang_alloc_v1(8, 0) != nullptr)
    return 10;
  if (lang_alloc_v1(8, 3) != nullptr)
    return 11;
  if (lang_alloc_v1(8, 0x20000000ULL) != nullptr)
    return 12;
  if (lang_alloc_v1(UINT64_MAX, 8) != nullptr)
    return 13;
  if (lang_alloc_v1(UINT64_MAX - 8, 8) != nullptr)
    return 14;
  if (lang_alloc_v1(UINT64_MAX - 32, 16) != nullptr)
    return 15;
  lang_free_v1(nullptr);
  return 0;
}

static int test_boundaries() {
  uint8_t seed = 1;
  for (uint64_t size : kBoundarySizes) {
    for (uint64_t align : kAlignments) {
      void *pointer = lang_alloc_v1(size, align);
      if (!pointer)
        return 20;
      if (!aligned(pointer, align))
        return 21;
      fill(pointer, size, seed);
      if (!matches(pointer, size, seed))
        return 22;
      lang_free_v1(pointer);
      ++seed;
    }
  }
  return 0;
}

struct Block {
  void *pointer;
  uint64_t size;
  uint8_t seed;
};

static int test_many_live_and_out_of_order() {
  constexpr uint32_t kCount = 256;
  Block blocks[kCount];
  for (uint32_t index = 0; index < kCount; ++index) {
    blocks[index].size = 1 + (index * 73u) % 2500u;
    blocks[index].seed = static_cast<uint8_t>(index * 11u + 3u);
    blocks[index].pointer = lang_alloc_v1(blocks[index].size, 16);
    if (!blocks[index].pointer)
      return 30;
    fill(blocks[index].pointer, blocks[index].size, blocks[index].seed);
  }

  // Interleave churn so a reuse bug that overwrites a live slot is visible.
  for (uint32_t index = 0; index < 512; ++index) {
    void *churn = lang_alloc_v1(48, 16);
    if (!churn)
      return 31;
    fill(churn, 48, static_cast<uint8_t>(index));
    if (!matches(churn, 48, static_cast<uint8_t>(index)))
      return 32;
    lang_free_v1(churn);
  }
  for (uint32_t index = 0; index < kCount; ++index)
    if (!matches(blocks[index].pointer, blocks[index].size, blocks[index].seed))
      return 33;

  // Free every other block, then reuse those slots while checking survivors.
  for (uint32_t index = 0; index < kCount; index += 2) {
    lang_free_v1(blocks[index].pointer);
    blocks[index].pointer = nullptr;
  }
  Block reused[128];
  for (uint32_t index = 0; index < 128; ++index) {
    reused[index].size = 1 + (index * 31u) % 1800u;
    reused[index].seed = static_cast<uint8_t>(index * 5u + 9u);
    reused[index].pointer = lang_alloc_v1(reused[index].size, 8);
    if (!reused[index].pointer)
      return 34;
    fill(reused[index].pointer, reused[index].size, reused[index].seed);
  }
  for (uint32_t index = 0; index < kCount; ++index)
    if (blocks[index].pointer &&
        !matches(blocks[index].pointer, blocks[index].size, blocks[index].seed))
      return 35;
  for (uint32_t index = 0; index < 128; ++index)
    if (!matches(reused[index].pointer, reused[index].size, reused[index].seed))
      return 36;
  for (uint32_t index = 1; index < kCount; index += 2)
    lang_free_v1(blocks[index].pointer);
  for (uint32_t index = 0; index < 128; ++index)
    lang_free_v1(reused[index].pointer);
  return 0;
}

static int test_large_small_interleave() {
  void *large = lang_alloc_v1(8192, 16);
  if (!large)
    return 40;
  fill(large, 8192, 0x5a);
  void *small = lang_alloc_v1(32, 16);
  if (!small)
    return 41;
  fill(small, 32, 0x77);
  if (!matches(large, 8192, 0x5a))
    return 42;
  lang_free_v1(small);
  small = lang_alloc_v1(64, 16);
  if (!small)
    return 43;
  fill(small, 64, 0x33);
  if (!matches(large, 8192, 0x5a) || !matches(small, 64, 0x33))
    return 44;
  lang_free_v1(small);
  lang_free_v1(large);
  return 0;
}

#if defined(__x86_64__) && defined(__linux__)
static bool deny_mmap() {
  struct sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, arch)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               offsetof(struct seccomp_data, nr)),
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_mmap, 0, 1),
      BPF_STMT(BPF_RET | BPF_K,
               SECCOMP_RET_ERRNO | (static_cast<uint32_t>(ENOMEM) &
                                    SECCOMP_RET_DATA)),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog program = {
      static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
    return false;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

static int test_reuse_without_mmap() {
  // Create several full pages in one class, then release them.  After the
  // filter is installed, only the per-class budget of empty pages may be
  // reused; if all historical peak pages were retained, enough slots for the
  // whole peak would remain available and the later loop would not fail.
  // 48-byte requests use 64-byte slots, so 20,000 live allocations span about
  // 20 pages and exceed the 16-page (1 MiB) per-class budget.
  constexpr uint32_t kPeakCount = 20000;
  void *peak[kPeakCount];
  for (uint32_t index = 0; index < kPeakCount; ++index) {
    peak[index] = lang_alloc_v1(48, 16);
    if (!peak[index])
      return 56;
  }
  for (uint32_t index = 0; index < kPeakCount; ++index)
    lang_free_v1(peak[index]);

  constexpr uint32_t kCount = 6;
  constexpr uint64_t kSizes[kCount] = {1, 48, 80, 200, 1000, 3000};
  void *warm[kCount];
  for (uint32_t index = 0; index < kCount; ++index) {
    warm[index] = lang_alloc_v1(kSizes[index], 16);
    if (!warm[index])
      return 50;
    fill(warm[index], kSizes[index], static_cast<uint8_t>(index + 1));
  }
  for (uint32_t index = 0; index < kCount; ++index)
    lang_free_v1(warm[index]);

  // Warm several distinct exact-size mappings within the byte budget.
  constexpr uint64_t kDirectSizes[] = {256 * 1024, 512 * 1024, 768 * 1024,
                                       1024 * 1024};
  constexpr uint32_t kDirectCount =
      sizeof(kDirectSizes) / sizeof(kDirectSizes[0]);
  for (uint64_t size : kDirectSizes) {
    void *pointer = lang_alloc_v1(size, 16);
    if (!pointer)
      return 49;
    fill(pointer, size, 0x5a);
    lang_free_v1(pointer);
  }

  if (!deny_mmap())
    return 77;

  // Each size must be available without mmap.
  for (uint32_t index = 0; index < kDirectCount; ++index) {
    const uint64_t size = kDirectSizes[index];
    void *pointer = lang_alloc_v1(size, 16);
    if (!pointer)
      return 59;
    fill(pointer, size, static_cast<uint8_t>(0x61 + index));
    if (!matches(pointer, size, static_cast<uint8_t>(0x61 + index)))
      return 60;
    lang_free_v1(pointer);
  }

  // The old allocator has no free storage here and calls mmap for every one
  // of these requests; the reuse allocator must satisfy them from warm pages.
  for (uint32_t index = 0; index < kCount; ++index) {
    void *pointer = lang_alloc_v1(kSizes[index], 16);
    if (!pointer)
      return 51;
    fill(pointer, kSizes[index], static_cast<uint8_t>(index + 21));
    if (!matches(pointer, kSizes[index], static_cast<uint8_t>(index + 21)))
      return 52;
    lang_free_v1(pointer);
  }

  // One slot in this class was previously used. With that slot live again,
  // a second allocation must come from the same page without another mmap.
  constexpr uint64_t kPartiallyUsedSize = 3000;
  void *first = lang_alloc_v1(kPartiallyUsedSize, 16);
  void *second = lang_alloc_v1(kPartiallyUsedSize, 16);
  if (!first || !second || first == second)
    return 61;
  fill(first, kPartiallyUsedSize, 0x30);
  fill(second, kPartiallyUsedSize, 0x31);
  if (!matches(first, kPartiallyUsedSize, 0x30) ||
      !matches(second, kPartiallyUsedSize, 0x31))
    return 62;
  lang_free_v1(second);
  lang_free_v1(first);

  for (uint32_t index = 0; index < 16; ++index) {
    void *pointer = lang_alloc_v1(48, 16);
    if (!pointer)
      return 53;
    fill(pointer, 48, static_cast<uint8_t>(index));
    if (!matches(pointer, 48, static_cast<uint8_t>(index)))
      return 54;
    lang_free_v1(pointer);
  }

  // A 64 KiB page holds 1,023 64-byte slots, so the 16-page budget covers at
  // most 16,368 live allocations.  The loop must still fail before 1 MiB / 64
  // + 1 = 16,385 requests while reusing at least half of the budget.
  constexpr uint32_t kSlotSize = 64;
  constexpr uint32_t kBound = (1024 * 1024) / kSlotSize + 1;
  constexpr uint32_t kMinimumLive = ((1024 * 1024) / kSlotSize) / 2;
  void *bounded[kBound];
  uint32_t live = 0;
  for (; live < kBound; ++live) {
    bounded[live] = lang_alloc_v1(48, 16);
    if (!bounded[live])
      break;
  }
  if (live == kBound)
    return 57;
  if (live < kMinimumLive)
    return 58;
  for (uint32_t index = 0; index < live; ++index)
    lang_free_v1(bounded[index]);

  // Neither the pool nor the direct cache has storage for this cold request.
  if (lang_alloc_v1(8192, 16) != nullptr)
    return 55;
  return 0;
}

static int test_direct_phase_change() {
  // Startup mappings fill the 8 MiB direct cache. Leave their payloads just
  // below the target map sizes so they round to those exact page multiples.
  constexpr uint64_t warm_sizes[] = {
      1 * 1024 * 1024 - 64, 1536 * 1024 - 64,
      2560 * 1024 - 64, 3 * 1024 * 1024 - 64};
  for (uint64_t size : warm_sizes) {
    void *warm = lang_alloc_v1(size, 16);
    if (!warm)
      return 90;
    lang_free_v1(warm);
  }

  // The steady-state batch has smaller buffers at nonmatching old sizes,
  // including several of one size simultaneously.
  constexpr uint64_t hot_sizes[] = {32752, 49187, 84007, 84007, 84007, 121483};
  constexpr size_t count = sizeof(hot_sizes) / sizeof(hot_sizes[0]);
  void *hot[count];
  for (size_t index = 0; index < count; ++index) {
    hot[index] = lang_alloc_v1(hot_sizes[index], 16);
    if (!hot[index])
      return 91;
  }
  for (void *pointer : hot)
    lang_free_v1(pointer);

  if (!deny_mmap())
    return 77;
  for (size_t index = 0; index < count; ++index) {
    hot[index] = lang_alloc_v1(hot_sizes[index], 16);
    if (!hot[index] || !aligned(hot[index], 16))
      return 92;
    fill(hot[index], hot_sizes[index], static_cast<uint8_t>(index + 17));
  }
  for (size_t index = 0; index < count; ++index)
    if (!matches(hot[index], hot_sizes[index], static_cast<uint8_t>(index + 17)))
      return 93;

  // This batch fits into the oldest startup mapping. The newer mappings
  // should still be reusable while the oldest is no longer available.
  void *newer = lang_alloc_v1(warm_sizes[1], 16);
  if (!newer)
    return 94;
  if (lang_alloc_v1(warm_sizes[0], 16))
    return 95;
  lang_free_v1(newer);
  for (void *pointer : hot)
    lang_free_v1(pointer);
  return 0;
}

static int wait_for_test_child(pid_t child) {
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, 0);
    if (waited == child)
      break;
    if (waited < 0 && errno == EINTR)
      continue;
    return 71;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 72;
}

// Prime four distinct exact-size direct mappings and limit the address space.
// Even the smallest retained region prevents the 1 MiB request from fitting
// unless every slot is released.
static int test_pressure_child(uint64_t size, uint64_t slack) {
  char statm[128]{};
  const int fd = open("/proc/self/statm", O_RDONLY);
  if (fd < 0)
    return 80;
  const ssize_t count = read(fd, statm, sizeof(statm) - 1);
  close(fd);
  if (count <= 0)
    return 81;
  const long page_size = sysconf(_SC_PAGESIZE);
  const auto pages = std::strtoull(statm, nullptr, 10);
  if (page_size <= 0 || pages == 0)
    return 82;
  const auto before = static_cast<rlim_t>(pages) * page_size;

  constexpr uint64_t kWarmSizes[] = {320 * 1024, 448 * 1024, 512 * 1024, 768 * 1024};
  for (uint64_t warm_size : kWarmSizes) {
    void *warm = lang_alloc_v1(warm_size, 16);
    if (!warm)
      return 83;
    *static_cast<volatile unsigned char *>(warm) = 7;
    lang_free_v1(warm);
  }

  rlimit limit{};
  if (getrlimit(RLIMIT_AS, &limit) != 0)
    return 84;
  limit.rlim_cur = before + slack;
  if (setrlimit(RLIMIT_AS, &limit) != 0)
    return 85;

  void *pointer = lang_alloc_v1(size, 16);
  if (!pointer)
    return size < 65536 ? 86 : 87;
  *static_cast<volatile unsigned char *>(pointer) = 8;
  lang_free_v1(pointer);
  return 0;
}

static int run_pressure_isolated(uint64_t size, uint64_t slack) {
  const pid_t child = fork();
  if (child < 0)
    return 70;
  if (child == 0)
    _exit(test_pressure_child(size, slack));
  return wait_for_test_child(child);
}

static int run_reuse_without_mmap_isolated(int (*test)()) {
  // Seccomp must not be installed in the process that owns the sanitizer
  // runtime: returning from main would ask ASan to mmap its cleanup guard
  // while mmap is denied.  The child installs the filter and leaves with
  // _exit, so sanitizer cleanup in the unfiltered parent is unaffected.
  const pid_t child = fork();
  if (child < 0)
    return 70;
  if (child == 0)
    _exit(test());

  return wait_for_test_child(child);
}
#endif

} // namespace

int main(int argc, char **argv) {
#if defined(__x86_64__) && defined(__linux__)
  if (argc > 1 && std::string_view(argv[1]) == "reuse-deny-mmap")
    return run_reuse_without_mmap_isolated(test_reuse_without_mmap);
  if (argc > 1 && std::string_view(argv[1]) == "direct-phase-change")
    return run_reuse_without_mmap_isolated(test_direct_phase_change);
  // Keep these first: the 4000-byte class must be cold in its isolated child.
  // None of the warmed sizes equals the target. Releasing only the first two
  // slots still leaves too much address space occupied for the 1 MiB request.
  int pressure = run_pressure_isolated(1024 * 1024, 1280 * 1024);
  if (!pressure)
    pressure = run_pressure_isolated(4000, 80 * 1024);
  if (!pressure)
    pressure = run_pressure_isolated(262144, 320 * 1024);
  if (pressure)
    return pressure;
#else
  (void)argv;
  if (argc > 1)
    return 77;
#endif
  int result = test_invalid_requests();
  if (!result)
    result = test_boundaries();
  if (!result)
    result = test_many_live_and_out_of_order();
  if (!result)
    result = test_large_small_interleave();
  return result;
}

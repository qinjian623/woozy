// Host-only concurrency harness for the native allocator.  std::thread and
// std::barrier are used only to create and stop test threads; the allocator
// under test (runtime/native/alloc.cpp) stays free of libc and C++ runtime
// dependencies.
//
// The harness intentionally exercises the shared pool and direct-cache state:
//   * several size classes and alignments allocated and freed at once,
//   * zero-byte payloads and mmap-backed large/over-aligned requests,
//   * blocks handed to another thread that verifies the exact byte pattern
//     before freeing them,
//   * a duplicate-live-address check at peak liveness every round,
//   * new direct sizes and repeated copies displacing the startup mappings,
//   * an eviction phase that exceeds the per-class empty-page budget so pages
//     are unmapped while other threads are still working,
//   * a final reuse pass that must return addresses observed earlier.

#include "../../runtime/native/alloc.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kMaxThreads = 16;
constexpr uint32_t kMaxLivePerThread = 2048;
constexpr uint32_t kRounds = 150;
constexpr uint32_t kSmallPerRound = 6;
constexpr uint32_t kLargePerRound = 3;
constexpr uint32_t kTransferEvery = 10;
constexpr uint32_t kEvictPerThread = 64;
constexpr uint64_t kEvictSize = 4000;
constexpr uint64_t kEvictAlign = 16;

constexpr uint64_t kSmallSizes[] = {
    0,    1,    7,    8,    15,   16,   17,   31,   32,   33,   47,
    48,   63,   64,   65,   95,   96,   127,  128,  191,  192,  255,
    256,  383,  384,  511,  512,  1023, 1024, 1535, 1536, 2047, 2048,
    3071, 3072, 4000, 4080};
constexpr uint32_t kSmallSizeCount =
    sizeof(kSmallSizes) / sizeof(kSmallSizes[0]);

constexpr uint64_t kSmallAligns[] = {1, 2, 4, 8, 16};
constexpr uint32_t kSmallAlignCount =
    sizeof(kSmallAligns) / sizeof(kSmallAligns[0]);

constexpr uint64_t kLargeSizes[] = {0,     4081,  4096, 4097,
                                    8192,  16384, 65536, 100000, 170000};
constexpr uint32_t kLargeSizeCount =
    sizeof(kLargeSizes) / sizeof(kLargeSizes[0]);

constexpr uint64_t kLargeAligns[] = {16, 32, 64, 128, 256, 4096, 65536};
constexpr uint32_t kLargeAlignCount =
    sizeof(kLargeAligns) / sizeof(kLargeAligns[0]);

struct Block {
  void *pointer;
  uint64_t size;
  uint64_t align;
  uint8_t seed;
  uint32_t owner_tid;
};

uintptr_t g_snapshot_ptrs[kMaxThreads][kMaxLivePerThread];
uint32_t g_snapshot_count[kMaxThreads];
std::vector<uintptr_t> g_ever[kMaxThreads];
int g_results[kMaxThreads];

// One queue per target thread; the producer pushes to its successor's queue.
// This guarantees every transferred block is freed by a different thread.
std::vector<Block> g_transfer_queues[kMaxThreads];
std::mutex g_transfer_mutex;
std::atomic<uint64_t> g_cross_frees{0};

static inline bool aligned(const void *pointer, uint64_t align) {
  return (reinterpret_cast<uintptr_t>(pointer) & (align - 1)) == 0;
}

static inline uint8_t pattern_byte(uint8_t seed, uint64_t index) {
  return static_cast<uint8_t>(seed + index * 37u + (index >> 3));
}

static void fill(void *pointer, uint64_t size, uint8_t seed) {
  auto *bytes = static_cast<uint8_t *>(pointer);
  for (uint64_t index = 0; index < size; ++index)
    bytes[index] = pattern_byte(seed, index);
}

static bool matches(const void *pointer, uint64_t size, uint8_t seed) {
  auto *bytes = static_cast<const uint8_t *>(pointer);
  for (uint64_t index = 0; index < size; ++index)
    if (bytes[index] != pattern_byte(seed, index))
      return false;
  return true;
}

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t value = a * 2654435761u ^ b * 2246822519u ^ c * 3266489917u;
  value ^= value >> 16;
  return value;
}

static bool snapshot_has_duplicates(uint32_t thread_count) {
  std::unordered_set<uintptr_t> live;
  uint64_t total = 0;
  for (uint32_t thread = 0; thread < thread_count; ++thread) {
    for (uint32_t index = 0; index < g_snapshot_count[thread]; ++index) {
      ++total;
      live.insert(g_snapshot_ptrs[thread][index]);
    }
  }
  return live.size() != total;
}

// Publish this thread's live pointers, wait for every other thread, then check
// that the merged set has no duplicate addresses.  All callers arrive at the
// barrier even when they have nothing to publish.
static int snapshot_check(uint32_t tid, uint32_t thread_count,
                          const std::vector<Block> &blocks,
                          std::barrier<> &barrier) {
  const bool overflow = blocks.size() > kMaxLivePerThread;
  const uint32_t count = overflow ? kMaxLivePerThread
                                  : static_cast<uint32_t>(blocks.size());
  g_snapshot_count[tid] = count;
  for (uint32_t index = 0; index < count; ++index)
    g_snapshot_ptrs[tid][index] =
        reinterpret_cast<uintptr_t>(blocks[index].pointer);
  barrier.arrive_and_wait();
  if (overflow)
    return 120;
  return snapshot_has_duplicates(thread_count) ? 121 : 0;
}

static int verify_block(const Block &block) {
  if (!block.pointer)
    return 130;
  if (!aligned(block.pointer, block.align))
    return 131;
  if (block.size != 0 && !matches(block.pointer, block.size, block.seed))
    return 132;
  return 0;
}

static void record_block(std::vector<Block> &blocks,
                         std::vector<uintptr_t> &ever, uint32_t tid,
                         uint32_t round, uint32_t index, uint64_t size,
                         uint64_t align, int &result) {
  void *pointer = lang_alloc_v1(size, align);
  if (!pointer) {
    if (!result)
      result = 100;
    return;
  }
  ever.push_back(reinterpret_cast<uintptr_t>(pointer));
  Block block{pointer, size, align,
              static_cast<uint8_t>(mix(tid, round, index) ^ 0x5Au), tid};
  if (!result && !aligned(pointer, align))
    result = 101;
  if (size != 0) {
    fill(pointer, size, block.seed);
    if (!result && !matches(pointer, size, block.seed))
      result = 102;
  }
  blocks.push_back(block);
}

static int run_worker(uint32_t tid, uint32_t thread_count,
                      std::barrier<> &barrier) {
  int result = 0;
  std::vector<Block> mine;
  std::vector<uintptr_t> ever;
  mine.reserve(kSmallPerRound + kLargePerRound);
  ever.reserve(kRounds * (kSmallPerRound + kLargePerRound));

  for (uint32_t round = 0; round < kRounds; ++round) {
    mine.clear();

    // Allocate a mixed batch.  Every one of these blocks is live when the
    // snapshot below is taken, so duplicate addresses are caught at peak
    // liveness rather than after the owner has already released them.
    for (uint32_t index = 0; index < kSmallPerRound; ++index) {
      const uint32_t pick = mix(tid, round, index);
      record_block(mine, ever, tid, round, index,
                   kSmallSizes[pick % kSmallSizeCount],
                   kSmallAligns[(pick >> 8) % kSmallAlignCount], result);
    }
    for (uint32_t index = 0; index < kLargePerRound; ++index) {
      const uint32_t pick = mix(tid + 1, round + 7, index + 13);
      if (index == 0) {
        // Every thread needs this working size at the same time. Rotate
        // between the former gap and a larger mapping as workloads move.
        const uint64_t size = round % 2 ? 71093 : 192000;
        record_block(mine, ever, tid, round, kSmallPerRound + index,
                     size, 16, result);
      } else {
        record_block(mine, ever, tid, round, kSmallPerRound + index,
                     kLargeSizes[pick % kLargeSizeCount],
                     kLargeAligns[(pick >> 8) % kLargeAlignCount], result);
      }
    }

    // Checkpoint A: all allocations of this round are live here.
    const int snapshot_result =
        snapshot_check(tid, thread_count, mine, barrier);
    if (!result && snapshot_result)
      result = snapshot_result;

    // Free half of the batch concurrently with the other threads.
    const uint32_t immediate = static_cast<uint32_t>(mine.size() / 2);
    for (uint32_t index = 0; index < immediate; ++index) {
      if (!result) {
        const int check = verify_block(mine[index]);
        if (check)
          result = check;
      }
      lang_free_v1(mine[index].pointer);
    }
    mine.erase(mine.begin(), mine.begin() + immediate);

    // Hand one block to the next thread.  Producers publish before the
    // barrier; after it, every consumer drains its own queue.  The block was
    // already covered by checkpoint A, so no new liveness window opens.
    if (round % kTransferEvery == 0) {
      if (!mine.empty()) {
        Block transferred = mine.back();
        mine.pop_back();
        const uint32_t target = (tid + 1) % thread_count;
        {
          std::lock_guard<std::mutex> lock(g_transfer_mutex);
          g_transfer_queues[target].push_back(transferred);
        }
      }
      barrier.arrive_and_wait();

      std::vector<Block> received;
      {
        std::lock_guard<std::mutex> lock(g_transfer_mutex);
        received.swap(g_transfer_queues[tid]);
      }
      for (const Block &block : received) {
        if (!result) {
          const int check = verify_block(block);
          if (check)
            result = check;
        }
        if (block.owner_tid != tid)
          g_cross_frees.fetch_add(1, std::memory_order_relaxed);
        lang_free_v1(block.pointer);
      }
    }

    // Free the rest, then wait for every thread so the next round starts with
    // no blocks left over from this one.
    for (const Block &block : mine) {
      if (!result) {
        const int check = verify_block(block);
        if (check)
          result = check;
      }
      lang_free_v1(block.pointer);
    }
    mine.clear();
    barrier.arrive_and_wait();
  }

  // Eviction phase: 15 slots per 64 KiB page for the 4096-byte class, so the
  // whole cohort spans more than the 16-page (1 MiB) empty-page budget.  When
  // pages drain, some are unmapped while other threads still allocate/free.
  {
    std::vector<Block> evicted;
    evicted.reserve(kEvictPerThread);
    for (uint32_t index = 0; index < kEvictPerThread; ++index)
      record_block(evicted, ever, tid, kRounds, index, kEvictSize,
                   kEvictAlign, result);

    const int snapshot_result =
        snapshot_check(tid, thread_count, evicted, barrier);
    if (!result && snapshot_result)
      result = snapshot_result;

    for (const Block &block : evicted) {
      if (!result) {
        const int check = verify_block(block);
        if (check)
          result = check;
      }
      lang_free_v1(block.pointer);
    }
    evicted.clear();
    // Make sure the whole cohort is released before reallocating, otherwise
    // the empty-page budget would not be pressed against its limit.
    barrier.arrive_and_wait();

    for (uint32_t index = 0; index < kEvictPerThread; ++index)
      record_block(evicted, ever, tid, kRounds + 1, index, kEvictSize,
                   kEvictAlign, result);

    const int reuse_result =
        snapshot_check(tid, thread_count, evicted, barrier);
    if (!result && reuse_result)
      result = reuse_result;

    for (const Block &block : evicted) {
      if (!result) {
        const int check = verify_block(block);
        if (check)
          result = check;
      }
      lang_free_v1(block.pointer);
    }
    barrier.arrive_and_wait();
  }

  g_ever[tid] = std::move(ever);
  return result;
}

static int final_reuse_check(const std::unordered_set<uintptr_t> &ever) {
  // After the stress phases, retained empty pages must serve the next batch
  // from addresses seen earlier; at least one exact prior address is required.
  constexpr uint32_t kSmallCount = 1200;
  constexpr uint32_t kEvictCount = 320;
  std::vector<void *> small;
  std::vector<uintptr_t> seen;
  small.reserve(kSmallCount);
  seen.reserve(kSmallCount + kEvictCount);
  uint64_t hits = 0;

  for (uint32_t index = 0; index < kSmallCount; ++index) {
    void *pointer = lang_alloc_v1(48, 16);
    if (!pointer)
      return 140;
    if (!aligned(pointer, 16))
      return 141;
    fill(pointer, 48, static_cast<uint8_t>(index));
    if (!matches(pointer, 48, static_cast<uint8_t>(index)))
      return 142;
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (ever.find(address) != ever.end())
      ++hits;
    seen.push_back(address);
    small.push_back(pointer);
  }

  std::unordered_set<uintptr_t> batch(seen.begin(), seen.end());
  if (batch.size() != seen.size())
    return 143;
  if (hits == 0)
    return 144;

  for (void *pointer : small)
    lang_free_v1(pointer);
  small.clear();
  seen.clear();

  // Repeat for the class used by the eviction phase.
  uint64_t evict_hits = 0;
  for (uint32_t index = 0; index < kEvictCount; ++index) {
    void *pointer = lang_alloc_v1(kEvictSize, kEvictAlign);
    if (!pointer)
      return 145;
    if (!aligned(pointer, kEvictAlign))
      return 146;
    fill(pointer, kEvictSize, static_cast<uint8_t>(index));
    if (!matches(pointer, kEvictSize, static_cast<uint8_t>(index)))
      return 147;
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (ever.find(address) != ever.end())
      ++evict_hits;
    seen.push_back(address);
    small.push_back(pointer);
  }
  std::unordered_set<uintptr_t> evict_batch(seen.begin(), seen.end());
  if (evict_batch.size() != seen.size())
    return 148;
  if (evict_hits == 0)
    return 149;

  for (void *pointer : small)
    lang_free_v1(pointer);

  // A direct mmap-backed allocation must still work after all the pool churn.
  void *large = lang_alloc_v1(65536, 4096);
  if (!large || !aligned(large, 4096))
    return 150;
  fill(large, 65536, 0x9c);
  if (!matches(large, 65536, 0x9c))
    return 151;
  lang_free_v1(large);
  return 0;
}

} // namespace

int main() {
  for (uint64_t size : {1024u, 1536u, 2560u, 3072u}) {
    void *startup = lang_alloc_v1(size * 1024 - 64, 16);
    if (!startup)
      return 161;
    lang_free_v1(startup);
  }

  unsigned hardware = std::thread::hardware_concurrency();
  uint32_t thread_count = hardware ? hardware : 4;
  if (thread_count < 4)
    thread_count = 4;
  if (thread_count > 8)
    thread_count = 8;
  if (thread_count > kMaxThreads)
    thread_count = kMaxThreads;

  std::barrier<> barrier(static_cast<std::ptrdiff_t>(thread_count));
  std::vector<std::thread> threads;
  threads.reserve(thread_count - 1);
  for (uint32_t tid = 1; tid < thread_count; ++tid)
    threads.emplace_back([tid, thread_count, &barrier] {
      g_results[tid] = run_worker(tid, thread_count, barrier);
    });
  g_results[0] = run_worker(0, thread_count, barrier);
  for (std::thread &thread : threads)
    thread.join();

  for (uint32_t tid = 0; tid < thread_count; ++tid)
    if (g_results[tid])
      return g_results[tid];

  std::unordered_set<uintptr_t> ever;
  for (uint32_t tid = 0; tid < thread_count; ++tid)
    ever.insert(g_ever[tid].begin(), g_ever[tid].end());

  if (g_cross_frees.load(std::memory_order_relaxed) == 0)
    return 160;
  return final_reuse_check(ever);
}

// No-libc static ELF smoke test for the automatic GC controller. It links the
// accepted cycle collector, Weak, world registry, real allocator and
// freestanding support, then runs 128 ~1 KiB garbage cycles through the
// controller without libc or pthreads.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"
#include "weak.hpp"

#include <cstdint>

namespace {

constexpr uint64_t kNodeMagic = 0x5354415449434e44ULL;
constexpr uint64_t kExtMagic = 0x5354415445585431ULL;
constexpr long kSysWrite = 1;
constexpr long kSysExitGroup = 231;
constexpr int kEintr = -4;

struct External {
  uint64_t magic;
  uint64_t kind;
};

struct Node {
  uint64_t refs;
  Node *edge;
  External *ext;
  LangWeakControl *weak_slot;
  uint64_t sentinel;
  uint64_t payload[117];
};

uint64_t g_ext_released;
uint64_t g_live_nodes;
uint64_t g_bad_callback;

void write_all(const char *bytes, uint64_t size) noexcept {
  while (size) {
    long result;
    __asm__ volatile("syscall" : "=a"(result)
      : "a"(kSysWrite), "D"(2), "S"(bytes), "d"(size)
      : "rcx", "r11", "memory");
    if (result == kEintr)
      continue;
    if (result <= 0)
      return;
    bytes += result;
    size -= static_cast<uint64_t>(result);
  }
}

[[noreturn]] void static_fail(const char *reason) noexcept {
  write_all("GC_RUNTIME_STATIC_FAIL:", sizeof("GC_RUNTIME_STATIC_FAIL:") - 1);
  uint64_t length = 0;
  while (reason[length])
    ++length;
  write_all(reason, length);
  write_all("\n", 1);
  __asm__ volatile("syscall" : : "a"(kSysExitGroup), "D"(1)
                   : "rcx", "r11", "memory");
  __builtin_unreachable();
}

External *external_new(uint64_t kind) noexcept {
  auto *ext = static_cast<External *>(
      lang_alloc_v1(sizeof(External), alignof(External)));
  if (!ext)
    static_fail("ext-alloc");
  ext->magic = kExtMagic;
  ext->kind = kind;
  return ext;
}

extern "C" void release_external(void *payload) noexcept {
  auto *ext = static_cast<External *>(payload);
  if (ext->magic != kExtMagic)
    static_fail("ext-magic");
  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  if (stats.busy != 0 || stats.enqueue_open != 0 ||
      lang_gc_world_poll_required_v1() != 0)
    ++g_bad_callback;
  ++g_ext_released;
  lang_free_v1(ext);
}

void node_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->edge)
    visitor(node->edge, context);
}

void node_release(void *object) noexcept;

void node_clear(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->ext) {
    External *ext = node->ext;
    node->ext = nullptr;
    if (!lang_gc_runtime_defer_release_v1(ext, release_external))
      release_external(ext);
  }
  Node *edge = node->edge;
  node->edge = nullptr;
  if (edge)
    node_release(edge);
}

void node_retain(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void node_release(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  (void)lang_gc_unregister_v1(node);
  if (node->ext) {
    External *ext = node->ext;
    node->ext = nullptr;
    if (!lang_gc_runtime_defer_release_v1(ext, release_external))
      release_external(ext);
  }
  if (node->weak_slot) {
    lang_weak_detach_v1(node->weak_slot);
    node->weak_slot = nullptr;
  }
  --g_live_nodes;
  lang_free_v1(node);
}

void node_invalidate(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  lang_weak_invalidate_slot_v1(&node->weak_slot);
}

const LangGcOps kOps = {node_trace, node_clear, node_retain, node_release,
                        node_invalidate};

Node *node_new(External *ext) noexcept {
  auto *node = static_cast<Node *>(lang_alloc_v1(sizeof(Node), alignof(Node)));
  if (!node)
    static_fail("node-alloc");
  node->refs = 1;
  node->edge = nullptr;
  node->ext = ext;
  node->weak_slot = nullptr;
  node->sentinel = kNodeMagic;
  node->payload[0] = kNodeMagic;
  node->payload[116] = kNodeMagic;
  ++g_live_nodes;
  return node;
}

void register_node(Node *node) noexcept {
  if (lang_gc_runtime_register_v1(node, &kOps) != LANG_GC_OK)
    static_fail("register");
}

Node *make_garbage(External *ext) noexcept {
  Node *node = node_new(ext);
  node->edge = node;
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
  register_node(node);
  __atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL);
  return node;
}

} // namespace

extern "C" int gc_static_main() noexcept {
  if (!lang_gc_world_register_v1())
    static_fail("world-register");
  if (!lang_gc_world_can_collect_v1())
    static_fail("can-collect");

  Node *root = node_new(nullptr);
  root->edge = root;
  __atomic_add_fetch(&root->refs, 1u, __ATOMIC_RELAXED);
  register_node(root);

  Node *weak_node = node_new(nullptr);
  weak_node->edge = weak_node;
  __atomic_add_fetch(&weak_node->refs, 1u, __ATOMIC_RELAXED);
  register_node(weak_node);
  LangWeakControl *weak =
      lang_weak_downgrade_v1(weak_node, &weak_node->weak_slot);
  if (!weak)
    static_fail("weak-downgrade");
  __atomic_sub_fetch(&weak_node->refs, 1u, __ATOMIC_ACQ_REL);

  constexpr int kGarbage = 128;
  for (int index = 0; index < kGarbage; ++index) {
    make_garbage(external_new(1));
    lang_gc_runtime_poll_v1();
    if (root->sentinel != kNodeMagic || root->edge != root ||
        root->payload[0] != kNodeMagic)
      static_fail("root-corrupt");
  }

  LangGcRuntimeStats stats{};
  lang_gc_runtime_stats_v1(&stats);
  if (stats.collect_ok == 0 || stats.total_registered_scanned == 0)
    static_fail("no-auto-collection");
  if (stats.pressure_limit != 128 && stats.pressure_limit != 256)
    static_fail("pressure-limit");
  if (lang_weak_upgrade_v1(weak) != nullptr)
    static_fail("weak-still-alive");
  lang_weak_release_v1(weak);

  __atomic_sub_fetch(&root->refs, 1u, __ATOMIC_ACQ_REL);
  LangGcStats cleanup{};
  if (lang_gc_runtime_collect_v1(2000000000LL, &cleanup) != 0)
    static_fail("final-collect");
  if (lang_gc_registry_live_v1() != 0)
    static_fail("registry-not-empty");
  if (g_live_nodes != 0)
    static_fail("nodes-not-freed");
  if (g_ext_released != static_cast<uint64_t>(kGarbage))
    static_fail("ext-not-released");
  if (g_bad_callback != 0)
    static_fail("callback-world-state");
  if (!lang_gc_world_unregister_v1())
    static_fail("world-unregister");

  write_all("GC_RUNTIME_STATIC_OK\n", sizeof("GC_RUNTIME_STATIC_OK\n") - 1);
  return 0;
}

extern "C" __attribute__((naked, no_stack_protector, noreturn)) void
_start() noexcept {
  __asm__ volatile(
      "xorl %%ebp, %%ebp\n\t"
      "andq $-16, %%rsp\n\t"
      "call gc_static_main\n\t"
      "movl %%eax, %%edi\n\t"
      "movl $231, %%eax\n\t"
      "syscall\n\t"
      "movl $60, %%eax\n\t"
      "syscall\n\t"
      "ud2\n\t"
      ::: "memory");
}

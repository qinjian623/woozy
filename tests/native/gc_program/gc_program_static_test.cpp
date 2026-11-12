// No-libc static ELF smoke test for lang-gc-program/1. It links the real
// allocator, panic, Weak, GC, world, controller and program layers, registers
// one ~1 KiB self-cycle, and performs the finite normal-main-return final
// collection through lang_gc_program_finish_v1.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_program.hpp"

#include <cstdint>

namespace {

constexpr uint64_t kSentinel = 0x535441545f505247ULL;
constexpr long kSysWrite = 1;
constexpr long kSysExitGroup = 231;
constexpr int kEintr = -4;

struct Node {
  uint64_t refs;
  Node *edge;
  uint64_t sentinel;
  uint64_t payload[123];
};

uint64_t g_live_nodes;

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
  write_all("GC_PROGRAM_STATIC_FAIL:", sizeof("GC_PROGRAM_STATIC_FAIL:") - 1);
  uint64_t length = 0;
  while (reason[length])
    ++length;
  write_all(reason, length);
  write_all("\n", 1);
  __asm__ volatile("syscall" : : "a"(kSysExitGroup), "D"(1)
                   : "rcx", "r11", "memory");
  __builtin_unreachable();
}

void node_retain(Node *node) noexcept {
  if (__atomic_load_n(&node->refs, __ATOMIC_RELAXED) == ~uint64_t{0})
    static_fail("refs-overflow");
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void node_retain_op(void *object) noexcept {
  node_retain(static_cast<Node *>(object));
}

void node_release(Node *node) noexcept {
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  lang_gc_program_unregister_v1(node);
  --g_live_nodes;
  lang_free_v1(node);
}

void node_release_op(void *object) noexcept {
  node_release(static_cast<Node *>(object));
}

void node_trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->edge)
    visitor(node->edge, context);
}

void node_clear(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  Node *edge = node->edge;
  node->edge = nullptr;
  if (edge)
    node_release(edge);
}

void node_invalidate(void *object) noexcept { (void)object; }

const LangGcOps kOps = {node_trace, node_clear, node_retain_op, node_release_op,
                        node_invalidate};

Node *node_new() noexcept {
  auto *node = static_cast<Node *>(lang_alloc_v1(sizeof(Node), alignof(Node)));
  if (!node)
    static_fail("node-alloc");
  node->refs = 1;
  node->edge = nullptr;
  node->sentinel = kSentinel;
  node->payload[0] = kSentinel;
  node->payload[122] = kSentinel;
  ++g_live_nodes;
  return node;
}

} // namespace

// panic.cpp declares this weak test observer. Define it so the static ELF has
// no unresolved weak symbol while no Vec events are generated.
extern "C" void lang_vec_observe_v1(uint32_t, const void *, uint64_t) {}

extern "C" int gc_program_static_main() noexcept {
  if (lang_gc_program_init_v1() != LANG_GC_PROGRAM_OK)
    static_fail("init");
  if (!lang::native::gc_program::enabled())
    static_fail("not-enabled");

  Node *node = node_new();
  node->edge = node;
  node_retain(node);
  if (!lang_gc_program_register_v1(node, &kOps))
    static_fail("register");
  node_release(node); // only the self edge remains
  if (lang_gc_registry_live_v1() != 1)
    static_fail("registry");

  if (lang_gc_program_finish_v1() != LANG_GC_PROGRAM_OK)
    static_fail("finish");
  if (lang_gc_registry_live_v1() != 0)
    static_fail("registry-not-empty");
  if (g_live_nodes != 0)
    static_fail("node-not-freed");
  if (!lang::native::gc_program::enabled())
    static_fail("hooks-disabled-too-early");

  write_all("GC_PROGRAM_STATIC_OK\n", sizeof("GC_PROGRAM_STATIC_OK\n") - 1);
  return 0;
}

extern "C" __attribute__((naked, no_stack_protector, noreturn)) void
_start() noexcept {
  __asm__ volatile(
      "xorl %%ebp, %%ebp\n\t"
      "andq $-16, %%rsp\n\t"
      "call gc_program_static_main\n\t"
      "movl %%eax, %%edi\n\t"
      "movl $231, %%eax\n\t"
      "syscall\n\t"
      "movl $60, %%eax\n\t"
      "syscall\n\t"
      "ud2\n\t"
      ::: "memory");
}

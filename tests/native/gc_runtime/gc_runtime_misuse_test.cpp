// Explicit Panic check: forking from a controller kernel clear/trace callback
// is an undefined path. The controller must terminate with a clear record.

#include "alloc.hpp"
#include "gc.hpp"
#include "gc_runtime.hpp"
#include "gc_world.hpp"

#include <cstdint>
#include <cstring>

namespace {

struct Node {
  uint64_t refs;
  Node *edge;
};

void trace(void *object, LangGcVisitor visitor, void *context) noexcept {
  auto *node = static_cast<Node *>(object);
  if (node->edge)
    visitor(node->edge, context);
}

void clear(void *object) noexcept {
  (void)object;
  // Forbidden: a pure clear callback is still inside the stopped kernel
  // collection. Controller prepare must reject and Panic.
  lang_gc_runtime_fork_prepare_v1();
}

void retain(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
}

void release(void *object) noexcept {
  auto *node = static_cast<Node *>(object);
  if (__atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL) != 0)
    return;
  (void)lang_gc_unregister_v1(node);
  lang_free_v1(node);
}

const LangGcOps kOps = {trace, clear, retain, release, nullptr};

} // namespace

int main() {
  if (!lang_gc_world_register_v1())
    return 2;
  auto *node = static_cast<Node *>(lang_alloc_v1(sizeof(Node), alignof(Node)));
  if (!node)
    return 3;
  std::memset(node, 0, sizeof(*node));
  node->refs = 1;
  node->edge = node;
  __atomic_add_fetch(&node->refs, 1u, __ATOMIC_RELAXED);
  if (lang_gc_runtime_register_v1(node, &kOps) != LANG_GC_OK)
    return 4;
  __atomic_sub_fetch(&node->refs, 1u, __ATOMIC_ACQ_REL);

  LangGcStats stats{};
  (void)lang_gc_runtime_collect_v1(2000000000LL, &stats);
  return 5; // controller prepare did not Panic
}

#pragma once
#include <cstdint>
struct LangWeakControl;
// Private runtime ABI. Element callbacks operate on storage, so no C++
// aggregate calling convention is assumed by generated LLVM code.
extern "C" {
// Null copy/drop means bit-copy/no element cleanup (trivial elements).
using LangValueOp = void (*)(void *);
struct LangVec {
  // Element bytes never overlap a live LangVec descriptor. Payloads of
  // aliased Vec values may still share element storage.
  // refs and iterations use atomic builtins without changing this layout.
  // Retain requires an existing live reference. Element access and structural
  // updates still require caller synchronization.
  // Mutations publish complete graph state before invoking arbitrary copy/drop
  // callbacks: init copies bytes before publishing initialized; set publishes
  // the new element before dropping the old owner outside a graph-critical
  // region; clear/shrink publish the new length/initialized prefix before
  // dropping staged removed owners. A registered Vec traces only initialized.
  uint64_t refs, length, initialized, stride, iterations;
  void *data;
  LangValueOp copy, drop;
  // Private ABI extension. Appending capacity keeps every original field at
  // its previous offset. New Vecs start with capacity == length.
  uint64_t capacity;
  // Canonical Weak slot, shared by every alias of this descriptor. It is null
  // until a downgrade installs the one control block, permanently tagged by
  // collector invalidation, or left installed while its control target is
  // cleared by the final strong release. Constructors write null before
  // publication; element operations never touch it.
  LangWeakControl *weak_control;
};
LangVec *lang_vec_new_v1(uint64_t, uint64_t, LangValueOp, LangValueOp) noexcept;
void lang_vec_retain_v1(LangVec *) noexcept;
void lang_vec_release_v1(LangVec *) noexcept;
void lang_vec_init_v1(LangVec *, const void *) noexcept;
void *lang_vec_at_v1(LangVec *, uint64_t) noexcept;
// Private dyn Trait box helpers. The box is a one-element LangVec whose
// element storage is the fat pointer's data field; clone retains the same box
// and release drops it once the last owner is gone.
void lang_trait_box_release_v1(void *) noexcept;
void *lang_trait_box_clone_v1(void *) noexcept;
void *lang_trait_box_target_v1(void *) noexcept;
void lang_vec_set_v1(LangVec *, uint64_t, const void *) noexcept;
bool lang_vec_try_move_trivial_v1(LangVec *, uint64_t destination,
                                  uint64_t source, uint64_t count) noexcept;
// Private std/vec.extend fast path. Returns false for element kinds or states
// that need the owning element path, so the public loop keeps its semantics.
bool lang_vec_extend_trivial_v1(LangVec *, LangVec *) noexcept;
// Private std/vec.insert fast path. Rotates the initialized range
// [first, last] one slot toward the end without invoking element callbacks.
// Returns false without changing the Vec when the range is trivial or the
// one-element carry cannot be allocated, so the public shift loop still runs.
bool lang_vec_rotate_right_one_v1(LangVec *, uint64_t first,
                                  uint64_t last) noexcept;
LangVec *lang_vec_slice_v1(LangVec *, uint64_t, uint64_t) noexcept;
uint64_t lang_vec_begin_v1(LangVec *) noexcept;
void lang_vec_end_v1(LangVec *) noexcept;
uint64_t lang_vec_length_v1(LangVec *) noexcept;
uint64_t lang_vec_capacity_v1(LangVec *) noexcept;
// Structural operations require caller synchronization and no active
// iterations whenever length or storage changes. Slice pins are never cleared
// by a mutation; ordinary final release rejects an active pin.
void lang_vec_reserve_v1(LangVec *, uint64_t minimum) noexcept;
// Like init/set, push transfers one pre-copied value into the container.
// Pop moves one owner out and never drops it.
void lang_vec_push_v1(LangVec *, const void *value) noexcept;
bool lang_vec_pop_v1(LangVec *, void *out) noexcept;
// Resize and slice perform logical copies: the element copy callback runs
// before the new edge is published, and no callback runs inside the short
// graph-critical publication window.
void lang_vec_resize_v1(LangVec *, uint64_t length, const void *fill) noexcept;
void lang_vec_clear_v1(LangVec *) noexcept;
// Private std/io byte-range append. Both descriptors must be trivial Vec[u8]
// values. The source range is [offset, offset + count); all range checks run
// before growth, and source self-aliasing is staged when target growth may
// release the old payload.
void lang_io_vec_append_bytes_v1(LangVec *, const LangVec *, uint64_t offset,
                                 uint64_t count) noexcept;
// Private runtime operation, not a source-language length-changing API.
void lang_vec_check_resize_v1(LangVec *) noexcept;
void *lang_alloc_v1(uint64_t, uint64_t) noexcept;
void lang_free_v1(void *) noexcept;
}

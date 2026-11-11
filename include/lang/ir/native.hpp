#pragma once
#include "lang/hir/hir.hpp"

namespace lang::ir {
// Native CFG. IDs, value types and ownership plans come from frozen HIR.
// Owning slots have runtime initialization flags; copy/drop are type-specific.
enum class Op {
  constant,
  function,
  load,
  address,
  store,
  unary,
  binary,
  convert,
  call,
  struct_init,
  enum_init,
  field_read,
  field_address,
  load_indirect,
  store_indirect,
  enum_test,
  enum_payload,
  string_literal,
  exit_begin,
  cleanup_begin,
  cleanup_end,
  copy, move, take, destroy, borrow, end_borrow, save_output,
  vec_init, vec_read, vec_set, vec_slice, iteration_begin, iteration_end,
  vec_length, iteration_test, iteration_read, iteration_next, ownership_begin, ownership_end, replace_indirect, begin_borrow, vec_check,
  // Appended: an address of one already bounds-checked Vec element. It is
  // confined to the field replacement of the assignment identified by origin.
  vec_element_address,
  system_call
};
struct ExitSite {
  HirNodeId origin;
  hir::ExitKind kind;
  std::optional<TypeId> error_type;
  bool operator==(const ExitSite &) const = default;
};
struct Instruction {
  Op op = Op::constant;
  std::optional<ValueId> result;
  TypeId type;                 // pointee type for address, Unit for store
  std::vector<ValueId> inputs; // call: callee then already evaluated formals
  LocalSlotId slot;
  SymbolId function;
  TypeId signature;
  std::optional<hir::SystemOperation> system; // system_call only
  SymbolId field;   // resolved declaration identity, never a source name
  SymbolId variant; // enum declaration identity; enum_test produces bool
  uint64_t bits = 0;
  // Only string_literal: validated UTF-8 bytes in immortal read-only storage.
  // Exact decoded UTF-8 bytes; literal storage has no dynamic owner.
  std::string utf8;
  std::optional<ExitSite> exit; // exit_begin only: immutable HIR plan identity
  std::optional<HirNodeId> cleanup; // cleanup_begin/end: defer registration
  std::optional<HirNodeId>
      origin; // call/convert: source HIR identity, including inline copies
  std::optional<hir::OwnerRef> owner;
  hir::UnaryOp unary = hir::UnaryOp::negate;
  hir::BinaryOp binary = hir::BinaryOp::add;
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
};
enum class Exit {
  missing,
  branch,
  conditional,
  return_,
  unreachable,
  try_call,
  error_return
};
struct Terminator {
  Exit kind = Exit::missing;
  std::vector<ValueId> inputs;
  std::vector<BlockId> targets;
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
  TypeId signature{0}; // try_call only; inputs: callee then evaluated formals
  std::optional<ExitSite>
      exit; // consumes exit_begin after all planned cleanups
  std::optional<HirNodeId>
      origin; // return/throw/call, or concrete Try entry branch
};
struct BlockParameter {
  ValueId value;
  TypeId type;
  HirNodeId call;
  bool error = false; // independently identifies which TryCall edge defines it
};
struct BasicBlock {
  std::vector<Instruction> instructions;
  Terminator terminator;
  // Only TryCall edges carry a value: success result (including Unit), or
  // error.
  std::vector<BlockParameter> parameters;
};
struct Slot {
  TypeId type;
  std::optional<uint32_t> parameter;
  bool inout = false;
  std::optional<hir::OwnerRef> owner;
  bool owning_parameter = false;
  std::optional<SymbolId> binding;
};
struct Handler {
  hir::CatchTarget target;
  BlockId block;
  LocalSlotId slot;
  BlockId protected_entry; // concrete Try instance; same HIR can be expanded
                           // repeatedly
};
struct Function {
  SymbolId symbol;
  TypeId signature;
  std::vector<Slot> slots;
  std::vector<BasicBlock> blocks; // block 0 is entry
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
  std::vector<Handler> handlers;
};
struct Module {
  hir::VerifiedHirModule hir;
  std::vector<Function> functions;
};
class VerifiedModule;
PhaseResult<VerifiedModule> verify(const Module &);
class VerifiedModule {
  std::shared_ptr<const Module> module_;
  explicit VerifiedModule(std::shared_ptr<const Module> m)
      : module_(std::move(m)) {}
  friend PhaseResult<VerifiedModule> verify(const Module &);

public:
  const Module &module() const { return *module_; }
};
PhaseResult<VerifiedModule> lower(const hir::VerifiedHirModule &);
std::string dump(const Module &);
} // namespace lang::ir

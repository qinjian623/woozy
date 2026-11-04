#pragma once
#include "lang/hir/hir.hpp"

namespace lang::ir {
// Trivial-value CFG. IDs and language types come from the public frontend. Unit has
// one trivial value; no ownership/cleanup operation is implemented here.
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
  string_literal
};
struct Instruction {
  Op op = Op::constant;
  std::optional<ValueId> result;
  TypeId type;                 // pointee type for address, Unit for store
  std::vector<ValueId> inputs; // call: callee then already evaluated formals
  LocalSlotId slot;
  SymbolId function;
  TypeId signature;
  SymbolId field;   // resolved declaration identity, never a source name
  SymbolId variant; // enum declaration identity; enum_test produces bool
  uint64_t bits = 0;
  // Only string_literal: validated UTF-8 bytes in immortal read-only storage.
  // Length is utf8.size(), never a terminator scan; no dynamic ownership here.
  std::string utf8;
  hir::UnaryOp unary = hir::UnaryOp::negate;
  hir::BinaryOp binary = hir::BinaryOp::add;
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
};
enum class Exit { missing, branch, conditional, return_, unreachable };
struct Terminator {
  Exit kind = Exit::missing;
  std::vector<ValueId> inputs;
  std::vector<BlockId> targets;
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
};
struct BasicBlock {
  std::vector<Instruction> instructions;
  Terminator terminator;
};
struct Slot {
  TypeId type;
  std::optional<uint32_t> parameter;
  bool inout = false;
};
struct Function {
  SymbolId symbol;
  TypeId signature;
  std::vector<Slot> slots;
  std::vector<BasicBlock> blocks; // block 0 is entry
  SourceSpan source{SourceSnapshotId{0}, 0, 0};
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

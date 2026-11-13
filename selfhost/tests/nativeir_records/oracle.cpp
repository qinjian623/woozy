// Header-only oracle for the Native CFG data records.
//
// This program includes the frozen C++ native.hpp and prints the same
// constructive trace as src/main.wz.  It does not link or rebuild the C++
// compiler and never calls the old frontend; enum ordinals and record fields
// come directly from the header.  ir::Module is deliberately not constructed:
// its hir field is a VerifiedHirModule with no public constructor, so the
// Module trace is derived from the HirModule and Function values it holds.
#include "lang/ir/native.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using lang::BlockId;
using lang::HirNodeId;
using lang::LocalSlotId;
using lang::ModuleId;
using lang::SourceSnapshotId;
using lang::SourceSpan;
using lang::SymbolId;
using lang::TypeId;
using lang::ValueId;
using namespace lang;
using namespace lang::ir;

namespace {

std::string num(uint64_t value) { return std::to_string(value); }
std::string flag(bool value) { return value ? "1" : "0"; }

template <class T> std::string opt_id(const std::optional<T> &value) {
  if (!value)
    return "0/0";
  return "1/" + num(value->value);
}

std::string opt_u32(const std::optional<uint32_t> &value) {
  if (!value)
    return "0/0";
  return "1/" + num(*value);
}

std::string opt_system(const std::optional<hir::SystemOperation> &value) {
  if (!value)
    return "0/0";
  return "1/" + num(static_cast<uint64_t>(*value));
}

std::string opt_exit(const std::optional<ExitSite> &value) {
  if (!value)
    return "0/0/0/0/0";
  return "1/" + num(value->origin.value) + "/" +
         num(static_cast<uint64_t>(value->kind)) + "/" +
         opt_id(value->error_type);
}

std::string opt_owner(const std::optional<hir::OwnerRef> &value) {
  if (!value)
    return "0/0/0";
  return "1/" + num(static_cast<uint64_t>(value->kind)) + "/" +
         num(value->identity);
}

std::string span_trace(const SourceSpan &value) {
  return num(value.snapshot.value) + "/" + num(value.begin_byte) + "/" +
         num(value.end_byte);
}

template <class T> std::string id_list(const std::vector<T> &values) {
  std::string out = num(values.size());
  for (const auto &value : values)
    out += "/" + num(value.value);
  return out;
}

std::string instruction_trace(const Instruction &value) {
  return "op=" + num(static_cast<uint64_t>(value.op)) + " result=" +
         opt_id(value.result) + " type=" + num(value.type.value) + " inputs=" +
         id_list(value.inputs) + " slot=" + num(value.slot.value) +
         " function=" + num(value.function.value) + " signature=" +
         num(value.signature.value) + " system=" + opt_system(value.system) +
         " c_callable=" + opt_u32(value.c_callable) + " methods=" +
         id_list(value.methods) + " c_type=" + num(value.c_type) + " c_field=" +
         num(value.c_field) + " field=" + num(value.field.value) + " variant=" +
         num(value.variant.value) + " bits=" + num(value.bits) + " utf8=" +
         num(value.utf8.size()) + " exit=" + opt_exit(value.exit) +
         " cleanup=" + opt_id(value.cleanup) + " origin=" +
         opt_id(value.origin) + " owner=" + opt_owner(value.owner) +
         " unary=" + num(static_cast<uint64_t>(value.unary)) + " binary=" +
         num(static_cast<uint64_t>(value.binary)) + " source=" +
         span_trace(value.source);
}

std::string terminator_trace(const Terminator &value) {
  return "kind=" + num(static_cast<uint64_t>(value.kind)) + " inputs=" +
         id_list(value.inputs) + " targets=" + id_list(value.targets) +
         " source=" + span_trace(value.source) + " signature=" +
         num(value.signature.value) + " exit=" + opt_exit(value.exit) +
         " origin=" + opt_id(value.origin);
}

std::string parameter_trace(const BlockParameter &value) {
  return "value=" + num(value.value.value) + " type=" +
         num(value.type.value) + " call=" + num(value.call.value) +
         " error=" + flag(value.error);
}

std::string slot_trace(const Slot &value) {
  return "type=" + num(value.type.value) + " parameter=" +
         opt_u32(value.parameter) + " inout=" + flag(value.inout) +
         " owner=" + opt_owner(value.owner) + " owning=" +
         flag(value.owning_parameter) + " binding=" + opt_id(value.binding);
}

std::string handler_trace(const Handler &value) {
  return "block=" + num(value.block.value) + " slot=" +
         num(value.slot.value) + " protected_entry=" +
         num(value.protected_entry.value) + " try_node=" +
         num(value.target.try_node.value) + " index=" + num(value.target.index);
}

Instruction option_zero_instruction() {
  Instruction value;
  value.op = Op::exit_begin;
  value.result = ValueId{0};
  value.exit = std::nullopt;
  value.cleanup = std::nullopt;
  value.origin = std::nullopt;
  value.owner = std::nullopt;
  value.unary = hir::UnaryOp::negate;
  value.binary = hir::BinaryOp::add;
  value.source = SourceSpan{SourceSnapshotId{0}, 0, 0};
  return value;
}

Instruction rich_instruction() {
  Instruction value;
  value.op = Op::trait_call;
  value.result = ValueId{7};
  value.type = TypeId{11};
  value.inputs = {ValueId{3}, ValueId{5}};
  value.slot = LocalSlotId{13};
  value.function = SymbolId{17};
  value.signature = TypeId{19};
  value.system = hir::SystemOperation{0};
  value.c_callable = 3u;
  value.methods = {SymbolId{23}, SymbolId{29}};
  value.c_type = 31u;
  value.c_field = 37u;
  value.field = SymbolId{41};
  value.variant = SymbolId{43};
  value.bits = 1099511627776ull;
  value.utf8 = "trace";
  value.exit = ExitSite{HirNodeId{0}, hir::ExitKind::cancel, TypeId{0}};
  value.cleanup = HirNodeId{0};
  value.origin = HirNodeId{0};
  value.owner = hir::OwnerRef{hir::OwnerKind::binding, 0};
  value.unary = hir::UnaryOp::bit_not;
  value.binary = hir::BinaryOp::greater_equal;
  value.source = SourceSpan{SourceSnapshotId{47}, 53, 59};
  return value;
}

Terminator rich_terminator() {
  Terminator value;
  value.kind = Exit::try_call;
  value.inputs = {ValueId{2}};
  value.targets = {BlockId{0}, BlockId{4}};
  value.source = SourceSpan{SourceSnapshotId{11}, 13, 17};
  value.signature = TypeId{7};
  value.exit = ExitSite{HirNodeId{0}, hir::ExitKind::normal, std::nullopt};
  value.origin = HirNodeId{0};
  return value;
}

Function build_function() {
  Function value;
  value.symbol = SymbolId{0};
  value.signature = TypeId{0};
  value.async = true;
  value.source = SourceSpan{SourceSnapshotId{7}, 11, 13};

  Slot binding_parameter;
  binding_parameter.type = TypeId{0};
  binding_parameter.parameter = 0u;
  binding_parameter.inout = true;
  binding_parameter.owner = hir::OwnerRef{hir::OwnerKind::binding, 0};
  binding_parameter.owning_parameter = true;
  binding_parameter.binding = SymbolId{0};
  Slot plain;
  plain.type = TypeId{5};
  value.slots = {binding_parameter, plain};

  BasicBlock entry;
  entry.instructions = {Instruction{}, rich_instruction()};
  entry.terminator = rich_terminator();
  entry.parameters = {BlockParameter{ValueId{0}, TypeId{0}, HirNodeId{0},
                                     false},
                      BlockParameter{ValueId{9}, TypeId{13}, HirNodeId{17},
                                     true}};
  BasicBlock other;
  other.terminator = Terminator{};
  value.blocks = {entry, other};

  value.handlers = {
      Handler{hir::CatchTarget{HirNodeId{0}, 0}, BlockId{1}, LocalSlotId{0},
              BlockId{0}}};
  return value;
}

} // namespace

int main() {
  const Op ops[] = {
      Op::constant,          Op::function,
      Op::load,              Op::address,
      Op::store,             Op::unary,
      Op::binary,            Op::convert,
      Op::call,              Op::struct_init,
      Op::enum_init,         Op::field_read,
      Op::field_address,     Op::load_indirect,
      Op::store_indirect,    Op::enum_test,
      Op::enum_payload,      Op::string_literal,
      Op::exit_begin,        Op::cleanup_begin,
      Op::cleanup_end,       Op::copy,
      Op::move,              Op::take,
      Op::destroy,           Op::borrow,
      Op::end_borrow,        Op::save_output,
      Op::vec_init,          Op::vec_read,
      Op::vec_set,           Op::vec_slice,
      Op::iteration_begin,   Op::iteration_end,
      Op::vec_length,        Op::iteration_test,
      Op::iteration_read,    Op::iteration_next,
      Op::ownership_begin,   Op::ownership_end,
      Op::replace_indirect,  Op::begin_borrow,
      Op::vec_check,         Op::vec_element_address,
      Op::system_call,       Op::foreign_call,
      Op::c_field_read,      Op::c_ptr_read,
      Op::c_ptr_write,       Op::c_buffer_read,
      Op::c_buffer_write,    Op::trait_object,
      Op::trait_call,
  };
  for (std::size_t index = 0; index < std::size(ops); ++index)
    std::cout << "op i=" << num(index) << " id="
              << num(static_cast<uint64_t>(ops[index])) << "\n";

  const Exit exits[] = {
      Exit::missing,   Exit::branch,     Exit::conditional, Exit::return_,
      Exit::unreachable, Exit::try_call, Exit::error_return, Exit::await_,
      Exit::cancel_return,
  };
  for (std::size_t index = 0; index < std::size(exits); ++index)
    std::cout << "exit i=" << num(index) << " id="
              << num(static_cast<uint64_t>(exits[index])) << "\n";

  std::cout << "instruction_default "
            << instruction_trace(Instruction{}) << "\n";
  std::cout << "instruction_zero_id "
            << instruction_trace(option_zero_instruction()) << "\n";
  std::cout << "instruction_rich "
            << instruction_trace(rich_instruction()) << "\n";
  std::cout << "terminator_default " << terminator_trace(Terminator{})
            << "\n";
  std::cout << "terminator_rich " << terminator_trace(rich_terminator())
            << "\n";

  const Function function = build_function();
  for (std::size_t index = 0; index < function.blocks[0].parameters.size();
       ++index)
    std::cout << "block_parameter i=" << num(index) << " "
              << parameter_trace(function.blocks[0].parameters[index]) << "\n";
  for (std::size_t index = 0; index < function.slots.size(); ++index)
    std::cout << "slot i=" << num(index) << " "
              << slot_trace(function.slots[index]) << "\n";
  for (std::size_t index = 0; index < function.handlers.size(); ++index)
    std::cout << "handler i=" << num(index) << " "
              << handler_trace(function.handlers[index]) << "\n";

  std::cout << "function symbol=" << num(function.symbol.value)
            << " signature=" << num(function.signature.value) << " is_async="
            << flag(function.async) << " slots=" << num(function.slots.size())
            << " blocks=" << num(function.blocks.size()) << " handlers="
            << num(function.handlers.size()) << " source="
            << span_trace(function.source) << "\n";
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    const auto &block = function.blocks[index];
    std::cout << "block i=" << num(index) << " instructions="
              << num(block.instructions.size()) << " parameters="
              << num(block.parameters.size()) << " terminator="
              << num(static_cast<uint64_t>(block.terminator.kind)) << "\n";
  }

  // ir::Module holds a VerifiedHirModule with no public constructor, so the
  // mutable builder module is described by the fields it would receive.
  hir::HirModule hmodule;
  hmodule.identity = ModuleId{0};
  hmodule.config = hir::Config{};
  std::cout << "module_empty hir_identity=" << num(hmodule.identity.value)
            << " hir_word_bits=" << num(hmodule.config.word_bits)
            << " functions=" << num(0) << "\n";
  std::cout << "module_built hir_identity=" << num(hmodule.identity.value)
            << " hir_word_bits=" << num(hmodule.config.word_bits)
            << " functions=" << num(1) << " function0_symbol="
            << num(function.symbol.value) << " function0_blocks="
            << num(function.blocks.size()) << " function0_slots="
            << num(function.slots.size()) << " function0_handlers="
            << num(function.handlers.size()) << " function0_async="
            << flag(function.async) << "\n";
  return 0;
}

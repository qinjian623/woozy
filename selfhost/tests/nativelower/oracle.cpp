// Real linked C++ oracle for the Native CFG dump.
//
// The record construction mirrors the Wz fixtures value for value.  main()
// builds a legal empty HIR module (module id 0, identity 0), runs the real
// lang::hir::verify and lang::ir::dump from the stage0 build, and prints the
// frozen 989-byte empty+special case.  --rich prints the 53-Op / 9-Exit
// synthetic module through the same real dump.  No ordinals, field names or
// printing rules are reimplemented here.
#include "lang/ir/native.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using lang::BlockId;
using lang::HirNodeId;
using lang::LocalSlotId;
using lang::SourceSnapshotId;
using lang::SourceSpan;
using lang::SymbolId;
using lang::TypeId;
using lang::ValueId;
using namespace lang;
using namespace lang::ir;

namespace {

SourceSpan span(uint64_t snapshot, uint64_t begin, uint64_t end) {
  return SourceSpan{SourceSnapshotId{snapshot}, begin, end};
}

Instruction base(Op op, std::optional<ValueId> result, TypeId type,
                 SourceSpan source) {
  Instruction instruction;
  instruction.op = op;
  instruction.result = result;
  instruction.type = type;
  instruction.source = source;
  return instruction;
}

std::optional<ValueId> some(uint64_t value) { return ValueId{value}; }

std::optional<uint32_t> some_u32(uint32_t value) { return value; }

ExitSite site(uint64_t origin, hir::ExitKind kind,
              std::optional<TypeId> error_type) {
  return ExitSite{HirNodeId{origin}, kind, error_type};
}

hir::OwnerRef owner(hir::OwnerKind kind, uint64_t identity) {
  return hir::OwnerRef{kind, identity};
}

Function function_zero() {
  Function f;
  f.symbol = SymbolId{20};
  f.signature = TypeId{5};
  f.async = false;
  f.source = span(2, 10, 30);
  {
    Slot slot;
    slot.type = TypeId{6};
    slot.parameter = 0;
    slot.inout = false;
    slot.binding = SymbolId{9};
    f.slots.push_back(slot);
  }
  {
    Slot slot;
    slot.type = TypeId{7};
    slot.parameter = 1;
    slot.inout = true;
    slot.owner = owner(hir::OwnerKind::binding, 1);
    slot.owning_parameter = true;
    slot.binding = SymbolId{19};
    f.slots.push_back(slot);
  }
  {
    Slot slot;
    slot.type = TypeId{12};
    slot.owner = owner(hir::OwnerKind::temporary, 7);
    f.slots.push_back(slot);
  }
  {
    Slot slot;
    slot.type = TypeId{3};
    slot.inout = true;
    f.slots.push_back(slot);
  }
  f.handlers.push_back(Handler{{HirNodeId{40}, 2}, BlockId{3}, LocalSlotId{2},
                               BlockId{1}});

  std::vector<Instruction> block_instructions;
  Instruction i = base(Op::constant, some(0), TypeId{6}, span(2, 50, 53));
  i.bits = 127;
  block_instructions.push_back(i);
  i = base(Op::store, std::nullopt, TypeId{1}, span(2, 36, 53));
  i.slot = LocalSlotId{0};
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::string_literal, some(1), TypeId{12}, span(2, 72, 90));
  i.utf8 = "A\xc3\xa9\xf0\x9f\x98\x80";
  block_instructions.push_back(i);
  i = base(Op::function, some(2), TypeId{5}, span(2, 95, 110));
  i.function = SymbolId{99};
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::load, some(3), TypeId{6}, span(2, 117, 121));
  i.slot = LocalSlotId{0};
  block_instructions.push_back(i);
  i = base(Op::address, some(4), TypeId{12}, span(2, 130, 140));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::unary, some(5), TypeId{6}, span(2, 150, 160));
  i.unary = hir::UnaryOp::logical_not;
  i.inputs.push_back(ValueId{3});
  block_instructions.push_back(i);
  i = base(Op::binary, some(6), TypeId{1}, span(2, 170, 180));
  i.binary = hir::BinaryOp::greater_equal;
  i.origin = HirNodeId{15};
  i.inputs.push_back(ValueId{5});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::convert, some(7), TypeId{7}, span(2, 190, 200));
  i.origin = HirNodeId{16};
  i.inputs.push_back(ValueId{6});
  block_instructions.push_back(i);
  i = base(Op::call, some(8), TypeId{9}, span(2, 210, 230));
  i.signature = TypeId{5};
  i.origin = HirNodeId{17};
  i.c_callable = some_u32(4);
  i.c_type = 3;
  i.inputs.push_back(ValueId{2});
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::struct_init, some(9), TypeId{9}, span(2, 240, 250));
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::enum_init, some(10), TypeId{10}, span(2, 260, 270));
  i.variant = SymbolId{41};
  i.inputs.push_back(ValueId{9});
  block_instructions.push_back(i);
  i = base(Op::field_read, some(11), TypeId{6}, span(2, 280, 290));
  i.field = SymbolId{42};
  i.inputs.push_back(ValueId{9});
  block_instructions.push_back(i);
  i = base(Op::field_address, some(12), TypeId{12}, span(2, 300, 310));
  i.field = SymbolId{43};
  i.inputs.push_back(ValueId{9});
  block_instructions.push_back(i);
  i = base(Op::load_indirect, some(13), TypeId{6}, span(2, 320, 330));
  i.inputs.push_back(ValueId{12});
  block_instructions.push_back(i);
  i = base(Op::store_indirect, std::nullopt, TypeId{1}, span(2, 340, 350));
  i.inputs.push_back(ValueId{12});
  i.inputs.push_back(ValueId{13});
  block_instructions.push_back(i);
  i = base(Op::enum_test, some(14), TypeId{1}, span(2, 360, 370));
  i.variant = SymbolId{41};
  i.inputs.push_back(ValueId{10});
  block_instructions.push_back(i);
  i = base(Op::enum_payload, some(15), TypeId{6}, span(2, 380, 390));
  i.field = SymbolId{42};
  i.variant = SymbolId{41};
  i.inputs.push_back(ValueId{10});
  block_instructions.push_back(i);
  i = base(Op::exit_begin, std::nullopt, TypeId{1}, span(2, 400, 410));
  i.exit = site(50, hir::ExitKind::error, TypeId{7});
  i.inputs.push_back(ValueId{15});
  block_instructions.push_back(i);
  i = base(Op::cleanup_begin, std::nullopt, TypeId{1}, span(2, 420, 430));
  i.cleanup = HirNodeId{60};
  block_instructions.push_back(i);
  i = base(Op::cleanup_end, std::nullopt, TypeId{1}, span(2, 440, 450));
  i.cleanup = HirNodeId{60};
  block_instructions.push_back(i);
  i = base(Op::copy, some(16), TypeId{12}, span(2, 460, 470));
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::move, some(17), TypeId{12}, span(2, 480, 490));
  i.origin = HirNodeId{18};
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::take, std::nullopt, TypeId{1}, span(2, 500, 510));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::destroy, std::nullopt, TypeId{1}, span(2, 520, 530));
  i.slot = LocalSlotId{2};
  i.owner = owner(hir::OwnerKind::temporary, 7);
  block_instructions.push_back(i);
  i = base(Op::borrow, some(18), TypeId{12}, span(2, 540, 550));
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::end_borrow, std::nullopt, TypeId{1}, span(2, 560, 570));
  i.owner = owner(hir::OwnerKind::binding, 3);
  block_instructions.push_back(i);
  i = base(Op::save_output, std::nullopt, TypeId{1}, span(2, 580, 590));
  i.inputs.push_back(ValueId{17});
  block_instructions.push_back(i);
  i = base(Op::vec_init, some(19), TypeId{20}, span(2, 600, 610));
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  i.inputs.push_back(ValueId{3});
  block_instructions.push_back(i);
  i = base(Op::vec_read, some(20), TypeId{6}, span(2, 620, 630));
  i.origin = HirNodeId{21};
  i.inputs.push_back(ValueId{19});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::vec_set, std::nullopt, TypeId{1}, span(2, 640, 650));
  i.inputs.push_back(ValueId{19});
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{20});
  block_instructions.push_back(i);
  i = base(Op::vec_slice, some(21), TypeId{20}, span(2, 660, 670));
  i.bits = 3;
  i.origin = HirNodeId{22};
  i.inputs.push_back(ValueId{19});
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::iteration_begin, std::nullopt, TypeId{1}, span(2, 680, 690));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::iteration_end, std::nullopt, TypeId{1}, span(2, 700, 710));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::vec_length, some(22), TypeId{7}, span(2, 720, 730));
  i.inputs.push_back(ValueId{19});
  block_instructions.push_back(i);
  i = base(Op::iteration_test, some(23), TypeId{1}, span(2, 740, 750));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::iteration_read, some(24), TypeId{6}, span(2, 760, 770));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::iteration_next, std::nullopt, TypeId{1}, span(2, 780, 790));
  i.slot = LocalSlotId{2};
  block_instructions.push_back(i);
  i = base(Op::ownership_begin, std::nullopt, TypeId{1}, span(2, 800, 810));
  i.origin = HirNodeId{70};
  i.bits = 1;
  block_instructions.push_back(i);
  i = base(Op::ownership_end, std::nullopt, TypeId{1}, span(2, 820, 830));
  i.origin = HirNodeId{70};
  i.bits = 1;
  block_instructions.push_back(i);
  i = base(Op::replace_indirect, std::nullopt, TypeId{1}, span(2, 840, 850));
  i.inputs.push_back(ValueId{12});
  i.inputs.push_back(ValueId{13});
  block_instructions.push_back(i);
  i = base(Op::begin_borrow, std::nullopt, TypeId{1}, span(2, 860, 870));
  i.slot = LocalSlotId{2};
  i.owner = owner(hir::OwnerKind::binding, 1);
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::vec_check, std::nullopt, TypeId{1}, span(2, 880, 890));
  i.origin = HirNodeId{21};
  i.inputs.push_back(ValueId{19});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::vec_element_address, some(25), TypeId{12}, span(2, 900, 910));
  i.origin = HirNodeId{21};
  i.inputs.push_back(ValueId{19});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::system_call, some(26), TypeId{7}, span(2, 920, 930));
  i.signature = TypeId{8};
  i.system = static_cast<hir::SystemOperation>(33);
  i.c_callable = some_u32(9);
  i.c_type = 3;
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::foreign_call, some(27), TypeId{9}, span(2, 940, 950));
  i.signature = TypeId{5};
  i.c_callable = some_u32(4);
  i.c_type = 7;
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::c_field_read, some(28), TypeId{6}, span(2, 960, 970));
  i.c_type = 3;
  i.c_field = 5;
  i.origin = HirNodeId{23};
  i.inputs.push_back(ValueId{1});
  block_instructions.push_back(i);
  i = base(Op::c_ptr_read, some(29), TypeId{6}, span(2, 980, 990));
  i.origin = HirNodeId{24};
  i.inputs.push_back(ValueId{1});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::c_ptr_write, std::nullopt, TypeId{1}, span(2, 1000, 1010));
  i.origin = HirNodeId{24};
  i.inputs.push_back(ValueId{1});
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{29});
  block_instructions.push_back(i);
  i = base(Op::c_buffer_read, some(30), TypeId{6}, span(2, 1020, 1030));
  i.origin = HirNodeId{25};
  i.inputs.push_back(ValueId{1});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);
  i = base(Op::c_buffer_write, std::nullopt, TypeId{1}, span(2, 1040, 1050));
  i.origin = HirNodeId{25};
  i.inputs.push_back(ValueId{1});
  i.inputs.push_back(ValueId{0});
  i.inputs.push_back(ValueId{30});
  block_instructions.push_back(i);
  i = base(Op::trait_object, some(31), TypeId{9}, span(2, 1060, 1070));
  i.variant = SymbolId{41};
  i.origin = HirNodeId{26};
  i.methods.push_back(SymbolId{77});
  i.methods.push_back(SymbolId{78});
  i.inputs.push_back(ValueId{9});
  block_instructions.push_back(i);
  i = base(Op::trait_call, some(32), TypeId{6}, span(2, 1080, 1090));
  i.signature = TypeId{8};
  i.bits = 2;
  i.field = SymbolId{79};
  i.origin = HirNodeId{27};
  i.inputs.push_back(ValueId{31});
  i.inputs.push_back(ValueId{0});
  block_instructions.push_back(i);

  {
    BasicBlock block;
    block.instructions = block_instructions;
    block.terminator.kind = Exit::conditional;
    block.terminator.inputs = {ValueId{32}};
    block.terminator.targets = {BlockId{1}, BlockId{2}};
    block.terminator.source = span(2, 60, 70);
    block.terminator.origin = HirNodeId{80};
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.parameters.push_back(
        BlockParameter{ValueId{40}, TypeId{6}, HirNodeId{50}, false});
    block.terminator.kind = Exit::try_call;
    block.terminator.signature = TypeId{5};
    block.terminator.inputs = {ValueId{0}, ValueId{1}};
    block.terminator.targets = {BlockId{2}};
    block.terminator.source = span(2, 30, 40);
    block.terminator.origin = HirNodeId{51};
    block.terminator.exit =
        site(53, hir::ExitKind::error, std::nullopt);
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.parameters.push_back(
        BlockParameter{ValueId{41}, TypeId{7}, HirNodeId{50}, true});
    block.instructions.push_back(
        base(Op::constant, some(33), TypeId{7}, span(2, 1100, 1110)));
    block.terminator.kind = Exit::return_;
    block.terminator.inputs = {ValueId{33}};
    block.terminator.source = span(2, 1120, 1130);
    block.terminator.origin = HirNodeId{52};
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.terminator.kind = Exit::branch;
    block.terminator.targets = {BlockId{0}};
    block.terminator.source = span(2, 1140, 1150);
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.terminator.source = span(2, 1160, 1170);
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.terminator.kind = Exit::unreachable;
    block.terminator.source = span(2, 1180, 1190);
    f.blocks.push_back(block);
  }
  return f;
}

Function function_one() {
  Function f;
  f.symbol = SymbolId{21};
  f.signature = TypeId{30};
  f.async = true;
  f.source = span(3, 1, 9);
  {
    Slot slot;
    slot.type = TypeId{29};
    f.slots.push_back(slot);
  }
  {
    BasicBlock block;
    block.instructions.push_back(
        base(Op::constant, some(0), TypeId{6}, span(3, 10, 20)));
    block.instructions.back().bits = 1;
    block.terminator.kind = Exit::await_;
    block.terminator.signature = TypeId{29};
    block.terminator.inputs = {ValueId{0}};
    block.terminator.targets = {BlockId{1}, BlockId{2}, BlockId{3}};
    block.terminator.source = span(3, 30, 40);
    block.terminator.origin = HirNodeId{90};
    block.terminator.exit =
        site(90, hir::ExitKind::cancel, std::nullopt);
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.parameters.push_back(
        BlockParameter{ValueId{1}, TypeId{6}, HirNodeId{90}, false});
    block.terminator.kind = Exit::return_;
    block.terminator.inputs = {ValueId{1}};
    block.terminator.source = span(3, 50, 60);
    block.terminator.origin = HirNodeId{91};
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.parameters.push_back(
        BlockParameter{ValueId{2}, TypeId{7}, HirNodeId{90}, true});
    block.terminator.kind = Exit::error_return;
    block.terminator.inputs = {ValueId{2}};
    block.terminator.source = span(3, 70, 80);
    f.blocks.push_back(block);
  }
  {
    BasicBlock block;
    block.terminator.kind = Exit::cancel_return;
    block.terminator.source = span(3, 90, 100);
    f.blocks.push_back(block);
  }
  return f;
}


std::vector<Function> rich_functions() {
  std::vector<Function> functions;
  functions.push_back(function_zero());
  functions.push_back(function_one());
  return functions;
}

// Real linked lang::ir::dump oracle.
//
// main() uses the real hir::verify on a legal empty app module (module id 0,
// HIR identity 0) and the real lang::ir::dump from src/ir/verify.cpp.  The
// default output is the frozen 989-byte "== empty" + "== special" case; the
// --rich mode prints the 53-Op / 9-Exit synthetic module in the same format.

Instruction empty_instruction_value() { return Instruction(); }

Function special_function() {
  Function f;
  f.symbol = SymbolId{UINT64_MAX};
  f.signature = TypeId{uint64_t(1) << 63};
  f.async = true;
  {
    Slot slot;
    slot.type = TypeId{0};
    slot.parameter = 0;
    slot.inout = true;
    f.slots.push_back(slot);
  }
  {
    Slot slot;
    slot.type = TypeId{UINT64_MAX};
    slot.inout = true;
    f.slots.push_back(slot);
  }
  {
    Slot slot;
    slot.type = TypeId{7};
    slot.parameter = UINT32_MAX;
    f.slots.push_back(slot);
  }
  f.handlers.push_back(Handler{{HirNodeId{0}, UINT32_MAX}, BlockId{UINT64_MAX},
                               LocalSlotId{UINT64_MAX}, BlockId{0}});
  BasicBlock b;
  b.parameters.push_back(
      BlockParameter{ValueId{0}, TypeId{UINT64_MAX}, HirNodeId{0}, true});
  Instruction i;
  i.op = Op::constant;
  i.result = ValueId{0};
  i.type = TypeId{0};
  i.bits = UINT64_MAX;
  i.c_callable = 0;
  i.c_type = UINT32_MAX;
  i.system = static_cast<hir::SystemOperation>(0);
  i.origin = HirNodeId{0};
  i.exit = ExitSite{HirNodeId{0}, hir::ExitKind::cancel, TypeId{0}};
  i.cleanup = HirNodeId{0};
  i.inputs = {ValueId{0}, ValueId{UINT64_MAX}};
  i.source = {SourceSnapshotId{UINT64_MAX}, uint64_t(1) << 63, UINT64_MAX};
  b.instructions.push_back(i);
  i = empty_instruction_value();
  i.op = Op::string_literal;
  i.utf8 = std::string("\0\xF0\x9F\x98\x80" "e\xCC\x81\t\"\\", 11);
  b.instructions.push_back(i);
  i = empty_instruction_value();
  i.op = Op::c_field_read;
  i.c_field = UINT32_MAX;
  i.c_callable = 0;
  b.instructions.push_back(i);
  i = empty_instruction_value();
  i.op = Op::enum_payload;
  i.field = SymbolId{0};
  i.variant = SymbolId{UINT64_MAX};
  b.instructions.push_back(i);
  i = empty_instruction_value();
  i.op = Op::unary;
  i.unary = hir::UnaryOp::bit_not;
  b.instructions.push_back(i);
  i = empty_instruction_value();
  i.op = Op::binary;
  i.binary = hir::BinaryOp::greater_equal;
  b.instructions.push_back(i);
  b.terminator.kind = Exit::try_call;
  b.terminator.signature = TypeId{UINT64_MAX};
  b.terminator.origin = HirNodeId{0};
  b.terminator.inputs = {ValueId{UINT64_MAX}, ValueId{0}};
  b.terminator.targets = {BlockId{0}, BlockId{UINT64_MAX}};
  f.blocks.push_back(b);
  return f;
}

} // namespace

int main(int argc, char **argv) {
  bool rich = argc > 1 && std::string(argv[1]) == "--rich";
  hir::HirModule h;
  auto project = std::make_shared<modules::Project>();
  project->backend = "native";
  project->word_bits = 64;
  project->modules.emplace_back();
  project->modules.back().id = ModuleId{0};
  project->modules.back().logical_path = "app";
  auto resolved = std::make_shared<modules::ResolvedProject>();
  resolved->project = project;
  h.resolved = resolved;
  h.config = {hir::Backend::native, 64};
  auto checked = hir::verify(h, {}, hir::VerifyMode::static_only);
  if (!checked.success()) {
    for (auto &d : checked.diagnostics)
      std::cerr << d.code << ": " << d.message << '\n';
    return 3;
  }
  ir::Module m{*checked.value, {}};
  if (rich) {
    m.functions = rich_functions();
    std::cout << ir::dump(m);
    return 0;
  }
  std::cout << "== empty\n" << ir::dump(m);
  m.functions.push_back(special_function());
  std::cout << "== special\n" << ir::dump(m);
  return 0;
}

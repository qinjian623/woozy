// Test-only observation for explicit numeric conversions.
//
// The verified Native IR is emitted exactly like the production CLI, then the
// generated functions are called through their actual LLVM FunctionType with
// bit patterns supplied by the test runner. Return bits are written raw, so the
// runner compares integers and IEEE values without any C++ ABI guess.
//
// The same program also mutates conversion instructions in the verified Native
// IR and requires the independent Native IR verifier to reject every real
// source/target mutation (narrowing, bool/address operands, arity/origin,
// same-type operand substitution, and compound/unary operand child
// substitution). A mutation
// may insert into the same instruction vector, so the original conversion is
// re-located by its stable result ValueId instead of keeping a reference.
#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
using namespace lang;
void need(bool ok, const std::string &s) {
  if (!ok)
    throw std::runtime_error(s);
}
void write(const std::filesystem::path &p, const std::string &s) {
  std::ofstream f(p, std::ios::binary);
  f.write(s.data(), std::streamsize(s.size()));
  need(bool(f), "write artifact");
}
int main(int argc, char **argv) {
  try {
    need(argc == 5, "manifest out optimization requests required");
    std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load");
    project.value->backend = "native";
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      for (auto &d : checked.diagnostics)
        std::cerr << d.code << ": " << d.message << '\n';
    need(checked.success(), "check real project");
    size_t shortened_operand_rejections = 0;
    const auto &hir_module = checked.value->module();
    for (const auto &conversion : hir_module.nodes) {
      if (conversion.kind() != hir::NodeKind::integer_convert)
        continue;
      const auto operand_id = conversion.children.at(0);
      const auto &operand = hir_module.nodes.at(operand_id.value);
      if (operand.kind() != hir::NodeKind::binary &&
          operand.kind() != hir::NodeKind::unary)
        continue;
      auto damaged = hir_module;
      damaged.nodes[operand_id.value].source.span =
          hir_module.nodes.at(operand.children.at(0).value).source.span;
      need(!hir::verify_types(damaged).success() &&
               !hir::verify(damaged).success(),
           "shortened conversion operand span accepted");
      need(hir::detail::produce_ownership(damaged, {}).success(),
           "shortened operand ownership recomputation failed");
      need(!hir::verify_types(damaged).success() &&
               !hir::verify(damaged).success(),
           "shortened conversion operand span accepted after ownership");
      ++shortened_operand_rejections;
    }
    auto lowered = ir::lower(*checked.value);
    need(lowered.success(), "lower verified HIR");
    const auto original = lowered.value->module();
    const auto &types = original.hir.module().types;
    TypeId boolean{}, narrow_float{}, wide_float{};
    for (uint64_t n = 1; n <= types.size(); ++n) {
      auto *t = types.find(TypeId{n});
      if (!t)
        continue;
      if (t->kind == hir::TypeKind::Bool)
        boolean = TypeId{n};
      if (t->kind == hir::TypeKind::Float && t->bits == 32)
        narrow_float = TypeId{n};
      if (t->kind == hir::TypeKind::Float && t->bits == 64)
        wide_float = TypeId{n};
    }
    // Every conversion instruction is real material; mutate its destination
    // and source types without touching the verified HIR and require a refusal.
    // The original conversion is identified by its result ValueId, a stable
    // value identity, because a callback may insert into the same vector.
    auto max_value = [](const ir::Function &fn) {
      ValueId next{0};
      for (const auto &b : fn.blocks) {
        for (const auto &p : b.parameters)
          next.value = std::max(next.value, p.value.value + 1);
        for (const auto &v : b.instructions)
          if (v.result)
            next.value = std::max(next.value, v.result->value + 1);
      }
      return next;
    };
    struct Definition {
      ValueId value;
      TypeId type;
      SourceSpan source;
      size_t block, position;
    };
    size_t rejected = 0, mutations = 0, insert_mutations = 0;
    size_t bool_mutations = 0, float_width_mutations = 0;
    size_t origin_mutations = 0, operand_mutations = 0, address_mutations = 0;
    size_t compound_operand_mutations = 0, unary_operand_mutations = 0;
    size_t external_operand_mutations = 0;
    for (size_t f = 0; f < original.functions.size(); ++f) {
      // ValueIds are function-local, so the source type is resolved inside the
      // same function that owns the conversion.
      std::map<uint64_t, TypeId> value_types;
      std::vector<Definition> definitions;
      for (size_t b = 0; b < original.functions[f].blocks.size(); ++b) {
        const auto &bl = original.functions[f].blocks[b];
        for (const auto &p : bl.parameters) {
          value_types[p.value.value] = p.type;
          definitions.push_back({p.value, p.type, original.functions[f].source,
                                 b, 0});
        }
        for (size_t p = 0; p < bl.instructions.size(); ++p) {
          const auto &v = bl.instructions[p];
          if (v.result) {
            value_types[v.result->value] = v.type;
            definitions.push_back({*v.result, v.type, v.source, b, p + 1});
          }
        }
      }
      auto contains = [](SourceSpan inner, SourceSpan outer) {
        return inner.snapshot == outer.snapshot &&
               inner.begin_byte >= outer.begin_byte &&
               inner.end_byte <= outer.end_byte;
      };
      for (size_t b = 0; b < original.functions[f].blocks.size(); ++b) {
        const auto &block = original.functions[f].blocks[b];
        for (size_t i = 0; i < block.instructions.size(); ++i) {
          if (block.instructions[i].op != ir::Op::convert)
            continue;
          auto found = value_types.find(block.instructions[i].inputs[0].value);
          need(found != value_types.end(), "convert input has no type");
          const auto source_type = found->second;
          need(block.instructions[i].result.has_value(),
               "conversion without a result value");
          const auto convert_result = *block.instructions[i].result;
          need(block.instructions[i].origin.has_value(),
               "lowered conversion has no HIR origin");
          const auto origin = *block.instructions[i].origin;
          need(origin.value < original.hir.module().nodes.size(),
               "conversion origin is out of range");
          const auto &hir_origin = original.hir.module().nodes[origin.value];
          need(hir_origin.kind() == hir::NodeKind::integer_convert &&
                   hir_origin.children.size() == 1,
               "conversion origin is not a HIR conversion");
          const auto &hir_operand =
              original.hir.module().nodes[hir_origin.children[0].value];
          // Locate the conversion in the mutated container by its result.
          // Never keep an element reference across a mutation callback: the
          // bool-source/address callbacks insert into this same vector, which
          // shifts elements and may reallocate the buffer (ASan UAF).
          auto locate = [](ir::Module &module, size_t function, size_t block,
                           ValueId result) -> ir::Instruction * {
            for (auto &instruction :
                 module.functions[function].blocks[block].instructions)
              if (instruction.op == ir::Op::convert && instruction.result &&
                  *instruction.result == result)
                return &instruction;
            return nullptr;
          };
          const auto source_entry = types.find(source_type);
          need(source_entry != nullptr, "conversion source type missing");
          const bool source_float = source_entry->kind == hir::TypeKind::Float;
          const auto target_entry = types.find(block.instructions[i].type);
          need(target_entry != nullptr, "conversion target type missing");
          const bool target_float = target_entry->kind == hir::TypeKind::Float;
          auto corrupt = [&](const char *label, auto change) {
            auto m = original;
            auto *first = locate(m, f, b, convert_result);
            need(first != nullptr,
                 std::string("original conversion missing before ") + label);
            const auto before_type = first->type;
            const auto before_inputs = first->inputs;
            const auto before_origin = first->origin;
            const auto before_size =
                m.functions[f].blocks[b].instructions.size();
            change(m);
            auto *instruction = locate(m, f, b, convert_result);
            need(instruction != nullptr,
                 std::string("original conversion lost after ") + label);
            if (m.functions[f].blocks[b].instructions.size() != before_size)
              ++insert_mutations;
            // Every listed mutation must be a real change, never a no-op.
            need(instruction->type != before_type ||
                     instruction->inputs != before_inputs ||
                     instruction->origin != before_origin,
                 std::string("no-op mutation ") + label);
            auto r = ir::verify(m);
            if (r.success())
              std::cerr << "accepted mutation " << label << " f=" << f
                        << " b=" << b << " i=" << i << " types "
                        << source_type.value << "->"
                        << instruction->type.value << " kinds "
                        << int(source_entry->kind) << " -> "
                        << int(types.find(instruction->type)->kind)
                        << " blocks=" << original.functions[f].blocks.size()
                        << " insns=" << block.instructions.size() << '\n';
            need(!r.success() && !r.value && r.failure == FailureKind::internal &&
                     !r.diagnostics.empty() &&
                     r.diagnostics[0].code == "E_IR_INVALID",
                 "corrupt conversion accepted");
            ++rejected;
          };
          if (boolean.value) {
            // A bool destination or a bool source is never a numeric pair.
            ++bool_mutations;
            corrupt("bool destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = boolean;
            });
          }
          if (source_float && narrow_float.value &&
              narrow_float != source_type && !target_float) {
            // float -> int rewritten as another float destination. For an
            // f64 source this is actual narrowing; for f32 it is a legal
            // width pair whose HIR target no longer matches.
            ++float_width_mutations;
            corrupt("float destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = narrow_float;
            });
          }
          if (source_float && target_float && narrow_float.value &&
              target_entry->bits == 64) {
            // f64 -> f64 identity rewritten as actual f64 -> f32 narrowing.
            ++float_width_mutations;
            corrupt("float narrowing target", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = narrow_float;
            });
          }
          if (source_float && target_float && wide_float.value &&
              target_entry->bits == 32) {
            // f32 -> f32 identity rewritten as a legal-looking widening pair;
            // the real HIR conversion still targets f32.
            ++float_width_mutations;
            corrupt("float widening retarget", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = wide_float;
            });
          }
          if (!source_float && boolean.value) {
            // int -> float rewritten as int -> bool.
            ++bool_mutations;
            corrupt("bool destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = boolean;
            });
          }
          if (boolean.value) {
            // Replace the real source value with a bool constant. This mutates
            // the conversion and then inserts before it in the same vector.
            ++bool_mutations;
            corrupt("bool source", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              ValueId next = max_value(m.functions[f]);
              ir::Instruction c;
              c.op = ir::Op::constant;
              c.type = boolean;
              c.result = next;
              c.source = instruction->source;
              instruction->inputs[0] = next;
              auto &instructions = m.functions[f].blocks[b].instructions;
              const auto position = std::find_if(
                  instructions.begin(), instructions.end(),
                  [&](const ir::Instruction &v) {
                    return v.result && *v.result == convert_result;
                  });
              need(position != instructions.end(),
                   "original conversion missing before insert");
              instructions.insert(position, c);
            });
          }
          {
            // The origin must identify this exact HIR conversion. Removing it
            // and replacing it with a real non-conversion HIR node are both
            // actual malformed graphs, not mere missing metadata on an
            // otherwise unrelated value.
            ++origin_mutations;
            corrupt("conversion origin removed", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->origin.reset();
            });
            ++origin_mutations;
            corrupt("conversion origin is not a conversion", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->origin = hir_origin.children[0];
            });
            std::optional<HirNodeId> other_origin;
            for (size_t n = 0; n < original.hir.module().nodes.size(); ++n) {
              const auto &candidate = original.hir.module().nodes[n];
              if (n != origin.value &&
                  candidate.kind() == hir::NodeKind::integer_convert &&
                  original.hir.module().scopes.at(candidate.scope.value)
                          .function == original.functions[f].symbol) {
                other_origin = HirNodeId{n};
                break;
              }
            }
            if (other_origin) {
              ++origin_mutations;
              corrupt("conversion origin is another conversion", [&](auto &m) {
                auto *instruction = locate(m, f, b, convert_result);
                need(instruction != nullptr, "original conversion missing");
                instruction->origin = *other_origin;
              });
            }
          }
          {
            // The real HIR operand may be a compound expression. Replacing
            // only the SSA input with an internal same-typed child keeps the
            // origin and every SourceSpan unchanged, but is not the value of
            // the complete operand and must be rejected.
            std::optional<size_t> input_position;
            for (size_t p = 0; p < i; ++p)
              if (block.instructions[p].result &&
                  *block.instructions[p].result ==
                      block.instructions[i].inputs[0])
                input_position = p;
            if (input_position) {
              const auto &input_instruction =
                  block.instructions[*input_position];
              if (hir_operand.kind() == hir::NodeKind::binary &&
                  input_instruction.op == ir::Op::binary &&
                  input_instruction.inputs.size() == 2) {
                for (size_t side = 0; side < 2; ++side) {
                  const auto child = input_instruction.inputs[side];
                  if (child == block.instructions[i].inputs[0])
                    continue;
                  auto child_type = value_types.find(child.value);
                  need(child_type != value_types.end() &&
                           child_type->second == source_type,
                       "compound child type differs");
                  ++compound_operand_mutations;
                  corrupt(side == 0 ? "compound operand replaced by left child"
                                    : "compound operand replaced by right child",
                          [&](auto &m) {
                    auto *instruction = locate(m, f, b, convert_result);
                    need(instruction != nullptr,
                         "original conversion missing");
                    instruction->inputs[0] = child;
                  });
                }
              }
              if (hir_operand.kind() == hir::NodeKind::unary &&
                  input_instruction.op == ir::Op::unary &&
                  input_instruction.inputs.size() == 1) {
                const auto child = input_instruction.inputs[0];
                if (child != block.instructions[i].inputs[0]) {
                  auto child_type = value_types.find(child.value);
                  need(child_type != value_types.end() &&
                           child_type->second == source_type,
                       "unary child type differs");
                  ++unary_operand_mutations;
                  corrupt("unary operand replaced by pre-negation child",
                          [&](auto &m) {
                    auto *instruction = locate(m, f, b, convert_result);
                    need(instruction != nullptr,
                         "original conversion missing");
                    instruction->inputs[0] = child;
                  });
                }
              }
            }
          }
          {
            std::optional<ValueId> other;
            bool other_inside = false;
            for (const auto &definition : definitions) {
              if (definition.value == block.instructions[i].inputs[0] ||
                  definition.type != source_type)
                continue;
              if (definition.block > b ||
                  (definition.block == b && definition.position > i))
                continue;
              other = definition.value;
              other_inside = contains(definition.source, hir_operand.source.span);
              break;
            }
            if (other) {
              ++operand_mutations;
              if (!other_inside)
                ++external_operand_mutations;
              corrupt("same-type other operand", [&](auto &m) {
                auto *instruction = locate(m, f, b, convert_result);
                need(instruction != nullptr, "original conversion missing");
                instruction->inputs[0] = *other;
              });
            }
          }
          {
            std::optional<LocalSlotId> slot;
            for (size_t s = 0; s < original.functions[f].slots.size(); ++s)
              if (original.functions[f].slots[s].type == source_type &&
                  original.functions[f].slots[s].parameter) {
                slot = LocalSlotId{s};
                break;
              }
            if (slot) {
              // Insert a real, initialized address value and use it where the
              // numeric scalar operand is required.
              ++address_mutations;
              corrupt("address operand", [&](auto &m) {
                auto *instruction = locate(m, f, b, convert_result);
                need(instruction != nullptr, "original conversion missing");
                ir::Instruction address;
                address.op = ir::Op::address;
                address.type = source_type;
                address.result = max_value(m.functions[f]);
                address.slot = *slot;
                address.source = instruction->source;
                instruction->inputs[0] = *address.result;
                auto &instructions = m.functions[f].blocks[b].instructions;
                const auto position = std::find_if(
                    instructions.begin(), instructions.end(),
                    [&](const ir::Instruction &v) {
                      return v.result && *v.result == convert_result;
                    });
                need(position != instructions.end(),
                     "original conversion missing before address insert");
                instructions.insert(position, address);
              });
            }
          }
          ++mutations;
        }
      }
    }
    need(mutations > 0, "project has no conversion instruction to mutate");
    // The insertion path must actually run, so the relocation is exercised.
    need(insert_mutations >= 1,
         "no insertion mutation exercised the relocation path");
    // Positive controls: the same verifier must accept each legal pair. Clone
    // real lowered conversions, preserving their HIR origin and operand, so a
    // refusal above is the real rule and not a blanket rejection or missing
    // metadata.
    size_t controls = 0;
    std::set<std::tuple<int, unsigned, int, unsigned>> control_pairs;
    for (size_t f = 0; f < original.functions.size(); ++f)
      for (size_t b = 0; b < original.functions[f].blocks.size(); ++b) {
        const auto &block = original.functions[f].blocks[b];
        for (const auto &source_instruction : block.instructions) {
          if (source_instruction.op != ir::Op::convert ||
              !source_instruction.result || source_instruction.inputs.empty())
            continue;
          auto source_found =
              std::find_if(block.parameters.begin(), block.parameters.end(),
                           [&](const auto &p) {
                             return p.value == source_instruction.inputs[0];
                           });
          std::optional<TypeId> source_type;
          if (source_found != block.parameters.end())
            source_type = source_found->type;
          else
            for (const auto &v : block.instructions)
              if (v.result &&
                  *v.result == source_instruction.inputs[0])
                source_type = v.type;
          need(source_type.has_value(), "control source type missing");
          const auto *st = types.find(*source_type);
          const auto *tt = types.find(source_instruction.type);
          need(st != nullptr && tt != nullptr, "control type missing");
          auto key = std::tuple{int(st->kind), st->bits, int(tt->kind), tt->bits};
          if (!control_pairs.insert(key).second)
            continue;
          auto m = original;
          auto &fn = m.functions[f];
          auto &bl = fn.blocks[b];
          ir::Instruction c = source_instruction;
          c.result = max_value(fn);
          bl.instructions.push_back(c);
          need(ir::verify(m).success(),
               "legal conversion control rejected");
          ++controls;
        }
      }
    need(controls >= 5, "expected five legal numeric conversion pair controls");
    SymbolId entry;
    std::map<std::string, SymbolId> names;
    for (const auto &s : resolved.value->symbols)
      if (s.kind == modules::SymbolKind::function) {
        names.emplace(s.name, s.id);
        if (s.name == "main")
          entry = s.id;
      }
    backend::native::Options options;
    options.optimization = unsigned(std::stoul(argv[3]));
    auto emitted = backend::native::emit(*lowered.value, entry, options);
    need(emitted.success(), "emit");
    write(out / "generated.o", std::string(emitted.value->object.begin(),
                                           emitted.value->object.end()));
    write(out / "before.ll", emitted.value->llvm_before_optimization);
    write(out / "after.ll", emitted.value->llvm_ir);
    write(out / "generated.nir", emitted.value->native_ir);
    need(emitted.value->llvm_before_optimization.find("fptrunc") ==
             std::string::npos,
         "unexpected fptrunc in float identity/widening IR");
    need(emitted.value->llvm_before_optimization.find("fpext float") !=
             std::string::npos,
         "missing fpext for f32->f64 widening");
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto load_ir = [&](const std::string &ir) {
      auto m = llvm::parseAssemblyString(ir, error, context);
      need(m && !llvm::verifyModule(*m), "verify emitted LLVM");
      for (auto &fn : *m) {
        need(!fn.getName().starts_with("llvm.fma") &&
                 !fn.getName().starts_with("llvm.fmuladd") &&
                 !fn.getName().starts_with("llvm.trunc") &&
                 fn.getName() != "trunc" && fn.getName() != "truncf" &&
                 fn.getName() != "truncl",
             "unexpected helper declaration " + fn.getName().str());
        for (auto &bb : fn)
          for (auto &instr : bb)
            if (llvm::isa<llvm::FPMathOperator>(instr))
              need(!instr.getFastMathFlags().any(), "fast/contract flag");
      }
      return m;
    };
    load_ir(emitted.value->llvm_ir);
    auto m = load_ir(emitted.value->llvm_before_optimization);
    auto *main = m->getFunction("main");
    main->deleteBody();
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "observe", main));
    auto writer = m->getOrInsertFunction(
        "write", llvm::FunctionType::get(
                     builder.getInt64Ty(),
                     {builder.getInt32Ty(), builder.getPtrTy(),
                      builder.getInt64Ty()},
                     false));
    std::ifstream requests(argv[4]);
    need(bool(requests), "open requests");
    std::string line;
    size_t count = 0;
    while (std::getline(requests, line)) {
      std::istringstream row(line);
      std::string name, result, bits;
      row >> name >> result;
      need(names.contains(name), "missing function " + name);
      auto *fn =
          m->getFunction("lang.fn." + std::to_string(names.at(name).value));
      need(fn, "missing generated function");
      unsigned wanted = result == "f32"   ? 32
                        : result == "f64" ? 64
                                          : unsigned(std::stoul(result.substr(1)));
      auto *ret = fn->getReturnType();
      if (result == "f32")
        need(ret->isFloatTy(), "actual return type is not f32 for " + name);
      else if (result == "f64")
        need(ret->isDoubleTy(), "actual return type is not f64 for " + name);
      else
        need(ret->isIntegerTy(wanted),
           "actual return width differs for " + name + " as " + result);
      std::vector<llvm::Value *> args;
      while (row >> bits) {
        need(args.size() < fn->arg_size(), "extra argument");
        auto *t = fn->getFunctionType()->getParamType(args.size());
        unsigned width = 0;
        if (t->isFloatTy())
          width = 32;
        else if (t->isDoubleTy())
          width = 64;
        else if (t->isIntegerTy())
          width = t->getIntegerBitWidth();
        need(width && bits.size() == width / 4,
             "actual argument width differs from request");
        auto *raw = llvm::ConstantInt::get(context, llvm::APInt(width, bits, 16));
        args.push_back(t->isIntegerTy()
                           ? static_cast<llvm::Value *>(raw)
                           : llvm::ConstantExpr::getBitCast(raw, t));
      }
      need(args.size() == fn->arg_size(), "argument count");
      auto *actual = builder.CreateCall(fn->getFunctionType(), fn, args);
      auto *raw = actual->getType()->isFloatingPointTy()
                      ? builder.CreateBitCast(actual,
                                              builder.getIntNTy(wanted))
                      : actual;
      auto *slot = builder.CreateAlloca(raw->getType());
      builder.CreateStore(raw, slot);
      builder.CreateCall(writer,
                         {builder.getInt32(1), slot,
                          builder.getInt64(wanted / 8)});
      ++count;
    }
    builder.CreateRet(builder.getInt32(0));
    need(!llvm::verifyModule(*m), "observation module invalid");
    std::string text;
    llvm::raw_string_ostream stream(text);
    m->print(stream, nullptr);
    write(out / "observe.ll", text);
    std::string metadata = "{\"status\":\"passed\",\"probes\":" +
                           std::to_string(count) +
                           ",\"bad_ir_rejected\":" + std::to_string(rejected) +
                           ",\"convert_instructions\":" +
                           std::to_string(mutations) +
                           ",\"shortened_operand_rejections\":" +
                           std::to_string(shortened_operand_rejections) +
                           ",\"insert_mutations\":" +
                           std::to_string(insert_mutations) +
                           ",\"legal_retarget_controls\":" +
                           std::to_string(controls) +
                           ",\"bool_mutations\":" +
                           std::to_string(bool_mutations) +
                           ",\"float_width_mutations\":" +
                           std::to_string(float_width_mutations) +
                           ",\"origin_mutations\":" +
                           std::to_string(origin_mutations) +
                           ",\"operand_mutations\":" +
                           std::to_string(operand_mutations) +
                           ",\"address_mutations\":" +
                           std::to_string(address_mutations) +
                           ",\"compound_operand_mutations\":" +
                           std::to_string(compound_operand_mutations) +
                           ",\"unary_operand_mutations\":" +
                           std::to_string(unary_operand_mutations) +
                           ",\"external_operand_mutations\":" +
                           std::to_string(external_operand_mutations) + "}\n";
    write(out / "metadata.json", metadata);
    std::cout << metadata;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

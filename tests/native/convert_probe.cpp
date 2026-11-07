// Test-only observation for explicit numeric conversions.
//
// The verified Native IR is emitted exactly like the production CLI, then the
// generated functions are called through their actual LLVM FunctionType with
// bit patterns supplied by the test runner. Return bits are written raw, so the
// runner compares integers and IEEE values without any C++ ABI guess.
//
// The same program also mutates conversion instructions in the verified Native
// IR and requires the independent Native IR verifier to reject every real
// source/target mutation (float->float, bool source/destination). A mutation
// may insert into the same instruction vector, so the original conversion is
// re-located by its stable result ValueId instead of keeping a reference.
#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
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
#include <sstream>
#include <string>
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
    auto lowered = ir::lower(*checked.value);
    need(lowered.success(), "lower verified HIR");
    const auto original = lowered.value->module();
    const auto &types = original.hir.module().types;
    TypeId boolean{}, narrow_float{};
    for (uint64_t n = 1; n <= types.size(); ++n) {
      auto *t = types.find(TypeId{n});
      if (!t)
        continue;
      if (t->kind == hir::TypeKind::Bool)
        boolean = TypeId{n};
      if (t->kind == hir::TypeKind::Float && t->bits == 32)
        narrow_float = TypeId{n};
    }
    // Every conversion instruction is real material; mutate its destination
    // and source types without touching the verified HIR and require a refusal.
    // The original conversion is identified by its result ValueId, a stable
    // value identity, because a callback may insert into the same vector.
    size_t rejected = 0, mutations = 0, insert_mutations = 0;
    for (size_t f = 0; f < original.functions.size(); ++f) {
      // ValueIds are function-local, so the source type is resolved inside the
      // same function that owns the conversion.
      std::map<uint64_t, TypeId> value_types;
      for (const auto &bl : original.functions[f].blocks) {
        for (const auto &p : bl.parameters)
          value_types[p.value.value] = p.type;
        for (const auto &v : bl.instructions)
          if (v.result)
            value_types[v.result->value] = v.type;
      }
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
          const ValueId convert_result = *block.instructions[i].result;
          // Locate the conversion in the mutated container by its result.
          // Never keep an element reference across a mutation callback: the
          // bool-source callback inserts into this same vector, which shifts
          // elements and may reallocate the buffer (ASan heap-use-after-free).
          auto locate = [](ir::Module &module, size_t function, size_t block,
                           ValueId result) -> ir::Instruction * {
            for (auto &instruction :
                 module.functions[function].blocks[block].instructions)
              if (instruction.op == ir::Op::convert && instruction.result &&
                  *instruction.result == result)
                return &instruction;
            return nullptr;
          };
          const bool source_float =
              types.find(source_type)->kind == hir::TypeKind::Float;
          auto corrupt = [&](const char *label, auto change) {
            auto m = original;
            auto *first = locate(m, f, b, convert_result);
            need(first != nullptr,
                 std::string("original conversion missing before ") + label);
            const auto before_type = first->type;
            const auto before_inputs = first->inputs;
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
                     instruction->inputs != before_inputs,
                 std::string("no-op mutation ") + label);
            auto r = ir::verify(m);
            if (r.success())
              std::cerr << "accepted mutation " << label << " f=" << f
                        << " b=" << b << " i=" << i << " types "
                        << source_type.value << "->"
                        << instruction->type.value << " kinds "
                        << int(types.find(source_type)->kind) << " -> "
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
            corrupt("bool destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = boolean;
            });
          }
          if (source_float && narrow_float.value &&
              narrow_float != source_type) {
            // float -> int rewritten as float -> float.
            corrupt("float destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = narrow_float;
            });
          }
          if (!source_float && boolean.value) {
            // int -> float rewritten as int -> bool.
            corrupt("bool destination", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              instruction->type = boolean;
            });
          }
          if (boolean.value) {
            // Replace the real source value with a bool constant. This mutates
            // the conversion and then inserts before it in the same vector.
            corrupt("bool source", [&](auto &m) {
              auto *instruction = locate(m, f, b, convert_result);
              need(instruction != nullptr, "original conversion missing");
              ValueId next{0};
              for (auto &fn : m.functions)
                for (auto &bl : fn.blocks) {
                  for (auto &p : bl.parameters)
                    next.value = std::max(next.value, p.value.value + 1);
                  for (auto &v : bl.instructions)
                    if (v.result)
                      next.value = std::max(next.value, v.result->value + 1);
                }
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
          ++mutations;
        }
      }
    }
    need(mutations > 0, "project has no conversion instruction to mutate");
    // The insertion path must actually run, so the relocation is exercised.
    need(insert_mutations >= 1,
         "no insertion mutation exercised the relocation path");
    // Positive controls: the same mutation machinery must accept a legal pair,
    // proving a refusal is the real rule and not a blanket rejection.
    TypeId some_float{}, some_int{}, other_int{};
    auto is_integer = [](hir::TypeKind k) {
      return k == hir::TypeKind::SignedInt || k == hir::TypeKind::UnsignedInt ||
             k == hir::TypeKind::Isize || k == hir::TypeKind::Usize;
    };
    for (uint64_t n = 1; n <= types.size(); ++n) {
      auto *t = types.find(TypeId{n});
      if (!t)
        continue;
      if (t->kind == hir::TypeKind::Float && !some_float.value)
        some_float = TypeId{n};
      if (is_integer(t->kind) && t->bits == 32 && !some_int.value)
        some_int = TypeId{n};
      if (is_integer(t->kind) && t->bits == 16 && !other_int.value)
        other_int = TypeId{n};
    }
    size_t controls = 0;
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
    auto append_control = [&](bool float_source, TypeId target) {
      for (size_t f = 0; f < original.functions.size(); ++f)
        for (size_t b = 0; b < original.functions[f].blocks.size(); ++b) {
          std::vector<std::pair<ValueId, TypeId>> available;
          for (const auto &p : original.functions[f].blocks[b].parameters)
            available.push_back({p.value, p.type});
          for (const auto &v : original.functions[f].blocks[b].instructions)
            if (v.result)
              available.push_back({*v.result, v.type});
          for (const auto &[value, value_type] : available) {
            if ((types.find(value_type)->kind == hir::TypeKind::Float) !=
                float_source)
              continue;
            auto m = original;
            auto &fn = m.functions[f];
            auto &bl = fn.blocks[b];
            ir::Instruction c;
            c.op = ir::Op::convert;
            c.type = target;
            c.result = max_value(fn);
            c.inputs = {value};
            c.source = fn.source;
            bl.instructions.push_back(c);
            if (!ir::verify(m).success())
              continue;
            ++controls;
            return true;
          }
        }
      return false;
    };
    need(append_control(false, some_float), "legal int->float control rejected");
    need(append_control(true, some_int), "legal float->int control rejected");
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
                           ",\"insert_mutations\":" +
                           std::to_string(insert_mutations) +
                           ",\"legal_retarget_controls\":" +
                           std::to_string(controls) + "}\n";
    write(out / "metadata.json", metadata);
    std::cout << metadata;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

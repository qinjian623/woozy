#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/SourceMgr.h>
#include <set>

using namespace lang;
void need(bool ok, const std::string &message) {
  if (!ok) throw std::runtime_error(message);
}
void write(const std::filesystem::path &path, const std::string &bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), bytes.size());
  need(bool(out), "write test artifact");
}
int main(int argc, char **argv) {
  try {
    need(argc == 4, "manifest, output directory and optimization required");
    std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load string source");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    if (!checked.success())
      for (auto &d : checked.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    need(checked.success(), "check real string source");
    auto type_count = checked.value->module().types.size();
    auto lowered = ir::lower(*checked.value);
    need(lowered.success(), "lower strings");
    const auto original = lowered.value->module();
    size_t rejected = 0;
    auto corrupt = [&](auto mutate) {
      auto m = original;
      bool found = false;
      for (auto &f : m.functions) for (auto &b : f.blocks) for (auto &i : b.instructions)
        if (!found && i.op == ir::Op::string_literal) { mutate(m, f, b, i); found = true; }
      need(found, "missing literal mutation target");
      auto result = ir::verify(m);
      need(!result.success() && !result.value && result.failure == FailureKind::internal &&
           result.diagnostics.at(0).code == "E_IR_INVALID", "corrupt string CFG accepted");
      ++rejected;
    };
    auto unit = [&] {
      for (uint64_t id = 1; id <= type_count; ++id)
        if (original.hir.module().types.find(TypeId{id})->kind == hir::TypeKind::Unit)
          return TypeId{id};
      throw std::runtime_error("missing shared Unit type");
    }();
    corrupt([&](auto &, auto &, auto &, auto &i) { i.type = unit; });
    corrupt([](auto &, auto &, auto &, auto &i) { i.result.reset(); });
    corrupt([](auto &, auto &, auto &, auto &i) { i.bits = 1; });
    corrupt([](auto &, auto &, auto &, auto &i) { i.inputs = {*i.result}; });
    corrupt([](auto &, auto &, auto &, auto &i) { i.op = ir::Op::constant; });
    for (std::string bytes : {"\x80", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8"})
      corrupt([&](auto &, auto &, auto &, auto &i) { i.utf8 = bytes; });
    corrupt([&](auto &, auto &, auto &, auto &i) {
      i.op = ir::Op::constant; i.type = unit; i.utf8 = "unexpected";
    });
    auto bad_hir = checked.value->module();
    for (auto &n : bad_hir.nodes)
      if (auto s = std::get_if<hir::StringLiteral>(&n.data)) { s->utf8 = "\x80"; break; }
    auto invalid = hir::verify(bad_hir);
    need(!invalid.success() && invalid.diagnostics.at(0).code == "E_IR_INVALID",
         "HIR verifier accepted invalid string");

    SymbolId entry;
    for (const auto &s : resolved.value->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function) entry = s.id;
    backend::native::Options options;
    options.optimization = unsigned(std::stoul(argv[3]));
    auto emitted = backend::native::emit(*lowered.value, entry, options);
    need(emitted.success(), "emit strings");
    need(checked.value->module().types.size() == type_count, "shared TypeContext changed");
    write(out / "generated.o", std::string(emitted.value->object.begin(), emitted.value->object.end()));
    write(out / "before.ll", emitted.value->llvm_before_optimization);
    write(out / "after.ll", emitted.value->llvm_ir);
    write(out / "strings.nir", emitted.value->native_ir);

    // Observe actual generated functions through their verified LLVM types.
    // No C++ struct, dlsym cast, guessed calling convention, or reference
    // implementation is used. Only the test main adds host write(2) calls.
    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    auto m = llvm::parseAssemblyString(emitted.value->llvm_before_optimization, error, context);
    need(m && !llvm::verifyModule(*m), "parse and verify generated LLVM");
    const auto &dl = m->getDataLayout();
    auto *length_type = dl.getIntPtrType(context);
    need(length_type->getBitWidth() == checked.value->config().word_bits,
         "length differs from target pointer width");
    std::set<std::string> literal_bytes, storage_bytes;
    // Uninstantiated templates stay in HIR but never produce LLVM storage.
    for (const auto &f : original.functions)
      for (const auto &b : f.blocks)
        for (const auto &i : b.instructions)
          if (i.op == ir::Op::string_literal) literal_bytes.insert(i.utf8);
    for (auto &g : m->globals()) {
      if (!g.getName().starts_with("string.bytes")) continue;
      need(g.isConstant() && g.hasPrivateLinkage() && g.hasGlobalUnnamedAddr(),
           "string storage is not private read-only data");
      auto *array = llvm::dyn_cast<llvm::ArrayType>(g.getValueType());
      need(array && array->getElementType()->isIntegerTy(8), "string storage element type");
      std::string bytes;
      for (unsigned j = 0; j < array->getNumElements(); ++j)
        bytes += char(llvm::cast<llvm::ConstantInt>(g.getInitializer()->getAggregateElement(j))->getZExtValue());
      need(literal_bytes.contains(bytes), "storage changed bytes or appended a terminator");
      storage_bytes.insert(bytes);
    }
    need(storage_bytes == literal_bytes, "literal storage missing from generated LLVM");
    auto *main = m->getFunction("main");
    main->deleteBody();
    llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "observe", main));
    auto writer = m->getOrInsertFunction("write", llvm::FunctionType::get(
        length_type, {builder.getInt32Ty(), builder.getPtrTy(), length_type}, false));
    std::string metadata = "{\"status\":\"passed\",\"native_ir_rejected\":" +
        std::to_string(rejected) + ",\"hir_rejected\":1,\"target_word_bits\":" +
        std::to_string(length_type->getBitWidth()) + ",\"probes\":[";
    bool first = true;
    for (const auto &f : checked.value->module().functions) {
      const auto &symbol = checked.value->module().resolved->symbols.at(f.symbol.value);
      if (!symbol.name.starts_with("probe_")) continue;
      auto *generated = m->getFunction("lang.fn." + std::to_string(f.symbol.value));
      need(generated && generated->arg_empty(), "probe function signature");
      auto *layout = llvm::dyn_cast<llvm::StructType>(generated->getReturnType());
      need(layout && layout->getNumElements() == 4 && layout->getElementType(0)->isPointerTy() &&
           layout->getElementType(1) == length_type && layout->getElementType(2) == length_type &&
           layout->getElementType(3)->isPointerTy(), "string return layout");
      auto *offsets = dl.getStructLayout(layout);
      need(offsets->getElementOffset(0) == 0 && offsets->getElementOffset(1) == 8 &&
           offsets->getElementOffset(2) == 16 && offsets->getElementOffset(3) == 24 &&
           dl.getTypeAllocSize(layout) == 32 && dl.getABITypeAlign(layout).value() == 8,
           "x86_64 string target layout changed");
      auto *value = builder.CreateCall(generated->getFunctionType(), generated);
      auto *pointer = builder.CreateExtractValue(value, {0});
      auto *length = builder.CreateExtractValue(value, {1});
      auto *slot = builder.CreateAlloca(length_type);
      builder.CreateStore(length, slot);
      builder.CreateCall(writer, {builder.getInt32(1), slot,
          llvm::ConstantInt::get(length_type, dl.getTypeStoreSize(length_type))});
      auto *rune_length = builder.CreateExtractValue(value, {2});
      builder.CreateStore(rune_length, slot);
      builder.CreateCall(writer, {builder.getInt32(1), slot,
          llvm::ConstantInt::get(length_type, dl.getTypeStoreSize(length_type))});
      builder.CreateCall(writer, {builder.getInt32(1), pointer, length});
      auto *owner = builder.CreateExtractValue(value, {3});
      auto *drop = llvm::BasicBlock::Create(context, "probe.drop", main);
      auto *next = llvm::BasicBlock::Create(context, "probe.next", main);
      builder.CreateCondBr(builder.CreateICmpNE(owner,
          llvm::ConstantPointerNull::get(builder.getPtrTy())), drop, next);
      builder.SetInsertPoint(drop);
      auto release = m->getOrInsertFunction("lang_handle_release_v1",
          llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false));
      builder.CreateCall(release, {owner});
      builder.CreateBr(next);
      builder.SetInsertPoint(next);
      metadata += std::string(first ? "" : ",") + "\"" + symbol.name + "\"";
      first = false;
    }
    builder.CreateRet(builder.getInt32(0));
    need(!llvm::verifyModule(*m), "invalid observation module");
    std::string observation;
    llvm::raw_string_ostream stream(observation);
    m->print(stream, nullptr);
    write(out / "observe.ll", observation);
    metadata += "],\"layout\":{\"size\":32,\"alignment\":8,\"offsets\":[0,8,16,24]},"
                "\"abi\":\"private LLVM function types; no C++ aggregate ABI\"}\n";
    write(out / "metadata.json", metadata);
    std::cout << metadata;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

  std::map<std::pair<TypeId, bool>, llvm::Function *> value_ops;
  std::vector<llvm::Value *> active;
  llvm::AllocaInst *storage(llvm::Type *type) {
    auto &entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> alloc(&entry, entry.begin());
    return alloc.CreateAlloca(type);
  }
  uint64_t stride(TypeId id) {
    auto size = module.getDataLayout().getTypeAllocSize(lower_type(id)).getFixedValue();
    return size ? size : 1;
  }
  llvm::Value *element_storage(TypeId id, llvm::Value *value) {
    if (module.getDataLayout().getTypeAllocSize(lower_type(id)).getFixedValue() == 0) {
      auto *address = storage(builder.getInt8Ty()); builder.CreateStore(builder.getInt8(0), address);
      return address;
    }
    auto *address = storage(lower_type(id)); builder.CreateStore(value, address); return address;
  }
  bool owning(TypeId id) const {
    return hir::classify_value(input.hir.module(), id).needs_drop;
  }
  llvm::Value *runtime(const char *name, llvm::Type *result,
                       std::vector<llvm::Value *> args) {
    std::vector<llvm::Type *> types;
    for (auto *v : args) types.push_back(v->getType());
    auto callee = module.getOrInsertFunction(name, llvm::FunctionType::get(result, types, false));
    return builder.CreateCall(callee, args);
  }
  llvm::Function *value_op(TypeId id, bool copy) {
    auto key = std::pair{id, copy};
    if (auto it = value_ops.find(key); it != value_ops.end()) return it->second;
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false),
        llvm::GlobalValue::InternalLinkage,
        std::string(copy ? "lang.copy." : "lang.drop.") + std::to_string(id.value), module);
    value_ops.emplace(key, fn);
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *address = fn->getArg(0);
    auto t = type(id);
    if (t.kind == hir::TypeKind::Vec) {
      auto *value = builder.CreateLoad(builder.getPtrTy(), address);
      runtime(copy ? "lang_vec_retain_v1" : "lang_vec_release_v1", builder.getVoidTy(), {value});
    } else if (t.kind == hir::TypeKind::NominalStruct) {
      auto fields = aggregates.fields(id);
      for (size_t n = 0; n < fields.size(); ++n) {
        size_t p = copy ? n : fields.size() - 1 - n;
        if (owning(fields[p].type))
          builder.CreateCall(value_op(fields[p].type, copy),
                             {builder.CreateStructGEP(lower_type(id), address, p)});
      }
    } else if (t.kind == hir::TypeKind::NominalEnum) {
      auto *tag = builder.CreateLoad(builder.getInt32Ty(),
                                    builder.CreateStructGEP(lower_type(id), address, 0));
      auto *done = llvm::BasicBlock::Create(context, "done", fn);
      auto *dispatch = builder.CreateSwitch(tag, done);
      const auto &variants = aggregates.declaration(id).variants;
      for (size_t v = 0; v < variants.size(); ++v) {
        auto *body = llvm::BasicBlock::Create(context, "payload", fn);
        dispatch->addCase(builder.getInt32(v), body); builder.SetInsertPoint(body);
        auto fields = aggregates.fields(id, variants[v].symbol);
        auto *payload = builder.CreateStructGEP(lower_type(id), address, v + 1);
        auto *payload_type = llvm::cast<llvm::StructType>(lower_type(id))->getElementType(v + 1);
        for (size_t n = 0; n < fields.size(); ++n) {
          size_t p = copy ? n : fields.size() - 1 - n;
          if (owning(fields[p].type))
            builder.CreateCall(value_op(fields[p].type, copy),
                               {builder.CreateStructGEP(payload_type, payload, p)});
        }
        builder.CreateBr(done);
      }
      builder.SetInsertPoint(done);
    }
    builder.CreateRetVoid(); builder.restoreIP(ip); return fn;
  }
  llvm::Value *copy_value(TypeId id, llvm::Value *value) {
    if (owning(id)) {
      auto *address = storage(lower_type(id)); builder.CreateStore(value, address);
      builder.CreateCall(value_op(id, true), {address});
    }
    return value;
  }
  void destroy_slot(size_t slot, TypeId type) {
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *drop = llvm::BasicBlock::Create(context, "destroy.live", fn);
    auto *done = llvm::BasicBlock::Create(context, "destroy.done", fn);
    builder.CreateCondBr(builder.CreateLoad(builder.getInt1Ty(), active.at(slot)), drop, done);
    builder.SetInsertPoint(drop);
    builder.CreateStore(builder.getFalse(), active.at(slot));
    builder.CreateCall(value_op(type, false), {slots.at(slot)});
    builder.CreateBr(done); builder.SetInsertPoint(done);
  }
  llvm::Value *checked_index(llvm::Value *index, TypeId id, SourceSpan source, uint32_t cause) {
    auto *wide = builder.CreateIntCast(index, builder.getIntNTy(65), signed_(type(id)));
    auto *bad = builder.CreateOr(
        builder.CreateICmpSLT(wide, llvm::ConstantInt::get(wide->getType(), 0)),
        builder.CreateICmpUGT(wide, llvm::ConstantInt::get(context, llvm::APInt::getMaxValue(64).zext(65))));
    guard(bad, cause, source);
    return builder.CreateIntCast(wide, builder.getInt64Ty(), false);
  }

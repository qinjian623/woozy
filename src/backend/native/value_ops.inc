  std::map<std::pair<TypeId, bool>, llvm::Function *> value_ops;
  llvm::Function *vec_retain_fast = nullptr, *vec_release_fast = nullptr;
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
  llvm::Value *element_storage(TypeId id, llvm::Value *value,
                               llvm::AllocaInst *&scratch) {
    const bool empty =
        module.getDataLayout().getTypeAllocSize(lower_type(id)).getFixedValue() == 0;
    if (!scratch) {
      scratch = storage(empty ? builder.getInt8Ty() : lower_type(id));
      if (empty) builder.CreateStore(builder.getInt8(0), scratch);
    }
    if (!empty) builder.CreateStore(value, scratch);
    return scratch;
  }
  bool constant_vec_element(TypeId id) const {
    const auto kind = type(id).kind;
    return kind == hir::TypeKind::Bool ||
           kind == hir::TypeKind::SignedInt ||
           kind == hir::TypeKind::UnsignedInt ||
           kind == hir::TypeKind::Isize ||
           kind == hir::TypeKind::Usize ||
           kind == hir::TypeKind::Float;
  }
  // A lexically negative float literal is an fneg applied to ConstantFP.
  // Recognize only that pure operation for Vec storage; this is not general
  // constant evaluation. APFloat::changeSign preserves the payload and the
  // sign bit, including negative zero.
  llvm::Constant *constant_vec_value(llvm::Value *value) {
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
      return constant;
    if (auto *constant = llvm::dyn_cast<llvm::ConstantFP>(value))
      return constant;
    auto *negate = llvm::dyn_cast<llvm::UnaryOperator>(value);
    if (!negate || negate->getOpcode() != llvm::Instruction::FNeg)
      return nullptr;
    auto *operand =
        llvm::dyn_cast<llvm::ConstantFP>(negate->getOperand(0));
    if (!operand) return nullptr;
    auto folded = operand->getValueAPF();
    folded.changeSign();
    return llvm::ConstantFP::get(context, folded);
  }
  // Constant scalars need no element cleanup or runtime evaluation. Copy the
  // table once, then publish the fully initialized element count.
  void constant_vec_init(llvm::Value *vec, TypeId element,
                         llvm::ArrayRef<llvm::Constant *> elements) {
    auto *element_type = lower_type(element);
    auto *array_type = llvm::ArrayType::get(element_type, elements.size());
    bool all_null = true;
    for (auto *value : elements) all_null = all_null && value->isNullValue();
    llvm::Constant *initializer =
        all_null ? llvm::ConstantAggregateZero::get(array_type)
                 : llvm::ConstantArray::get(array_type, elements);
    auto *data = new llvm::GlobalVariable(
        module, array_type, true, llvm::GlobalValue::PrivateLinkage,
        initializer, "vec.constants");
    data->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    data->setAlignment(module.getDataLayout().getABITypeAlign(array_type));

    auto *target = vec_field(vec, 5, builder.getPtrTy());
    auto bytes = module.getDataLayout().getTypeAllocSize(array_type).getFixedValue();
    vec_memory(builder.CreateMemCpy(target, llvm::MaybeAlign(1), data, data->getAlign(), bytes), false);
    vec_memory(builder.CreateStore(builder.getInt64(elements.size()),
                                   builder.CreateStructGEP(vec_type, vec, 2)), true);
  }
  llvm::Value *element_storage(TypeId id, llvm::Value *value) {
    llvm::AllocaInst *scratch = nullptr;
    return element_storage(id, value, scratch);
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
  // The test-only Vec observer is a weak declaration. A null resolved address
  // means no observer is linked, so the common refcount updates can stay
  // inline. Any resolved observer, including a reentrant one, keeps the
  // pre-existing runtime path and therefore its exact event order. The
  // overflow cases also return to the runtime: retain at UINT64_MAX and
  // release at zero or at the final reference. Refcount 1 must stay with the
  // runtime's iteration check, reverse element drop and free.
  llvm::Function *vec_rc(bool retain) {
    auto *&cached = retain ? vec_retain_fast : vec_release_fast;
    if (cached) return cached;
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage,
        retain ? "lang.vec.retain" : "lang.vec.release", module);
    cached = fn;
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *vec = fn->getArg(0);
    auto observer = module.getOrInsertFunction(
        "lang_vec_observe_v1",
        llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getInt32Ty(), builder.getPtrTy(),
                                 builder.getInt64Ty()},
                                false));
    auto *observer_decl = llvm::cast<llvm::Function>(observer.getCallee());
    observer_decl->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);
    auto runtime_fn = module.getOrInsertFunction(
        retain ? "lang_vec_retain_v1" : "lang_vec_release_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto *fast = llvm::BasicBlock::Create(context, "fast", fn);
    auto *slow = llvm::BasicBlock::Create(context, "runtime", fn);
    auto *update = llvm::BasicBlock::Create(context, "update", fn);
    auto *done = llvm::BasicBlock::Create(context, "done", fn);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            observer_decl, llvm::ConstantPointerNull::get(builder.getPtrTy())),
        slow, fast);
    builder.SetInsertPoint(fast);
    auto *refs = vec_field(vec, 0, builder.getInt64Ty());
    auto *defer =
        retain ? builder.CreateICmpEQ(
                     refs, llvm::ConstantInt::get(builder.getInt64Ty(),
                                                  llvm::APInt::getMaxValue(64)))
               : builder.CreateICmpULT(refs, builder.getInt64(2));
    builder.CreateCondBr(defer, slow, update);
    builder.SetInsertPoint(update);
    auto *next = retain ? builder.CreateAdd(refs, builder.getInt64(1))
                        : builder.CreateSub(refs, builder.getInt64(1));
    vec_memory(builder.CreateStore(
                   next, builder.CreateStructGEP(vec_type, vec, 0)),
               true);
    builder.CreateBr(done);
    builder.SetInsertPoint(slow);
    builder.CreateCall(runtime_fn, {vec});
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return fn;
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
      builder.CreateCall(vec_rc(copy), {value});
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

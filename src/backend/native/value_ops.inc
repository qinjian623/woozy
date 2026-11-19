  std::map<std::pair<TypeId, bool>, llvm::Function *> value_ops;
  std::map<std::pair<uint64_t, uint64_t>, llvm::GlobalVariable *> trait_vtables;
  std::map<uint64_t, llvm::Function *> trait_drop_fns, trait_clone_fns;
  llvm::Function *vec_retain_fast = nullptr, *vec_release_fast = nullptr;
  std::vector<llvm::Value *> active;
  llvm::AllocaInst *storage(llvm::Type *type, const char *name = "") {
    auto &entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> alloc(&entry, entry.begin());
    return alloc.CreateAlloca(type, nullptr, name);
  }
  void start_lifetime(llvm::AllocaInst *alloca_inst) {
    if (alloca_inst)
      builder.CreateLifetimeStart(alloca_inst);
  }
  void end_lifetime(llvm::AllocaInst *alloca_inst) {
    if (alloca_inst)
      builder.CreateLifetimeEnd(alloca_inst);
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
  // A Function or Weak always owns a runtime reference (its environment/control
  // slot) even while the shared HIR classifier still treats Function as
  // trivial. Aggregates containing either must be copied/dropped field-wise.
  bool contains_function(TypeId id, std::set<uint64_t> &active,
                         unsigned depth) {
    if (depth > 64) return false;
    const auto &t = type(id);
    if (t.kind == hir::TypeKind::Function || t.kind == hir::TypeKind::Weak)
      return true;
    if (t.kind != hir::TypeKind::NominalStruct &&
        t.kind != hir::TypeKind::NominalEnum)
      return false;
    if (!active.insert(id.value).second) return false;
    if (t.kind == hir::TypeKind::NominalStruct) {
      for (auto field : aggregates.fields(id))
        if (contains_function(field.type, active, depth + 1)) {
          active.erase(id.value);
          return true;
        }
    } else {
      for (const auto &variant : aggregates.declaration(id).variants)
        for (auto field : aggregates.fields(id, variant.symbol))
          if (contains_function(field.type, active, depth + 1)) {
            active.erase(id.value);
            return true;
          }
    }
    active.erase(id.value);
    return false;
  }
  bool owning(TypeId id) {
    std::set<uint64_t> active;
    return hir::classify_value(input.hir.module(), id).needs_drop ||
           contains_function(id, active, 0);
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
    llvm::IRBuilderBase::InsertPointGuard debug_guard(builder);
    builder.SetCurrentDebugLocation({});
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
    auto *check = llvm::BasicBlock::Create(context, "check", fn);
    auto *update = llvm::BasicBlock::Create(context, "update", fn);
    auto *done = llvm::BasicBlock::Create(context, "done", fn);
    builder.CreateCondBr(
        builder.CreateICmpNE(
            observer_decl, llvm::ConstantPointerNull::get(builder.getPtrTy())),
        slow, fast);
    builder.SetInsertPoint(fast);
    auto *address = builder.CreateStructGEP(vec_type, vec, 0);
    auto *initial = builder.CreateLoad(builder.getInt64Ty(), address);
    initial->setAlignment(llvm::Align(8));
    initial->setAtomic(llvm::AtomicOrdering::Monotonic);
    vec_memory(initial, true);
    builder.CreateBr(check);
    builder.SetInsertPoint(check);
    auto *refs = builder.CreatePHI(builder.getInt64Ty(), 2);
    refs->addIncoming(initial, fast);
    auto *defer =
        retain ? builder.CreateICmpEQ(
                     refs, llvm::ConstantInt::get(builder.getInt64Ty(),
                                                  llvm::APInt::getMaxValue(64)))
               : builder.CreateICmpULT(refs, builder.getInt64(2));
    builder.CreateCondBr(defer, slow, update);
    builder.SetInsertPoint(update);
    auto *next = retain ? builder.CreateAdd(refs, builder.getInt64(1))
                        : builder.CreateSub(refs, builder.getInt64(1));
    auto *exchange = builder.CreateAtomicCmpXchg(
        address, refs, next, llvm::Align(8),
        retain ? llvm::AtomicOrdering::Monotonic : llvm::AtomicOrdering::Release,
        llvm::AtomicOrdering::Monotonic);
    vec_memory(exchange, true);
    refs->addIncoming(builder.CreateExtractValue(exchange, 0), update);
    builder.CreateCondBr(builder.CreateExtractValue(exchange, 1), done, check);
    builder.SetInsertPoint(slow);
    builder.CreateCall(runtime_fn, {vec});
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
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
    llvm::IRBuilderBase::InsertPointGuard debug_guard(builder);
    builder.SetCurrentDebugLocation({});
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *address = fn->getArg(0);
    auto t = type(id);
    if (t.kind == hir::TypeKind::Vec) {
      auto *value = builder.CreateLoad(builder.getPtrTy(), address);
      builder.CreateCall(vec_rc(copy), {value});
    } else if (t.kind == hir::TypeKind::Function) {
      auto *value = builder.CreateLoad(fn_type, address);
      auto *env = builder.CreateExtractValue(value, {1});
      auto *present = llvm::BasicBlock::Create(context, "fn.env", fn);
      auto *done = llvm::BasicBlock::Create(context, "fn.done", fn);
      builder.CreateCondBr(
          builder.CreateICmpNE(env,
                               llvm::ConstantPointerNull::get(
                                   builder.getPtrTy())),
          present, done);
      builder.SetInsertPoint(present);
      builder.CreateCall(
          module.getOrInsertFunction(
              copy ? "lang_vec_retain_v1" : "lang_vec_release_v1",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getPtrTy()}, false)),
          {env});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    } else if (t.kind == hir::TypeKind::Weak) {
      auto *value = builder.CreateLoad(weak_type, address);
      auto *control = builder.CreateExtractValue(value, {0});
      auto *present = llvm::BasicBlock::Create(context, "weak.control", fn);
      auto *done = llvm::BasicBlock::Create(context, "weak.done", fn);
      builder.CreateCondBr(
          builder.CreateICmpNE(control,
                               llvm::ConstantPointerNull::get(
                                   builder.getPtrTy())),
          present, done);
      builder.SetInsertPoint(present);
      builder.CreateCall(
          module.getOrInsertFunction(
              copy ? "lang_weak_retain_v1" : "lang_weak_release_v1",
              llvm::FunctionType::get(builder.getVoidTy(),
                                      {builder.getPtrTy()}, false)),
          {control});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    } else if (t.kind == hir::TypeKind::Slice) {
      // The storage already holds a live 24-byte descriptor for copy; drop
      // clears it before ending the pin and releasing the strong reference.
      runtime(copy ? "lang_slice_copy_v1" : "lang_slice_drop_v1",
              builder.getVoidTy(), {address});
    } else if (t.kind == hir::TypeKind::String) {
      auto *value = builder.CreateLoad(lower_type(id), address);
      auto *owner = builder.CreateExtractValue(value, {3});
      auto *present = llvm::BasicBlock::Create(context, "text.owner", fn);
      auto *done = llvm::BasicBlock::Create(context, "text.done", fn);
      builder.CreateCondBr(builder.CreateICmpNE(owner,
          llvm::ConstantPointerNull::get(builder.getPtrTy())), present, done);
      builder.SetInsertPoint(present);
      runtime(copy ? "lang_handle_retain_v1" : "lang_handle_release_v1", builder.getVoidTy(), {owner});
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
    } else if (t.kind == hir::TypeKind::ThreadHandle ||
               t.kind == hir::TypeKind::MutexHandle ||
               t.kind == hir::TypeKind::CondvarHandle ||
               t.kind == hir::TypeKind::AtomicI64Handle || t.kind == hir::TypeKind::IpcHandle) {
      runtime(copy ? "lang_handle_retain_v1" : "lang_handle_release_v1",
              builder.getVoidTy(), {builder.CreateLoad(builder.getPtrTy(), address)});
    } else if (t.kind == hir::TypeKind::Task) {
      // A Task handle owns one executor reference. Copies retain it and every
      // owning slot releases exactly once; the promise value stays alive until
      // the last handle is gone, independent of how often it was awaited.
      runtime(copy ? "lang_async_task_retain_v1" : "lang_async_task_release_v1",
              builder.getVoidTy(),
              {builder.CreateLoad(builder.getPtrTy(), address)});
    } else if (t.kind == hir::TypeKind::CForeignResource) {
      const auto &decl = input.hir.module().resolved->project->c_type(t.c_type);
      if (decl.registration && copy)
        unsupported("managed registration resource is move-only");
      auto *value = builder.CreateLoad(builder.getPtrTy(), address);
      if (copy) {
        if (decl.retain_symbol.empty())
          unsupported("C resource has no retain_symbol and cannot be copied");
        builder.CreateCall(module.getOrInsertFunction(
            cffi::retain_symbol(t.c_type),
            llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false)), {value});
      } else {
        used_c_resources.insert(t.c_type);
        builder.CreateCall(module.getOrInsertFunction(
            cffi::release_symbol(t.c_type),
            llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()}, false)), {value});
      }
    } else if (t.kind == hir::TypeKind::TraitObject) {
      auto *value = builder.CreateLoad(dyn_type, address);
      auto *data = builder.CreateExtractValue(value, {0});
      auto *vtable = builder.CreateExtractValue(value, {1});
      auto *slot = builder.CreateGEP(
          builder.getPtrTy(), vtable,
          builder.getInt32(copy ? 1u : 0u));
      auto *callback = builder.CreateLoad(builder.getPtrTy(), slot);
      if (copy) {
        auto *clone =
            builder.CreateCall(llvm::FunctionType::get(builder.getPtrTy(),
                                                       {builder.getPtrTy()},
                                                       false),
                               callback, {data});
        auto *next = builder.CreateInsertValue(value, clone, {0});
        builder.CreateStore(next, address);
      } else {
        builder.CreateCall(
            llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                    false),
            callback, {data});
      }
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
    builder.CreateRetVoid(); return fn;
  }
  llvm::Function *trait_box_op(TypeId concrete, bool clone) {
    auto &cache = clone ? trait_clone_fns : trait_drop_fns;
    if (auto it = cache.find(concrete.value); it != cache.end())
      return it->second;
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(
            clone ? static_cast<llvm::Type *>(builder.getPtrTy())
                  : static_cast<llvm::Type *>(builder.getVoidTy()),
            {builder.getPtrTy()}, false),
        llvm::GlobalValue::InternalLinkage,
        std::string(clone ? "lang.trait.clone." : "lang.trait.drop.") +
            std::to_string(concrete.value),
        module);
    cache.emplace(concrete.value, fn);
    llvm::IRBuilderBase::InsertPointGuard debug_guard(builder);
    builder.SetCurrentDebugLocation({});
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *data = fn->getArg(0);
    auto *call = runtime(clone ? "lang_trait_box_clone_v1"
                              : "lang_trait_box_release_v1",
                         clone ? static_cast<llvm::Type *>(builder.getPtrTy())
                               : static_cast<llvm::Type *>(builder.getVoidTy()),
                         {data});
    if (clone)
      builder.CreateRet(call);
    else
      builder.CreateRetVoid();
    return fn;
  }
  llvm::Function *trait_method_thunk(TypeId dyn, TypeId concrete,
                                     SymbolId target) {
    const auto *hf = hir_functions.at(target);
    const auto target_sig = type(hf->signature);
    std::vector<llvm::Type *> parameters{builder.getPtrTy()};
    for (size_t i = 1; i < target_sig.parameters.size(); ++i)
      parameters.push_back(lower_type(target_sig.parameters[i].type));
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(lower_type(target_sig.result), parameters,
                                false),
        llvm::GlobalValue::InternalLinkage,
        "lang.trait.method." + std::to_string(dyn.value) + "." +
            std::to_string(concrete.value) + "." +
            std::to_string(target.value),
        module);
    llvm::IRBuilderBase::InsertPointGuard debug_guard(builder);
    builder.SetCurrentDebugLocation({});
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *data = fn->getArg(0);
    auto *loaded = builder.CreateLoad(lower_type(concrete), data);
    llvm::Value *receiver = loaded;
    llvm::AllocaInst *receiver_copy = nullptr;
    if (owning(concrete)) {
      receiver_copy = storage(lower_type(concrete));
      builder.CreateStore(loaded, receiver_copy);
      builder.CreateCall(value_op(concrete, true), {receiver_copy});
      receiver = builder.CreateLoad(lower_type(concrete), receiver_copy);
    }
    std::vector<llvm::Value *> actual{receiver};
    for (unsigned i = 1; i < fn->arg_size(); ++i)
      actual.push_back(fn->getArg(i));
    auto *result = builder.CreateCall(functions.at(target), actual);
    if (receiver_copy)
      builder.CreateCall(value_op(concrete, false), {receiver_copy});
    builder.CreateRet(result);
    return fn;
  }
  llvm::GlobalVariable *
  trait_vtable(TypeId dyn, TypeId concrete,
               const std::vector<SymbolId> &methods) {
    auto key = std::pair<uint64_t, uint64_t>{dyn.value, concrete.value};
    if (auto it = trait_vtables.find(key); it != trait_vtables.end())
      return it->second;
    const auto *tr = [&]() -> const hir::TraitDecl * {
      for (const auto &candidate : input.hir.module().traits)
        if (candidate.symbol == type(dyn).declaration)
          return &candidate;
      return nullptr;
    }();
    if (!tr || tr->methods.size() != methods.size())
      fail("trait object vtable has no matching trait declaration");
    std::vector<llvm::Type *> fields(2 + methods.size(), builder.getPtrTy());
    auto *layout = llvm::StructType::create(
        context, fields,
        "lang.trait.vtable." + std::to_string(dyn.value) + "." +
            std::to_string(concrete.value));
    std::vector<llvm::Constant *> entries;
    entries.push_back(llvm::ConstantExpr::getBitCast(
        trait_box_op(concrete, false), builder.getPtrTy()));
    entries.push_back(llvm::ConstantExpr::getBitCast(
        trait_box_op(concrete, true), builder.getPtrTy()));
    for (auto target : methods)
      entries.push_back(llvm::ConstantExpr::getBitCast(
          trait_method_thunk(dyn, concrete, target), builder.getPtrTy()));
    auto *table = new llvm::GlobalVariable(
        module, layout, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(layout, entries),
        "lang.trait.vtable.data." + std::to_string(dyn.value) + "." +
            std::to_string(concrete.value));
    table->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    trait_vtables.emplace(key, table);
    return table;
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

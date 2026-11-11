  struct ThreadBridge {
    llvm::StructType *context;
    llvm::Function *entry, *copy, *drop;
  };
  std::map<TypeId, ThreadBridge> thread_bridges;
  ThreadBridge thread_bridge(TypeId entry_type) {
    if (auto found = thread_bridges.find(entry_type); found != thread_bridges.end())
      return found->second;
    auto argument = type(entry_type).parameters.at(0).type;
    auto *layout = llvm::StructType::get(context, {builder.getPtrTy(), lower_type(argument)});
    auto make = [&](llvm::Type *result, const char *name) {
      return llvm::Function::Create(
          llvm::FunctionType::get(result, {builder.getPtrTy()}, false),
          llvm::GlobalValue::InternalLinkage, name, module);
    };
    ThreadBridge bridge{layout, make(builder.getInt64Ty(), "lang.thread.entry"), nullptr, nullptr};
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", bridge.entry));
    auto *data = bridge.entry->getArg(0);
    auto *entry = builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(layout, data, 0));
    auto *value = builder.CreateLoad(lower_type(argument), builder.CreateStructGEP(layout, data, 1));
    builder.CreateRet(builder.CreateCall(signature(entry_type), entry, {value}));
    if (owning(argument)) {
      for (bool copy : {true, false}) {
        auto *fn = make(builder.getVoidTy(), copy ? "lang.thread.copy" : "lang.thread.drop");
        (copy ? bridge.copy : bridge.drop) = fn;
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
        builder.CreateCall(value_op(argument, copy),
            {builder.CreateStructGEP(layout, fn->getArg(0), 1)});
        builder.CreateRetVoid();
      }
    }
    builder.restoreIP(ip);
    thread_bridges.emplace(entry_type, bridge);
    return bridge;
  }
  llvm::Value *thread_spawn(const ir::Instruction &i, llvm::ArrayRef<llvm::Value *> args) {
    auto bridge = thread_bridge(type(i.signature).parameters.at(0).type);
    auto *data = storage(bridge.context);
    builder.CreateStore(args[0], builder.CreateStructGEP(bridge.context, data, 0));
    builder.CreateStore(args[1], builder.CreateStructGEP(bridge.context, data, 1));
    auto callback = [&](llvm::Function *fn) -> llvm::Value * {
      return fn ? static_cast<llvm::Value *>(fn)
                : llvm::ConstantPointerNull::get(builder.getPtrTy());
    };
    return runtime("lang_thread_spawn_v1", builder.getPtrTy(),
        {bridge.entry, data,
         builder.getInt64(module.getDataLayout().getTypeAllocSize(bridge.context).getFixedValue()),
         callback(bridge.copy), callback(bridge.drop)});
  }

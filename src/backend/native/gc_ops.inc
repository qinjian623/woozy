  // Source-level automatic cycle collection (MEM02).
  //
  // Native structs/enums are inline values, so the only heap objects this
  // backend owns are LangVec descriptors (including closure environments).
  // A Vec is registered only when its element type can contain a strong edge
  // to another heap object. Numeric/scalar Vecs and Vecs containing only
  // strings, handles, Weak, or foreign resources are never registered.
  //
  // The trace callback walks only the initialized prefix and reports one
  // direct edge per owning field. It never descends into a Vec target: that
  // target has its own registered trace callback. clear is the runtime's
  // established LangVec clear, which publishes the empty prefix before it
  // drops old owners. retain/release are the ordinary Vec strong operations,
  // and the final release already unregisters the object before payload drop.
  std::map<std::string, llvm::Function *> gc_trace_functions;
  std::map<std::string, llvm::GlobalVariable *> gc_ops_globals;
  llvm::StructType *gc_ops_record = nullptr;
  llvm::Function *gc_invalidate_callback = nullptr;

  bool has_strong_edge(TypeId id) {
    std::set<uint64_t> active;
    return has_strong_edge(id, active, 0);
  }
  bool has_strong_edge(TypeId id, std::set<uint64_t> &active, unsigned depth) {
    if (depth > 64) return true; // Conservative: register rather than miss.
    const auto &t = type(id);
    switch (t.kind) {
    case hir::TypeKind::Vec:
    case hir::TypeKind::Function:
    case hir::TypeKind::Slice:
      return true;
    case hir::TypeKind::NominalStruct:
    case hir::TypeKind::NominalEnum: {
      if (!active.insert(id.value).second) return false;
      bool found = false;
      if (t.kind == hir::TypeKind::NominalStruct) {
        for (auto field : aggregates.fields(id))
          if (has_strong_edge(field.type, active, depth + 1)) {
            found = true;
            break;
          }
      } else {
        for (const auto &variant : aggregates.declaration(id).variants) {
          if (found) break;
          for (auto field : aggregates.fields(id, variant.symbol))
            if (has_strong_edge(field.type, active, depth + 1)) {
              found = true;
              break;
            }
        }
      }
      active.erase(id.value);
      return found;
    }
    default:
      return false;
    }
  }
  llvm::FunctionType *gc_trace_type() {
    return llvm::FunctionType::get(
        builder.getVoidTy(),
        {builder.getPtrTy(), builder.getPtrTy(), builder.getPtrTy()}, false);
  }
  void gc_visitor_call(llvm::Value *target, llvm::Value *visitor,
                       llvm::Value *visit_context) {
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *present = llvm::BasicBlock::Create(context, "gc.edge", fn);
    auto *done = llvm::BasicBlock::Create(context, "gc.edge.done", fn);
    builder.CreateCondBr(
        builder.CreateICmpNE(target, llvm::ConstantPointerNull::get(
                                         builder.getPtrTy())),
        present, done);
    builder.SetInsertPoint(present);
    builder.CreateCall(
        llvm::FunctionType::get(builder.getVoidTy(),
                                {builder.getPtrTy(), builder.getPtrTy()}, false),
        visitor, {target, visit_context});
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
  }
  // Emit tracing for one value in storage. Only direct owned edges are
  // reported; nested Vec targets are separate registered objects.
  void gc_trace_value(TypeId id, llvm::Value *address, llvm::Value *visitor,
                      llvm::Value *visit_context) {
    const auto &t = type(id);
    switch (t.kind) {
    case hir::TypeKind::Vec:
      gc_visitor_call(builder.CreateLoad(builder.getPtrTy(), address), visitor,
                      visit_context);
      return;
    case hir::TypeKind::Function: {
      auto *value = builder.CreateLoad(fn_type, address);
      gc_visitor_call(builder.CreateExtractValue(value, {1}), visitor, visit_context);
      return;
    }
    case hir::TypeKind::Slice: {
      auto *owner = builder.CreateLoad(
          builder.getPtrTy(), builder.CreateStructGEP(slice_type, address, 0));
      gc_visitor_call(owner, visitor, visit_context);
      return;
    }
    case hir::TypeKind::NominalStruct: {
      auto fields = aggregates.fields(id);
      for (size_t n = 0; n < fields.size(); ++n)
        if (has_strong_edge(fields[n].type))
          gc_trace_value(fields[n].type,
                         builder.CreateStructGEP(lower_type(id), address, n),
                         visitor, visit_context);
      return;
    }
    case hir::TypeKind::NominalEnum: {
      const auto &variants = aggregates.declaration(id).variants;
      auto *layout = llvm::cast<llvm::StructType>(lower_type(id));
      auto *tag = builder.CreateLoad(
          builder.getInt32Ty(), builder.CreateStructGEP(layout, address, 0));
      auto *fn = builder.GetInsertBlock()->getParent();
      auto *done = llvm::BasicBlock::Create(context, "gc.enum.done", fn);
      auto *dispatch = builder.CreateSwitch(tag, done, variants.size());
      for (size_t v = 0; v < variants.size(); ++v) {
        auto *body = llvm::BasicBlock::Create(context, "gc.enum.payload", fn);
        dispatch->addCase(builder.getInt32(v), body);
        builder.SetInsertPoint(body);
        auto fields = aggregates.fields(id, variants[v].symbol);
        auto *payload = builder.CreateStructGEP(layout, address, v + 1);
        auto *payload_type = layout->getElementType(v + 1);
        for (size_t n = 0; n < fields.size(); ++n)
          if (has_strong_edge(fields[n].type))
            gc_trace_value(fields[n].type,
                           builder.CreateStructGEP(payload_type, payload, n),
                           visitor, visit_context);
        builder.CreateBr(done);
      }
      builder.SetInsertPoint(done);
      return;
    }
    default:
      return;
    }
  }
  llvm::Function *gc_trace_for_vec(TypeId element) {
    const std::string key = "v" + std::to_string(element.value);
    if (auto found = gc_trace_functions.find(key);
        found != gc_trace_functions.end())
      return found->second;
    auto *fn = llvm::Function::Create(
        gc_trace_type(), llvm::GlobalValue::InternalLinkage,
        "lang.gc.trace." + key, module);
    gc_trace_functions.emplace(key, fn);
    auto ip = builder.saveIP();
    auto *entry = llvm::BasicBlock::Create(context, "entry", fn);
    auto *check = llvm::BasicBlock::Create(context, "prefix.check", fn);
    auto *body = llvm::BasicBlock::Create(context, "prefix.body", fn);
    auto *next = llvm::BasicBlock::Create(context, "prefix.next", fn);
    auto *done = llvm::BasicBlock::Create(context, "prefix.done", fn);
    builder.SetInsertPoint(entry);
    auto *object = fn->getArg(0), *visitor = fn->getArg(1),
         *visit_context = fn->getArg(2);
    auto *initialized = builder.CreateLoad(
        builder.getInt64Ty(), builder.CreateStructGEP(vec_type, object, 2));
    auto *data = builder.CreateLoad(
        builder.getPtrTy(), builder.CreateStructGEP(vec_type, object, 5));
    auto *stride = builder.CreateLoad(
        builder.getInt64Ty(), builder.CreateStructGEP(vec_type, object, 3));
    builder.CreateBr(check);
    builder.SetInsertPoint(check);
    auto *index = builder.CreatePHI(builder.getInt64Ty(), 2);
    index->addIncoming(builder.getInt64(0), entry);
    builder.CreateCondBr(builder.CreateICmpULT(index, initialized), body, done);
    builder.SetInsertPoint(body);
    auto *offset = builder.CreateMul(index, stride);
    auto *element_address = builder.CreateGEP(builder.getInt8Ty(), data, offset);
    gc_trace_value(element, element_address, visitor, visit_context);
    builder.CreateBr(next);
    builder.SetInsertPoint(next);
    index->addIncoming(builder.CreateAdd(index, builder.getInt64(1)), next);
    builder.CreateBr(check);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return fn;
  }
  // A closure environment is a one-row LangVec holding an anonymous record.
  llvm::Function *gc_trace_for_fields(const std::vector<TypeId> &fields) {
    const std::string key = "e" + id_list(fields);
    if (auto found = gc_trace_functions.find(key);
        found != gc_trace_functions.end())
      return found->second;
    auto *fn = llvm::Function::Create(
        gc_trace_type(), llvm::GlobalValue::InternalLinkage,
        "lang.gc.trace." + key, module);
    gc_trace_functions.emplace(key, fn);
    auto ip = builder.saveIP();
    auto *entry = llvm::BasicBlock::Create(context, "entry", fn);
    auto *body = llvm::BasicBlock::Create(context, "row", fn);
    auto *done = llvm::BasicBlock::Create(context, "done", fn);
    builder.SetInsertPoint(entry);
    auto *object = fn->getArg(0), *visitor = fn->getArg(1),
         *visit_context = fn->getArg(2);
    auto *initialized = builder.CreateLoad(
        builder.getInt64Ty(), builder.CreateStructGEP(vec_type, object, 2));
    builder.CreateCondBr(builder.CreateICmpUGT(initialized, builder.getInt64(0)),
                         body, done);
    builder.SetInsertPoint(body);
    auto *row = builder.CreateLoad(
        builder.getPtrTy(), builder.CreateStructGEP(vec_type, object, 5));
    auto *layout = env_record_type(fields);
    for (size_t n = 0; n < fields.size(); ++n)
      if (has_strong_edge(fields[n]))
        gc_trace_value(fields[n],
                       builder.CreateStructGEP(layout, row, n), visitor,
                       visit_context);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return fn;
  }
  llvm::Function *gc_invalidate() {
    if (gc_invalidate_callback) return gc_invalidate_callback;
    auto *fn = llvm::Function::Create(
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage, "lang.gc.invalidate_weak", module);
    gc_invalidate_callback = fn;
    auto ip = builder.saveIP();
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", fn));
    auto *slot = builder.CreateStructGEP(vec_type, fn->getArg(0), 9);
    auto callee = module.getOrInsertFunction(
        "lang_weak_invalidate_slot_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    builder.CreateCall(callee, {slot});
    builder.CreateRetVoid();
    builder.restoreIP(ip);
    return fn;
  }
  llvm::GlobalVariable *gc_ops(const std::string &key,
                               llvm::Function *trace) {
    if (auto found = gc_ops_globals.find(key); found != gc_ops_globals.end())
      return found->second;
    if (!gc_ops_record)
      gc_ops_record = llvm::StructType::get(
          context, {builder.getPtrTy(), builder.getPtrTy(),
                    builder.getPtrTy(), builder.getPtrTy(),
                    builder.getPtrTy()});
    auto clear = module.getOrInsertFunction(
        "lang_vec_clear_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto retain = module.getOrInsertFunction(
        "lang_vec_retain_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto release = module.getOrInsertFunction(
        "lang_vec_release_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getPtrTy()},
                                false));
    auto *initializer = llvm::ConstantStruct::get(
        gc_ops_record,
        {trace, llvm::cast<llvm::Function>(clear.getCallee()),
         llvm::cast<llvm::Function>(retain.getCallee()),
         llvm::cast<llvm::Function>(release.getCallee()), gc_invalidate()});
    auto *global = new llvm::GlobalVariable(
        module, gc_ops_record, true, llvm::GlobalValue::PrivateLinkage,
        initializer, "lang.gc.ops." + key);
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::None);
    gc_ops_globals.emplace(key, global);
    return global;
  }
  void gc_register_object(llvm::Value *object, llvm::GlobalVariable *ops) {
    auto callee = module.getOrInsertFunction(
        "lang_gc_program_register_v1",
        llvm::FunctionType::get(builder.getInt1Ty(),
                                {builder.getPtrTy(), builder.getPtrTy()},
                                false));
    builder.CreateCall(callee, {object, ops});
  }
  void gc_register_vec(llvm::Value *object, TypeId element) {
    if (!has_strong_edge(element)) return;
    gc_register_object(object,
                       gc_ops("v" + std::to_string(element.value),
                              gc_trace_for_vec(element)));
  }
  void gc_register_fields(llvm::Value *object,
                          const std::vector<TypeId> &fields) {
    bool needed = false;
    for (auto field : fields)
      if (has_strong_edge(field)) {
        needed = true;
        break;
      }
    if (!needed) return;
    gc_register_object(object, gc_ops("e" + id_list(fields),
                                      gc_trace_for_fields(fields)));
  }
  void gc_poll() {
    auto callee = module.getOrInsertFunction(
        "lang_gc_runtime_poll_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {}, false));
    builder.CreateCall(callee, {});
  }
  // A stable safepoint is a function entry, a natural-loop header, or a
  // natural-loop exit. Header polls bound garbage during a loop; exit polls
  // collect the final iteration before the surrounding normal cleanup runs.
  std::pair<std::set<size_t>, std::set<size_t>> gc_loop_safepoints(
      const ir::Function &f) {
    const size_t n = f.blocks.size();
    std::set<size_t> safepoints;
    std::set<size_t> exits;
    if (!n) return {safepoints, exits};
    std::vector<std::vector<size_t>> predecessors(n);
    for (size_t b = 0; b < n; ++b)
      for (auto target : f.blocks[b].terminator.targets)
        if (target.value < n) predecessors[target.value].push_back(b);
    std::vector<bool> reachable(n, false);
    std::vector<size_t> work{0};
    reachable[0] = true;
    while (!work.empty()) {
      const size_t b = work.back();
      work.pop_back();
      for (auto target : f.blocks[b].terminator.targets)
        if (target.value < n && !reachable[target.value]) {
          reachable[target.value] = true;
          work.push_back(target.value);
        }
    }
    std::vector<std::set<size_t>> dominators(n);
    for (size_t b = 0; b < n; ++b) {
      if (!reachable[b]) {
        dominators[b].insert(b);
        continue;
      }
      for (size_t other = 0; other < n; ++other)
        if (reachable[other]) dominators[b].insert(other);
    }
    dominators[0] = {0};
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 1; b < n; ++b) {
        if (!reachable[b]) continue;
        std::set<size_t> next;
        bool first = true;
        for (auto p : predecessors[b]) {
          if (!reachable[p]) continue;
          if (first) {
            next = dominators[p];
            first = false;
          } else {
            std::set<size_t> intersection;
            for (auto value : next)
              if (dominators[p].count(value)) intersection.insert(value);
            next = std::move(intersection);
          }
        }
        if (first) next.clear();
        next.insert(b);
        if (next != dominators[b]) {
          dominators[b] = std::move(next);
          changed = true;
        }
      }
    }
    for (size_t b = 0; b < n; ++b) {
      if (!reachable[b]) continue;
      for (auto target : f.blocks[b].terminator.targets) {
        if (target.value >= n || !reachable[target.value] ||
            !dominators[b].count(target.value))
          continue;
        const size_t header = target.value;
        safepoints.insert(header);
        std::set<size_t> body{header};
        std::vector<size_t> pending;
        if (b != header) {
          body.insert(b);
          pending.push_back(b);
        }
        while (!pending.empty()) {
          const size_t node = pending.back();
          pending.pop_back();
          for (auto predecessor : predecessors[node])
            if (reachable[predecessor] && !body.count(predecessor)) {
              body.insert(predecessor);
              pending.push_back(predecessor);
            }
        }
        for (auto node : body)
          for (auto successor : f.blocks[node].terminator.targets)
            if (successor.value < n && reachable[successor.value] &&
                !body.count(successor.value)) {
              safepoints.insert(successor.value);
              exits.insert(successor.value);
            }
      }
    }
    safepoints.erase(0);
    exits.erase(0);
    return {std::move(safepoints), std::move(exits)};
  }
  // A loop exit is a stable point after the last iteration. If any object is
  // registered, perform one bounded collection so the final iteration's
  // unrooted values do not outlive the surrounding normal local cleanup.
  // This is still automatic backend collection; source never calls collect.
  void gc_collect_loop_exit() {
    auto live = module.getOrInsertFunction(
        "lang_gc_registry_live_v1",
        llvm::FunctionType::get(builder.getInt64Ty(), {}, false));
    auto *count = builder.CreateCall(live, {});
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *collect = llvm::BasicBlock::Create(context, "gc.exit.collect", fn);
    auto *done = llvm::BasicBlock::Create(context, "gc.exit.done", fn);
    builder.CreateCondBr(builder.CreateICmpUGT(count, builder.getInt64(0)),
                         collect, done);
    builder.SetInsertPoint(collect);
    auto collect_fn = module.getOrInsertFunction(
        "lang_gc_runtime_collect_v1",
        llvm::FunctionType::get(builder.getInt64Ty(),
                                {builder.getInt64Ty(), builder.getPtrTy()},
                                false));
    builder.CreateCall(collect_fn,
                       {builder.getInt64(10000000),
                        llvm::ConstantPointerNull::get(builder.getPtrTy())});
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
  }
  llvm::Value *gc_enter_critical() {
    auto callee = module.getOrInsertFunction(
        "lang_gc_program_enter_critical_v1",
        llvm::FunctionType::get(builder.getInt32Ty(), {}, false));
    return builder.CreateCall(callee, {});
  }
  void gc_leave_critical(llvm::Value *token) {
    auto callee = module.getOrInsertFunction(
        "lang_gc_program_leave_critical_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()},
                                false));
    builder.CreateCall(callee, {token});
  }
  llvm::Value *gc_enter_safe() {
    auto callee = module.getOrInsertFunction(
        "lang_gc_program_enter_safe_v1",
        llvm::FunctionType::get(builder.getInt32Ty(), {}, false));
    return builder.CreateCall(callee, {});
  }
  void gc_leave_safe(llvm::Value *token) {
    auto callee = module.getOrInsertFunction(
        "lang_gc_program_leave_safe_v1",
        llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()},
                                false));
    builder.CreateCall(callee, {token});
  }

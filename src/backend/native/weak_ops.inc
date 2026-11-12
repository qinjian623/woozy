  // ---- Source Weak runtime operations -------------------------------------
  //
  // Weak[T] is {control, code}. The canonical projection below is selected from
  // the verified target declaration, never from a type_key string. The runtime
  // owns the control block and the shared slot on the target descriptor, so no
  // address-keyed registry is manufactured here.
  struct WeakProjection {
    enum class Kind { vec, handle, box } kind;
    // Canonical wrapper carrying the shared private field; zero for a direct
    // Vec identity.
    TypeId wrapper = TypeId{0};
  };
  WeakProjection weak_projection(TypeId target) {
    const auto &t = type(target);
    if (t.kind == hir::TypeKind::Vec)
      return {WeakProjection::Kind::vec, TypeId{0}};
    // Only the exact resolved std declaration selected by weak_target is
    // accepted. A same-shaped user struct or a multi-field wrapper is not a
    // Weak target even if its bytes happen to look like one.
    if (t.kind != hir::TypeKind::NominalStruct ||
        !hir::weak_target(input.hir.module(), target))
      fail("Weak target is not a canonical source declaration");
    auto fields = aggregates.fields(target);
    if (fields.size() != 1)
      fail("Weak target does not have the canonical private field");
    switch (type(fields.front().type).kind) {
    case hir::TypeKind::Vec:
      // std/box.Box[T]: the canonical target is the private cell Vec.
      return {WeakProjection::Kind::box, target};
    case hir::TypeKind::ThreadHandle:
    case hir::TypeKind::MutexHandle:
    case hir::TypeKind::CondvarHandle:
    case hir::TypeKind::AtomicI64Handle:
    case hir::TypeKind::IpcHandle:
      // std/thread.Thread, std/sync.Mutex/Condvar/AtomicI64, std/ipc.Memory.
      return {WeakProjection::Kind::handle, target};
    default:
      fail("Weak target private field is not a canonical runtime identity");
      return {WeakProjection::Kind::vec, TypeId{0}};
    }
  }
  llvm::Value *weak_vec_slot(llvm::Value *vec) {
    // LangVec.weak_control is the tenth field, at byte offset 72.
    return builder.CreateStructGEP(vec_type, vec, 9);
  }
  llvm::Value *weak_handle_slot(llvm::Value *handle) {
    // LangHandle.weak_control is the fourth field, at byte offset 24.
    return builder.CreateStructGEP(handle_type, handle, 3);
  }
  llvm::Value *null_pointer() {
    return llvm::ConstantPointerNull::get(builder.getPtrTy());
  }
  // Resolve the owned object whose ARC identity the target observes and the
  // canonical slot whose installed control block belongs to that identity.
  llvm::Value *weak_slot_address(const WeakProjection &projection,
                                 llvm::Value *value, llvm::Value **object) {
    if (projection.kind == WeakProjection::Kind::vec) {
      *object = value;
      return weak_vec_slot(value);
    }
    *object = builder.CreateExtractValue(value, {0});
    return projection.kind == WeakProjection::Kind::box
               ? weak_vec_slot(*object)
               : weak_handle_slot(*object);
  }
  // LangWeak is exactly {control, code}.
  llvm::Value *weak_value(llvm::Value *control, llvm::Value *code) {
    llvm::Value *value = llvm::Constant::getNullValue(weak_type);
    value = builder.CreateInsertValue(value, control, {0});
    return builder.CreateInsertValue(value, code, {1});
  }
  // LangFn is exactly {code, env}. The env-first code pointer is preserved.
  llvm::Value *function_value_of(llvm::Value *code, llvm::Value *env) {
    llvm::Value *value = llvm::Constant::getNullValue(fn_type);
    value = builder.CreateInsertValue(value, code, {0});
    return builder.CreateInsertValue(value, env, {1});
  }
  // Downgrade a function value. An empty environment denotes the immortal
  // {null, code} encoding and must never be treated as a failed downgrade. A
  // failed downgrade of a real environment denotes dead {null, null}; it must
  // never be reinterpreted as immortal.
  llvm::Value *weak_downgrade_function(llvm::Value *function) {
    auto *code = builder.CreateExtractValue(function, {0});
    auto *env = builder.CreateExtractValue(function, {1});
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *captured = llvm::BasicBlock::Create(context, "weak.fn.env", fn);
    auto *immortal =
        llvm::BasicBlock::Create(context, "weak.fn.immortal", fn);
    auto *alive = llvm::BasicBlock::Create(context, "weak.fn.alive", fn);
    auto *dead = llvm::BasicBlock::Create(context, "weak.fn.dead", fn);
    auto *done = llvm::BasicBlock::Create(context, "weak.fn.done", fn);
    builder.CreateCondBr(builder.CreateICmpNE(env, null_pointer()), captured,
                         immortal);
    builder.SetInsertPoint(captured);
    auto *control = runtime("lang_weak_downgrade_v1", builder.getPtrTy(),
                            {env, weak_vec_slot(env)});
    builder.CreateCondBr(
        builder.CreateICmpEQ(control, null_pointer()), dead, alive);
    builder.SetInsertPoint(alive);
    auto *alive_value = weak_value(control, code);
    builder.CreateBr(done);
    auto *alive_end = builder.GetInsertBlock();
    builder.SetInsertPoint(dead);
    auto *dead_value = weak_value(null_pointer(), null_pointer());
    builder.CreateBr(done);
    auto *dead_end = builder.GetInsertBlock();
    builder.SetInsertPoint(immortal);
    auto *immortal_value = weak_value(null_pointer(), code);
    builder.CreateBr(done);
    auto *immortal_end = builder.GetInsertBlock();
    builder.SetInsertPoint(done);
    auto *joined = builder.CreatePHI(weak_type, 3);
    joined->addIncoming(alive_value, alive_end);
    joined->addIncoming(dead_value, dead_end);
    joined->addIncoming(immortal_value, immortal_end);
    return joined;
  }
  // Downgrade a Vec, Box or canonical handle. The caller holds a strong
  // reference for the duration of the call, so the returned control block owns
  // exactly one real Weak reference.
  llvm::Value *weak_downgrade_object(const WeakProjection &projection,
                                     llvm::Value *value) {
    llvm::Value *object = nullptr;
    auto *slot = weak_slot_address(projection, value, &object);
    auto *control = runtime("lang_weak_downgrade_v1", builder.getPtrTy(),
                            {object, slot});
    return weak_value(control, null_pointer());
  }
  // upgrade returns the canonical std/option.Option[T]. The exact declaration
  // and None/Some payload shape are re-checked here so forged or swapped HIR
  // metadata cannot silently produce a different nominal field layout.
  void check_weak_option(TypeId option) {
    const auto &t = type(option);
    if (t.kind != hir::TypeKind::NominalEnum || t.type_args.size() != 1)
      fail("Weak upgrade result is not Option[T]");
    const auto &declaration = aggregates.declaration(option);
    const auto &symbol =
        input.hir.module().resolved->symbols.at(t.declaration.value);
    const auto &module =
        input.hir.module().resolved->project->module(symbol.module);
    if (module.logical_path != "std/option" || symbol.name != "Option" ||
        declaration.variants.size() != 2)
      fail("Weak upgrade result is not the canonical std/option.Option");
    const auto &none = input.hir.module().resolved->symbols.at(
        declaration.variants[0].symbol.value);
    const auto &some = input.hir.module().resolved->symbols.at(
        declaration.variants[1].symbol.value);
    if (none.name != "None" || some.name != "Some" ||
        !declaration.variants[0].payload.empty() ||
        declaration.variants[1].payload.size() != 1)
      fail("Weak upgrade result does not have the canonical Option shape");
    auto payload = aggregates.fields(option, declaration.variants[1].symbol);
    if (payload.size() != 1 || payload.front().type != t.type_args[0])
      fail("Weak upgrade Option payload differs from the Weak target");
  }
  // Wrap one transferred strong reference in the exact nominal payload type.
  // Vec and handle targets are already the lowered identity; Box and the
  // standard handle wrappers carry that identity in their single private field.
  llvm::Value *weak_payload(TypeId target, const WeakProjection *projection,
                            llvm::Value *upgraded, llvm::Value *code) {
    if (type(target).kind == hir::TypeKind::Function)
      return function_value_of(code, upgraded);
    if (projection->kind == WeakProjection::Kind::vec) return upgraded;
    return builder.CreateInsertValue(
        llvm::Constant::getNullValue(lower_type(target)), upgraded, {0});
  }
  // Upgrade a Weak value. The control block may already be dead, and a function
  // environment may be absent because the value is immortal. upgrade never
  // calls the stored code on the dead path, and it transfers the new strong
  // reference from lang_weak_upgrade_v1 exactly once.
  llvm::Value *weak_upgrade(TypeId option, TypeId target,
                            const WeakProjection *projection,
                            llvm::Value *weak) {
    auto *control = builder.CreateExtractValue(weak, {0});
    auto *code = builder.CreateExtractValue(weak, {1});
    const bool function = type(target).kind == hir::TypeKind::Function;
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *has_control =
        llvm::BasicBlock::Create(context, "weak.up.control", fn);
    auto *no_control =
        llvm::BasicBlock::Create(context, "weak.up.empty", fn);
    auto *some = llvm::BasicBlock::Create(context, "weak.up.some", fn);
    auto *none = llvm::BasicBlock::Create(context, "weak.up.none", fn);
    auto *done = llvm::BasicBlock::Create(context, "weak.up.done", fn);
    builder.CreateCondBr(builder.CreateICmpNE(control, null_pointer()),
                         has_control, no_control);

    builder.SetInsertPoint(has_control);
    auto *upgraded =
        runtime("lang_weak_upgrade_v1", builder.getPtrTy(), {control});
    auto *live = builder.CreateICmpNE(upgraded, null_pointer());
    auto *live_payload = weak_payload(target, projection, upgraded, code);
    builder.CreateCondBr(live, some, none);
    auto *live_end = builder.GetInsertBlock();

    builder.SetInsertPoint(no_control);
    llvm::Value *immortal_payload = nullptr;
    if (function) {
      // A null control with a non-null code is the only immortal encoding.
      // Dead {null, null} and empty values fall through to None.
      auto *immortal = builder.CreateICmpNE(code, null_pointer());
      immortal_payload = function_value_of(code, null_pointer());
      builder.CreateCondBr(immortal, some, none);
    } else {
      builder.CreateBr(none);
    }
    auto *empty_end = builder.GetInsertBlock();

    builder.SetInsertPoint(some);
    auto *payload = builder.CreatePHI(lower_type(target), function ? 2 : 1);
    payload->addIncoming(live_payload, live_end);
    if (function) payload->addIncoming(immortal_payload, empty_end);
    auto *some_value = stdlib_variant(option, 1, payload);
    builder.CreateBr(done);
    auto *some_end = builder.GetInsertBlock();

    builder.SetInsertPoint(none);
    auto *none_value = stdlib_variant(option, 0, nullptr);
    builder.CreateBr(done);
    auto *none_end = builder.GetInsertBlock();

    builder.SetInsertPoint(done);
    auto *joined = builder.CreatePHI(lower_type(option), 2);
    joined->addIncoming(some_value, some_end);
    joined->addIncoming(none_value, none_end);
    return joined;
  }
  // Recover the single Weak[T] type argument from a verified operation type.
  const hir::Type *weak_operand(TypeId id) {
    const auto *t = aggregates.types.find(id);
    if (!t || t->kind != hir::TypeKind::Weak || t->type_args.size() != 1)
      fail("Weak operation did not receive a Weak[T] value");
    return t;
  }
  llvm::Value *weak_system_call(const ir::Instruction &i,
                                llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    if (!i.system ||
        (*i.system != SystemOperation::weak_empty &&
         *i.system != SystemOperation::weak_downgrade &&
         *i.system != SystemOperation::weak_upgrade))
      return nullptr;
    if (input.hir.config().word_bits != 64)
      unsupported("Weak runtime operations require the native/64 ABI");
    const auto &signature = type(i.signature);
    if (signature.kind != hir::TypeKind::Function || signature.throws_type)
      fail("Weak operation signature is invalid");
    if (*i.system == SystemOperation::weak_empty) {
      if (!args.empty()) fail("Weak empty takes no value arguments");
      const auto *weak = weak_operand(signature.result);
      if (!hir::weak_target(input.hir.module(), weak->type_args[0]))
        fail("Weak target is not a canonical source declaration");
      return llvm::Constant::getNullValue(lower_type(i.type));
    }
    if (*i.system == SystemOperation::weak_downgrade) {
      if (args.size() != 1 || signature.parameters.size() != 1)
        fail("Weak downgrade requires one value argument");
      const TypeId target = signature.parameters[0].type;
      const auto *weak = weak_operand(i.type);
      if (weak->type_args[0] != target)
        fail("Weak downgrade result differs from its target");
      if (type(target).kind == hir::TypeKind::Function)
        return weak_downgrade_function(args[0]);
      return weak_downgrade_object(weak_projection(target), args[0]);
    }
    if (args.size() != 1 || signature.parameters.size() != 1)
      fail("Weak upgrade requires one Weak value");
    const auto *weak = weak_operand(signature.parameters[0].type);
    const TypeId target = weak->type_args[0];
    const auto &option = type(i.type);
    if (option.type_args.size() != 1 || option.type_args[0] != target)
      fail("Weak upgrade result does not carry the target type");
    check_weak_option(i.type);
    if (type(target).kind == hir::TypeKind::Function)
      return weak_upgrade(i.type, target, nullptr, args[0]);
    auto projection = weak_projection(target);
    return weak_upgrade(i.type, target, &projection, args[0]);
  }

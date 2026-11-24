  llvm::Value *value_address(TypeId id, llvm::Value *value) {
    auto *address = storage(lower_type(id));
    builder.CreateStore(value, address);
    return address;
  }
  llvm::Value *text_binary(const ir::Instruction &i, llvm::Value *a,
                            llvm::Value *b) {
    const auto input_type = value_types.at(i.inputs[0]);
    auto *left = value_address(input_type, a), *right = value_address(input_type, b);
    using B = hir::BinaryOp;
    if (i.binary == B::add) {
      auto *out = storage(lower_type(i.type));
      start_lifetime(out);
      runtime("lang_text_concat_v1", builder.getVoidTy(), {out, left, right});
      auto *loaded = builder.CreateLoad(lower_type(i.type), out);
      end_lifetime(out);
      return loaded;
    }
    auto *order = runtime("lang_text_compare_v1", builder.getInt32Ty(), {left, right});
    auto *zero = builder.getInt32(0);
    switch (i.binary) {
    case B::equal: return builder.CreateICmpEQ(order, zero);
    case B::not_equal: return builder.CreateICmpNE(order, zero);
    case B::less: return builder.CreateICmpSLT(order, zero);
    case B::less_equal: return builder.CreateICmpSLE(order, zero);
    case B::greater: return builder.CreateICmpSGT(order, zero);
    case B::greater_equal: return builder.CreateICmpSGE(order, zero);
    default: fail("invalid verified text operator");
    }
    return nullptr;
  }
  llvm::Value *value_system_call(const ir::Instruction &i,
                                  llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    const auto sig = type(i.signature);
    auto pointer = [&](unsigned index) { return value_address(sig.parameters[index].type, args[index]); };
    switch (*i.system) {
    case SystemOperation::unicode_normalize:
    case SystemOperation::unicode_casefold: {
      auto *out = storage(lower_type(i.type));
      start_lifetime(out);
      llvm::Value *status = nullptr;
      if (*i.system == SystemOperation::unicode_normalize)
        status = runtime("lang_unicode_normalize_v1", builder.getInt64Ty(),
                         {out, pointer(0), args[1]});
      else
        status = runtime("lang_unicode_casefold_v1", builder.getInt64Ty(),
                         {out, pointer(0)});
      guard(builder.CreateICmpSLT(status, builder.getInt64(0)), 10, i.source);
      auto *loaded = builder.CreateLoad(lower_type(i.type), out);
      end_lifetime(out);
      return loaded;
    }
    case SystemOperation::unicode_boundaries:
      return runtime("lang_unicode_grapheme_boundaries_v1", builder.getPtrTy(), {pointer(0)});
    case SystemOperation::text_len: return builder.CreateExtractValue(args[0], {2});
    case SystemOperation::text_byte_len: return builder.CreateExtractValue(args[0], {1});
    case SystemOperation::text_cursor_at:
      return runtime("lang_text_cursor_at_v1", builder.getInt64Ty(), {pointer(0), args[1]});
    case SystemOperation::text_cursor_read:
      return runtime("lang_text_cursor_read_v1", builder.getInt64Ty(), {pointer(0), args[1]});
    case SystemOperation::text_count_rune:
      return runtime("lang_text_count_rune_v1", builder.getInt64Ty(), {pointer(0), args[1]});
    case SystemOperation::text_bytes:
      return runtime("lang_text_bytes_v1", builder.getPtrTy(), {pointer(0)});
    case SystemOperation::text_cursor_slice:
    case SystemOperation::unicode_text_take: {
      auto *out = storage(lower_type(i.type));
      start_lifetime(out);
      runtime(*i.system == SystemOperation::text_cursor_slice
                  ? "lang_text_cursor_slice_v1" : "lang_unicode_text_take_v1",
              builder.getVoidTy(), {out, pointer(0), args[1], args[2]});
      auto *loaded = builder.CreateLoad(lower_type(i.type), out);
      end_lifetime(out);
      return loaded;
    }
    case SystemOperation::text_from_bytes:
    case SystemOperation::text_from_rune: {
      const auto text = type(i.type).type_args.at(0);
      auto *out = storage(lower_type(text));
      start_lifetime(out);
      auto *status = runtime(*i.system == SystemOperation::text_from_bytes
                                  ? "lang_text_from_bytes_v1" : "lang_text_from_rune_v1",
                             builder.getInt64Ty(), {out, args[0]});
      auto *result = stdlib_result(i.type, status,
          [&] { return builder.CreateLoad(lower_type(text), out); });
      end_lifetime(out);
      return result;
    }
    default: break;
    }
    auto element = [&] {
      if (type(i.type).kind == hir::TypeKind::Vec) return type(i.type).type_args.at(0);
      return type(sig.parameters.at(0).type).type_args.at(0);
    };
    switch (*i.system) {
    case SystemOperation::vec_empty:
    case SystemOperation::vec_make: {
      auto t = element();
      auto callback = [&](bool copy) -> llvm::Value * {
        return owning(t) ? static_cast<llvm::Value *>(value_op(t, copy))
                         : llvm::ConstantPointerNull::get(builder.getPtrTy());
      };
      auto *value = runtime("lang_vec_new_v1", builder.getPtrTy(),
                            {builder.getInt64(0), builder.getInt64(stride(t)), callback(true), callback(false)});
      if (*i.system == SystemOperation::vec_make)
        runtime("lang_vec_resize_v1", builder.getVoidTy(), {value, args[0], element_storage(t, args[1])});
      gc_register_vec(value, t);
      return value;
    }
    case SystemOperation::vec_len: return vec_field(args[0], 1, builder.getInt64Ty());
    case SystemOperation::vec_capacity: return vec_field(args[0], 8, builder.getInt64Ty());
    case SystemOperation::vec_reserve:
      runtime("lang_vec_reserve_v1", builder.getVoidTy(), {args[0], args[1]}); break;
    case SystemOperation::vec_push: {
      auto t = element();
      auto *copy = copy_value(t, args[1]);
      runtime("lang_vec_push_v1", builder.getVoidTy(), {args[0], element_storage(t, copy)}); break;
    }
    case SystemOperation::vec_pop: {
      auto t = element();
      auto *layout = lower_type(t);
      const auto &slot_layout = module.getDataLayout();
      const uint64_t bytes = slot_layout.getTypeAllocSize(layout).getFixedValue();
      // lang_vec_pop_v1 does exactly this for a trivial element: reject the
      // empty pop before the pin check, acquire-load iterations and panic when
      // pinned, move the original last slot's typed bytes, publish the shorter
      // length and initialized=min(original initialized,last), then report
      // Some. Inline that path only when one typed load/store covers the slot:
      // non-owning, non-empty (a zero-sized slot has no loadable bytes), an
      // allocation size equal to the published stride, and ABI alignment within
      // the 16-byte Vec payload guarantee. Owning elements and every other
      // shape keep the runtime ABI call, including its GC/callback handling.
      if (!owning(t) && bytes != 0 && bytes == stride(t) &&
          slot_layout.getABITypeAlign(layout).value() <= 16) {
        auto *length = vec_field(args[0], 1, builder.getInt64Ty());
        // The empty pop reports None before any pin check. The success branch
        // is stdlib_option's own block, so the moved SSA value is the payload;
        // no out scratch or lifetime is needed.
        return stdlib_option(
            i.type, builder.CreateICmpNE(length, builder.getInt64(0)),
            [&]() -> llvm::Value * {
              // Same acquire pin check as the runtime: panic before any element
              // or header byte changes.
              auto *iterations = builder.CreateLoad(
                  builder.getInt64Ty(),
                  builder.CreateStructGEP(vec_type, args[0], 4));
              iterations->setAtomic(llvm::AtomicOrdering::Acquire);
              iterations->setAlignment(llvm::Align(8));
              vec_memory(iterations, true);
              guard(builder.CreateICmpNE(iterations, builder.getInt64(0)), 8,
                    i.source);
              auto *last = builder.CreateSub(length, builder.getInt64(1));
              auto *data = vec_field(args[0], 5, builder.getPtrTy());
              auto *address = builder.CreateGEP(layout, data, last);
              auto *moved = builder.CreateLoad(layout, address);
              vec_memory(moved, false);
              // Publish the shorter prefix before the moved bytes leave the
              // Vec; pop never shrinks storage.
              vec_memory(builder.CreateStore(
                             last,
                             builder.CreateStructGEP(vec_type, args[0], 1)),
                         true);
              auto *initialized = vec_field(args[0], 2, builder.getInt64Ty());
              auto *kept = builder.CreateSelect(
                  builder.CreateICmpUGT(initialized, last), last, initialized);
              vec_memory(builder.CreateStore(
                             kept,
                             builder.CreateStructGEP(vec_type, args[0], 2)),
                         true);
              return moved;
            });
      }
      const bool empty = bytes == 0;
      auto *out = storage(empty ? builder.getInt8Ty() : layout);
      start_lifetime(out);
      auto *present = runtime("lang_vec_pop_v1", builder.getInt1Ty(), {args[0], out});
      auto *result = stdlib_option(i.type, present,
          [&] { return builder.CreateLoad(layout, out); });
      end_lifetime(out);
      return result;
    }
    case SystemOperation::vec_resize: {
      auto t = element();
      const auto kind = type(t).kind;
      const bool scalar = kind == hir::TypeKind::SignedInt ||
                          kind == hir::TypeKind::UnsignedInt ||
                          kind == hir::TypeKind::Isize ||
                          kind == hir::TypeKind::Usize ||
                          kind == hir::TypeKind::Float;
      auto *vec = args[0];
      auto *length = args[1];
      auto *fill = args[2];
      if (!scalar || owning(t)) {
        runtime("lang_vec_resize_v1", builder.getVoidTy(),
                {vec, length, element_storage(t, fill)});
        break;
      }

      // A scalar fill is already an SSA snapshot. Within capacity, preserve
      // the runtime's no-op and iteration-pin order around a typed fill loop.
      auto *function = builder.GetInsertBlock()->getParent();
      auto block = [&](const char *name) {
        return llvm::BasicBlock::Create(context, name, function);
      };
      auto *changed = block("vec.resize.changed");
      auto *slow = block("vec.resize.grow_storage");
      auto *in_place = block("vec.resize.in_place");
      auto *shorten = block("vec.resize.shorten");
      auto *loop = block("vec.resize.fill");
      auto *publish = block("vec.resize.publish");
      auto *done = block("vec.resize.done");
      auto *old = vec_field(vec, 1, builder.getInt64Ty());
      builder.CreateCondBr(builder.CreateICmpEQ(length, old), done, changed);
      builder.SetInsertPoint(changed);
      auto *capacity = vec_field(vec, 8, builder.getInt64Ty());
      builder.CreateCondBr(builder.CreateICmpUGT(length, capacity), slow,
                           in_place);

      builder.SetInsertPoint(slow);
      runtime("lang_vec_resize_v1", builder.getVoidTy(),
              {vec, length, element_storage(t, fill)});
      builder.CreateBr(done);

      builder.SetInsertPoint(in_place);
      auto *iterations = vec_field(vec, 4, builder.getInt64Ty());
      iterations->setAtomic(llvm::AtomicOrdering::Acquire);
      iterations->setAlignment(llvm::Align(8));
      guard(builder.CreateICmpNE(iterations, builder.getInt64(0)), 8, i.source);
      auto *data = vec_field(vec, 5, builder.getPtrTy());
      auto *ready = builder.GetInsertBlock();
      builder.CreateCondBr(builder.CreateICmpULT(length, old), shorten, loop);

      builder.SetInsertPoint(shorten);
      auto *initialized = vec_field(vec, 2, builder.getInt64Ty());
      auto *keep = builder.CreateSelect(
          builder.CreateICmpULT(initialized, length), initialized, length);
      builder.CreateBr(publish);

      builder.SetInsertPoint(loop);
      auto *index = builder.CreatePHI(builder.getInt64Ty(), 2);
      index->addIncoming(old, ready);
      auto *address = builder.CreateGEP(lower_type(t), data, index);
      vec_memory(builder.CreateStore(fill, address), false);
      auto *next = builder.CreateAdd(index, builder.getInt64(1));
      index->addIncoming(next, loop);
      auto *back_edge = builder.CreateCondBr(
          builder.CreateICmpEQ(next, length), publish, loop);
      if (module.getTargetTriple().str() == "x86_64-unknown-linux-gnu" &&
          machine->getOptLevel() == llvm::CodeGenOptLevel::Default) {
        // Keep the optimized fill loop within an x86 instruction cache line.
        auto *alignment = llvm::MDNode::get(
            context, {llvm::MDString::get(context, "llvm.loop.align"),
                      llvm::ConstantAsMetadata::get(builder.getInt32(64))});
        auto *loop_id = llvm::MDNode::getDistinct(context, {nullptr, alignment});
        loop_id->replaceOperandWith(0, loop_id);
        back_edge->setMetadata(llvm::LLVMContext::MD_loop, loop_id);
      }

      builder.SetInsertPoint(publish);
      auto *prefix = builder.CreatePHI(builder.getInt64Ty(), 2);
      prefix->addIncoming(keep, shorten);
      prefix->addIncoming(length, loop);
      vec_memory(builder.CreateStore(
                     prefix, builder.CreateStructGEP(vec_type, vec, 2)), true);
      vec_memory(builder.CreateStore(
                     length, builder.CreateStructGEP(vec_type, vec, 1)), true);
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      break;
    }
    case SystemOperation::vec_clear: {
      if (owning(element())) {
        runtime("lang_vec_clear_v1", builder.getVoidTy(), {args[0]});
        break;
      }
      auto *length = vec_field(args[0], 1, builder.getInt64Ty());
      auto *initialized = vec_field(args[0], 2, builder.getInt64Ty());
      auto *present = builder.CreateICmpNE(
          builder.CreateOr(length, initialized), builder.getInt64(0));
      auto *function = builder.GetInsertBlock()->getParent();
      auto *clear = llvm::BasicBlock::Create(context, "vec.clear", function);
      auto *done = llvm::BasicBlock::Create(context, "vec.cleared", function);
      builder.CreateCondBr(present, clear, done);
      builder.SetInsertPoint(clear);
      auto *iterations = builder.CreateLoad(
          builder.getInt64Ty(),
          builder.CreateStructGEP(vec_type, args[0], 4));
      iterations->setAtomic(llvm::AtomicOrdering::Acquire);
      iterations->setAlignment(llvm::Align(8));
      vec_memory(iterations, true);
      guard(builder.CreateICmpNE(iterations, builder.getInt64(0)), 8, i.source);
      for (unsigned field : {2u, 1u}) {
        auto *address = builder.CreateStructGEP(vec_type, args[0], field);
        vec_memory(builder.CreateStore(builder.getInt64(0), address), true);
      }
      builder.CreateBr(done);
      builder.SetInsertPoint(done);
      break;
    }
    case SystemOperation::vec_try_move_trivial:
      return runtime("lang_vec_try_move_trivial_v1", builder.getInt1Ty(),
                     {args[0], args[1], args[2], args[3]});
    case SystemOperation::vec_extend_trivial:
      return runtime("lang_vec_extend_trivial_v1", builder.getInt1Ty(),
                     {args[0], args[1]});
    case SystemOperation::vec_pin_begin:
      runtime("lang_vec_begin_v1", builder.getInt64Ty(), {args[0]}); break;
    case SystemOperation::vec_pin_end:
      runtime("lang_vec_end_v1", builder.getVoidTy(), {args[0]}); break;
    default: fail("invalid verified value operation");
    }
    return builder.getInt8(0);
  }

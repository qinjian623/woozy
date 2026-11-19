  // All offsets and sizes come from the same storage types as codegen. In
  // particular an enum stores a tag followed by every variant's record,
  // rather than a C union; DW_TAG_variant_part hides inactive payloads.
  llvm::DIType *pointer(llvm::DIType *pointee) {
    const auto &dl = module.getDataLayout();
    return di.createPointerType(pointee, dl.getPointerSizeInBits(),
                                dl.getPointerABIAlignment(0).value() * 8);
  }
  llvm::DIType *unsigned_type(unsigned bits) {
    return di.createBasicType("u" + std::to_string(bits), bits,
                              llvm::dwarf::DW_ATE_unsigned);
  }
  llvm::DIDerivedType *member(llvm::DIType *parent, llvm::StructType *layout,
      unsigned index, llvm::StringRef name, llvm::DIType *field,
      SourceSpan source = {SourceSnapshotId{0}, 0, 0}) {
    const auto &dl = module.getDataLayout();
    const auto p = position(source);
    auto *storage = layout->getElementType(index);
    return di.createMemberType(parent, name, p ? p->file : nullptr,
        p ? p->line : 0, dl.getTypeAllocSizeInBits(storage),
        dl.getABITypeAlign(storage).value() * 8,
        dl.getStructLayout(layout)->getElementOffsetInBits(index),
        llvm::DINode::FlagZero, field);
  }
  llvm::DICompositeType *record(llvm::StringRef name, llvm::StructType *layout,
      const std::vector<std::pair<std::string, llvm::DIType *>> &fields) {
    const auto &dl = module.getDataLayout();
    auto *result = di.createStructType(nullptr, name, nullptr, 0,
        dl.getTypeSizeInBits(layout), dl.getABITypeAlign(layout).value() * 8,
        llvm::DINode::FlagZero, nullptr, di.getOrCreateArray({}));
    std::vector<llvm::Metadata *> members;
    for (size_t i = 0; i < fields.size(); ++i)
      members.push_back(member(result, layout, i, fields[i].first, fields[i].second));
    di.replaceArrays(result, di.getOrCreateArray(members));
    return result;
  }
  std::string type_name(TypeId id) const {
    const auto t = *aggregates.types.find(id);
    std::string name;
    switch (t.kind) {
    case hir::TypeKind::Unit: return "Unit";
    case hir::TypeKind::Bool: return "bool";
    case hir::TypeKind::SignedInt: return "i" + std::to_string(t.bits);
    case hir::TypeKind::UnsignedInt: return "u" + std::to_string(t.bits);
    case hir::TypeKind::Float: return "f" + std::to_string(t.bits);
    case hir::TypeKind::Isize: return "isize";
    case hir::TypeKind::Usize: return "usize";
    case hir::TypeKind::String: return "string";
    case hir::TypeKind::Vec: name = "Vec"; break;
    case hir::TypeKind::Slice: name = "Slice"; break;
    case hir::TypeKind::Weak: name = "Weak"; break;
    case hir::TypeKind::Task: name = "Task"; break;
    case hir::TypeKind::Function: {
      name = "func(";
      for (size_t p = 0; p < t.parameters.size(); ++p) {
        if (p) name += ",";
        if (t.parameters[p].mode == hir::ParamMode::inout) name += "inout ";
        name += type_name(t.parameters[p].type);
      }
      name += ")->" + type_name(t.result);
      if (t.throws_type) name += " throws " + type_name(*t.throws_type);
      return name;
    }
    case hir::TypeKind::NominalStruct:
    case hir::TypeKind::NominalEnum:
    case hir::TypeKind::TraitObject: {
      const auto &s = hir.resolved->symbols.at(t.declaration.value);
      name = hir.resolved->project->modules.at(s.module.value).logical_path + "." + s.name;
      if (t.kind == hir::TypeKind::TraitObject) name = "dyn " + name;
      break;
    }
    case hir::TypeKind::ThreadHandle: return "ThreadHandle";
    case hir::TypeKind::MutexHandle: return "MutexHandle";
    case hir::TypeKind::CondvarHandle: return "CondvarHandle";
    case hir::TypeKind::AtomicI64Handle: return "AtomicI64Handle";
    case hir::TypeKind::IpcHandle: return "IpcHandle";
    default: return "<Woozy type: layout unavailable>";
    }
    if (!t.type_args.empty()) {
      name += "[";
      for (size_t a = 0; a < t.type_args.size(); ++a) {
        if (a) name += ",";
        name += type_name(t.type_args[a]);
      }
      name += "]";
    }
    return name;
  }
  llvm::DIType *type(TypeId id) {
    if (auto it = types.find(id); it != types.end()) return it->second;
    // lower_type can intern substituted storage-only types in aggregates.
    const auto t = *aggregates.types.find(id);
    const auto name = type_name(id);
    auto *storage = lower_type(id);
    const auto &dl = module.getDataLayout();
    llvm::DIType *result = nullptr;
    switch (t.kind) {
    case hir::TypeKind::Bool:
    case hir::TypeKind::SignedInt:
    case hir::TypeKind::UnsignedInt:
    case hir::TypeKind::Isize:
    case hir::TypeKind::Usize:
    case hir::TypeKind::Float: {
      auto encoding = t.kind == hir::TypeKind::Bool ? llvm::dwarf::DW_ATE_boolean
          : t.kind == hir::TypeKind::Float ? llvm::dwarf::DW_ATE_float
          : t.kind == hir::TypeKind::SignedInt || t.kind == hir::TypeKind::Isize
              ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned;
      result = di.createBasicType(name, dl.getTypeAllocSizeInBits(storage), encoding);
      break;
    }
    case hir::TypeKind::NominalStruct:
    case hir::TypeKind::NominalEnum: {
      const auto &decl = aggregates.declaration(id);
      const auto p = position(decl.source.span);
      auto *layout = llvm::cast<llvm::StructType>(storage);
      auto *forward = di.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
          name, nullptr, p ? p->file : nullptr, p ? p->line : 0);
      types.emplace(id, forward); // recursive Vec[Self] and function fields
      std::vector<llvm::Metadata *> members;
      auto fields = [&](llvm::DICompositeType *parent, llvm::StructType *ir,
                        const std::vector<hir::FieldDecl> &fs) {
        std::vector<llvm::Metadata *> elements;
        for (size_t f = 0; f < fs.size(); ++f)
          elements.push_back(member(parent, ir, f,
              hir.resolved->symbols.at(fs[f].symbol.value).name,
              type(fs[f].type), fs[f].source.span));
        return elements;
      };
      if (t.kind == hir::TypeKind::NominalStruct) {
        members = fields(forward, layout, aggregates.fields(id));
      } else {
        std::vector<llvm::Metadata *> enumerators, variants;
        for (size_t v = 0; v < decl.variants.size(); ++v)
          enumerators.push_back(di.createEnumerator(
              hir.resolved->symbols.at(decl.variants[v].symbol.value).name, v, true));
        auto *tag_type = di.createEnumerationType(forward, name + ".tag", p ? p->file : nullptr,
            p ? p->line : 0, dl.getTypeSizeInBits(layout->getElementType(0)),
            dl.getABITypeAlign(layout->getElementType(0)).value() * 8,
            di.getOrCreateArray(enumerators), unsigned_type(32));
        auto *tag = member(forward, layout, 0, "tag", tag_type, decl.source.span);
        members.push_back(tag);
        for (size_t v = 0; v < decl.variants.size(); ++v) {
          const auto &variant = decl.variants[v];
          const auto vn = hir.resolved->symbols.at(variant.symbol.value).name;
          auto *ir = llvm::cast<llvm::StructType>(layout->getElementType(v + 1));
          auto *payload = record(name + "." + vn, ir, {});
          di.replaceArrays(payload, di.getOrCreateArray(fields(payload, ir,
              aggregates.fields(id, variant.symbol))));
          auto vp = position(variant.source.span);
          variants.push_back(di.createVariantMemberType(forward, vn,
              vp ? vp->file : nullptr, vp ? vp->line : 0, dl.getTypeSizeInBits(ir),
              dl.getABITypeAlign(ir).value() * 8,
              dl.getStructLayout(layout)->getElementOffsetInBits(v + 1),
              llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), v),
              llvm::DINode::FlagZero, payload));
        }
        members.push_back(di.createVariantPart(forward, "", p ? p->file : nullptr,
            p ? p->line : 0, dl.getTypeSizeInBits(layout),
            dl.getABITypeAlign(layout).value() * 8, llvm::DINode::FlagZero,
            tag, di.getOrCreateArray(variants)));
      }
      auto *complete = di.createStructType(nullptr, name, p ? p->file : nullptr,
          p ? p->line : 0, dl.getTypeAllocSizeInBits(storage), dl.getABITypeAlign(storage).value() * 8,
          llvm::DINode::FlagZero, nullptr, di.getOrCreateArray(members));
      di.replaceTemporary(llvm::TempDICompositeType(forward), complete);
      result = complete;
      break;
    }
    case hir::TypeKind::Vec: {
      // The value is a pointer to LangVec, whose data is typed element storage.
      auto *forward = di.createReplaceableCompositeType(llvm::dwarf::DW_TAG_structure_type,
          name, nullptr, nullptr, 0);
      result = pointer(forward);
      types.emplace(id, result);
      auto *descriptor = record(name, vec_layout, {
          {"refs", unsigned_type(64)}, {"len", unsigned_type(64)},
          {"initialized", unsigned_type(64)}, {"stride", unsigned_type(64)},
          {"iterations", unsigned_type(64)}, {"data", pointer(type(t.type_args[0]))},
          {"copy", pointer(nullptr)}, {"drop", pointer(nullptr)},
          {"capacity", unsigned_type(64)}, {"weak_control", pointer(nullptr)}});
      di.replaceTemporary(llvm::TempDICompositeType(forward), descriptor);
      break;
    }
    case hir::TypeKind::String:
      // len counts bytes for UTF-8 printers; length is the language rune count.
      result = record(name, llvm::cast<llvm::StructType>(storage), {
          {"data", pointer(unsigned_type(8))}, {"len", unsigned_type(64)},
          {"length", unsigned_type(64)}, {"owner", pointer(nullptr)}});
      break;
    case hir::TypeKind::Slice: {
      // There is no direct data pointer: element i is owner->data[start+i].
      hir::Type vec_type;
      vec_type.kind = hir::TypeKind::Vec;
      vec_type.type_args = {t.type_args[0]};
      const auto vec = aggregates.types.intern(vec_type);
      result = record(name, llvm::cast<llvm::StructType>(storage), {
          {"owner", type(vec)}, {"start", unsigned_type(64)}, {"len", unsigned_type(64)}});
      break;
    }
    case hir::TypeKind::Function:
      result = record(name, llvm::cast<llvm::StructType>(storage), {
          {"code", pointer(nullptr)}, {"env", pointer(nullptr)}}); break;
    case hir::TypeKind::Weak:
      result = record(name, llvm::cast<llvm::StructType>(storage), {
          {"control", pointer(nullptr)}, {"code", pointer(nullptr)}}); break;
    case hir::TypeKind::TraitObject:
      result = record(name, llvm::cast<llvm::StructType>(storage), {
          {"data", pointer(nullptr)}, {"vtable", pointer(nullptr)}}); break;
    case hir::TypeKind::ThreadHandle:
    case hir::TypeKind::MutexHandle:
    case hir::TypeKind::CondvarHandle:
    case hir::TypeKind::AtomicI64Handle:
    case hir::TypeKind::IpcHandle:
      // This describes the header subobject only, not the runtime-private
      // trailing payload. Codegen uses exactly this canonical LangHandle IR.
      result = di.createTypedef(pointer(record(name + ".header", handle_layout, {
          {"refs", unsigned_type(64)}, {"kind", unsigned_type(64)},
          {"destroy", pointer(nullptr)}, {"weak_control", pointer(nullptr)}})),
          name, nullptr, 0, nullptr); break;
    case hir::TypeKind::Task:
      result = pointer(di.createForwardDecl(llvm::dwarf::DW_TAG_structure_type,
                                            name, nullptr, nullptr, 0)); break;
    case hir::TypeKind::Unit:
      result = di.createStructType(nullptr, name, nullptr, 0, dl.getTypeSizeInBits(storage),
          dl.getABITypeAlign(storage).value() * 8, llvm::DINode::FlagZero, nullptr,
          di.getOrCreateArray({})); break;
    default:
      // C foreign types need their binding's canonical ABI, not guessed fields.
      result = di.createUnspecifiedType(name); break;
    }
    types[id] = result;
    return result;
  }

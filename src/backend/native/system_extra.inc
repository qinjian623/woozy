  llvm::Value *stdlib_variant(TypeId id, unsigned tag, llvm::Value *payload) {
    llvm::Value *result = llvm::UndefValue::get(lower_type(id));
    result = builder.CreateInsertValue(result, builder.getInt32(tag), {0});
    if (payload) result = builder.CreateInsertValue(result, payload, {tag + 1, 0});
    return result;
  }
  llvm::Value *stdlib_result(TypeId id, llvm::Value *status,
                            const std::function<llvm::Value *()> &success) {
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *ok = llvm::BasicBlock::Create(context, "result.ok", fn);
    auto *error = llvm::BasicBlock::Create(context, "result.error", fn);
    auto *done = llvm::BasicBlock::Create(context, "result.done", fn);
    builder.CreateCondBr(builder.CreateICmpSLT(status, builder.getInt64(0)), error, ok);
    builder.SetInsertPoint(ok);
    auto *value = stdlib_variant(id, 0, success());
    auto *ok_end = builder.GetInsertBlock();
    builder.CreateBr(done);
    builder.SetInsertPoint(error);
    auto *code = builder.CreateTrunc(builder.CreateNeg(status), builder.getInt32Ty());
    auto *failure = stdlib_variant(id, 1, code);
    auto *error_end = builder.GetInsertBlock();
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    auto *joined = builder.CreatePHI(lower_type(id), 2);
    joined->addIncoming(value, ok_end);
    joined->addIncoming(failure, error_end);
    return joined;
  }
  llvm::Value *stdlib_option(TypeId id, llvm::Value *present,
                            const std::function<llvm::Value *()> &success) {
    auto *fn = builder.GetInsertBlock()->getParent();
    auto *some = llvm::BasicBlock::Create(context, "option.some", fn);
    auto *none = llvm::BasicBlock::Create(context, "option.none", fn);
    auto *done = llvm::BasicBlock::Create(context, "option.done", fn);
    builder.CreateCondBr(present, some, none);
    builder.SetInsertPoint(some);
    auto *value = stdlib_variant(id, 1, success());
    auto *some_end = builder.GetInsertBlock();
    builder.CreateBr(done);
    builder.SetInsertPoint(none);
    auto *empty = stdlib_variant(id, 0, nullptr);
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    auto *joined = builder.CreatePHI(lower_type(id), 2);
    joined->addIncoming(value, some_end);
    joined->addIncoming(empty, none);
    return joined;
  }
#include "network_ops.inc"
  llvm::Value *extra_system_call(const ir::Instruction &i,
                                 llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    if (auto *value = network_system_call(i,args)) return value;
    const auto operation = *i.system;
    if (operation >= SystemOperation::math_fabs && operation <= SystemOperation::math_signbit) {
      const auto *spec = hir::system_operation_spec(operation);
      const auto name = "lang_"+std::string(spec->name.substr(8))+"_v1";
      if (operation == SystemOperation::math_frexp) {
        auto *exponent = storage(builder.getInt32Ty());
        auto *fraction = runtime(name.c_str(),builder.getDoubleTy(),{args[0],exponent});
        llvm::Value *result = llvm::UndefValue::get(lower_type(i.type));
        result = builder.CreateInsertValue(result,fraction,{0});
        return builder.CreateInsertValue(result,builder.CreateLoad(builder.getInt32Ty(),exponent),{1});
      }
      return runtime(name.c_str(),lower_type(i.type),args);
    }

    if (operation >= SystemOperation::num_parse_i64 &&
        operation <= SystemOperation::num_format_f64) {
      const auto *spec = hir::system_operation_spec(operation);
      const bool parse = operation <= SystemOperation::num_parse_f64;
      const auto result = parse ? type(i.type).type_args.at(0) : i.type;
      auto *out = storage(lower_type(result));
      const auto name = "lang_" + std::string(spec->name.substr(8)) + "_v1";
      if (parse) {
        const auto parameter = type(i.signature).parameters[0].type;
        auto *status = runtime(name.c_str(), builder.getInt64Ty(),
                              {element_storage(parameter, args[0]), out});
        return stdlib_result(i.type, status, [&]() -> llvm::Value * {
          return builder.CreateLoad(lower_type(result), out);
        });
      }
      runtime(name.c_str(), builder.getVoidTy(), {out, args[0]});
      return builder.CreateLoad(lower_type(result), out);
    }
    if (operation == SystemOperation::fs_stat_bytes) {
      // The runtime stat ABI takes a borrowed path pointer plus an explicit
      // byte length. A Vec descriptor keeps its element pointer in field 5 and
      // its current length in field 1; an empty Vec has a null pointer and a
      // zero length, which the runtime accepts without forming an address.
      auto *path = vec_field(args[0], 5, builder.getPtrTy());
      auto *length = vec_field(args[0], 1, builder.getInt64Ty());
      const auto result_type = type(i.type).type_args.at(0);
      auto *out = storage(lower_type(result_type));
      auto *status = runtime("lang_sys_stat_v1", builder.getInt64Ty(),
                             {path, length, out});
      return stdlib_result(i.type, status, [&]() -> llvm::Value * {
        return builder.CreateLoad(lower_type(result_type), out);
      });
    }
    const char *name = nullptr;
    bool output = false, output_first = false, raw = false;
    switch (*i.system) {
    case SystemOperation::fs_stat: name = "lang_sys_stat_v1"; output = true; break;
    case SystemOperation::fs_lstat: name = "lang_sys_lstat_v1"; output = true; break;
    case SystemOperation::fs_fstat: name = "lang_sys_fstat_v1"; output = true; break;
    case SystemOperation::fs_rename: name = "lang_sys_rename_v1"; break;
    case SystemOperation::fs_chmod: name = "lang_sys_chmod_v1"; break;
    case SystemOperation::fs_fchmod: name = "lang_sys_fchmod_v1"; break;
    case SystemOperation::fs_chdir: name = "lang_sys_chdir_v1"; break;
    case SystemOperation::fs_getcwd: name = "lang_sys_getcwd_vec_v1"; raw = true; break;
    case SystemOperation::fs_getdents: name = "lang_sys_getdents_vec_v1"; raw = true; break;
    case SystemOperation::fs_readlink: name = "lang_sys_readlink_vec_v1"; raw = true; break;
    case SystemOperation::fs_symlink: name = "lang_sys_symlink_v1"; break;
    case SystemOperation::fs_link: name = "lang_sys_link_v1"; break;
    case SystemOperation::fs_truncate: name = "lang_sys_truncate_v1"; break;
    case SystemOperation::fs_ftruncate: name = "lang_sys_ftruncate_v1"; break;
    case SystemOperation::fs_fsync: name = "lang_sys_fsync_v1"; break;
    case SystemOperation::fs_fdatasync: name = "lang_sys_fdatasync_v1"; break;
    case SystemOperation::fs_dup: name = "lang_sys_dup_v1"; break;
    case SystemOperation::fs_dup2: name = "lang_sys_dup2_v1"; break;
    case SystemOperation::fs_pipe: name = "lang_sys_pipe_v1"; output = output_first = true; break;
    case SystemOperation::fs_mkfifo: name = "lang_sys_mkfifo_v1"; break;
    case SystemOperation::fs_access: name = "lang_sys_access_v1"; break;
    case SystemOperation::os_getuid: name = "lang_sys_getuid_v1"; break;
    case SystemOperation::os_geteuid: name = "lang_sys_geteuid_v1"; break;
    case SystemOperation::os_getgid: name = "lang_sys_getgid_v1"; break;
    case SystemOperation::os_getegid: name = "lang_sys_getegid_v1"; break;
    case SystemOperation::os_getppid: name = "lang_sys_getppid_v1"; break;
    case SystemOperation::os_gettid: name = "lang_sys_gettid_v1"; break;
    case SystemOperation::os_setpgid: name = "lang_sys_setpgid_v1"; break;
    case SystemOperation::os_getpgid: name = "lang_sys_getpgid_v1"; break;
    case SystemOperation::os_setsid: name = "lang_sys_setsid_v1"; break;
    case SystemOperation::os_kill: name = "lang_sys_kill_v1"; break;
    case SystemOperation::clock_gettime: name = "lang_sys_clock_gettime_v1"; output = true; break;
    case SystemOperation::nanosleep: name = "lang_sys_nanosleep_vec_v1"; raw = true; break;
    case SystemOperation::getrandom: name = "lang_sys_getrandom_vec_v1"; raw = true; break;
    default: return nullptr;
    }
    const auto signature = type(i.signature);
    std::vector<llvm::Value *> actual;
    for (size_t a = 0; a < args.size(); ++a) {
      const auto parameter = signature.parameters[a].type;
      if (type(parameter).kind == hir::TypeKind::String) {
        actual.push_back(builder.CreateExtractValue(args[a], {0}));
        actual.push_back(builder.CreateExtractValue(args[a], {1}));
      } else if (type(parameter).kind == hir::TypeKind::NominalStruct) {
        actual.push_back(element_storage(parameter, args[a]));
      } else actual.push_back(args[a]);
    }
    llvm::AllocaInst *out = nullptr;
    TypeId result_type{0};
    if (output) {
      result_type = type(i.type).type_args.at(0);
      out = storage(lower_type(result_type));
      if (output_first) actual.insert(actual.begin(), out);
      else actual.push_back(out);
    }
    auto *status = runtime(name, builder.getInt64Ty(), actual);
    if (raw) return status;
    return stdlib_result(i.type, status, [&]() -> llvm::Value * {
      if (out) return builder.CreateLoad(lower_type(result_type), out);
      const auto success = type(i.type).type_args.at(0);
      if (type(success).kind == hir::TypeKind::Unit) return builder.getInt8(0);
      return builder.CreateIntCast(status, lower_type(success), signed_(type(success)));
    });
  }

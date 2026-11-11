  llvm::Value *process_system_call(const ir::Instruction &i,
                                   llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    const hir::SystemOperationSpec *spec = nullptr;
    for (const auto &candidate : hir::process_system_operations)
      if (candidate.operation == *i.system) { spec = &candidate; break; }
    if (!spec) return nullptr;
    const auto operation = *i.system;
    const std::string name = "lang_" + std::string(spec->name.substr(8)) + "_v1";
    const auto &signature = type(i.signature);
    std::vector<llvm::Value *> actual;
    for (size_t a = 0; a < args.size(); ++a) {
      const auto parameter = signature.parameters[a].type;
      actual.push_back(type(parameter).kind == hir::TypeKind::String
                       ? element_storage(parameter,args[a]) : args[a]);
    }
    if (operation == SystemOperation::ipc_length)
      return runtime(name.c_str(),builder.getInt64Ty(),actual);
    const bool wait = operation == SystemOperation::ipc_cond_wait ||
                      operation == SystemOperation::ipc_sem_wait ||
                      operation == SystemOperation::ipc_sem_trywait;
    const bool plain = type(i.type).kind == hir::TypeKind::Unit || wait ||
                       operation == SystemOperation::ipc_sem_value;
    if (plain) {
      auto *out = operation == SystemOperation::ipc_sem_value ? storage(builder.getInt32Ty()) : nullptr;
      if (out) actual.insert(actual.begin(),out);
      auto *status = runtime(name.c_str(),builder.getInt64Ty(),actual);
      auto *bad = builder.CreateICmpSLT(status,builder.getInt64(0));
      if (wait) {
        const int64_t expected = operation == SystemOperation::ipc_sem_trywait ? -11 : -110;
        bad = builder.CreateAnd(bad,builder.CreateICmpNE(status,builder.getInt64(expected)));
      }
      guard(bad,10,i.source);
      if (out) return builder.CreateLoad(builder.getInt32Ty(),out);
      if (wait) return builder.CreateICmpEQ(status,builder.getInt64(0));
      return builder.getInt8(0);
    }
    const auto result = type(i.type).type_args.at(0);
    const bool getenv = operation == SystemOperation::process_getenv ||
                        operation == SystemOperation::process_getenv_bytes;
    const auto output_type = getenv ? type(result).type_args.at(0) : result;
    bool output = false;
    switch (operation) {
    case SystemOperation::process_args:
    case SystemOperation::process_args_bytes:
    case SystemOperation::process_environment:
    case SystemOperation::process_environment_bytes:
    case SystemOperation::process_getenv:
    case SystemOperation::process_getenv_bytes:
    case SystemOperation::signal_mask:
    case SystemOperation::signal_pending:
    case SystemOperation::signal_wait:
    case SystemOperation::ipc_map_anon:
    case SystemOperation::ipc_map_file:
    case SystemOperation::ipc_read_bytes:
    case SystemOperation::ipc_read_i32:
    case SystemOperation::ipc_read_i64:
    case SystemOperation::ipc_atomic_load:
    case SystemOperation::ipc_atomic_add:
    case SystemOperation::ipc_atomic_compare: output = true; break;
    default: break;
    }
    auto *out = output ? storage(lower_type(output_type)) : nullptr;
    if (out) actual.insert(actual.begin(),out);
    auto *wait_status = operation == SystemOperation::process_waitpid ? storage(builder.getInt32Ty()) : nullptr;
    if (wait_status) actual.push_back(wait_status);
    auto *status = runtime(name.c_str(),builder.getInt64Ty(),actual);
    return stdlib_result(i.type,status,[&]() -> llvm::Value * {
      if (getenv) return stdlib_option(result,builder.CreateICmpEQ(status,builder.getInt64(1)),[&]() -> llvm::Value * {
        return builder.CreateLoad(lower_type(output_type),out);
      });
      if (wait_status) {
        llvm::Value *value = llvm::UndefValue::get(lower_type(result));
        value = builder.CreateInsertValue(value,builder.CreateTrunc(status,builder.getInt32Ty()),{0});
        return builder.CreateInsertValue(value,builder.CreateLoad(builder.getInt32Ty(),wait_status),{1});
      }
      if (out) return builder.CreateLoad(lower_type(output_type),out);
      if (type(result).kind == hir::TypeKind::Unit) return builder.getInt8(0);
      return builder.CreateIntCast(status,lower_type(result),signed_(type(result)));
    });
  }

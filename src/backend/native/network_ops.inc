  llvm::Value *network_system_call(const ir::Instruction &i,
                                   llvm::ArrayRef<llvm::Value *> args) {
    using hir::SystemOperation;
    const char *name = nullptr;
    bool output = false;
    switch (*i.system) {
    case SystemOperation::net_socket: name = "lang_net_socket_v1"; break;
    case SystemOperation::net_bind: name = "lang_net_bind_v1"; break;
    case SystemOperation::net_connect: name = "lang_net_connect_v1"; break;
    case SystemOperation::net_listen: name = "lang_net_listen_v1"; break;
    case SystemOperation::net_accept: name = "lang_net_accept_record_v1"; output = true; break;
    case SystemOperation::net_send: name = "lang_net_send_vec_v1"; break;
    case SystemOperation::net_recv: name = "lang_net_recv_vec_v1"; break;
    case SystemOperation::net_sendto: name = "lang_net_sendto_vec_v1"; break;
    case SystemOperation::net_recvfrom: name = "lang_net_recvfrom_vec_v1"; output = true; break;
    case SystemOperation::net_shutdown: name = "lang_net_shutdown_v1"; break;
    case SystemOperation::net_getsockname: name = "lang_net_getsockname_v1"; output = true; break;
    case SystemOperation::net_getpeername: name = "lang_net_getpeername_v1"; output = true; break;
    case SystemOperation::net_setsockopt_int: name = "lang_net_setsockopt_int_v1"; break;
    case SystemOperation::net_getsockopt_int: name = "lang_net_getsockopt_int_v1"; output = true; break;
    case SystemOperation::net_setsockopt_bytes: name = "lang_net_setsockopt_vec_v1"; break;
    case SystemOperation::net_getsockopt_bytes: name = "lang_net_getsockopt_vec_v1"; break;
    case SystemOperation::net_socketpair: name = "lang_net_socketpair_v1"; output = true; break;
    case SystemOperation::net_poll: name = "lang_net_poll_vec_v1"; break;
    case SystemOperation::net_inet_pton: name = "lang_net_inet_pton_v1"; output = true; break;
    case SystemOperation::net_inet_ntop: name = "lang_net_format_address_v1"; output = true; break;
    case SystemOperation::net_lookup: name = "lang_net_lookup_v1"; output = true; break;
    default: return nullptr;
    }
    const auto signature = type(i.signature);
    std::vector<llvm::Value *> actual;
    for (size_t a = 0; a < args.size(); ++a) {
      const auto parameter = signature.parameters[a].type;
      if (type(parameter).kind == hir::TypeKind::String) {
        if (*i.system == SystemOperation::net_lookup)
          actual.push_back(element_storage(parameter,args[a]));
        else {
          actual.push_back(builder.CreateExtractValue(args[a],{0}));
          actual.push_back(builder.CreateExtractValue(args[a],{1}));
        }
      } else if (type(parameter).kind == hir::TypeKind::NominalStruct)
        actual.push_back(element_storage(parameter,args[a]));
      else actual.push_back(args[a]);
    }
    const auto result = type(i.type).type_args.at(0);
    auto *out = output ? storage(lower_type(result)) : nullptr;
    if (out) actual.push_back(out);
    auto *status = runtime(name,builder.getInt64Ty(),actual);
    return stdlib_result(i.type,status,[&]() -> llvm::Value * {
      if (out) return builder.CreateLoad(lower_type(result),out);
      if (type(result).kind == hir::TypeKind::Unit) return builder.getInt8(0);
      return builder.CreateIntCast(status,lower_type(result),signed_(type(result)));
    });
  }

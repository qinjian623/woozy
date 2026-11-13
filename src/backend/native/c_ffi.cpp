#include "c_ffi.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace lang::backend::native::cffi {
namespace {
using modules::CDeclKind;
using modules::CTypeRef;
using modules::Project;

std::string scalar_c(const std::string &name, bool unit_void = true) {
  if (name == "Unit") return unit_void ? "void" : "uint8_t";
  if (name == "bool") return "_Bool";
  if (name == "i8") return "int8_t";
  if (name == "i16") return "int16_t";
  if (name == "i32") return "int32_t";
  if (name == "i64") return "int64_t";
  if (name == "u8") return "uint8_t";
  if (name == "u16") return "uint16_t";
  if (name == "u32") return "uint32_t";
  if (name == "u64") return "uint64_t";
  if (name == "isize") return "intptr_t";
  if (name == "usize") return "size_t";
  if (name == "f32") return "float";
  if (name == "f64") return "double";
  throw std::invalid_argument("unknown C scalar spelling");
}
std::string buffer_name(const CTypeRef &ref) {
  if (ref.kind != CTypeRef::Kind::buffer || ref.args.size() != 1 ||
      ref.args[0].kind != CTypeRef::Kind::scalar)
    throw std::invalid_argument("invalid C buffer type");
  return std::string("wf_buf_") + (ref.writable ? "mut_" : "ro_") +
         ref.args[0].scalar;
}
// The managed callback userdata slot is exactly PtrMut[Unit]; the binding
// contract rejects any other spelling before generation.
bool unit_pointer(const CTypeRef &ref) {
  return ref.kind == CTypeRef::Kind::pointer && ref.writable &&
         ref.args.size() == 1 &&
         ref.args[0].kind == CTypeRef::Kind::scalar &&
         ref.args[0].scalar == "Unit";
}
struct Generator {
  const Project &p;
  std::ostringstream out;
  std::set<uint32_t> records;
  std::set<uint32_t> callbacks;
  std::set<uint32_t> defined;

  explicit Generator(const Project &project) : p(project) {}

  void collect(const CTypeRef &ref) {
    if (ref.kind == CTypeRef::Kind::scalar || ref.kind == CTypeRef::Kind::buffer)
      return;
    if (ref.kind == CTypeRef::Kind::pointer) {
      collect(ref.args.at(0));
      return;
    }
    const auto &d = p.c_type(ref.named);
    if (d.kind == CDeclKind::record) {
      if (!records.insert(ref.named).second) return;
      for (const auto &f : d.fields) collect(f.type);
    } else if (d.kind == CDeclKind::callback) {
      if (!callbacks.insert(ref.named).second) return;
      collect(d.result);
      for (const auto &param : d.parameters) collect(param.type);
    }
  }

  std::string ctype(const CTypeRef &ref, bool pointer_target = false) const {
    switch (ref.kind) {
    case CTypeRef::Kind::scalar:
      if (ref.scalar == "Unit" && pointer_target) return "void";
      return scalar_c(ref.scalar);
    case CTypeRef::Kind::pointer: {
      const auto &target = ref.args.at(0);
      auto base = ctype(target, true);
      if (target.kind == CTypeRef::Kind::scalar && target.scalar == "Unit")
        return ref.writable ? "void *" : "const void *";
      return (ref.writable ? std::string() : "const ") + base + " *";
    }
    case CTypeRef::Kind::buffer:
      return buffer_name(ref);
    case CTypeRef::Kind::named: {
      const auto &d = p.c_type(ref.named);
      switch (d.kind) {
      case CDeclKind::record: return "wf_rec_" + std::to_string(d.id);
      case CDeclKind::resource: return "void *";
      case CDeclKind::callback: return "wf_cb_" + std::to_string(d.id);
      case CDeclKind::opaque: return "void";
      }
    }
    }
    throw std::logic_error("unknown C type expression");
  }

  void record_definition(uint32_t id, std::set<uint32_t> &active) {
    if (defined.contains(id)) return;
    if (!active.insert(id).second)
      throw std::invalid_argument("recursive C record by value");
    const auto &d = p.c_type(id);
    for (const auto &f : d.fields) {
      if (f.type.kind == CTypeRef::Kind::named &&
          p.c_type(f.type.named).kind == CDeclKind::record)
        record_definition(f.type.named, active);
    }
    out << "struct wf_rec_" << id << " {\n";
    for (const auto &f : d.fields)
      out << "  " << ctype(f.type) << " " << f.name << ";\n";
    out << "};\n";
    out << "_Static_assert(sizeof(struct wf_rec_" << id << ") == " << d.size
        << ", \"record size\");\n";
    out << "_Static_assert(_Alignof(struct wf_rec_" << id << ") == " << d.align
        << ", \"record align\");\n";
    for (const auto &f : d.fields)
      out << "_Static_assert(offsetof(struct wf_rec_" << id << ", " << f.name
          << ") == " << f.offset << ", \"record field offset\");\n";
    active.erase(id);
    defined.insert(id);
  }

  void emit_callback_trampoline(uint64_t function, uint32_t c_type) {
    const auto &d = p.c_type(c_type);
    const bool unit = d.result.kind == CTypeRef::Kind::scalar &&
                      d.result.scalar == "Unit";
    const auto result = unit ? std::string("void") : ctype(d.result);
    const auto bridge = callback_bridge_symbol(function, c_type);
    out << "extern void " << bridge << "(void *out, void *const *args);\n";
    out << result << " " << callback_trampoline_symbol(function, c_type)
        << "(";
    for (size_t i = 0; i < d.parameters.size(); ++i) {
      if (i) out << ", ";
      out << ctype(d.parameters[i].type) << " p" << i;
    }
    if (d.parameters.empty()) out << "void";
    out << ") {\n";
    if (d.parameters.empty()) {
      out << "  void *args[1] = {0};\n";
    } else {
      out << "  void *args[" << d.parameters.size() << "] = {";
      for (size_t i = 0; i < d.parameters.size(); ++i) {
        if (i) out << ", ";
        out << "&p" << i;
      }
      out << "};\n";
    }
    if (unit) {
      out << "  " << bridge << "(0, args);\n";
    } else {
      out << "  " << result << " result;\n";
      out << "  " << bridge << "(&result, args);\n";
      out << "  return result;\n";
    }
    out << "}\n";
  }

  // One C trampoline per callback type/userdata mapping. The runtime context
  // is the userdata parameter; all other callback parameters are packed in
  // declaration order and the typed LLVM dispatcher reconstructs the source
  // call. No per-instance code or global current-closure slot is involved.
  void emit_registration_trampoline(uint32_t c_type, size_t userdata) {
    const auto &d = p.c_type(c_type);
    const bool unit = d.result.kind == CTypeRef::Kind::scalar &&
                      d.result.scalar == "Unit";
    const auto result = unit ? std::string("void") : ctype(d.result);
    const auto dispatch =
        registration_callback_dispatch_symbol(c_type, userdata);
    out << "extern void " << dispatch
        << "(void *context, void *out, void *const *args);\n";
    out << result << " "
        << registration_callback_trampoline_symbol(c_type, userdata) << "(";
    for (size_t i = 0; i < d.parameters.size(); ++i) {
      if (i) out << ", ";
      out << ctype(d.parameters[i].type) << " p" << i;
    }
    if (d.parameters.empty()) out << "void";
    out << ") {\n";
    const size_t packed = d.parameters.size() - (userdata < d.parameters.size());
    if (packed == 0) {
      out << "  void *args[1] = {0};\n";
    } else {
      out << "  void *args[" << packed << "] = {";
      size_t emitted = 0;
      for (size_t i = 0; i < d.parameters.size(); ++i) {
        if (i == userdata) continue;
        if (emitted++) out << ", ";
        out << "&p" << i;
      }
      out << "};\n";
    }
    if (unit) {
      out << "  " << dispatch << "(p" << userdata << ", 0, args);\n";
    } else {
      out << "  " << result << " result;\n";
      out << "  " << dispatch << "(p" << userdata << ", &result, args);\n";
      out << "  return result;\n";
    }
    out << "}\n";
  }

  void emit_callable(uint32_t id) {
    const auto &d = p.c_callable(id);
    const bool close = registration_close(p, d);
    std::vector<std::string> parameter_types;
    for (const auto &param : d.parameters) {
      auto type = ctype(param.type);
      if (type == "void")
        throw std::invalid_argument("Unit is not a C value parameter");
      parameter_types.push_back(type);
    }
    auto result = ctype(d.result.type);
    if (close) {
      // The explicit close operation never reaches the C symbol from Source:
      // runtime close drains leases and callbacks, then calls unregister.
      out << "extern void lang_c_registration_close_v1(void *);\n";
    } else {
      out << "extern " << result << " " << d.symbol << "(";
      for (size_t i = 0; i < parameter_types.size(); ++i) {
        if (i) out << ", ";
        out << parameter_types[i];
      }
      if (parameter_types.empty()) out << "void";
      out << ");\n";
    }
    out << "void " << adapter_symbol(id) << "(void *out";
    for (size_t i = 0; i < parameter_types.size(); ++i)
      out << ", const void *a" << i;
    out << ") {\n";
    for (size_t i = 0; i < parameter_types.size(); ++i) {
      out << "  " << parameter_types[i] << " v" << i << ";\n";
      out << "  memcpy(&v" << i << ", a" << i << ", sizeof v" << i << ");\n";
    }
    out << "  ";
    if (result != "void") out << result << " result = ";
    out << (close ? "lang_c_registration_close_v1" : d.symbol) << "(";
    for (size_t i = 0; i < parameter_types.size(); ++i) {
      if (i) out << ", ";
      out << "v" << i;
    }
    out << ");\n";
    if (result != "void") {
      out << "  memcpy(out, &result, sizeof result);\n";
    } else {
      out << "  (void)out;\n";
    }
    out << "}\n";
  }
};

} // namespace

std::string adapter_symbol(uint32_t callable) {
  return "lang_c_adapter_v1_" + std::to_string(callable);
}
std::string release_symbol(uint32_t c_type) {
  return "lang_c_release_v1_" + std::to_string(c_type);
}
std::string retain_symbol(uint32_t c_type) {
  return "lang_c_retain_v1_" + std::to_string(c_type);
}
std::string callback_bridge_symbol(uint64_t function, uint32_t c_type) {
  return "lang_c_callback_bridge_v1_" + std::to_string(function) + "_" +
         std::to_string(c_type);
}
std::string callback_trampoline_symbol(uint64_t function, uint32_t c_type) {
  return "lang_c_callback_trampoline_v1_" + std::to_string(function) + "_" +
         std::to_string(c_type);
}
std::string registration_callback_trampoline_symbol(uint32_t c_type,
                                                    size_t userdata) {
  return "lang_c_registration_trampoline_v1_" + std::to_string(c_type) +
         "_" + std::to_string(userdata);
}
std::string registration_callback_dispatch_symbol(uint32_t c_type,
                                                  size_t userdata) {
  return "lang_c_registration_dispatch_v1_" + std::to_string(c_type) + "_" +
         std::to_string(userdata);
}
std::string registration_unregister_symbol(uint32_t c_type) {
  return "lang_c_registration_unregister_v1_" + std::to_string(c_type);
}

std::optional<RegistrationCallback>
registration_callback(const Project &project,
                      const modules::CCallableDecl &callable) {
  if (!callable.registers) return std::nullopt;
  const auto &registers = *callable.registers;
  if (registers.callback_parameter >= callable.parameters.size() ||
      registers.userdata_parameter >= callable.parameters.size() ||
      registers.callback_parameter == registers.userdata_parameter)
    throw std::invalid_argument("registration parameter index out of range");
  const auto &callback_ref = callable.parameters[registers.callback_parameter].type;
  if (callback_ref.kind != CTypeRef::Kind::named || !callback_ref.named ||
      callback_ref.named > project.c_types.size())
    throw std::invalid_argument("registration callback is not a named C type");
  const auto &callback = project.c_type(callback_ref.named);
  if (callback.kind != CDeclKind::callback)
    throw std::invalid_argument("registration callback is not a C callback");
  if (registers.callback_userdata_parameter >= callback.parameters.size() ||
      !unit_pointer(callback.parameters[registers.callback_userdata_parameter].type))
    throw std::invalid_argument("callback userdata is not PtrMut[Unit]");
  if (!unit_pointer(callable.parameters[registers.userdata_parameter].type))
    throw std::invalid_argument("registration userdata is not PtrMut[Unit]");
  if (callable.result.type.kind != CTypeRef::Kind::named ||
      !callable.result.type.named ||
      callable.result.type.named > project.c_types.size())
    throw std::invalid_argument("registration result is not a C resource");
  const auto &resource = project.c_type(callable.result.type.named);
  if (resource.kind != CDeclKind::resource || !resource.registration)
    throw std::invalid_argument("registration result is not a managed resource");
  if (resource.registration->callback_type != callback_ref.named)
    throw std::invalid_argument("registration callback type differs from resource");
  if (resource.registration->unregister_symbol.empty())
    throw std::invalid_argument("managed resource has no unregister symbol");
  return RegistrationCallback{resource.id, callback.id,
                              registers.callback_parameter,
                              registers.userdata_parameter,
                              registers.callback_userdata_parameter};
}

bool registration_close(const Project &project,
                        const modules::CCallableDecl &callable) {
  if (callable.registers || callable.parameters.size() != 1) return false;
  if (callable.result.type.kind != CTypeRef::Kind::scalar ||
      callable.result.type.scalar != "Unit")
    return false;
  const auto &ref = callable.parameters.front().type;
  if (ref.kind != CTypeRef::Kind::named) return false;
  const auto &resource = project.c_type(ref.named);
  return resource.kind == CDeclKind::resource && resource.registration &&
         callable.symbol == resource.registration->unregister_symbol;
}

std::string adapter_source(
    const Project &project, const std::set<uint32_t> &callables,
    const std::set<uint32_t> &resource_types,
    const std::set<std::pair<uint64_t, uint32_t>> &callbacks) {
  Generator g(project);
  for (auto id : callables) {
    const auto &d = project.c_callable(id);
    for (const auto &param : d.parameters) g.collect(param.type);
    g.collect(d.result.type);
  }
  for (auto id : resource_types) {
    if (id < 1 || id > project.c_types.size() ||
        project.c_type(id).kind != CDeclKind::resource)
      throw std::invalid_argument("unknown C resource type");
  }
  // Managed registrations derive their one generic trampoline per callback
  // type/userdata mapping from the constructor set, plus one raw unregister
  // helper per constructed resource. Nothing here is per registration value.
  std::set<std::pair<uint32_t, size_t>> registration_mappings;
  std::set<uint32_t> registration_resources;
  for (uint32_t id : callables) {
    auto mapping = registration_callback(project, project.c_callable(id));
    if (!mapping) continue;
    registration_mappings.insert(
        {mapping->callback_type, mapping->callback_userdata_parameter});
    registration_resources.insert(mapping->resource_type);
  }
  // Every scalar buffer spelling is tiny. Emitting the complete scalar set
  // keeps pointer-to-buffer spelling and record fields deterministic.
  static const char *scalars[] = {"i8", "i16", "i32", "i64", "u8", "u16",
                                  "u32", "u64", "isize", "usize", "f32", "f64",
                                  "bool"};
  g.out << "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n\n";
  for (const char *s : scalars) {
    auto type = scalar_c(s);
    g.out << "typedef struct { const " << type << " *data; uint64_t len; } "
          << "wf_buf_ro_" << s << ";\n";
    g.out << "typedef struct { " << type << " *data; uint64_t len; } "
          << "wf_buf_mut_" << s << ";\n";
  }
  for (uint32_t id : g.records)
    g.out << "typedef struct wf_rec_" << id << " wf_rec_" << id << ";\n";
  std::set<uint32_t> active;
  for (uint32_t id : g.records) g.record_definition(id, active);
  for (uint32_t id : g.callbacks) {
    const auto &d = project.c_type(id);
    auto result = g.ctype(d.result);
    g.out << "typedef " << result << " (*wf_cb_" << id << ")(";
    for (size_t i = 0; i < d.parameters.size(); ++i) {
      if (i) g.out << ", ";
      g.out << g.ctype(d.parameters[i].type);
    }
    if (d.parameters.empty()) g.out << "void";
    g.out << ");\n";
  }
  for (const auto &[function, c_type] : callbacks)
    g.emit_callback_trampoline(function, c_type);
  for (const auto &[c_type, userdata] : registration_mappings)
    g.emit_registration_trampoline(c_type, userdata);
  for (uint32_t id : resource_types) {
    const auto &d = project.c_type(id);
    if (d.registration) {
      // The Source value is the runtime context wrapper, not the raw foreign
      // token: last release closes the registration instead of calling C.
      g.out << "extern void lang_c_registration_release_v1(void *);\n";
      g.out << "void " << release_symbol(id) << "(void *p) { if (p) "
            << "lang_c_registration_release_v1(p); }\n";
      continue;
    }
    g.out << "extern void " << d.release_symbol << "(void *);\n";
    g.out << "void " << release_symbol(id) << "(void *p) { if (p) "
          << d.release_symbol << "(p); }\n";
    if (!d.retain_symbol.empty()) {
      g.out << "extern void " << d.retain_symbol << "(void *);\n";
      g.out << "void " << retain_symbol(id) << "(void *p) { if (p) "
            << d.retain_symbol << "(p); }\n";
    }
  }
  for (uint32_t id : registration_resources) {
    const auto &d = project.c_type(id);
    g.out << "extern void " << d.registration->unregister_symbol
          << "(void *);\n";
    g.out << "void " << registration_unregister_symbol(id)
          << "(void *p) { if (p) " << d.registration->unregister_symbol
          << "(p); }\n";
  }
  for (uint32_t id : callables) g.emit_callable(id);
  return g.out.str();
}
} // namespace lang::backend::native::cffi

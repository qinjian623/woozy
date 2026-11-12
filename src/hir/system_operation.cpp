#include "lang/hir/hir.hpp"
#include <charconv>
#include <functional>
#include <stdexcept>

namespace lang::hir {
namespace {
TypeId build_signature(TypeContext &types, const HirModule *module,
                       SystemOperation operation, const std::vector<TypeId> &args) {
  const auto *spec = system_operation_spec(operation);
  if (!spec || args.size() != spec->type_arity)
    throw std::invalid_argument("unknown system operation or type argument count");
  for (auto arg : args)
    if (!types.find(arg))
      throw std::invalid_argument("system operation type argument is not public");
  auto invalid = []() -> TypeId {
    throw std::invalid_argument("unknown system operation value type");
  };
  std::function<TypeId(std::string_view, size_t)> value_type;
  value_type = [&](std::string_view name, size_t depth) -> TypeId {
    if (depth > 128) return invalid();
    if (name == "A" || name == "B") {
      const size_t index = name.front() - 'A';
      if (index >= args.size()) return invalid();
      return args[index];
    }
    Type t;
    if (name == "func(A) i64" || name == "func(A) Unit") {
      t.kind = TypeKind::Function;
      t.result = value_type(name.substr(8), depth + 1);
      t.parameters.push_back({ParamMode::ordinary, value_type("A", depth + 1)});
      return types.intern(t);
    }
    const auto bracket = name.find('[');
    auto base = name.substr(0, bracket);
    std::vector<TypeId> parameters;
    if (bracket != std::string_view::npos) {
      if (name.back() != ']') return invalid();
      auto inner = name.substr(bracket + 1, name.size() - bracket - 2);
      size_t start = 0, nesting = 0;
      for (size_t p = 0; p <= inner.size(); ++p) {
        if (p == inner.size() || (inner[p] == ',' && nesting == 0)) {
          if (p == start) return invalid();
          parameters.push_back(value_type(inner.substr(start, p - start), depth + 1));
          start = p + 1;
        } else if (inner[p] == '[') ++nesting;
        else if (inner[p] == ']') {
          if (!nesting) return invalid();
          --nesting;
        }
      }
      if (nesting) return invalid();
    }
    if (base == "Vec" || base == "Slice" || base == "Weak") {
      if (parameters.size() != 1) return invalid();
      t.kind = base == "Vec" ? TypeKind::Vec
              : base == "Slice" ? TypeKind::Slice
                                : TypeKind::Weak;
      t.type_args = std::move(parameters);
      return types.intern(t);
    }
    if (base == "Task") {
      if (parameters.size() != 2) return invalid();
      t.kind = TypeKind::Task;
      t.type_args = std::move(parameters);
      return types.intern(t);
    }
    if (base == "Ptr" || base == "PtrMut") {
      if (parameters.size() != 1) return invalid();
      t.kind = TypeKind::CForeignPointer;
      t.bits = base == "PtrMut" ? 1 : 0;
      t.type_args = std::move(parameters);
      return types.intern(t);
    }
    if (base == "Buffer" || base == "BufferMut") {
      if (parameters.size() != 1) return invalid();
      const auto *element = types.find(parameters[0]);
      if (!element ||
          !(element->kind == TypeKind::SignedInt ||
            element->kind == TypeKind::UnsignedInt ||
            element->kind == TypeKind::Isize ||
            element->kind == TypeKind::Usize ||
            element->kind == TypeKind::Float ||
            element->kind == TypeKind::Bool ||
            element->kind == TypeKind::TypeParameter))
        return invalid();
      t.kind = TypeKind::CForeignBuffer;
      t.bits = base == "BufferMut" ? 1 : 0;
      t.type_args = std::move(parameters);
      return types.intern(t);
    }
    if (base == "Result" || base == "Option" || base.starts_with("std/")) {
      if (!module || !module->resolved || !module->resolved->project)
        throw std::invalid_argument("system operation requires its standard type module");
      const std::string_view package = base == "Result" ? "std/result" :
          base == "Option" ? "std/option" : base.substr(0, base.rfind('.'));
      const std::string_view spelling = base == "Result" || base == "Option" ? base :
          base.substr(base.rfind('.') + 1);
      const NominalDecl *found = nullptr;
      for (const auto &nominal : module->nominals) {
        const auto &symbol = module->resolved->symbols.at(nominal.symbol.value);
        const auto &source = module->resolved->project->module(symbol.module);
        if (source.logical_path == package && symbol.name == spelling &&
            symbol.kind == modules::SymbolKind::type && symbol.exported && !source.binding) {
          if (found) return invalid();
          found = &nominal;
        }
      }
      if (!found || found->params.size() != parameters.size())
        throw std::invalid_argument("system operation standard type is missing or has wrong arity");
      if (base == "Result" || base == "Option") {
        const bool result = base == "Result";
        if (found->kind != TypeKind::NominalEnum || found->variants.size() != 2)
          return invalid();
        for (size_t i = 0; i < 2; ++i) {
          const auto &variant = found->variants[i];
          const auto &symbol = module->resolved->symbols.at(variant.symbol.value);
          const auto expected = result ? (i == 0 ? "Ok" : "Err") : (i == 0 ? "None" : "Some");
          const size_t payload = result || i == 1 ? 1 : 0;
          if (symbol.name != expected || variant.payload.size() != payload) return invalid();
          if (payload) {
            const auto *field = types.find(variant.payload[0].type);
            const size_t parameter = result ? i : 0;
            if (!field || field->kind != TypeKind::TypeParameter ||
                field->declaration != found->params[parameter].symbol) return invalid();
          }
        }
      }
      std::vector<std::pair<std::string_view, std::string_view>> fields;
      if (base == "std/os.File") fields = {{"descriptor", "i32"}};
      else if (base == "std/os.Timespec") fields = {{"seconds", "i64"}, {"nanoseconds", "i64"}};
      else if (base == "std/os.Pipe") fields = {{"read_end", "std/os.File"}, {"write_end", "std/os.File"}};
      else if (base == "std/os.FileInfo") fields = {
          {"device", "u64"}, {"inode", "u64"}, {"links", "u64"},
          {"mode", "u32"}, {"uid", "u32"}, {"gid", "u32"}, {"rdev", "u64"},
          {"size", "i64"}, {"block_size", "i64"}, {"blocks", "i64"},
          {"accessed", "std/os.Timespec"}, {"modified", "std/os.Timespec"},
          {"changed", "std/os.Timespec"}};
      if (base == "std/net.Socket") fields = {{"descriptor", "i32"}};
      else if (base == "std/net.Address") fields = {
          {"family", "i32"}, {"port", "u16"}, {"flags", "u16"},
          {"scope", "u32"}, {"reserved", "u32"}, {"data", "Vec[u8]"}};
      else if (base == "std/net.Accepted") fields = {{"socket", "std/net.Socket"}, {"address", "std/net.Address"}};
      else if (base == "std/net.Received") fields = {{"count", "usize"}, {"address", "std/net.Address"}};
      else if (base == "std/net.Pair") fields = {{"first", "std/net.Socket"}, {"second", "std/net.Socket"}};
      else if (base == "std/net.PollFd") fields = {{"fd", "i32"}, {"events", "i16"}, {"revents", "i16"}};
      if (base == "std/math.Fraction") fields = {{"fraction", "f64"}, {"exponent", "i32"}};
      if (base == "std/process.WaitStatus") fields = {{"pid", "i32"}, {"status", "i32"}};
      else if (base == "std/signal.Info") fields = {
          {"number", "i32"}, {"code", "i32"}, {"pid", "i32"}, {"uid", "u32"},
          {"status", "i32"}, {"reserved", "u32"}, {"value", "i64"}};
      if (!fields.empty()) {
        if (found->kind != TypeKind::NominalStruct || found->fields.size() != fields.size())
          return invalid();
        for (size_t i = 0; i < fields.size(); ++i) {
          const auto &field = found->fields[i];
          const auto &symbol = module->resolved->symbols.at(field.symbol.value);
          if (symbol.name != fields[i].first ||
              field.type != value_type(fields[i].second, depth + 1)) return invalid();
        }
      }
      t.kind = found->kind;
      t.declaration = found->symbol;
      t.type_args = std::move(parameters);
      return types.intern(t);
    }
    if (!parameters.empty()) return invalid();
    if (name == "Unit") t.kind = TypeKind::Unit;
    else if (name == "bool") t.kind = TypeKind::Bool;
    else if (name == "string") t.kind = TypeKind::String;
    else if (name == "usize") t.kind = TypeKind::Usize;
    else if (name == "isize") t.kind = TypeKind::Isize;
    else if (name == "__woozy_Thread") t.kind = TypeKind::ThreadHandle;
    else if (name == "__woozy_Mutex") t.kind = TypeKind::MutexHandle;
    else if (name == "__woozy_Condvar") t.kind = TypeKind::CondvarHandle;
    else if (name == "__woozy_AtomicI64") t.kind = TypeKind::AtomicI64Handle;
    else if (name == "__woozy_IpcHandle") t.kind = TypeKind::IpcHandle;
    else if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u' || name[0] == 'f')) {
      auto result = std::from_chars(name.data() + 1, name.data() + name.size(), t.bits);
      if (result.ec != std::errc{} || result.ptr != name.data() + name.size()) return invalid();
      t.kind = name[0] == 'i' ? TypeKind::SignedInt :
          name[0] == 'u' ? TypeKind::UnsignedInt : TypeKind::Float;
    } else return invalid();
    return types.intern(t);
  };
  Type signature;
  signature.kind = TypeKind::Function;
  signature.result = value_type(spec->result, 0);
  if (spec->count > spec->parameters.size()) return invalid();
  for (size_t p = 0; p < spec->count; ++p)
    signature.parameters.push_back({ParamMode::ordinary, value_type(spec->parameters[p], 0)});
  return types.intern(signature);
}
} // namespace
TypeId system_operation_signature(TypeContext &types, SystemOperation operation,
                                 const std::vector<TypeId> &args) {
  return build_signature(types, nullptr, operation, args);
}
TypeId system_operation_signature(TypeContext &types, const HirModule &module,
                                 SystemOperation operation,
                                 const std::vector<TypeId> &args) {
  return build_signature(types, &module, operation, args);
}
} // namespace lang::hir

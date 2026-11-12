#pragma once
#include "lang/hir/hir.hpp"
#include <algorithm>
#include <stdexcept>

namespace lang::hir::detail {
// Only declaration types enter this conversion. No host module is loaded.
inline TypeId binding_type(const modules::Project &p, TypeContext &ctx,
                           const std::string &name) {
  Type t;
  for (const auto &d : p.foreign_bindings)
    if (d.identity == name && std::holds_alternative<modules::BindingType>(d.detail)) {
      t.kind = TypeKind::ForeignOpaque;
      t.binding = d.id;
      return ctx.intern(t);
    }
  if (name == "Unit") t.kind = TypeKind::Unit;
  else if (name == "bool") t.kind = TypeKind::Bool;
  else if (name == "string") t.kind = TypeKind::String;
  else if (name == "isize") t.kind = TypeKind::Isize;
  else if (name == "usize") t.kind = TypeKind::Usize;
  else if (name == "f32" || name == "f64") {
    t.kind = TypeKind::Float; t.bits = name == "f32" ? 32 : 64;
  } else if (name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
             name == "u8" || name == "u16" || name == "u32" || name == "u64") {
    t.kind = name[0] == 'i' ? TypeKind::SignedInt : TypeKind::UnsignedInt;
    t.bits = unsigned(std::stoul(name.substr(1)));
  } else throw std::invalid_argument("unknown foreign declaration type: " + name);
  return ctx.intern(t);
}
inline std::vector<ForeignBinding> binding_records(const modules::Project &p,
                                                    TypeContext &ctx) {
  std::vector<ForeignBinding> out;
  if (p.foreign_bindings.empty()) return out;
  auto error = binding_type(p, ctx, "lang.py.Error");
  for (const auto &d : p.foreign_bindings) {
    if (d.id.value != out.size())
      throw std::invalid_argument("foreign declaration index/identity mismatch");
    ForeignBinding b{d.id, TypeId{0}, {}, {}, error};
    if (std::holds_alternative<modules::BindingType>(d.detail))
      b.type = binding_type(p, ctx, d.identity);
    else if (auto v = std::get_if<modules::BindingValue>(&d.detail))
      b.type = binding_type(p, ctx, v->type);
    else if (auto prop = std::get_if<modules::BindingProperty>(&d.detail))
      b.type = binding_type(p, ctx, prop->type);
    else if (auto index = std::get_if<modules::BindingIndexer>(&d.detail))
      b.type = binding_type(p, ctx, index->result_type);
    else if (auto slice = std::get_if<modules::BindingSlice>(&d.detail))
      b.type = binding_type(p, ctx, slice->result_type);
    else {
      const auto &c = std::get<modules::BindingCallable>(d.detail);
      Type sig; sig.kind = TypeKind::Function;
      sig.result = binding_type(p, ctx, c.return_type);
      if (c.error_type) sig.throws_type = binding_type(p, ctx, *c.error_type);
      for (const auto &param : c.parameters) {
        auto mode = param.mode == "ordinary" ? ParamMode::ordinary :
                    param.mode == "move" ? ParamMode::move : ParamMode::inout;
        sig.parameters.push_back({mode, binding_type(p, ctx, param.type)});
      }
      b.signature = ctx.intern(sig);
      if (c.owner_type) b.owner = binding_type(p, ctx, *c.owner_type);
      Type target; target.kind = TypeKind::ForeignCallable; target.binding = d.id;
      b.type = ctx.intern(target);
    }
    out.push_back(b);
  }
  return out;
}
// Labels are in source order; omitted indices are in declaration order.
// A null result means this candidate cannot bind this argument list.
struct BoundArguments {
  std::vector<uint32_t> parameters, omitted;
};
inline std::optional<BoundArguments>
bind_arguments(const modules::BindingCallable &c,
               const std::vector<std::optional<std::string>> &labels) {
  BoundArguments out;
  std::vector<bool> seen(c.parameters.size());
  bool named = false;
  size_t positional = 0;
  for (const auto &label : labels) {
    size_t pi = positional;
    if (label) {
      named = true;
      pi = 0;
      while (pi < c.parameters.size() && c.parameters[pi].name != *label) ++pi;
      if (pi == c.parameters.size() || c.parameters[pi].kind == "positional_only") return {};
    } else {
      if (named || pi >= c.parameters.size() || c.parameters[pi].kind == "keyword_only") return {};
      ++positional;
    }
    if (seen[pi]) return {};
    seen[pi] = true;
    out.parameters.push_back(uint32_t(pi));
  }
  for (size_t i = 0; i < seen.size(); ++i)
    if (!seen[i]) {
      if (!c.parameters[i].optional) return {};
      out.omitted.push_back(uint32_t(i));
    }
  return out;
}

// Canonical TypeId for one frozen C binding type expression. The named
// declaration index is 1-based in Project::c_types. Pointer/buffer write
// permission is encoded in Type::bits; the pointee/element is type_args[0].
inline TypeId c_type_id(const modules::Project &p, TypeContext &ctx,
                        const modules::CTypeRef &ref, size_t depth = 0) {
  if (depth > 32) throw std::invalid_argument("C type nesting exceeds the limit");
  Type t;
  auto scalar = [&](std::string_view name) {
    if (name == "Unit") t.kind = TypeKind::Unit;
    else if (name == "bool") t.kind = TypeKind::Bool;
    else if (name == "isize") t.kind = TypeKind::Isize;
    else if (name == "usize") t.kind = TypeKind::Usize;
    else if (name == "f32" || name == "f64") {
      t.kind = TypeKind::Float; t.bits = name == "f32" ? 32 : 64;
    } else if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u')) {
      t.kind = name[0] == 'i' ? TypeKind::SignedInt : TypeKind::UnsignedInt;
      t.bits = unsigned(std::stoul(std::string(name.substr(1))));
    } else throw std::invalid_argument("unknown C scalar type");
  };
  switch (ref.kind) {
  case modules::CTypeRef::Kind::scalar:
    scalar(ref.scalar);
    return ctx.intern(std::move(t));
  case modules::CTypeRef::Kind::pointer:
  case modules::CTypeRef::Kind::buffer: {
    if (ref.args.size() != 1)
      throw std::invalid_argument("C pointer/buffer arity");
    t.kind = ref.kind == modules::CTypeRef::Kind::pointer
                 ? TypeKind::CForeignPointer
                 : TypeKind::CForeignBuffer;
    t.bits = ref.writable ? 1 : 0;
    t.type_args.push_back(c_type_id(p, ctx, ref.args[0], depth + 1));
    return ctx.intern(std::move(t));
  }
  case modules::CTypeRef::Kind::named: {
    if (!ref.named || ref.named > p.c_types.size())
      throw std::invalid_argument("C type index");
    const auto &d = p.c_type(ref.named);
    t.c_type = ref.named;
    switch (d.kind) {
    case modules::CDeclKind::record: t.kind = TypeKind::CForeignRecord; break;
    case modules::CDeclKind::resource: t.kind = TypeKind::CForeignResource; break;
    case modules::CDeclKind::callback: t.kind = TypeKind::CForeignCallback; break;
    case modules::CDeclKind::opaque: t.kind = TypeKind::CForeignOpaque; break;
    }
    return ctx.intern(std::move(t));
  }
  }
  throw std::invalid_argument("unknown C type expression");
}
inline TypeId c_callable_signature(const modules::Project &p, TypeContext &ctx,
                                   const modules::CCallableDecl &callable) {
  Type sig;
  sig.kind = TypeKind::Function;
  for (const auto &param : callable.parameters)
    sig.parameters.push_back({ParamMode::ordinary,
                              c_type_id(p, ctx, param.type)});
  sig.result = c_type_id(p, ctx, callable.result.type);
  return ctx.intern(std::move(sig));
}
} // namespace lang::hir::detail

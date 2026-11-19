#pragma once
#include "lang/ir/native.hpp"
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace lang::backend::native {
// Source declarations use the emitter's actual LLVM storage. Lowering-only
// temporaries have no user variable and remain absent from debugger locals.
class NativeDebugInfo {
  const hir::HirModule &hir;
  llvm::Module &module;
  ir::detail::AggregateTypes &aggregates;
  std::function<llvm::Type *(TypeId)> lower_type;
  llvm::StructType *vec_layout, *handle_layout;
  llvm::DIBuilder di;
  llvm::LLVMContext &context;
  std::map<SourceSnapshotId, const SourceSnapshot *> sources;
  std::map<SourceSnapshotId, llvm::DIFile *> files;
  std::map<TypeId, llvm::DIType *> types;
  std::map<ScopeId, llvm::DILocalScope *> scopes;
  std::map<SymbolId, std::vector<ScopeId>> function_scopes;
  std::map<ScopeId, std::vector<const hir::HirNode *>> declarations;
  std::map<SymbolId, std::map<SymbolId, const hir::HirNode *>> function_bindings;
  std::map<const hir::HirNode *, llvm::DILocalScope *> declaration_scopes;
  std::map<size_t, llvm::DILocalVariable *> locals;
  std::set<size_t> bounded_locations;
  const hir::HirFunction *function = nullptr;
  llvm::DISubprogram *subprogram = nullptr;
  bool optimized;

  struct PositionInfo {
    llvm::DIFile *file;
    unsigned line, column;
  };
  std::optional<PositionInfo> position(SourceSpan span) const {
    auto it = sources.find(span.snapshot);
    if (it == sources.end() || span.begin_byte >= span.end_byte ||
        span.end_byte > it->second->original.size()) return {};
    const auto p = it->second->position(span.begin_byte);
    if (p.line >= std::numeric_limits<unsigned>::max() ||
        p.character >= std::numeric_limits<unsigned>::max()) return {};
    return PositionInfo{files.at(span.snapshot), unsigned(p.line + 1),
                        unsigned(p.character + 1)};
  }
  llvm::DILocalScope *scope(ScopeId id) {
    if (auto it = scopes.find(id); it != scopes.end()) return it->second;
    const auto &s = hir.scopes.at(id.value);
    if (s.function != function->symbol) return nullptr;
    auto *parent = s.parent ? scope_at(*s.parent, s.source.span) : subprogram;
    auto p = position(s.source.span);
    if (!parent || !p) return nullptr;
    auto *result = di.createLexicalBlock(parent, p->file, p->line, p->column);
    scopes.emplace(id, result);
    return result;
  }
  llvm::DILocalScope *scope_at(ScopeId id, SourceSpan use) {
    auto *result = scope(id);
    if (!result) return nullptr;
    // Each declaration starts a nested range for the remainder of its actual
    // lexical scope. This keeps later locals out of earlier initializers and
    // makes shadowing begin only after the new binding has been initialized.
    for (const auto *n : declarations[id]) {
      if (n->source.span.snapshot != use.snapshot ||
          n->source.span.end_byte > use.begin_byte) continue;
      auto it = declaration_scopes.find(n);
      if (it == declaration_scopes.end()) {
        auto p = position(n->source.span);
        if (!p) continue;
        result = di.createLexicalBlock(result, p->file, p->line, p->column);
        declaration_scopes.emplace(n, result);
      } else result = it->second;
    }
    return result;
  }
#include "debug_types.inc"
  llvm::DIType *signature_type(TypeId id) {
    if (aggregates.types.find(id)->kind == hir::TypeKind::Unit) return nullptr;
    if (auto *value = type(id)) return value;
    // nullptr is DWARF void. Unknown aggregate/layout descriptions must not
    // claim that a real value returns void or has a scalar representation.
    return di.createUnspecifiedType("<Woozy type: layout unavailable>");
  }
  const modules::Symbol *symbol(SymbolId id) const {
    if (id.value >= hir.resolved->symbols.size()) return nullptr;
    const auto &s = hir.resolved->symbols[id.value];
    return s.definition && s.declaration && !s.name.empty() && s.name != "_"
        ? &s : nullptr;
  }
#include "debug_bindings.inc"
public:
  NativeDebugInfo(llvm::Module &m, const hir::HirModule &h, bool opt,
      ir::detail::AggregateTypes &a, std::function<llvm::Type *(TypeId)> lower,
      llvm::StructType *vec, llvm::StructType *handle)
      : hir(h), module(m), aggregates(a), lower_type(std::move(lower)),
        vec_layout(vec), handle_layout(handle), di(m), context(m.getContext()), optimized(opt) {
    const auto &project = *hir.resolved->project;
    for (const auto &m : project.modules)
      for (const auto &f : m.files) {
        const auto &s = *f->source;
        sources.emplace(s.id, &s);
        const auto path = std::filesystem::absolute(s.path).lexically_normal();
        auto relative = path.lexically_relative(project.root);
        if (relative.empty() || *relative.begin() == "..") {
          if (m.logical_path == "std" || m.logical_path.starts_with("std/"))
            relative = std::filesystem::path("stdlib") / m.logical_path.substr(m.logical_path == "std" ? 3 : 4)
                       / path.filename();
          else
            relative = std::filesystem::path("dependencies") / std::to_string(m.id.value)
                       / m.logical_path / path.filename();
        }
        files.emplace(s.id, di.createFile(relative.generic_string(), ""));
      }
    for (size_t i = 0; i < hir.scopes.size(); ++i)
      function_scopes[hir.scopes[i].function].push_back(ScopeId{i});
    index_bindings();
    module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                         llvm::DEBUG_METADATA_VERSION);
    module.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    // C describes the supported scalar expressions to existing debuggers;
    // Woozy has no assigned DW_LANG code. No C source is generated.
    if (!files.empty())
      di.createCompileUnit(llvm::dwarf::DW_LANG_C, files.begin()->second,
                           "Woozy C++ seed", optimized, "", 0, {},
                           llvm::DICompileUnit::FullDebug, 0, true, false,
                           llvm::DICompileUnit::DebugNameTableKind::None);
  }
  void begin(const ir::Function &f, const hir::HirFunction &hf,
             llvm::Function &fn) {
    function = &hf; subprogram = nullptr;
    scopes.clear(); declaration_scopes.clear(); locals.clear(); bounded_locations.clear();
    std::map<SymbolId, size_t> first_binding_slot;
    for (size_t i = 0; i < f.slots.size(); ++i) {
      if (f.async) bounded_locations.insert(i);
      if (const auto binding = f.slots[i].binding) {
        const auto [first, inserted] = first_binding_slot.emplace(*binding, i);
        // Each expanded defer has its own storage for the same source name.
        if (!inserted) {
          bounded_locations.insert(first->second);
          bounded_locations.insert(i);
        }
      }
    }
    for (const auto &block : f.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.op == ir::Op::move || instruction.op == ir::Op::take ||
            instruction.op == ir::Op::destroy)
          bounded_locations.insert(instruction.slot.value);
    auto p = position(f.source);
    if (!p) return;
    const auto &s = hir.resolved->symbols.at(f.symbol.value);
    if (!s.definition || !s.declaration) return;
    const auto &sig = *hir.types.find(f.signature);
    std::vector<llvm::Metadata *> signature{signature_type(sig.result)};
    for (auto param : sig.parameters) signature.push_back(signature_type(param.type));
    auto flags = llvm::DISubprogram::SPFlagDefinition;
    if (optimized) flags |= llvm::DISubprogram::SPFlagOptimized;
    std::string name = s.name.empty() ? "<lambda>" : s.name;
    SymbolId origin = f.symbol;
    const hir::FunctionInstance *instance = nullptr;
    for (const auto &i : hir.instances)
      if (i.symbol == f.symbol) { instance = &i; origin = i.origin; break; }
    for (const auto &impl : hir.impls)
      if (std::find(impl.methods.begin(), impl.methods.end(), origin) != impl.methods.end()) {
        hir::Substitutions substitutions;
        if (instance)
          for (const auto &t : hir.templates)
            if (t.function.symbol == origin) {
              for (size_t a = 0; a < t.params.size(); ++a)
                substitutions.emplace_back(t.params[a].symbol, instance->type_args.at(a));
              break;
            }
        const auto receiver = hir::substitute(impl.self_type, substitutions, aggregates.types);
        name = type_name(receiver) + "." + name;
        break;
      }
    if (instance && !instance->type_args.empty()) {
      name += "[";
      for (size_t a = 0; a < instance->type_args.size(); ++a) {
        if (a) name += ",";
        name += type_name(instance->type_args[a]);
      }
      name += "]";
    }
    subprogram = di.createFunction(p->file, name, "", p->file,
        p->line, di.createSubroutineType(di.getOrCreateTypeArray(signature)),
        p->line, llvm::DINode::FlagPrototyped, flags);
    fn.setSubprogram(subprogram);
    scopes.emplace(hf.scope, subprogram);
    const auto &bindings = function_bindings[f.symbol];
    // LLVM CoroSplit migrates these real alloca declarations into its frame.
    // Never assume a frame field index or advertise an uninitialized slot.
    for (size_t i = 0; i < f.slots.size(); ++i) {
      const auto &slot = f.slots[i];
      if (!slot.binding) continue;
      auto *t = type(slot.type);
      const auto *capture = capture_symbol(*slot.binding);
      const auto *sym = capture ? capture : symbol(*slot.binding);
      if (!t || !sym) continue;
      auto decl = position(*sym->declaration);
      if (!decl) continue;
      llvm::DILocalVariable *var = nullptr;
      if (slot.parameter) {
        var = di.createParameterVariable(subprogram, sym->name,
            *slot.parameter + 1, decl->file, decl->line, t, true);
      } else if (auto it = bindings.find(*slot.binding); it != bindings.end()) {
        auto after = it->second->source.span;
        after.begin_byte = after.end_byte;
        if (auto *local_scope = scope_at(it->second->scope, after))
          var = di.createAutoVariable(local_scope, sym->name, decl->file,
                                       decl->line, t, true);
      } else if (capture) {
        var = di.createAutoVariable(subprogram, sym->name, decl->file, decl->line, t, true);
      } else if (auto it = extra_binding_scopes.find(*slot.binding);
                 it != extra_binding_scopes.end()) {
        if (auto *local_scope = scope(it->second))
          var = di.createAutoVariable(local_scope, sym->name, decl->file, decl->line, t, true);
      }
      if (var) locals.emplace(i, var);
    }
    // Function verification precedes the compile-unit finalize. Complete all
    // recursive type cycles now that their temporary forward nodes are gone.
    for (const auto &[id, t] : types)
      if (!t->isResolved()) t->resolveCycles();
  }
  llvm::DebugLoc location(SourceSpan span) {
    if (!subprogram) return {};
    auto p = position(span);
    if (!p) return {};
    llvm::DILocalScope *local = subprogram;
    uint64_t width = std::numeric_limits<uint64_t>::max();
    std::optional<ScopeId> selected;
    for (auto id : function_scopes[function->symbol]) {
      const auto &s = hir.scopes[id.value];
      const auto range = s.source.span;
      if (s.function == function->symbol && range.snapshot == span.snapshot &&
          range.begin_byte <= span.begin_byte && range.end_byte >= span.end_byte &&
          range.end_byte - range.begin_byte < width) {
        selected = id; width = range.end_byte - range.begin_byte;
      }
    }
    if (selected)
      if (auto *candidate = scope_at(*selected, span)) local = candidate;
    // Instructions must retain the real source file even if generated code
    // uses a node from another file (e.g. an expanded default argument).
    if (local->getFile() != p->file)
      local = di.createLexicalBlockFile(local, p->file, 0);
    return llvm::DILocation::get(context, p->line, p->column, local);
  }
  void declare(size_t slot, llvm::Value *storage, llvm::BasicBlock *block,
               SourceSpan source = {SourceSnapshotId{0}, 0, 0}) {
    auto it = locals.find(slot);
    if (it == locals.end()) return;
    auto *var = it->second;
    auto p = position(source);
    auto *loc = llvm::DILocation::get(context, p ? p->line : var->getLine(),
                                     p ? p->column : 0, var->getScope());
    // An address-valued dbg.value has a bounded location range and respects
    // later kills. dbg.declare is hoisted by CoroSplit and can accidentally
    // advertise an uninitialized frame field for the whole resume function.
    if (bounded_locations.contains(slot))
      di.insertDbgValueIntrinsic(storage, var,
          di.createExpression({llvm::dwarf::DW_OP_deref}), loc, block);
    else
      // For stable synchronous storage LLVM preserves a declare more reliably
      // at O0, including short scalar lifetimes with no machine instructions
      // between the store and its first source use. Its lexical range starts
      // after initialization and this slot has no move/destroy in the CFG.
      di.insertDeclare(storage, var, di.createExpression(), loc, block);
  }
  void read(size_t slot, llvm::Value *storage, llvm::BasicBlock *block,
            SourceSpan source) {
    if (!bounded_locations.contains(slot)) return;
    auto it = locals.find(slot);
    if (it == locals.end()) return;
    auto loc = location(source);
    if (!loc) return;
    // A verified source read proves this binding is initialized here. Reissue
    // its address after a suspend instead of extending a stale pre-suspend
    // location. Cleanup/initializer reads outside its lexical scope do not
    // make the name visible.
    for (llvm::DIScope *s = loc->getScope(); s; s = s->getScope())
      if (s == it->second->getScope()) {
        di.insertDbgValueIntrinsic(storage, it->second,
            di.createExpression({llvm::dwarf::DW_OP_deref}), loc.get(), block);
        break;
      }
  }
  void unavailable(size_t slot, llvm::BasicBlock *block, SourceSpan source) {
    auto it = locals.find(slot);
    if (it == locals.end()) return;
    auto loc = location(source);
    if (!loc) loc = llvm::DILocation::get(context, 0, 0, subprogram);
    // Moved or destroyed owners can leave valid-looking bytes in storage.
    // End their address location range as soon as that source value dies.
    di.insertDbgValueIntrinsic(llvm::PoisonValue::get(llvm::Type::getInt1Ty(context)),
        it->second, di.createExpression(), loc.get(), block);
  }
  void end() {
    if (subprogram) di.finalizeSubprogram(subprogram);
    subprogram = nullptr;
  }
  void finalize() {
    // Source functions can be inlined into the native entry or a callback
    // thunk. Give generated functions artificial, line-zero subprograms so
    // LLVM preserves the inlined source locations without assigning a fake
    // .wz line to wrapper/runtime instructions.
    if (!files.empty()) {
      // Otherwise GDB can resolve a generated frame to the preceding .wz line.
      llvm::DIBuilder generated(module);
      auto *file = generated.createFile("<woozy-generated>", "");
      generated.createCompileUnit(llvm::dwarf::DW_LANG_C, file,
                                  "Woozy C++ seed", optimized, "", 0, {},
                                  llvm::DICompileUnit::FullDebug, 0, true, false,
                                  llvm::DICompileUnit::DebugNameTableKind::None);
      auto *unknown = generated.createSubroutineType(generated.getOrCreateTypeArray({}));
      for (auto &fn : module) {
        if (fn.isDeclaration()) continue;
        if (!fn.getSubprogram()) {
          auto flags = llvm::DISubprogram::SPFlagDefinition;
          if (optimized) flags |= llvm::DISubprogram::SPFlagOptimized;
          auto name = fn.getName() == "main" ? llvm::StringRef("__woozy_native_entry") : fn.getName();
          auto *sp = generated.createFunction(file, name, "", file, 0,
              unknown, 0, llvm::DINode::FlagArtificial, flags);
          fn.setSubprogram(sp);
          generated.finalizeSubprogram(sp);
        }
        auto *unknown_location = llvm::DILocation::get(context, 0, 0, fn.getSubprogram());
        for (auto &block : fn)
          for (auto &instruction : block)
            if (!instruction.getDebugLoc()) instruction.setDebugLoc(unknown_location);
      }
      generated.finalize();
    }
    di.finalize();
  }
};
} // namespace lang::backend::native

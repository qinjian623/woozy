#include "lang/sema/check.hpp"
#include "../hir/ownership.hpp"
#include "../hir/pattern_analysis.hpp"
#include "../hir/foreign.hpp"
#include "../hir/literal_value.hpp"
#include <algorithm>
#include <functional>
#include <gmpxx.h>
#include <map>
#include <set>
#include <tuple>

namespace lang::sema {
namespace {
using namespace hir;
using Ref = modules::NodeRef;
using Key = std::tuple<uint64_t, size_t, uint64_t>;
Key key(Ref r) { return {r.module.value, r.file, r.node.value}; }
struct Rejected {
  Diagnostic diagnostic;
  FailureKind kind;
};
struct Value {
  TypeId type{0};
  std::optional<mpz_class> integer;
  std::optional<bool> boolean;
  // Already-rounded source IEEE value: low 32 bits for f32, all 64 for f64.
  // It carries NaN/infinity/signed zero/subnormals without a host double.
  std::optional<uint64_t> floating;
  // Complete literal byte sequence, including embedded NULs. This is only set
  // for string literals and const values derived from them; runtime strings
  // stay ordinary Read nodes and are never treated as constants.
  std::optional<std::string> string;
};
struct Expr {
  HirNodeId id;
  Value value;
};
struct Local {
  TypeId type;
  bool writable;
  bool ordinary_parameter = false;
};
struct Constant {
  Value value;
  std::optional<SymbolId> function;
  std::vector<TypeId> type_args;
};
class Checker {
  HirModule m;
  std::stop_token stop;
  std::map<Key, SymbolId> occurrences, definitions;
  std::map<uint64_t, TypeId> signatures;
  // Trait prototypes are registered before their nominal field bodies are
  // populated. Recheck their complete inout graphs after fields exist.
  std::map<uint64_t, std::pair<SymbolId, bool>> trait_signature_context;
  std::map<uint64_t, std::vector<Parameter>> parameters;
  std::map<uint64_t, Local> locals;
  std::map<uint64_t, Constant> constants;
  std::set<uint64_t> evaluating;
  SymbolId function{0};
  ScopeId scope{0};
  size_t cleanup_depth = 0;
  TypeId unit{0}, boolean{0}, i64{0};
  std::vector<HirNodeId> loops, break_targets;
  std::shared_ptr<modules::ResolvedProject> owned;
  std::map<uint64_t, size_t> nominal_ids;
  std::map<uint64_t, TypeId> self_types;
  std::map<uint64_t, std::vector<GenericParam>> generic_params;
  std::map<uint64_t, std::optional<TraitRef>> bounds;
  // Traits are registered before other declarations so struct/impl bounds can
  // validate their ordered application arity. These are the real declared
  // parameters, never a copy of a consumer's expectation.
  std::map<uint64_t, std::vector<GenericParam>> trait_parameters;
  std::map<uint64_t, Ref> bodies;
  std::map<uint64_t, size_t> template_ids;
  std::map<std::pair<uint64_t, std::vector<uint64_t>>, SymbolId> instance_cache;
  size_t instantiation_depth = 0;
  size_t constraint_depth = 0;
  using TraitKey = std::tuple<uint64_t, uint64_t, std::vector<uint64_t>>;
  std::map<TraitKey, bool> trait_results;
  std::set<TraitKey> active_constraints;
  // Trait applications carry one declared bound per trait parameter. These
  // are checked after every trait/impl declaration is complete, and again for
  // concrete uses, so a partially registered declaration cannot cache a false
  // negative.
  std::set<TraitKey> active_trait_bounds;
  size_t instantiated_nodes = 0;
  std::vector<GenericParam> active_params;
  std::set<uint64_t> signature_parameters;

  const AstFile &file(Ref r) const {
    return *m.resolved->project->module(r.module).files.at(r.file);
  }
  const AstNode &ast(Ref r) const { return file(r).node(r.node); }
  Ref child(Ref r, size_t i) const {
    r.node = ast(r).children.at(i);
    return r;
  }
  Ref at(Ref r, AstNodeId id) const {
    r.node = id;
    return r;
  }
  std::string_view token(Ref r, size_t n = 0) const {
    return file(r).tokens.at(ast(r).token_begin + n).spelling;
  }
  std::string_view first(Ref r) const { return token(r, token(r) == "pub"); }
  Source source(Ref r) const { return {r, ast(r).span}; }
  [[noreturn]] void reject(Ref r, std::string message,
                           std::string code = "E_TYPE_MISMATCH") const {
    Diagnostic d;
    d.code = std::move(code);
    d.message = std::move(message);
    d.phase = "type";
    d.severity = "error";
    d.primary = ast(r).span;
    const auto kind = d.code == "E_CAPABILITY_UNAVAILABLE"
                          ? FailureKind::unsupported
                          : FailureKind::source;
    throw Rejected{std::move(d), kind};
  }
  [[noreturn]] void unavailable(Ref r, std::string feature) const {
    reject(r, std::move(feature), "E_CAPABILITY_UNAVAILABLE");
  }
  SymbolId definition(Ref r) const { return definitions.at(key(r)); }
  std::optional<SymbolId> occurrence(Ref r) const {
    if (ast(r).kind == AstKind::Parenthesized)
      return occurrence(child(r, 0));
    auto it = occurrences.find(key(r));
    return it == occurrences.end() ? std::nullopt : std::optional(it->second);
  }
  const modules::Symbol &symbol(SymbolId s) const {
    return m.resolved->symbols.at(s.value);
  }
  const Type &type(TypeId t) const { return *m.types.find(t); }
  TraitKey trait_key(TypeId actual, const TraitRef &trait) const {
    std::vector<uint64_t> args;
    for (auto a : trait.args)
      args.push_back(a.value);
    return {actual.value, trait.declaration.value, std::move(args)};
  }
  const TraitDecl *trait_decl(SymbolId s) const {
    for (const auto &t : m.traits)
      if (t.symbol == s)
        return &t;
    return nullptr;
  }
  TraitRef check_trait_application(Ref r, SymbolId declaration,
                                   std::vector<TypeId> args) const {
    auto found = trait_parameters.find(declaration.value);
    if (found == trait_parameters.end())
      reject(r, "trait application has no real declaration", "E_NAME_KIND");
    if (args.size() != found->second.size())
      reject(r, "wrong number of trait type arguments", "E_TYPE_MISMATCH");
    return TraitRef{declaration, std::move(args)};
  }
  // Check the complete application [trait.params -> args, trait.Self -> self].
  // Each declared parameter bound is substituted and applied to its actual
  // argument. Nested applications are checked before asking whether the
  // argument has exactly one implementation. This is deliberately separate
  // from the parse-time arity check: declarations are only complete after all
  // traits and impls have been registered.
  void check_trait_parameter_bounds(Ref r, const TraitRef &trait,
                                    TypeId application, size_t depth = 0) {
    auto found = trait_parameters.find(trait.declaration.value);
    const auto *tr = trait_decl(trait.declaration);
    if (found == trait_parameters.end() || !tr)
      reject(r, "trait application has no real declaration", "E_NAME_KIND");
    if (trait.args.size() != found->second.size())
      reject(r, "wrong number of trait type arguments", "E_TYPE_MISMATCH");
    if (depth >= 48)
      reject(r, "trait constraint graph exceeds checking budget",
             "E_TYPE_MISMATCH");
    auto key = trait_key(application, trait);
    if (!active_trait_bounds.insert(key).second)
      return;
    for (auto arg : trait.args)
      check_inout_graph(r, arg, true);
    Substitutions sub{{tr->self, application}};
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, trait.args[i]});
    for (size_t i = 0; i < found->second.size(); ++i) {
      const auto &p = found->second[i];
      if (!p.trait)
        continue;
      TraitRef bound = *p.trait;
      for (auto &arg : bound.args)
        arg = substitute(arg, sub, m.types);
      check_trait_parameter_bounds(r, bound, trait.args[i], depth + 1);
      if (!satisfies(r, trait.args[i], bound))
        reject(r,
               "trait argument does not satisfy the trait declaration's "
               "parameter constraint");
    }
    // Every complete application must recheck the method graphs after real
    // Self/trait-argument substitution. Method-own parameters may still be
    // templates here; their concrete instances and calls are checked again.
    check_trait_application_data(r, trait, application);
    active_trait_bounds.erase(key);
  }
  bool integer(TypeId t) const {
    auto k = type(t).kind;
    return k == TypeKind::SignedInt || k == TypeKind::UnsignedInt ||
           k == TypeKind::Isize || k == TypeKind::Usize;
  }
  bool signed_(TypeId t) const {
    return type(t).kind == TypeKind::SignedInt ||
           type(t).kind == TypeKind::Isize;
  }
  unsigned width(TypeId t) const { return bit_width(type(t), m.config); }
  mpz_class modulus(TypeId t) const { return mpz_class(1) << width(t); }
  mpz_class wrap(mpz_class v, TypeId t) const {
    mpz_class mod = modulus(t);
    mpz_mod(v.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    if (signed_(t) && v >= (mod >> 1))
      v -= mod;
    return v;
  }
  void range(Ref r, const mpz_class &v, TypeId t) const {
    mpz_class hi = signed_(t) ? mpz_class(modulus(t) >> 1) : modulus(t);
    mpz_class lo = signed_(t) ? mpz_class(-hi) : mpz_class(0);
    if (v < lo || v >= hi)
      reject(r, "integer constant is outside the target type range");
  }
  uint64_t bits(const mpz_class &v, TypeId t) const {
    mpz_class n;
    const auto mod = modulus(t);
    mpz_mod(n.get_mpz_t(), v.get_mpz_t(), mod.get_mpz_t());
    uint64_t result = 0;
    size_t count = 0;
    mpz_export(&result, &count, -1, sizeof(result), 0, 0, n.get_mpz_t());
    return result;
  }
  HirNodeId node(Ref r, TypeId t, NodeData data,
                 std::vector<HirNodeId> children = {}) {
    check_cancelled(stop);
    HirNodeId id{m.nodes.size()};
    m.nodes.push_back(
        {t, source(r), scope, std::move(children), std::move(data)});
    return id;
  }
  Expr int_literal(Ref r, mpz_class v, TypeId t) {
    if (!integer(t))
      reject(r, "integer literal requires an integer context");
    range(r, v, t);
    auto id = node(r, t, IntegerLiteral{v.get_str(), bits(v, t)});
    return {id, {t, std::move(v), {}}};
  }
  TypeId scalar(TypeKind k, unsigned bits = 0) {
    Type t;
    t.kind = k;
    t.bits = bits;
    return m.types.intern(t);
  }
  ParamMode mode(Ref r) const {
    const auto &n = ast(r);
    // Parameters in FunctionType have no name. Declaration parameters do.
    for (size_t i = n.token_begin; i < file(r).node(n.children[0]).token_begin;
         ++i) {
      auto s = file(r).tokens[i].spelling;
      if (s == "inout")
        return ParamMode::inout;
      if (s == "move")
        return ParamMode::move;
    }
    return ParamMode::ordinary;
  }
  struct ArgumentMode {
    Ref value;
    Ref marked;
    ParamMode requested;
    bool has_marker;
  };
  ArgumentMode argument_mode(Ref r) const {
    auto value = child(r, 0);
    while (ast(value).kind == AstKind::Parenthesized)
      value = child(value, 0);
    auto marked = value;
    auto requested = ParamMode::ordinary;
    if (ast(value).kind == AstKind::Unary &&
        (token(value) == "inout" || token(value) == "move")) {
      requested = token(value) == "inout" ? ParamMode::inout : ParamMode::move;
      value = child(value, 0);
    }
    return {value, marked, requested, requested != ParamMode::ordinary};
  }
  TypeId parameter_type(SymbolId s) {
    Type t;
    t.kind = TypeKind::TypeParameter;
    t.declaration = s;
    return m.types.intern(t);
  }
  bool template_inout_type(TypeId t) const {
    if (type(t).kind != TypeKind::TypeParameter)
      return false;
    auto declaration = type(t).declaration;
    if (symbol(declaration).kind != modules::SymbolKind::type_parameter)
      return false;
    if (signature_parameters.count(declaration.value))
      return true;
    return std::any_of(active_params.begin(), active_params.end(),
                       [&](const GenericParam &p) {
                         return p.symbol == declaration;
                       });
  }
  bool scalar_inout_type(TypeId t) const {
    return integer(t) || type(t).kind == TypeKind::Bool ||
           type(t).kind == TypeKind::Float;
  }
  // Numeric/bool plus finite nominal aggregates whose fields are trivial copy
  // data. Top-level Unit/Function stay closed; they are permitted only inside
  // an aggregate by hir::inout_data_type. The helper re-reads the real
  // declaration and substituted field types at every check.
  bool aggregate_inout_type(TypeId t,
                            std::optional<SymbolId> trait_self = {}) const {
    if (scalar_inout_type(t))
      return true;
    std::set<uint64_t> parameters = signature_parameters;
    for (const auto &p : active_params)
      parameters.insert(p.symbol.value);
    if (trait_self)
      parameters.insert(trait_self->value);
    return inout_data_type(m, t, parameters);
  }
  // The trait prototype registers its real Self SymbolId only while checking
  // that declaration. This is not a method generic parameter: it never enters
  // generic_params, the instance key, or active_params.
  bool trait_self_type(TypeId t, SymbolId self) const {
    return type(t).kind == TypeKind::TypeParameter &&
           type(t).declaration == self &&
           symbol(self).kind == modules::SymbolKind::type_parameter;
  }
  const ImplDecl *impl_of(SymbolId target) const {
    for (const auto &im : m.impls)
      if (std::find(im.methods.begin(), im.methods.end(), target) !=
          im.methods.end())
        return &im;
    return nullptr;
  }
  bool impl_method(SymbolId target) const { return impl_of(target) != nullptr; }
  bool declared_self(SymbolId target) const {
    const auto *im = impl_of(target);
    if (!im)
      return false;
    const auto found = parameters.find(target.value);
    if (found == parameters.end() || found->second.empty() ||
        found->second.front().label != "self")
      return false;
    const auto sig = signatures.at(target.value);
    const auto &params = type(sig).parameters;
    return !params.empty() && params.front().type == im->self_type;
  }
  // This source route covers an inherent method in a non-generic impl, plus a
  // method in a generic impl whether or not its declaration adds generic
  // parameters of its own. register_function records the merged list in
  // declaration order: the impl parameters first, then the method's own
  // parameters. The impl prefix must use the real ordered SymbolIds; the own
  // suffix comes from that registration and its source declaration. A type
  // parameter on either layer may carry an existing single-trait constraint;
  // select_method, finish_arguments, and instantiate check the actual concrete
  // arguments, so a declaration with a constraint is not treated as satisfied.
  // Concrete move and non-self scalar inout share this source eligibility;
  // neither adds a public representation.
  bool inherent_method_source(SymbolId target) const {
    const auto *im = impl_of(target);
    if (!im || im->trait)
      return false;
    auto generics = generic_params.find(target.value);
    if (generics == generic_params.end() ||
        generics->second.size() < im->params.size())
      return false;
    for (size_t i = 0; i < im->params.size(); ++i)
      if (generics->second[i].symbol != im->params[i].symbol)
        return false;
    return true;
  }
  // An instance call needs a declared ordinary self; a type-qualified call
  // needs a method whose declaration has no self at all. Receiver presence
  // alone is not a declaration and cannot open this route through a static
  // call. The call form is checked separately from inherent_method_source so
  // instantiation can validate the declaration prefix/suffix without a use.
  bool inherent_method_route(SymbolId target, bool receiver,
                             TypeId signature) const {
    if (!inherent_method_source(target))
      return false;
    const auto &params = type(signature).parameters;
    const bool self = declared_self(target);
    if (receiver)
      return self && !params.empty() &&
             params.front().mode == ParamMode::ordinary;
    return !self;
  }
  struct TraitMethodSource {
    const ImplDecl *impl;
    const TraitDecl *trait;
    const MethodPrototype *prototype;
  };
  // The merged generic list of an impl method is the impl prefix followed by
  // the method's own parameters. A trait prototype has only its own list.
  std::vector<GenericParam> method_own_parameters(SymbolId method) const {
    auto generics = generic_params.find(method.value);
    if (generics == generic_params.end())
      return {};
    size_t prefix = 0;
    if (const auto *im = impl_of(method))
      prefix = im->params.size();
    if (generics->second.size() < prefix)
      return {};
    return {generics->second.begin() + prefix, generics->second.end()};
  }
  // Compare a trait prototype with its implementation by position, alpha
  // renaming only the method's own suffix. The impl prefix is excluded from
  // that renaming. Names may differ; every real SymbolId and bound must match.
  bool trait_impl_signature_matches(const TraitDecl &tr, const ImplDecl &im,
                                    const MethodPrototype &prototype,
                                    SymbolId method) {
    auto generics = generic_params.find(method.value);
    if (generics == generic_params.end() ||
        generics->second.size() < im.params.size())
      return false;
    for (size_t i = 0; i < im.params.size(); ++i)
      if (generics->second[i].symbol != im.params[i].symbol)
        return false;
    auto own = method_own_parameters(method);
    auto prototype_own = method_own_parameters(prototype.symbol);
    if (own.size() != prototype_own.size())
      return false;
    if (!im.trait || im.trait->declaration != tr.symbol ||
        im.trait->args.size() != tr.params.size())
      return false;
    Substitutions sub{{tr.self, im.self_type}};
    for (size_t i = 0; i < tr.params.size(); ++i)
      sub.push_back({tr.params[i].symbol, im.trait->args[i]});
    // Build every positional method-own mapping before comparing any bound.
    // Bounds may name Self, trait parameters or sibling own parameters, so a
    // per-index raw comparison would reject a pure alpha rename and would
    // depend on traversal order.
    for (size_t i = 0; i < own.size(); ++i)
      sub.push_back({prototype_own[i].symbol, parameter_type(own[i].symbol)});
    for (size_t i = 0; i < own.size(); ++i) {
      if (!prototype_own[i].trait != !own[i].trait)
        return false;
      if (prototype_own[i].trait) {
        TraitRef expected = *prototype_own[i].trait;
        for (auto &arg : expected.args)
          arg = substitute(arg, sub, m.types);
        if (expected != *own[i].trait)
          return false;
      }
    }
    auto found = signatures.find(method.value);
    if (found == signatures.end() ||
        found->second != substitute(prototype.signature, sub, m.types))
      return false;
    auto declared = parameters.find(method.value);
    if (declared == parameters.end() ||
        declared->second.size() != prototype.parameters.size())
      return false;
    for (size_t i = 0; i < declared->second.size(); ++i)
      if (declared->second[i].label != prototype.parameters[i].label)
        return false;
    return true;
  }
  // A concrete receiver or type qualifier may use a method from a real trait
  // impl only when the impl names a trait whose prototype has the same source
  // name, labels and alpha-substituted signature. A value-form call needs the
  // prototype's first parameter to be the real ordinary Self; ordinary is not
  // inferred from the spelling `self`. A type-qualified call supplies every
  // declared parameter explicitly, so it has no synthetic receiver. New
  // top-level inout is permitted at non-self positions and at position zero
  // only when the raw first type is a method parameter, not the trait Self.
  // Callback data is checked from the complete substitution, not rejected by
  // this source-entitlement relation. Method-own generics are matched by
  // position; this is still only a source check, while finish_arguments,
  // select_method and instantiate verify the actual implementation and key.
  std::optional<TraitMethodSource>
  trait_impl_inout_source(SymbolId target, bool receiver) {
    const auto *im = impl_of(target);
    if (!im || !im->trait)
      return {};
    const TraitDecl *tr = trait_decl(im->trait->declaration);
    if (!tr)
      return {};
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (symbol(p.symbol).name == symbol(target).name) {
        if (prototype)
          return {};
        prototype = &p;
      }
    if (!prototype ||
        !trait_impl_signature_matches(*tr, *im, *prototype, target))
      return {};
    auto found = signatures.find(target.value);
    if (found == signatures.end())
      return {};
    const auto proto_sig = type(prototype->signature);
    const auto method_sig = type(found->second);
    if (prototype->parameters.empty() || proto_sig.parameters.empty()) {
      if (receiver)
        return {};
      return TraitMethodSource{im, tr, prototype};
    }
    if (method_sig.parameters.empty())
      return {};
    if (receiver) {
      const auto &raw_self = type(proto_sig.parameters.front().type);
      if (raw_self.kind != TypeKind::TypeParameter ||
          raw_self.declaration != tr->self ||
          proto_sig.parameters.front().mode != ParamMode::ordinary ||
          method_sig.parameters.front().mode != ParamMode::ordinary ||
          method_sig.parameters.front().type != im->self_type)
        return {};
      return TraitMethodSource{im, tr, prototype};
    }
    // Type/T.method(...) starts at declaration index zero and supplies the
    // actual first-position Self explicitly. Its move/inout mode is allowed
    // here; typed_call and the substituted signature still distinguish it
    // from an own U named `self` and from a value-form receiver. Dependent
    // callback data is validated from the complete substituted trait
    // application and the concrete method signature.
    return TraitMethodSource{im, tr, prototype};
  }
  // Move eligibility is separate from the older inout route. A value-form
  // call requires a real ordinary trait-Self first parameter, but this batch
  // no longer requires its label to be literally `self`; an owner-labelled
  // ordinary receiver is already the same receiver. A type-qualified call
  // supplies every declared parameter explicitly, so it starts at index zero
  // and also admits a static method with no Self or a first move parameter
  // whose own type parameter happens to be named `self`. Actual self modes
  // are classified from the raw declaration in typed_call, not from the
  // substituted signature where an own parameter may already equal Self.
  std::optional<TraitMethodSource>
  trait_impl_move_source(SymbolId target, bool receiver) {
    const auto *im = impl_of(target);
    if (!im || !im->trait)
      return {};
    const TraitDecl *tr = trait_decl(im->trait->declaration);
    if (!tr)
      return {};
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (symbol(p.symbol).name == symbol(target).name) {
        if (prototype)
          return {};
        prototype = &p;
      }
    if (!prototype ||
        !trait_impl_signature_matches(*tr, *im, *prototype, target))
      return {};
    auto found = signatures.find(target.value);
    if (found == signatures.end())
      return {};
    const auto proto_sig = type(prototype->signature);
    const auto method_sig = type(found->second);
    if (prototype->parameters.empty() || proto_sig.parameters.empty() ||
        method_sig.parameters.empty())
      return {};
    if (receiver) {
      const auto &raw_self = type(proto_sig.parameters.front().type);
      if (raw_self.kind != TypeKind::TypeParameter ||
          raw_self.declaration != tr->self ||
          proto_sig.parameters.front().mode != ParamMode::ordinary ||
          method_sig.parameters.front().mode != ParamMode::ordinary ||
          method_sig.parameters.front().type != im->self_type)
        return {};
    }
    return TraitMethodSource{im, tr, prototype};
  }
  // A constraint call is limited to a real prototype of the required trait,
  // reached through a bounded template receiver with a real ordinary self.
  // Ordinary is decided from the real trait Self type, not the label. Later
  // top-level inout may be a fixed scalar, a method-own parameter, or the
  // trait Self; the substituted call and instance still check the concrete
  // data type. Method-own generics are completed by typed_call before the
  // ConstraintCall is made.
  const MethodPrototype *constraint_method_source(SymbolId target,
                                                  const TraitRef &trait,
                                                  TypeId receiver) {
    const TraitDecl *tr = trait_decl(trait.declaration);
    if (!tr || type(receiver).kind != TypeKind::TypeParameter)
      return nullptr;
    auto bounded = bounds.find(type(receiver).declaration.value);
    if (bounded == bounds.end() || !bounded->second ||
        *bounded->second != trait)
      return nullptr;
    if (std::none_of(active_params.begin(), active_params.end(),
                     [&](const GenericParam &p) {
                       return p.symbol == type(receiver).declaration;
                     }))
      return nullptr;
    auto generics = generic_params.find(target.value);
    if (generics == generic_params.end())
      return nullptr;
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == target) {
        if (prototype)
          return nullptr;
        prototype = &p;
      }
    if (!prototype)
      return nullptr;
    if (prototype->parameters.empty())
      return nullptr;
    const auto &sig = type(prototype->signature);
    if (sig.parameters.empty() ||
        sig.parameters.front().mode != ParamMode::ordinary)
      return nullptr;
    const auto &raw_self = type(sig.parameters.front().type);
    if (raw_self.kind != TypeKind::TypeParameter ||
        raw_self.declaration != tr->self)
      return nullptr;
    return prototype;
  }
  // Move eligibility for a value-form bounded T call. This uses the same
  // real trait/bound/prototype relation as the older inout source, but it
  // accepts either ordinary receiver label and does not inspect the old
  // inout restrictions: move qualification is a separate decision.
  const MethodPrototype *constraint_method_move_source(SymbolId target,
                                                       const TraitRef &trait,
                                                       TypeId receiver) {
    const TraitDecl *tr = trait_decl(trait.declaration);
    if (!tr || type(receiver).kind != TypeKind::TypeParameter)
      return nullptr;
    auto bounded = bounds.find(type(receiver).declaration.value);
    if (bounded == bounds.end() || !bounded->second ||
        *bounded->second != trait)
      return nullptr;
    if (std::none_of(active_params.begin(), active_params.end(),
                     [&](const GenericParam &p) {
                       return p.symbol == type(receiver).declaration;
                     }))
      return nullptr;
    auto generics = generic_params.find(target.value);
    if (generics == generic_params.end())
      return nullptr;
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == target) {
        if (prototype)
          return nullptr;
        prototype = &p;
      }
    if (!prototype)
      return nullptr;
    if (prototype->parameters.empty())
      return nullptr;
    const auto &sig = type(prototype->signature);
    if (sig.parameters.empty() ||
        sig.parameters.front().mode != ParamMode::ordinary)
      return nullptr;
    const auto &raw_self = type(sig.parameters.front().type);
    if (raw_self.kind != TypeKind::TypeParameter ||
        raw_self.declaration != tr->self)
      return nullptr;
    return prototype;
  }
  // Move eligibility for a type-qualified T.method(...) call. It has no
  // runtime receiver, so every declared parameter is explicit. Move and the
  // new top-level inout forms can coexist. Actual first-position Self is
  // classified from the raw prototype by typed_call; callback data is not a
  // separate source-entitlement restriction.
  const MethodPrototype *constraint_static_method_move_source(
      SymbolId target, const TraitRef &trait, TypeId receiver) {
    const TraitDecl *tr = trait_decl(trait.declaration);
    if (!tr || type(receiver).kind != TypeKind::TypeParameter)
      return nullptr;
    auto bounded = bounds.find(type(receiver).declaration.value);
    if (bounded == bounds.end() || !bounded->second ||
        *bounded->second != trait)
      return nullptr;
    if (std::none_of(active_params.begin(), active_params.end(),
                     [&](const GenericParam &p) {
                       return p.symbol == type(receiver).declaration;
                     }))
      return nullptr;
    auto generics = generic_params.find(target.value);
    if (generics == generic_params.end())
      return nullptr;
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == target) {
        if (prototype)
          return nullptr;
        prototype = &p;
      }
    if (!prototype)
      return nullptr;
    const auto &sig = type(prototype->signature);
    const bool top_level_inout =
        std::any_of(sig.parameters.begin(), sig.parameters.end(),
                    [](const auto &p) { return p.mode == ParamMode::inout; });
    if (top_level_inout) {
      for (const auto &p : sig.parameters)
        if (p.mode != ParamMode::ordinary && p.mode != ParamMode::move &&
            p.mode != ParamMode::inout)
          return nullptr;
      return prototype;
    }
    for (const auto &p : sig.parameters)
      if (p.mode != ParamMode::ordinary && p.mode != ParamMode::move)
        return nullptr;
    return prototype;
  }
  // A type-qualified T.method(...) has no runtime receiver. All declared
  // parameters are explicit, including a first-position actual Self, which
  // may now use move/inout. Method-own generics are completed by typed_call;
  // the required trait bound and concrete data are checked by the caller.
  const MethodPrototype *constraint_static_method_source(SymbolId target,
                                                          const TraitRef &trait,
                                                          TypeId receiver) {
    const TraitDecl *tr = trait_decl(trait.declaration);
    if (!tr || type(receiver).kind != TypeKind::TypeParameter)
      return nullptr;
    auto bounded = bounds.find(type(receiver).declaration.value);
    if (bounded == bounds.end() || !bounded->second ||
        *bounded->second != trait)
      return nullptr;
    if (std::none_of(active_params.begin(), active_params.end(),
                     [&](const GenericParam &p) {
                       return p.symbol == type(receiver).declaration;
                     }))
      return nullptr;
    auto generics = generic_params.find(target.value);
    if (generics == generic_params.end())
      return nullptr;
    const MethodPrototype *prototype = nullptr;
    for (const auto &p : tr->methods)
      if (p.symbol == target) {
        if (prototype)
          return nullptr;
        prototype = &p;
      }
    if (!prototype)
      return nullptr;
    const auto &sig = type(prototype->signature);
    const bool top_level_inout =
        std::any_of(sig.parameters.begin(), sig.parameters.end(),
                    [](const auto &p) { return p.mode == ParamMode::inout; });
    for (size_t i = 0; i < sig.parameters.size(); ++i) {
      const auto &p = sig.parameters[i];
      if (top_level_inout) {
        if (p.mode != ParamMode::ordinary && p.mode != ParamMode::move &&
            p.mode != ParamMode::inout)
          return nullptr;
      } else {
        if (p.mode != ParamMode::ordinary && p.mode != ParamMode::move)
          return nullptr;
      }
    }
    return prototype;
  }
  // A direct call to a generic free function. Trait constraints do not change
  // this source identity; they are checked from the finished type arguments.
  bool free_generic_template(SymbolId target) const {
    auto found = generic_params.find(target.value);
    return found != generic_params.end() && !found->second.empty() &&
           !impl_method(target);
  }
  bool check_inout_graph(Ref r, TypeId root, bool allow_template,
                         std::optional<SymbolId> trait_self = std::nullopt,
                         bool trait_self_callback = false) {
    std::set<uint64_t> visited_args, expanded_nominals;
    bool direct_inout = false;
    // A declaration that cannot contain any function with an inout parameter
    // cannot hide an illegal callback, whatever its growing return graph is.
    // This is only a pruning proof, not a second POD policy: any declaration
    // that can reach an inout is expanded and checked by the common rule.
    std::map<uint64_t, bool> inout_memo;
    auto has_inout = [&](TypeId root) -> bool {
      if (auto it = inout_memo.find(root.value); it != inout_memo.end())
        return it->second;
      // Cache only a completed reachability search, never a cycle back-edge.
      std::set<uint64_t> seen_types, seen_declarations;
      std::vector<TypeId> pending{root};
      while (!pending.empty()) {
        check_cancelled(stop);
        const auto id = pending.back();
        pending.pop_back();
        if (!seen_types.insert(id.value).second)
          continue;
        const auto t = type(id);
        pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
        if (t.kind == TypeKind::Function) {
          for (const auto &p : t.parameters) {
            if (p.mode == ParamMode::inout)
              return inout_memo[root.value] = true;
            pending.push_back(p.type);
          }
          pending.push_back(t.result);
          if (t.throws_type)
            pending.push_back(*t.throws_type);
        }
        if (t.kind != TypeKind::NominalStruct &&
            t.kind != TypeKind::NominalEnum)
          continue;
        const auto &d = nominal(id);
        if (!seen_declarations.insert(d.symbol.value).second)
          continue;
        for (const auto &f : d.fields)
          pending.push_back(f.type);
        for (const auto &v : d.variants)
          for (const auto &f : v.payload)
            pending.push_back(f.type);
      }
      return inout_memo[root.value] = false;
    };
    // Visit each depth before expanding a growing return graph further.
    std::vector<std::pair<TypeId, size_t>> work{{root, 0}};
    auto visit = [&](TypeId id, size_t depth) { work.emplace_back(id, depth); };
    for (size_t next = 0; next < work.size(); ++next) {
      const auto [id, depth] = work[next];
      check_cancelled(stop);
      if (depth > 512)
        unavailable(r, "inout type graph exceeds checking budget");
      const auto t = type(id);
      if (t.kind == TypeKind::TypeParameter)
        continue;
      if (t.kind == TypeKind::Vec) {
        if (!visited_args.insert(id.value).second)
          continue;
        for (auto a : t.type_args)
          visit(a, depth + 1);
        continue;
      }
      if (t.kind == TypeKind::NominalStruct ||
          t.kind == TypeKind::NominalEnum) {
        if (visited_args.insert(id.value).second)
          for (auto a : t.type_args)
            visit(a, depth + 1);
        const auto &d = nominal(id);
        // Distinct complete applications must each have their fields checked.
        if (expanded_nominals.insert(id.value).second && has_inout(id)) {
          auto subs = nominal_subs(id);
          auto field = [&](const FieldDecl &f) {
            visit(substitute(f.type, subs, m.types), depth + 1);
          };
          for (const auto &f : d.fields)
            field(f);
          for (const auto &v : d.variants)
            for (const auto &f : v.payload)
              field(f);
        }
        continue;
      }
      if (t.kind != TypeKind::Function ||
          !visited_args.insert(id.value).second)
        continue;
      for (size_t i = 0; i < t.parameters.size(); ++i) {
        const auto &p = t.parameters[i];
        if (p.mode == ParamMode::inout) {
          // Callback modes do not change the enclosing function's modes.
          if (depth == 0)
            direct_inout = true;
          // A trait prototype may name its real Self in any inout slot,
          // including the actual first-position receiver. The declaration is
          // only deferred symbolic data; every complete trait application is
          // checked again after substituting Self and the ordered arguments.
          // A nested FunctionType still has no receiver of its own, so its
          // first slot is callback data and reaches the same concrete check.
          const bool trait_self_slot =
              trait_self && trait_self_type(p.type, *trait_self);
          const auto data_self =
              trait_self && (depth > 0 || i > 0) ? trait_self : std::nullopt;
          if (!aggregate_inout_type(p.type, data_self) &&
              !(allow_template && template_inout_type(p.type)) &&
              !trait_self_slot)
            unavailable(
                r,
                "inout accepts only trivial numeric/bool whole bindings");
        }
        visit(p.type, depth + 1);
      }
      visit(t.result, depth + 1);
      if (t.throws_type)
        visit(*t.throws_type, depth + 1);
    }
    return direct_inout;
  }
  // Validate every method prototype of one complete trait application after
  // substituting the real Self and all ordered trait arguments. Unresolved
  // type parameters from the surrounding template (including nominal
  // arguments such as Holder[U]) remain deferred; concrete data is checked
  // with the same inout_data_type graph used by ordinary calls.
  void check_trait_application_data(Ref r, const TraitRef &trait,
                                    TypeId application) {
    const auto *tr = trait_decl(trait.declaration);
    if (!tr || trait.args.size() != tr->params.size())
      return;
    Substitutions sub{{tr->self, application}};
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, trait.args[i]});
    std::set<uint64_t> base;
    std::set<uint64_t> seen;
    std::function<void(TypeId)> allow = [&](TypeId id) {
      if (!seen.insert(id.value).second)
        return;
      const auto t = type(id);
      if (t.kind == TypeKind::TypeParameter) {
        base.insert(t.declaration.value);
        return;
      }
      for (auto a : t.type_args)
        allow(a);
      for (const auto &p : t.parameters)
        allow(p.type);
      if (t.kind == TypeKind::Function) {
        allow(t.result);
        if (t.throws_type)
          allow(*t.throws_type);
      }
    };
    allow(application);
    for (auto arg : trait.args)
      allow(arg);
    auto saved = std::move(signature_parameters);
    for (const auto &method : tr->methods) {
      signature_parameters = base;
      for (const auto &p : method_own_parameters(method.symbol))
        signature_parameters.insert(p.symbol.value);
      check_inout_graph(r, substitute(method.signature, sub, m.types), true);
    }
    signature_parameters = std::move(saved);
  }
  // Trait methods share the existing non-self and actual-self move/inout
  // rules. A raw first-position Self is accepted at declaration; concrete
  // applications recheck its data through the common inout graph. Nested
  // callbacks are not rejected merely for depending on Self, a trait argument
  // or a method-own parameter.
  void check_generic_trait_method_modes(
      Ref r, const std::vector<GenericParam> &own, TypeId signature,
      std::optional<SymbolId> trait_self, bool generic_trait,
      const std::vector<GenericParam> *trait_params = nullptr) {
    if (!generic_trait && own.empty())
      return;
    (void)trait_self;
    (void)own;
    (void)trait_params;
    const auto top = type(signature);
    const auto parameters = top.parameters;
    for (const auto &p : parameters) {
      if (p.mode == ParamMode::ordinary)
        continue;
      if (p.mode != ParamMode::move && p.mode != ParamMode::inout)
        unavailable(r, "trait method parameter mode is not implemented");
    }
  }
  void check_function_value_inout(Ref r, SymbolId target, TypeId sig) {
    if (!check_inout_graph(r, sig, true))
      return;
    auto found = generic_params.find(target.value);
    if (found != generic_params.end() && !found->second.empty() &&
        !free_generic_template(target))
      unavailable(r, "generic/method inout is not implemented");
  }
  std::vector<GenericParam> generics(Ref r) {
    std::vector<GenericParam> ps;
    for (auto c : ast(r).children) {
      auto cr = at(r, c);
      if (ast(cr).kind != AstKind::GenericParameter)
        continue;
      auto id = definition(cr);
      std::optional<TraitRef> trait;
      if (!ast(cr).children.empty()) {
        auto br = child(cr, 0);
        auto declaration = occurrence(br);
        if (!declaration ||
            symbol(*declaration).kind != modules::SymbolKind::trait)
          reject(cr, "generic constraint must name a trait");
        std::vector<TypeId> args;
        if (ast(br).kind == AstKind::NeutralBracket) {
          for (size_t i = 1; i < ast(br).children.size(); ++i)
            args.push_back(parse_type(child(br, i)));
        } else {
          for (auto brc : ast(br).children)
            args.push_back(parse_type(at(br, brc)));
        }
        trait = check_trait_application(br, *declaration, std::move(args));
      }
      bounds[id.value] = trait;
      ps.push_back({id, trait, source(cr)});
      parameter_type(id);
    }
    return ps;
  }
  const NominalDecl &nominal(TypeId id) const {
    return m.nominals.at(nominal_ids.at(type(id).declaration.value));
  }
  Substitutions nominal_subs(TypeId id) const {
    Substitutions result;
    const auto &d = nominal(id);
    for (size_t i = 0; i < d.params.size(); ++i)
      result.push_back({d.params[i].symbol, type(id).type_args.at(i)});
    return result;
  }
  TypeId field_type(TypeId id, const FieldDecl &f) {
    return substitute(f.type, nominal_subs(id), m.types);
  }
  TypeId parse_type(Ref r, std::optional<SymbolId> trait_self = std::nullopt,
                    bool trait_self_callback = false) {
    check_cancelled(stop);
    const auto &n = ast(r);
    if (n.kind == AstKind::ParenthesizedType ||
        n.kind == AstKind::Parenthesized || n.kind == AstKind::BracketItem)
      return parse_type(child(r, 0), trait_self, trait_self_callback);
    if (n.kind == AstKind::FunctionType)
      return signature(r, trait_self, true, trait_self_callback);
    std::optional<SymbolId> s;
    std::vector<TypeId> args;
    if (n.kind == AstKind::NeutralBracket) {
      s = occurrence(child(r, 0));
      for (size_t i = 1; i < n.children.size(); ++i)
        args.push_back(parse_type(child(r, i)));
    } else if (n.kind == AstKind::NamedType || n.kind == AstKind::Name ||
               n.kind == AstKind::Member) {
      s = occurrence(r);
      for (auto c : n.children)
        if (n.kind == AstKind::NamedType)
          args.push_back(parse_type(at(r, c)));
    } else
      reject(r, "generic arguments must be types", "E_NAME_KIND");
    if (!s)
      reject(r, "type has no resolved symbol", "E_NAME_KIND");
    auto kind = symbol(*s).kind;
    if (kind == modules::SymbolKind::type_parameter) {
      if (!args.empty())
        reject(r, "type parameter does not accept arguments");
      auto it = self_types.find(s->value);
      return it == self_types.end() ? parameter_type(*s) : it->second;
    }
    if (kind == modules::SymbolKind::type) {
      if (auto ex = m.resolved->foreign_export(*s)) {
        if (!args.empty()) reject(r, "external type does not accept type arguments");
        if (!ex->declaration) reject(r, "external type lacks declaration");
        return m.foreign_bindings.at(ex->declaration->value).type;
      }
      auto it = nominal_ids.find(s->value);
      if (it == nominal_ids.end())
        unavailable(r, "foreign types require T10/T11");
      const auto &d = m.nominals[it->second];
      if (args.size() != d.params.size())
        reject(r, "wrong number of type arguments");
      Type t;
      t.kind = d.kind;
      t.declaration = *s;
      t.type_args = std::move(args);
      return m.types.intern(t);
    }
    if (kind != modules::SymbolKind::builtin_type)
      reject(r, "name does not denote a value type", "E_NAME_KIND");
    const auto name = symbol(*s).name;
    if (name == "Vec") {
      if (args.size() != 1)
        reject(r, "Vec requires one element type");
      if (type(args[0]).kind == TypeKind::ForeignOpaque)
        unavailable(r, "external values in Vec are not supported");
      Type t;
      t.kind = TypeKind::Vec;
      t.type_args = args;
      return m.types.intern(t);
    }
    if (name == "Result")
      unavailable(
          r, "container, owned string and Result support requires T08/T09");
    if (!args.empty())
      reject(r, "scalar type does not accept type arguments");
    if (name == "Unit")
      return unit;
    if (name == "bool")
      return boolean;
    if (name == "string")
      return scalar(TypeKind::String);
    if (name == "isize")
      return scalar(TypeKind::Isize);
    if (name == "usize")
      return scalar(TypeKind::Usize);
    if (name == "f32" || name == "f64")
      return scalar(TypeKind::Float, name == "f32" ? 32 : 64);
    if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u')) {
      unsigned w = name == "i8" || name == "u8"     ? 8
                   : name == "i16" || name == "u16" ? 16
                   : name == "i32" || name == "u32" ? 32
                                                    : 64;
      return scalar(
          name[0] == 'i' ? TypeKind::SignedInt : TypeKind::UnsignedInt, w);
    }
    unavailable(r, "type is not implemented");
  }
  TypeId signature(Ref r, std::optional<SymbolId> trait_self = std::nullopt,
                   bool callback = false, bool trait_self_callback = false) {
    Type t;
    t.kind = TypeKind::Function;
    t.result = unit;
    const auto parts = split_signature(file(r), r.node);
    for (size_t i = 0; i < parts.parameters.size(); ++i) {
      auto cr = at(r, parts.parameters[i]);
      auto pm = mode(cr);
      auto pt = parse_type(child(cr, 0), trait_self, trait_self_callback);

      if (pm != ParamMode::ordinary && type(pt).kind == TypeKind::ForeignOpaque)
        unavailable(cr, "external move/inout parameters are not supported");
      // A trait prototype may declare its actual first-position receiver or
      // any other slot `inout Self`. The raw Self is deferred here because no
      // concrete type exists yet; every complete trait application substitutes
      // Self and checks the resulting data with the common POD rule. A nested
      // FunctionType has no receiver, so its first slot remains callback data.
      const bool trait_self_slot =
          trait_self && trait_self_type(pt, *trait_self);
      const auto data_self =
          trait_self && (callback || i > 0) ? trait_self : std::nullopt;
      if (pm == ParamMode::inout && !aggregate_inout_type(pt, data_self) &&
          !template_inout_type(pt) && !trait_self_slot)
        unavailable(cr,
                    "inout accepts only trivial numeric/bool or POD aggregate "
                    "whole bindings");
      t.parameters.push_back({pm, pt});
    }
    if (parts.result)
      t.result =
          parse_type(at(r, *parts.result), trait_self, trait_self_callback);
    if (parts.throws_type)
      t.throws_type = parse_type(at(r, *parts.throws_type), trait_self,
                                 trait_self_callback);
    return m.types.intern(t);
  }
  std::optional<mpz_class> literal_integer(Ref r) const {
    while (ast(r).kind == AstKind::Parenthesized)
      r = child(r, 0);
    bool negative = false;
    if (ast(r).kind == AstKind::Unary && (token(r) == "+" || token(r) == "-")) {
      negative = token(r) == "-";
      r = child(r, 0);
      while (ast(r).kind == AstKind::Parenthesized)
        r = child(r, 0);
    }
    const auto &n = ast(r);
    if (n.kind != AstKind::Literal ||
        file(r).tokens[n.token_begin].kind != TokenKind::integer)
      return {};
    mpz_class v = parse_integer_spelling(token(r));
    return negative ? mpz_class(-v) : v;
  }
  // The exact RNE kernel lives in the shared literal helper. The checker only
  // supplies the already-determined target width.
  uint64_t round_ratio(bool negative, const mpz_class &numerator,
                       const mpz_class &denominator, TypeId t) const {
    return exact_ratio_to_ieee(negative, numerator, denominator,
                               type(t).bits);
  }
  // Round (numerator / denominator) * 2^shift for exact integer inputs.
  uint64_t round_fraction(bool negative, const mpz_class &numerator,
                          const mpz_class &denominator, long shift,
                          TypeId t) const {
    if (shift >= 0)
      return round_ratio(negative, numerator << shift, denominator, t);
    return round_ratio(negative, numerator, denominator << -shift, t);
  }
  // Decode an already-rounded source IEEE value into exact integer fields
  // (magnitude = significand * 2^shift). NaN/Infinity are classified first.
  struct Ieee {
    enum class Kind { finite, infinite, nan } kind = Kind::finite;
    bool negative = false;
    mpz_class significand;
    long shift = 0;
  };
  static Ieee decode_ieee(uint64_t bits, unsigned width) {
    const unsigned fraction_bits = width == 32 ? 23 : 52;
    const unsigned exponent_bits = width == 32 ? 8 : 11;
    const long bias = width == 32 ? 127 : 1023;
    Ieee v;
    v.negative = (bits >> (width - 1)) != 0;
    const uint64_t exponent =
        (bits >> fraction_bits) & ((uint64_t(1) << exponent_bits) - 1);
    const uint64_t fraction =
        bits & ((uint64_t(1) << fraction_bits) - 1);
    if (exponent == (uint64_t(1) << exponent_bits) - 1) {
      v.kind = fraction ? Ieee::Kind::nan : Ieee::Kind::infinite;
    } else if (exponent == 0) {
      v.significand = fraction;
      v.shift = long(1 - bias) - long(fraction_bits);
    } else {
      v.significand = (uint64_t(1) << fraction_bits) | fraction;
      v.shift = long(exponent) - bias - long(fraction_bits);
    }
    return v;
  }
  // Correctly round the decimal rational directly to IEEE precision. No host
  // double intermediate (in particular for f32 halfway and subnormal values).
  uint64_t float_bits(Ref r, TypeId t) const {
    return ieee_float_bits(token(r), false, type(t).bits);
  }

  std::string_view binary_token(Ref r) const {
    return file(r).tokens.at(ast(child(r, 0)).token_end).spelling;
  }
  static BinaryOp binary_op(std::string_view s) {
    static const std::map<std::string_view, BinaryOp> ops{
        {"+", BinaryOp::add},          {"-", BinaryOp::subtract},
        {"*", BinaryOp::multiply},     {"/", BinaryOp::divide},
        {"%", BinaryOp::remainder},    {"<<", BinaryOp::shift_left},
        {">>", BinaryOp::shift_right}, {"&", BinaryOp::bit_and},
        {"|", BinaryOp::bit_or},       {"^", BinaryOp::bit_xor},
        {"==", BinaryOp::equal},       {"!=", BinaryOp::not_equal},
        {"<", BinaryOp::less},         {"<=", BinaryOp::less_equal},
        {">", BinaryOp::greater},      {">=", BinaryOp::greater_equal}};
    return ops.at(s);
  }
  void match(Ref r, TypeId actual, std::optional<TypeId> expected) const {
    if (expected && actual != *expected)
      reject(r, "value type does not match the expected type");
  }
  bool contextual(Ref r) const {
    const auto &n = ast(r);
    if (n.kind == AstKind::Parenthesized)
      return contextual(child(r, 0));
    if (n.kind == AstKind::Unary &&
        (token(r) == "+" || token(r) == "-" || token(r) == "move"))
      return contextual(child(r, 0));
    if (n.kind == AstKind::Literal) {
      auto kind = file(r).tokens[n.token_begin].kind;
      return kind == TokenKind::integer || kind == TokenKind::floating;
    }
    if (n.kind == AstKind::Binary)
      return contextual(child(r, 0)) && contextual(child(r, 1));
    return false;
  }
  Value fold(BinaryOp op, Value a, const Value &b) const {
    Value result{a.type, {}, {}};
    if (!a.integer || !b.integer)
      return result;
    const mpz_class &x = *a.integer, &y = *b.integer;
    mpz_class z;
    switch (op) {
    case BinaryOp::add:
      z = x + y;
      break;
    case BinaryOp::subtract:
      z = x - y;
      break;
    case BinaryOp::multiply:
      z = x * y;
      break;
    case BinaryOp::divide:
    case BinaryOp::remainder:
      if (y == 0)
        return result; // retain the runtime Panic operation
      if (op == BinaryOp::divide)
        mpz_tdiv_q(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
      else
        mpz_tdiv_r(z.get_mpz_t(), x.get_mpz_t(), y.get_mpz_t());
      break;
    case BinaryOp::shift_left:
    case BinaryOp::shift_right:
      if (y < 0 || y >= width(a.type))
        return result;
      if (op == BinaryOp::shift_left)
        z = x << y.get_ui();
      else
        z = x >> y.get_ui();
      break;
    case BinaryOp::bit_and:
      z = x & y;
      break;
    case BinaryOp::bit_or:
      z = x | y;
      break;
    case BinaryOp::bit_xor:
      z = x ^ y;
      break;
    default:
      result.type = boolean;
      result.boolean = op == BinaryOp::equal        ? x == y
                       : op == BinaryOp::not_equal  ? x != y
                       : op == BinaryOp::less       ? x < y
                       : op == BinaryOp::less_equal ? x <= y
                       : op == BinaryOp::greater    ? x > y
                                                    : x >= y;
      return result;
    }
    result.integer = wrap(std::move(z), a.type);
    return result;
  }
  // Exact-ratio fold for the four float arithmetic operators and ordered
  // comparisons. Every step rounds to the expression's own f32/f64 type,
  // exactly as the runtime operation would, including NaN, infinity, signed
  // zero and subnormal results.
  Value float_fold(BinaryOp op, const Value &a, const Value &b) const {
    Value result{a.type, {}, {}, {}};
    if (!a.floating || !b.floating)
      return result;
    const unsigned width = type(a.type).bits;
    const uint64_t infinity =
        width == 32 ? 0x7f800000ULL : 0x7ff0000000000000ULL;
    const uint64_t nan_bits =
        width == 32 ? 0x7fc00000ULL : 0x7ff8000000000000ULL;
    auto x = decode_ieee(*a.floating, width);
    auto y = decode_ieee(*b.floating, width);
    auto signed_infinity = [&](bool negative) {
      return uint64_t(negative) << (width - 1) | infinity;
    };
    auto signed_zero = [&](bool negative) {
      return uint64_t(negative) << (width - 1);
    };
    switch (op) {
    case BinaryOp::add:
    case BinaryOp::subtract: {
      if (op == BinaryOp::subtract)
        y.negative = !y.negative;
      if (x.kind == Ieee::Kind::nan || y.kind == Ieee::Kind::nan) {
        result.floating = nan_bits;
      } else if (x.kind == Ieee::Kind::infinite &&
                 y.kind == Ieee::Kind::infinite) {
        result.floating =
            x.negative == y.negative ? signed_infinity(x.negative) : nan_bits;
      } else if (x.kind == Ieee::Kind::infinite) {
        result.floating = signed_infinity(x.negative);
      } else if (y.kind == Ieee::Kind::infinite) {
        result.floating = signed_infinity(y.negative);
      } else {
        const long shift = std::min(x.shift, y.shift);
        mpz_class sum = x.significand << (x.shift - shift);
        if (x.negative)
          sum = -sum;
        mpz_class addend = y.significand << (y.shift - shift);
        if (y.negative)
          addend = -addend;
        sum += addend;
        mpz_class magnitude = sum < 0 ? mpz_class(-sum) : sum;
        // An exact zero is +0 under RNE unless both addends are -0.
        const bool negative_zero =
            sum == 0 && x.significand == 0 && y.significand == 0 &&
            x.negative && y.negative;
        result.floating = round_fraction(sum < 0 || negative_zero, magnitude, 1,
                                         shift, a.type);
      }
      break;
    }
    case BinaryOp::multiply: {
      const bool negative = x.negative != y.negative;
      if (x.kind == Ieee::Kind::nan || y.kind == Ieee::Kind::nan) {
        result.floating = nan_bits;
      } else if (x.kind == Ieee::Kind::infinite ||
                 y.kind == Ieee::Kind::infinite) {
        const Ieee &finite = x.kind == Ieee::Kind::infinite ? y : x;
        result.floating = finite.kind == Ieee::Kind::finite &&
                                  finite.significand == 0
                              ? nan_bits
                              : signed_infinity(negative);
      } else {
        result.floating =
            round_fraction(negative, x.significand * y.significand, 1,
                           x.shift + y.shift, a.type);
      }
      break;
    }
    case BinaryOp::divide: {
      const bool negative = x.negative != y.negative;
      if (x.kind == Ieee::Kind::nan || y.kind == Ieee::Kind::nan) {
        result.floating = nan_bits;
      } else if (x.kind == Ieee::Kind::infinite) {
        result.floating = y.kind == Ieee::Kind::infinite
                              ? nan_bits
                              : signed_infinity(negative);
      } else if (y.kind == Ieee::Kind::infinite) {
        result.floating = signed_zero(negative);
      } else if (y.significand == 0) {
        result.floating = x.significand == 0 ? nan_bits
                                            : signed_infinity(negative);
      } else if (x.significand == 0) {
        result.floating = signed_zero(negative);
      } else {
        result.floating = round_fraction(negative, x.significand, y.significand,
                                         x.shift - y.shift, a.type);
      }
      break;
    }
    case BinaryOp::equal:
    case BinaryOp::not_equal:
    case BinaryOp::less:
    case BinaryOp::less_equal:
    case BinaryOp::greater:
    case BinaryOp::greater_equal: {
      // Compare the exact decoded values, never a host double or a tolerance.
      // NaN is unordered except for !=; every zero compares equal regardless
      // of sign.
      int comparison;
      if (x.kind == Ieee::Kind::nan || y.kind == Ieee::Kind::nan) {
        comparison = 2; // unordered
      } else if (x.kind == Ieee::Kind::infinite ||
                 y.kind == Ieee::Kind::infinite) {
        if (x.kind == Ieee::Kind::infinite &&
            y.kind == Ieee::Kind::infinite)
          comparison = x.negative == y.negative ? 0 : x.negative ? -1 : 1;
        else if (x.kind == Ieee::Kind::infinite)
          comparison = x.negative ? -1 : 1;
        else
          comparison = y.negative ? 1 : -1;
      } else if (x.significand == 0 && y.significand == 0) {
        comparison = 0;
      } else if (x.negative != y.negative) {
        comparison = x.negative ? -1 : 1;
      } else {
        const long shift = std::min(x.shift, y.shift);
        const mpz_class xs = x.significand << (x.shift - shift);
        const mpz_class ys = y.significand << (y.shift - shift);
        comparison = xs < ys ? -1 : xs > ys ? 1 : 0;
        if (x.negative)
          comparison = -comparison;
      }
      result.boolean =
          comparison == 2
              ? op == BinaryOp::not_equal
              : op == BinaryOp::equal        ? comparison == 0
              : op == BinaryOp::not_equal    ? comparison != 0
              : op == BinaryOp::less         ? comparison < 0
              : op == BinaryOp::less_equal   ? comparison <= 0
              : op == BinaryOp::greater      ? comparison > 0
                                             : comparison >= 0;
      break;
    }
    default:
      break;
    }
    return result;
  }
  Expr expression(Ref r, std::optional<TypeId> expected = {}) {
    check_cancelled(stop);
    if (auto v = literal_integer(r))
      return int_literal(r, *v, expected.value_or(i64));
    const auto &n = ast(r);
    Expr result{HirNodeId{0}, {unit, {}, {}}};
    switch (n.kind) {
    case AstKind::BracketItem:
    case AstKind::Parenthesized:
      return expression(child(r, 0), expected);
    case AstKind::Literal:
      if (token(r) == "true" || token(r) == "false") {
        bool b = token(r) == "true";
        result = {node(r, boolean, BoolLiteral{b}), {boolean, {}, b}};
      } else if (file(r).tokens[n.token_begin].kind == TokenKind::floating) {
        TypeId t = expected.value_or(scalar(TypeKind::Float, 64));
        if (type(t).kind != TypeKind::Float)
          reject(r, "float literal requires float context");
        const uint64_t bits = float_bits(r, t);
        result = {node(r, t, FloatLiteral{bits}), {t, {}, {}, bits}};
      } else if (file(r).tokens[n.token_begin].kind ==
                 TokenKind::string_literal) {
        auto t = scalar(TypeKind::String);
        // The lexer already decoded escapes and normalized physical newlines.
        // Keep its complete byte sequence, including embedded NULs.
        const auto text = file(r).tokens[n.token_begin].string_value.value();
        result = {node(r, t, StringLiteral{text}), {t, {}, {}, {}, text}};
      } else
        unavailable(r, "string/owned literals require T09");
      break;
    case AstKind::Name:
    case AstKind::Member: {
      auto s = occurrence(r);
      if (!s) {
        if (n.kind != AstKind::Member)
          reject(r, "unresolved value");
        auto base_ref = child(r, 0);
        if (auto bt = type_expression(base_ref)) {
          if (type(*bt).kind == TypeKind::NominalEnum) {
            const auto &d = nominal(*bt);
            auto name = token(r, n.token_end - n.token_begin - 1);
            for (const auto &v : d.variants)
              if (symbol(v.symbol).name == name) {
                if (!v.payload.empty())
                  reject(r, "variant payload requires a constructor call");
                result = {node(r, *bt, EnumInit{v.symbol, {}}), {*bt, {}, {}}};
                break;
              }
            if (result.value.type == unit)
              reject(r, "unknown enum variant");
            break;
          }
          reject(r, "type member is not a value");
        }
        auto base = expression(base_ref);
        if (type(base.value.type).kind == TypeKind::ForeignOpaque)
          unavailable(r, "external data attributes and escaping method values are not supported");
        const auto f = find_field(r, base.value.type);
        auto t = field_type(base.value.type, f);
        result = {node(r, t, FieldRead{f.symbol}, {base.id}), {t, {}, {}}};
        break;
      }
      if (auto ex = m.resolved->foreign_export(*s)) {
        if (ex->kind != modules::ForeignExportKind::value || !ex->declaration)
          unavailable(r, "external function/type values cannot escape a call");
        const auto &b = m.foreign_bindings.at(ex->declaration->value);
        result = {node(r, b.type, ForeignRead{b.binding}), {b.type, {}, {}}};
        break;
      }
      auto k = symbol(*s).kind;
      if (k == modules::SymbolKind::function) {
        auto it = signatures.find(s->value);
        if (it == signatures.end())
          unavailable(r, "foreign function calls require T10/T11");
        std::vector<TypeId> args;
        auto sig = it->second;
        if (!generic_params[s->value].empty()) {
          Substitutions sub;
          if (expected)
            infer(r, sig, *expected, sub);
          args = finish_arguments(r, generic_params[s->value], sub);
          sig = substitute(sig, sub, m.types);
        }
        check_function_value_inout(r, *s, sig);
        result = {node(r, sig, FunctionValue{*s, args}), {sig, {}, {}}};
      } else if (k == modules::SymbolKind::constant &&
                 !locals.count(s->value)) {
        const auto c = constant(*s);
        if (c.value.integer) {
          match(r, c.value.type, expected);
          return int_literal(r, *c.value.integer, c.value.type);
        }
        if (c.value.floating) {
          match(r, c.value.type, expected);
          result = {node(r, c.value.type, FloatLiteral{*c.value.floating}),
                    c.value};
        } else if (c.value.boolean)
          result = {node(r, boolean, BoolLiteral{*c.value.boolean}), c.value};
        else if (c.value.string)
          result = {node(r, c.value.type, StringLiteral{*c.value.string}),
                    c.value};
        else if (c.function) {
          check_function_value_inout(r, *c.function, c.value.type);
          result = {
              node(r, c.value.type, FunctionValue{*c.function, c.type_args}),
              c.value};
        } else
          unavailable(r, "constant value cannot be materialized by scalar HIR");
      } else {
        auto l = locals.find(s->value);
        if (l == locals.end())
          unavailable(r, "binding is not a checked scalar local");
        result = {node(r, l->second.type, Read{*s}), {l->second.type, {}, {}}};
        if (auto c = constants.find(s->value); c != constants.end())
          result.value = c->second.value;
        if (result.value.boolean)
          m.nodes[result.id.value].data = BoolLiteral{*result.value.boolean};
        if (result.value.string)
          m.nodes[result.id.value].data =
              StringLiteral{*result.value.string};
      }
      break;
    }
    case AstKind::Unary: {
      auto op = token(r);
      if (op == "move") {
        auto er = child(r, 0);
        while (ast(er).kind == AstKind::Parenthesized)
          er = child(er, 0);
        if (ast(er).kind == AstKind::Name) {
          auto sid = occurrence(er);
          if (!sid || !locals.count(sid->value))
            reject(r, "move requires a local owner", "E_CALL_ARGUMENT");
          auto l = locals.at(sid->value);
          if (l.ordinary_parameter ||
              (l.writable && symbol(*sid).definition &&
               ast(*symbol(*sid).definition).kind == AstKind::Parameter))
            reject(r, "borrowed parameter cannot be moved", "E_CALL_ARGUMENT");
          if (type(l.type).kind == TypeKind::ForeignOpaque)
            unavailable(r, "external move is not supported");
          match(r, l.type, expected);
          return {node(r, l.type, Move{sid}), {l.type, {}, {}}};
        }
        if (ast(er).kind == AstKind::Member ||
            ast(er).kind == AstKind::NeutralBracket)
          unavailable(r, "partial field/element move is not supported");
        auto e = expression(er, expected);
        if (type(e.value.type).kind == TypeKind::ForeignOpaque)
          unavailable(r, "external move is not supported");
        return {node(r, e.value.type, Move{{}}, {e.id}), e.value};
      }
      if (op == "inout")
        reject(r, "argument mode marker outside its matching call",
               "E_CALL_ARGUMENT");
      auto a = expression(child(r, 0),
                          op == "!" ? std::optional(boolean) : expected);
      if (op == "+") {
        if (!integer(a.value.type) &&
            type(a.value.type).kind != TypeKind::Float)
          reject(r, "unary plus requires a numeric operand");
        return a;
      }
      UnaryOp u = op == "!"   ? UnaryOp::logical_not
                  : op == "~" ? UnaryOp::bit_not
                              : UnaryOp::negate;
      if (op == "!")
        match(r, a.value.type, boolean);
      else if (op == "~" && !integer(a.value.type))
        reject(r, "bitwise not requires an integer operand");
      else if (!integer(a.value.type) &&
               type(a.value.type).kind != TypeKind::Float) {
        reject(r, "integer unary operator requires an integer operand");
      }
      result = {node(r, a.value.type, Unary{u}, {a.id}), a.value};
      if (a.value.integer)
        result.value.integer = wrap(op == "~" ? mpz_class(~*a.value.integer)
                                              : mpz_class(-*a.value.integer),
                                    a.value.type);
      if (a.value.floating && (op == "-" || op == "+"))
        result.value.floating =
            op == "-" ? *a.value.floating ^ (uint64_t(1) << (width(a.value.type) - 1))
                       : *a.value.floating;
      if (a.value.boolean)
        result.value.boolean = !*a.value.boolean;
      break;
    }
    case AstKind::Binary: {
      auto op = binary_token(r);
      if (op == "&&" || op == "||") {
        auto a = expression(child(r, 0), boolean),
             b = expression(child(r, 1), boolean);
        Value v{boolean, {}, {}};
        if (a.value.boolean && ((op == "&&" && !*a.value.boolean) ||
                                (op == "||" && *a.value.boolean)))
          v.boolean = *a.value.boolean;
        else if (a.value.boolean && b.value.boolean)
          v.boolean = op == "&&" ? *a.value.boolean && *b.value.boolean
                                 : *a.value.boolean || *b.value.boolean;
        result = {
            node(r, boolean,
                 ShortCircuit{op == "&&" ? LogicalOp::and_ : LogicalOp::or_},
                 {a.id, b.id}),
            v};
        break;
      }
      const auto bop = binary_op(op);
      bool comparison = bop >= BinaryOp::equal;
      bool shift = bop == BinaryOp::shift_left || bop == BinaryOp::shift_right;
      std::optional<TypeId> context = comparison ? std::nullopt : expected;
      Expr a{HirNodeId{0}, {}}, b{HirNodeId{0}, {}};
      if (!shift && contextual(child(r, 0)) && !contextual(child(r, 1))) {
        b = expression(child(r, 1), context);
        a = expression(child(r, 0), b.value.type);
        // Construction order does not determine execution order; children do.
      } else {
        a = expression(child(r, 0), context);
        b = expression(child(r, 1),
                       shift ? std::nullopt : std::optional(a.value.type));
      }
      if (shift) {
        if (!integer(a.value.type) || !integer(b.value.type))
          reject(r, "shift operands must be integers");
      } else
        match(r, b.value.type, a.value.type);
      const auto k = type(a.value.type).kind;
      if ((bop == BinaryOp::remainder || bop == BinaryOp::bit_and ||
           bop == BinaryOp::bit_or || bop == BinaryOp::bit_xor) &&
          !integer(a.value.type))
        reject(r, "bitwise and remainder operators require integer operands");
      if (!integer(a.value.type) && k != TypeKind::Float &&
          !(k == TypeKind::Bool &&
            (bop == BinaryOp::equal || bop == BinaryOp::not_equal)))
        reject(r, "invalid scalar operator operands");
      auto v = integer(a.value.type)
                   ? fold(bop, a.value, b.value)
                   : k == TypeKind::Float ? float_fold(bop, a.value, b.value)
                                          : Value{boolean, {}, {}, {}};
      if (k == TypeKind::Bool && a.value.boolean && b.value.boolean)
        v.boolean = bop == BinaryOp::equal
                        ? *a.value.boolean == *b.value.boolean
                        : *a.value.boolean != *b.value.boolean;
      v.type = comparison ? boolean : a.value.type;
      result = {node(r, v.type, Binary{bop}, {a.id, b.id}), v};
      break;
    }
    case AstKind::Call:
      result = call(r, expected);
      break;
    case AstKind::List: {
      std::optional<TypeId> element;
      if (expected) {
        if (type(*expected).kind != TypeKind::Vec)
          reject(r, "list requires Vec context");
        element = type(*expected).type_args.at(0);
      }
      std::vector<HirNodeId> values;
      for (auto c : n.children) {
        auto e = expression(at(r, c), element);
        if (!element)
          element = e.value.type;
        values.push_back(e.id);
      }
      if (!element)
        reject(r, "empty list requires a Vec context");
      Type t;
      t.kind = TypeKind::Vec;
      t.type_args = {*element};
      auto vt = m.types.intern(t);
      result = {node(r, vt, VecInit{}, values), {vt, {}, {}}};
      break;
    }
    case AstKind::Slice: {
      auto base = expression(child(r, 0));
      if (type(base.value.type).kind == TypeKind::ForeignOpaque)
        unavailable(r, "external slicing is not supported");
      if (type(base.value.type).kind == TypeKind::String)
        unavailable(r, "string slicing is not supported");
      if (type(base.value.type).kind != TypeKind::Vec)
        reject(r, "slice receiver must be Vec");
      std::vector<HirNodeId> values{base.id};
      VecSlice slice;
      // A start endpoint is a BracketItem; an end endpoint is a plain expr.
      // Confirm the delimiter using token ranges, including nested slices.
      size_t colon = ast(child(r, 0)).token_end + 1;
      if (n.children.size() > 1 &&
          ast(child(r, 1)).kind == AstKind::BracketItem)
        colon = ast(child(r, 1)).token_end;
      if (file(r).tokens.at(colon).spelling != ":")
        reject(r, "invalid slice delimiter");
      for (size_t i = 1; i < n.children.size(); ++i) {
        auto e = expression(child(r, i));
        if (!integer(e.value.type))
          reject(r, "slice endpoint must be integer");
        values.push_back(e.id);
        if (ast(child(r, i)).token_begin < colon)
          slice.start = e.id;
        else
          slice.end = e.id;
      }
      result = {node(r, base.value.type, slice, values), base.value};
      break;
    }
    case AstKind::NeutralBracket: {
      auto base = child(r, 0);
      auto s = occurrence(base);
      if (!s || symbol(*s).kind != modules::SymbolKind::function) {
        auto v = expression(base);
        if (type(v.value.type).kind == TypeKind::ForeignOpaque)
          unavailable(r, "external indexing is not supported");
        if (type(v.value.type).kind == TypeKind::String)
          unavailable(r, "string indexing is not supported");
        if (type(v.value.type).kind != TypeKind::Vec)
          reject(r, "index receiver must be Vec");
        if (n.children.size() != 2)
          reject(r, "Vec requires one index");
        auto index = expression(child(r, 1));
        if (!integer(index.value.type))
          reject(r, "Vec index must be integer");
        auto t = type(v.value.type).type_args.at(0);
        result = {node(r, t, IndexRead{}, {v.id, index.id}), {t, {}, {}}};
        break;
      }
      auto ps = generic_params.at(s->value);
      Substitutions sub;
      if (n.children.size() - 1 != ps.size())
        reject(r, "wrong number of generic arguments");
      std::vector<TypeId> args;
      for (size_t i = 0; i < ps.size(); ++i) {
        auto t = parse_type(child(r, i + 1));
        args.push_back(t);
        sub.push_back({ps[i].symbol, t});
      }
      check_constraints(r, ps, args, &sub);
      auto sig = substitute(signatures.at(s->value), sub, m.types);
      check_function_value_inout(r, *s, sig);
      result = {node(r, sig, FunctionValue{*s, args}), {sig, {}, {}}};
      break;
    }
    case AstKind::FunctionLiteral: {
      auto s = definition(r);
      auto sig = signatures.at(s.value);
      std::vector<TypeId> args;
      for (const auto &p : generic_params[s.value])
        args.push_back(parameter_type(p.symbol));
      check_function_value_inout(r, s, sig);
      result = {node(r, sig, FunctionValue{s, args}), {sig, {}, {}}};
      break;
    }
    default:
      unavailable(r, "this expression requires aggregate, ownership, or "
                     "external-call support");
    }
    match(r, result.value.type, expected);
    return result;
  }
  Expr place(Ref r, bool inout) {
    if (ast(r).kind == AstKind::Parenthesized)
      return place(child(r, 0), inout);
    if (ast(r).kind == AstKind::NeutralBracket) {
      if (inout)
        unavailable(r, "indexed inout is not supported");
      auto base = expression(child(r, 0));
      if (type(base.value.type).kind != TypeKind::Vec)
        reject(r, "index receiver must be Vec");
      if (ast(r).children.size() != 2)
        reject(r, "Vec requires one index");
      auto index = expression(child(r, 1));
      if (!integer(index.value.type))
        reject(r, "Vec index must be integer");
      auto t = type(base.value.type).type_args.at(0);
      return {node(r, t, IndexPlace{}, {base.id, index.id}), {t, {}, {}}};
    }
    if (ast(r).kind == AstKind::Member) {
      if (inout)
        unavailable(r, "field inout is not implemented");
      auto base = place(child(r, 0), false);
      if (m.nodes[base.id.value].kind() == NodeKind::index_place)
        unavailable(r, "indexed value-field writeback is not supported");
      auto f = find_field(r, base.value.type);
      auto t = field_type(base.value.type, f);
      return {node(r, t, FieldPlace{f.symbol}, {base.id}), {t, {}, {}}};
    }
    if (ast(r).kind != AstKind::Name)
      unavailable(
          r, "only a whole writable scalar binding can be used as a place");
    auto s = occurrence(r);
    if (!s || !locals.count(s->value))
      reject(r, "place is not a local binding",
             inout ? "E_CALL_ARGUMENT" : "E_TYPE_MISMATCH");
    auto l = locals.at(s->value);
    if (l.ordinary_parameter) {
      if (inout)
        reject(
            r,
            "an ordinary parameter is not a var or an exclusive inout binding",
            "E_CALL_ARGUMENT");
      reject(r,
             "ordinary parameters are read-only; declare a var local to "
             "change the value",
             "E_TYPE_MISMATCH");
    }
    if (!l.writable)
      reject(r, "let/const bindings are not writable",
             inout ? "E_CALL_ARGUMENT" : "E_TYPE_MISMATCH");
    if (inout && !aggregate_inout_type(l.type) &&
        !template_inout_type(l.type))
      unavailable(r, "inout requires a whole trivial numeric/bool or POD "
                     "aggregate binding");
    return {node(r, l.type, Place{*s}), {l.type, {}, {}}};
  }
  bool contains_parameter(TypeId id) const {
    std::vector<TypeId> pending{id};
    std::set<uint64_t> visited;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      if (!visited.insert(current.value).second)
        continue;
      const auto &t = type(current);
      if (t.kind == TypeKind::TypeParameter)
        return true;
      pending.insert(pending.end(), t.type_args.begin(), t.type_args.end());
      for (auto p : t.parameters)
        pending.push_back(p.type);
      if (t.kind == TypeKind::Function) {
        pending.push_back(t.result);
        if (t.throws_type)
          pending.push_back(*t.throws_type);
      }
    }
    return false;
  }
  std::optional<TypeId> type_expression(Ref r) {
    if (ast(r).kind == AstKind::Parenthesized)
      return type_expression(child(r, 0));
    auto base = r;
    if (ast(r).kind == AstKind::NeutralBracket)
      base = child(r, 0);
    auto s = occurrence(base);
    if (s && (symbol(*s).kind == modules::SymbolKind::type ||
              symbol(*s).kind == modules::SymbolKind::type_parameter ||
              symbol(*s).kind == modules::SymbolKind::builtin_type))
      return parse_type(r);
    return {};
  }
  FieldDecl find_field(Ref r, TypeId t) {
    if (type(t).kind != TypeKind::NominalStruct)
      reject(r, "field receiver is not a struct");
    auto name = file(r).tokens[ast(r).token_end - 1].spelling;
    for (const auto &f : nominal(t).fields)
      if (symbol(f.symbol).name == name) {
        if (symbol(f.symbol).module != r.module && !f.exported)
          reject(r, "field is private", "E_PRIVATE_ACCESS");
        return f;
      }
    reject(r, "unknown struct field");
  }
  void infer(Ref r, TypeId pattern, TypeId actual, Substitutions &sub) {
    auto p = type(pattern), a = type(actual);
    if (p.kind == TypeKind::TypeParameter) {
      for (auto [id, t] : sub)
        if (id == p.declaration) {
          if (t != actual)
            reject(r, "conflicting generic type inference");
          return;
        }
      sub.push_back({p.declaration, actual});
      return;
    }
    if (p.kind != a.kind || p.bits != a.bits ||
        p.declaration != a.declaration ||
        p.parameters.size() != a.parameters.size() ||
        p.type_args.size() != a.type_args.size() ||
        p.throws_type.has_value() != a.throws_type.has_value())
      reject(r, "type inference mismatch");
    for (size_t i = 0; i < p.type_args.size(); ++i)
      infer(r, p.type_args[i], a.type_args[i], sub);
    if (p.kind == TypeKind::Function) {
      for (size_t i = 0; i < p.parameters.size(); ++i) {
        if (p.parameters[i].mode != a.parameters[i].mode)
          reject(r, "function argument mode mismatch");
        infer(r, p.parameters[i].type, a.parameters[i].type, sub);
      }
      infer(r, p.result, a.result, sub);
      if (p.throws_type)
        infer(r, *p.throws_type, *a.throws_type, sub);
    }
  }
  bool try_infer(Ref r, TypeId pattern, TypeId actual, Substitutions &sub) {
    try {
      infer(r, pattern, actual, sub);
      return true;
    } catch (const Rejected &) {
      return false;
    }
  }
  std::vector<TypeId> finish_arguments(Ref r,
                                       const std::vector<GenericParam> &ps,
                                       Substitutions &sub) {
    std::vector<TypeId> args;
    for (const auto &p : ps) {
      auto it = std::find_if(sub.begin(), sub.end(),
                             [&](auto x) { return x.first == p.symbol; });
      if (it == sub.end())
        reject(r,
               "cannot infer type argument; provide explicit type arguments");
      args.push_back(it->second);
    }
    check_constraints(r, ps, args, &sub);
    return args;
  }
  bool satisfies(Ref r, TypeId actual, const TraitRef &trait) {
    auto key = trait_key(actual, trait);
    if (auto it = trait_results.find(key); it != trait_results.end())
      return it->second;
    if (constraint_depth >= 48 || !active_constraints.insert(key).second)
      return false;
    ++constraint_depth;
    const auto t = type(actual);
    size_t found = 0;
    if (t.kind == TypeKind::TypeParameter) {
      auto bound = bounds.find(t.declaration.value);
      found = bound != bounds.end() && bound->second &&
                      *bound->second == trait
                  ? 1
                  : 0;
    } else
      for (const auto &im : m.impls) {
        if (!im.trait || im.trait->declaration != trait.declaration ||
            im.trait->args.size() != trait.args.size())
          continue;
        // The impl's Self and every ordered trait argument must admit one
        // common substitution. A per-column match could accept
        // Match[X,bool] for Match[i32,Y] with inconsistent X.
        Substitutions sub;
        if (!try_infer(r, im.self_type, actual, sub))
          continue;
        bool matched = true;
        for (size_t a = 0; a < trait.args.size(); ++a)
          if (!try_infer(r, im.trait->args[a], trait.args[a], sub)) {
            matched = false;
            break;
          }
        if (!matched)
          continue;
        bool valid = true;
        for (auto p : im.params) {
          auto it = std::find_if(sub.begin(), sub.end(), [&](auto entry) {
            return entry.first == p.symbol;
          });
          if (it == sub.end()) {
            valid = false;
            break;
          }
          if (p.trait) {
            TraitRef bound = *p.trait;
            for (auto &arg : bound.args)
              arg = substitute(arg, sub, m.types);
            if (!satisfies(r, it->second, bound)) {
              valid = false;
              break;
            }
          }
        }
        if (valid)
          ++found;
      }
    --constraint_depth;
    active_constraints.erase(key);
    return trait_results[key] = found == 1;
  }
  // The declared bound may itself contain sibling template parameters, for
  // example [A, B, T Carry[A, B]]. Substitute the whole application through
  // the same table that produced the actual arguments before selecting an
  // impl; matching the raw A/B placeholders against concrete types would
  // reject every such declaration.
  void check_constraints(Ref r, const std::vector<GenericParam> &ps,
                         const std::vector<TypeId> &args,
                         const Substitutions *sub = nullptr) {
    // Bounds may intern substituted member types. Do not keep a reference
    // into the TypeContext across that call: the vector can reallocate while
    // the remaining requested arguments still need to be checked.
    const std::vector<TypeId> requested = args;
    for (size_t i = 0; i < ps.size(); ++i) {
      if (!ps[i].trait)
        continue;
      TraitRef bound = *ps[i].trait;
      if (sub)
        for (auto &arg : bound.args)
          arg = substitute(arg, *sub, m.types);
      check_trait_parameter_bounds(r, bound, requested.at(i));
      if (!satisfies(r, requested.at(i), bound))
        reject(r, "generic constraint requires one applicable explicit trait "
                  "implementation");
    }
  }
  struct Selection {
    SymbolId method;
    Substitutions substitutions;
  };
  Selection select_method(Ref r, TypeId receiver, std::string_view name,
                          std::optional<TraitRef> required = {}) {
    std::optional<Selection> result;
    for (const auto &im : m.impls) {
      if (required &&
          (!im.trait || im.trait->declaration != required->declaration))
        continue;
      Substitutions sub;
      if (!try_infer(r, im.self_type, receiver, sub))
        continue;
      if (required) {
        // A required application selects impls by Self and every trait
        // argument together. This is what makes phantom-only impl params and
        // same-Self applications distinguishable.
        if (im.trait->args.size() != required->args.size())
          continue;
        bool matched = true;
        for (size_t a = 0; a < required->args.size(); ++a)
          if (!try_infer(r, im.trait->args[a], required->args[a], sub)) {
            matched = false;
            break;
          }
        if (!matched)
          continue;
      }
      const auto saved_depth = constraint_depth;
      try {
        finish_arguments(r, im.params, sub);
      } catch (const Rejected &) {
        constraint_depth = saved_depth;
        continue;
      }
      for (auto method : im.methods)
        if (symbol(method).name == name) {
          // Trait methods use the trait's public interface. Inherent
          // methods have their own pub declaration.
          if (symbol(method).module != r.module &&
              !(im.trait ? symbol(im.trait->declaration).exported
                         : symbol(method).exported))
            reject(r, "method is private", "E_PRIVATE_ACCESS");
          if (result)
            reject(r, "ambiguous method implementation");
          result = Selection{method, sub};
        }
    }
    if (!result)
      reject(r, "no matching explicit method implementation");
    return *result;
  }
  Expr constructor(Ref r, TypeId target, std::optional<SymbolId> variant = {}) {
    const auto &d = nominal(target);
    std::vector<FieldDecl> fields = d.fields;
    if (variant) {
      for (const auto &v : d.variants)
        if (v.symbol == *variant)
          fields = v.payload;
    }
    if (ast(r).children.size() - 1 != fields.size())
      reject(r, "constructor requires every field exactly once",
             "E_CALL_ARGUMENT");
    std::vector<CallArg> args;
    std::vector<HirNodeId> children;
    std::vector<bool> seen(fields.size());
    bool named = false;
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto ar = child(r, i);
      std::optional<std::string> label;
      size_t pi = i - 1;
      if (token(ar, 1) == ":") {
        named = true;
        label = std::string(token(ar));
        pi = 0;
        while (pi < fields.size() && symbol(fields[pi].symbol).name != *label)
          ++pi;
      } else {
        if (!variant)
          reject(ar, "struct constructor requires named fields",
                 "E_CALL_ARGUMENT");
        if (named)
          reject(ar, "positional argument follows named argument",
                 "E_CALL_ARGUMENT");
      }
      if (pi == fields.size() || seen[pi])
        reject(ar, "unknown or duplicate constructor field", "E_CALL_ARGUMENT");
      if (!variant && symbol(fields[pi].symbol).module != r.module &&
          !fields[pi].exported)
        reject(ar, "field is private", "E_PRIVATE_ACCESS");
      seen[pi] = true;
      auto er = child(ar, 0);
      while (ast(er).kind == AstKind::Parenthesized)
        er = child(er, 0);
      if (ast(er).kind == AstKind::Unary &&
          (token(er) == "inout" || token(er) == "move"))
        reject(ar, "constructor fields require ordinary arguments",
               "E_CALL_ARGUMENT");
      auto e = expression(er, field_type(target, fields[pi]));
      args.push_back(
          {uint32_t(i - 1), uint32_t(pi), label, ParamMode::ordinary, e.id});
      children.push_back(e.id);
    }
    NodeData data = variant ? NodeData(EnumInit{*variant, args})
                            : NodeData(StructInit{args});
    return {node(r, target, std::move(data), std::move(children)),
            {target, {}, {}}};
  }
  std::vector<ForeignBindingId> foreign_members(TypeId owner, std::string_view name,
                                                       std::string_view kind) const {
    std::vector<ForeignBindingId> result;
    const auto &d = m.resolved->project->foreign_binding(type(owner).binding);
    for (auto id : std::get<modules::BindingType>(d.detail).members) {
      const auto &member = m.resolved->project->foreign_binding(id);
      const auto &c = std::get<modules::BindingCallable>(member.detail);
      if (c.call_kind == kind && (kind == "callable" || member.name == name))
        result.push_back(id);
    }
    return result;
  }
  Expr foreign_call(Ref r, Ref target, const std::vector<ForeignBindingId> &candidates,
                    std::optional<Expr> receiver = {}) {
    std::vector<std::optional<std::string>> labels;
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto ar = child(r, i);
      labels.push_back(token(ar, 1) == ":" ? std::optional(std::string(token(ar))) : std::nullopt);
    }
    auto build = [&](Checker &builder, ForeignBindingId binding,
                     const hir::detail::BoundArguments &bound) -> Expr {
      const auto b = builder.m.foreign_bindings.at(binding.value);
      const auto sig = builder.type(*b.signature);
      Call call; call.kind = CalleeKind::foreign; call.binding = binding;
      call.signature = *b.signature; call.omitted = bound.omitted;
      std::vector<HirNodeId> children, receiver_nodes;
      if (receiver) receiver_nodes.push_back(receiver->id);
      call.callee = builder.node(target, b.type, ForeignRead{binding}, receiver_nodes);
      children.push_back(*call.callee);
      for (size_t i = 0; i < labels.size(); ++i) {
        auto ar = builder.child(r, i + 1), er = builder.child(ar, 0);
        while (builder.ast(er).kind == AstKind::Parenthesized) er = builder.child(er, 0);
        if (builder.ast(er).kind == AstKind::Unary &&
            (builder.token(er) == "move" || builder.token(er) == "inout"))
          builder.reject(ar, "external arguments require ordinary mode", "E_CALL_ARGUMENT");
        auto pi = bound.parameters[i];
        if (sig.parameters[pi].mode != ParamMode::ordinary)
          builder.unavailable(ar, "external move/inout is not supported");
        auto e = builder.expression(er, sig.parameters[pi].type);
        call.args.push_back({uint32_t(i), pi, labels[i], ParamMode::ordinary, e.id});
        children.push_back(e.id);
      }
      auto id = builder.node(r, sig.result, std::move(call), std::move(children));
      return {id, {sig.result, {}, {}}};
    };
    if (candidates.size() == 1) {
      auto binding = candidates.front();
      const auto &decl = std::get<modules::BindingCallable>(m.resolved->project->foreign_binding(binding).detail);
      auto bound = hir::detail::bind_arguments(decl, labels);
      if (!bound) reject(r, "no external candidate accepts these arguments", "E_CALL_ARGUMENT");
      return build(*this, binding, *bound);
    }
    std::optional<ForeignBindingId> selected;
    std::optional<Rejected> failure;
    for (auto binding : candidates) {
      const auto &d = m.resolved->project->foreign_binding(binding);
      const auto &decl = std::get<modules::BindingCallable>(d.detail);
      auto bound = hir::detail::bind_arguments(decl, labels);
      if (!bound) continue;
      Checker trial(*this);
      trial.owned = std::make_shared<modules::ResolvedProject>(*owned);
      trial.m.resolved = trial.owned;
      try { build(trial, binding, *bound); }
      catch (const Rejected &e) { failure = e; continue; }
      if (selected) reject(r, "ambiguous external call", "E_CALL_ARGUMENT");
      selected = binding;
    }
    if (!selected) {
      if (failure) throw *failure;
      reject(r, "no external candidate accepts these arguments", "E_CALL_ARGUMENT");
    }
    const auto &decl = std::get<modules::BindingCallable>(m.resolved->project->foreign_binding(*selected).detail);
    return build(*this, *selected, *hir::detail::bind_arguments(decl, labels));
  }
  Expr call(Ref r, std::optional<TypeId> expected) {
    auto callee_ref = child(r, 0);
    if (auto sid = occurrence(callee_ref))
      if (auto ex = m.resolved->foreign_export(*sid); ex && !ex->candidates.empty())
        return foreign_call(r, callee_ref, ex->candidates);
    if (auto t = type_expression(callee_ref)) {
      if (type(*t).kind == TypeKind::NominalStruct)
        return constructor(r, *t);
      return scalar_call(r);
    }
    // Parentheses around an immediately-called target do not save or return
    // a method value. Remove only those transparent wrappers while keeping
    // the actual Member/Call source identity used by the HIR.
    auto transparent = [&](Ref value) {
      while (ast(value).kind == AstKind::Parenthesized)
        value = child(value, 0);
      return value;
    };
    auto suffix_ref = transparent(callee_ref);
    auto method_ref = suffix_ref;
    std::vector<Ref> method_arg_refs;
    std::vector<TypeId> method_args;
    bool method_explicit = false, method_args_parsed = false;
    if (ast(suffix_ref).kind == AstKind::NeutralBracket &&
        !ast(suffix_ref).children.empty()) {
      auto inner = transparent(child(suffix_ref, 0));
      if (ast(inner).kind == AstKind::Member && !occurrence(inner)) {
        // The suffix may still be an element index: a value receiver can
        // expose a Vec field with the same spelling as a method. Do not parse
        // any bracket item as a type until the receiver's real type proves
        // that this Member is a method rather than a field.
        method_explicit = true;
        for (size_t i = 1; i < ast(suffix_ref).children.size(); ++i)
          method_arg_refs.push_back(child(suffix_ref, i));
        method_ref = inner;
      }
    }
    auto parse_method_args = [&] {
      if (method_args_parsed)
        return;
      method_args_parsed = true;
      for (auto arg : method_arg_refs)
        method_args.push_back(parse_type(arg));
    };
    if (ast(method_ref).kind == AstKind::Member && !occurrence(method_ref)) {
      auto base = child(method_ref, 0);
      auto name = file(r).tokens[ast(method_ref).token_end - 1].spelling;
      auto method_subs = [&](Selection selected) {
        if (method_explicit) {
          parse_method_args();
          std::vector<GenericParam> unbound;
          for (auto p : generic_params[selected.method.value]) {
            bool bound = false;
            for (auto [s, t] : selected.substitutions)
              if (s == p.symbol)
                bound = true;
            if (!bound)
              unbound.push_back(p);
          }
          if (unbound.size() != method_args.size())
            reject(r, "wrong number of method type arguments");
          for (size_t i = 0; i < unbound.size(); ++i)
            selected.substitutions.push_back(
                {unbound[i].symbol, method_args[i]});
        }
        return selected;
      };
      auto explicit_method_sub = [&](SymbolId method) {
        Substitutions sub;
        if (!method_explicit)
          return sub;
        parse_method_args();
        auto ps = generic_params[method.value];
        if (method_args.size() != ps.size())
          reject(r, "wrong number of method type arguments");
        for (size_t i = 0; i < ps.size(); ++i)
          sub.push_back({ps[i].symbol, method_args[i]});
        return sub;
      };
      if (auto t = type_expression(base)) {
        if (type(*t).kind == TypeKind::TypeParameter) {
          auto bound = bounds.find(type(*t).declaration.value);
          if (bound == bounds.end() || !bound->second)
            reject(r, "method requires a declared trait constraint");
          const auto &application = *bound->second;
          if (const auto *tr = trait_decl(application.declaration))
            for (const auto &method : tr->methods)
              if (symbol(method.symbol).name == name) {
                if (!constraint_static_method_source(
                        method.symbol, application, *t) &&
                    !constraint_static_method_move_source(
                        method.symbol, application, *t))
                  unavailable(
                      r, "trait-static move/inout or Self callback parameters "
                         "are not implemented");
                // Replace the trait application and Self before the method's
                // own parameters are inferred. A phantom trait argument still
                // changes the signature's TypeParameter environment even when
                // it is absent from the textual signature.
                Substitutions sub{{tr->self, *t}};
                for (size_t i = 0; i < tr->params.size(); ++i)
                  sub.push_back(
                      {tr->params[i].symbol, application.args.at(i)});
                auto sig = substitute(method.signature, sub, m.types);
                return typed_call(r, method.symbol, sig,
                                  explicit_method_sub(method.symbol), {},
                                  expected, application, *t);
              }
          reject(r, "method is not declared by the constraint");
        }
        if (type(*t).kind == TypeKind::NominalEnum) {
          // A real variant keeps constructor priority. If no variant has this
          // name, the type-qualified member may still be an inherent method;
          // method selection below keeps the same impl/SymbolId rules.
          for (const auto &v : nominal(*t).variants)
            if (symbol(v.symbol).name == name) {
              if (method_explicit)
                reject(callee_ref, "enum variants do not take type arguments",
                       "E_CALL_ARGUMENT");
              return constructor(r, *t, v.symbol);
            }
        }
        auto selected = method_subs(select_method(r, *t, name));
        if (const auto *im = impl_of(selected.method);
            im && im->trait &&
            !trait_impl_inout_source(selected.method, false))
          unavailable(r, "trait-static actual self or method source "
                         "signature is not implemented");
        return typed_call(r, selected.method, {}, selected.substitutions, {},
                          expected);
      }
      auto receiver = expression(base);
      if (type(receiver.value.type).kind == TypeKind::ForeignOpaque) {
        if (method_explicit) unavailable(r, "external generic methods are not supported");
        auto candidates = foreign_members(receiver.value.type, name, "method");
        if (candidates.empty()) unavailable(r, "external member is not declared");
        return foreign_call(r, method_ref, candidates, receiver);
      }
      if (type(receiver.value.type).kind == TypeKind::NominalStruct)
        for (const auto &f : nominal(receiver.value.type).fields)
          if (symbol(f.symbol).name == name) {
            if (method_explicit) {
              auto field = find_field(method_ref, receiver.value.type);
              auto t = field_type(receiver.value.type, field);
              if (type(t).kind != TypeKind::Vec)
                reject(method_ref,
                       "function fields do not take type arguments",
                       "E_CALL_ARGUMENT");
              // A resolved field shadows same-named methods for value calls,
              // so this bracket is the field Vec's element index. Build the
              // callee from the already evaluated receiver and index once.
              if (ast(suffix_ref).children.size() != 2)
                reject(suffix_ref, "Vec requires one index");
              auto index = expression(child(suffix_ref, 1));
              if (!integer(index.value.type))
                reject(suffix_ref, "Vec index must be integer");
              auto element = type(t).type_args.at(0);
              Expr field_value{
                  node(method_ref, t, FieldRead{field.symbol}, {receiver.id}),
                  {t, {}, {}}};
              Expr callee{
                  node(suffix_ref, element, IndexRead{},
                       {field_value.id, index.id}),
                  {element, {}, {}}};
              return scalar_call(r, callee);
            }
            auto field = find_field(method_ref, receiver.value.type);
            auto t = field_type(receiver.value.type, field);
            Expr callee{
                node(method_ref, t, FieldRead{field.symbol}, {receiver.id}),
                {t, {}, {}}};
            return scalar_call(r, callee);
          }
      if (type(receiver.value.type).kind == TypeKind::TypeParameter) {
        auto bound = bounds[type(receiver.value.type).declaration.value];
        if (!bound)
          reject(r, "method requires a declared trait constraint");
        const auto &application = *bound;
        if (const auto *tr = trait_decl(application.declaration))
          for (const auto &method : tr->methods)
            if (symbol(method.symbol).name == name) {
              Substitutions sub{{tr->self, receiver.value.type}};
              for (size_t i = 0; i < tr->params.size(); ++i)
                sub.push_back({tr->params[i].symbol, application.args.at(i)});
              auto sig = substitute(method.signature, sub, m.types);
              return typed_call(r, method.symbol, sig,
                                explicit_method_sub(method.symbol), receiver,
                                expected, application);
            }
        reject(r, "method is not declared by the constraint");
      }
      auto selected = method_subs(select_method(r, receiver.value.type, name));
      return typed_call(r, selected.method, {}, selected.substitutions,
                        receiver, expected);
    }
    auto base = callee_ref;
    std::vector<TypeId> explicit_args;
    if (ast(base).kind == AstKind::NeutralBracket) {
      auto target = child(base, 0);
      auto target_symbol = occurrence(target);
      if (!target_symbol ||
          symbol(*target_symbol).kind != modules::SymbolKind::function)
        return scalar_call(r);
      for (size_t i = 1; i < ast(base).children.size(); ++i)
        explicit_args.push_back(parse_type(child(base, i)));
      base = target;
    }
    auto s = occurrence(base);
    if (s && symbol(*s).kind == modules::SymbolKind::function &&
        signatures.count(s->value)) {
      if (generic_params[s->value].empty() &&
          ast(callee_ref).kind != AstKind::NeutralBracket)
        return scalar_call(r);
      Substitutions sub;
      auto ps = generic_params[s->value];
      if (ast(callee_ref).kind == AstKind::NeutralBracket) {
        if (explicit_args.size() != ps.size())
          reject(r, "wrong number of generic arguments");
        for (size_t i = 0; i < ps.size(); ++i)
          sub.push_back({ps[i].symbol, explicit_args[i]});
      }
      return typed_call(r, *s, {}, sub, {}, expected);
    }
    return scalar_call(r);
  }
  void check_call_borrows(Ref r, const std::vector<CallArg> &args,
                          const std::vector<HirNodeId> &children) const {
    // In the supported scalar subset each binding identifies its entire
    // storage. Include nested reads/places in arguments and the callee.
    std::set<uint64_t> borrowed;
    for (const auto &a : args)
      if (a.mode == ParamMode::inout) {
        auto id =
            std::get<Place>(m.nodes[a.expression.value].data).symbol.value;
        if (!borrowed.insert(id).second)
          reject(r, "same binding borrowed by inout twice", "E_CALL_ARGUMENT");
      }
    for (auto root : children) {
      std::vector<HirNodeId> todo{root};
      while (!todo.empty()) {
        auto id = todo.back();
        todo.pop_back();
        const auto &n = m.nodes[id.value];
        if (auto p = std::get_if<Read>(&n.data);
            p && borrowed.count(p->symbol.value))
          reject(r, "inout binding is also read by this call",
                 "E_CALL_ARGUMENT");
        if (auto p = std::get_if<Move>(&n.data);
            p && p->binding && borrowed.count(p->binding->value))
          reject(r, "inout binding is also moved by this call",
                 "E_CALL_ARGUMENT");
        if (auto p = std::get_if<Place>(&n.data);
            p && borrowed.count(p->symbol.value)) {
          bool top = false;
          for (auto &a : args)
            if (a.mode == ParamMode::inout && a.expression == id)
              top = true;
          if (!top)
            reject(r, "nested call aliases an inout argument",
                   "E_CALL_ARGUMENT");
        }
        todo.insert(todo.end(), n.children.begin(), n.children.end());
      }
    }
  }
  Expr typed_call(Ref r, SymbolId target,
                  std::optional<TypeId> signature_override, Substitutions sub,
                  std::optional<Expr> receiver, std::optional<TypeId> expected,
                  std::optional<TraitRef> constraint = {},
                  std::optional<TypeId> constraint_type = {}) {
    auto original = signature_override.value_or(signatures.at(target.value));
    // Copy: substitution below can intern new types and reallocate the
    // TypeContext while this signature is still inspected.
    auto original_type = type(original);
    // A value-form call always binds parameter zero as its synthetic
    // receiver. Reject an actual move/inout Self before arity or mode
    // processing, so it remains an explicit capability limit rather than a
    // misleading argument-count/type diagnostic. Type/T.method(...) has no
    // synthetic receiver and supplies this first parameter explicitly.
    if (receiver && !original_type.parameters.empty() &&
        original_type.parameters.front().mode != ParamMode::ordinary)
      unavailable(r, "move/inout self is not implemented");
    if (receiver && !original_type.parameters.empty() &&
        original_type.parameters.front().mode == ParamMode::ordinary) {
      const auto *im = impl_of(target);
      if ((constraint &&
           !constraint_method_move_source(target, *constraint,
                                          receiver->value.type)) ||
          (!constraint && im && im->trait &&
           !trait_impl_move_source(target, true)))
        reject(r, "value call requires an ordinary trait Self first parameter",
               "E_CALL_ARGUMENT");
    }
    if (expected)
      infer(r, original_type.result, *expected, sub);
    const auto ps = parameters.at(target.value);
    size_t offset = receiver ? 1 : 0;
    if (ast(r).children.size() - 1 + offset != ps.size())
      reject(r, "wrong number of arguments", "E_CALL_ARGUMENT");
    std::vector<CallArg> args;
    std::vector<HirNodeId> children;
    std::vector<bool> seen(ps.size());
    std::vector<std::optional<Expr>> values(ast(r).children.size() - 1);
    std::vector<size_t> indices(values.size());
    std::vector<std::optional<std::string>> labels(values.size());
    std::vector<ParamMode> modes(values.size());
    if (receiver) {
      infer(r, original_type.parameters.at(0).type, receiver->value.type, sub);
      args.push_back({0, 0, {}, ParamMode::ordinary, receiver->id});
      children.push_back(receiver->id);
      seen[0] = true;
    }
    for (size_t i = 0; i < values.size(); ++i) {
      auto ar = child(r, i + 1);
      size_t pi = i + offset;
      if (token(ar, 1) == ":") {
        labels[i] = std::string(token(ar));
        pi = 0;
        while (pi < ps.size() && ps[pi].label != *labels[i])
          ++pi;
      }
      if (pi >= ps.size() || seen[pi])
        reject(ar, "unknown or duplicate argument", "E_CALL_ARGUMENT");
      seen[pi] = true;
      indices[i] = pi;
    }
    // Concrete move and non-self scalar inout on a generic free function are
    // independent of its trait constraints; finish_arguments below still
    // requires the explicit impl. The inherent-method route accepts declared
    // constraints on either layer for the same reason: finish_arguments checks
    // the actual merged impl/own arguments, not merely the declarations.
    // A concrete receiver may likewise use a real trait impl method, and a
    // bounded template receiver may use a real trait prototype, but only when
    // the source helper above has proved the actual prototype/self relation.
    // Actual self modes are source-classified below; a type-qualified call
    // supplies the first parameter explicitly and may use move/inout.
    const bool free_template =
        !receiver && !constraint && free_generic_template(target);
    const bool free_move = free_template;
    const bool free_inout = free_template;
    const bool inherent =
        !constraint &&
        inherent_method_route(target, receiver.has_value(), original);
    // A type-qualified inherent call has no synthetic receiver and may pass
    // the declared first self parameter explicitly. This also admits later
    // move/inout parameters for that same source method; a literal declared
    // self still follows declared_self, while a non-literal first parameter
    // was already an explicit static parameter.
    const bool inherent_static =
        !constraint && !receiver && inherent_method_source(target);
    // Move and inout eligibility use separate source helpers. Both admit an
    // ordinary receiver named self or owner and an explicit Type/T parameter
    // list. Complete callback data is checked after type substitution.
    const bool trait_impl_inout =
        !constraint &&
        trait_impl_inout_source(target, receiver.has_value()).has_value();
    const bool trait_impl_move =
        !constraint &&
        trait_impl_move_source(target, receiver.has_value()).has_value();
    std::optional<TypeId> constraint_receiver;
    if (constraint) {
      if (constraint_type)
        constraint_receiver = *constraint_type;
      else if (receiver)
        constraint_receiver = receiver->value.type;
      // The constrained receiver T is fixed by the enclosing template, not a
      // variable to infer from an argument. Pre-seeding makes a concrete
      // argument conflict at source instead of silently substituting T.
      if (constraint_receiver &&
          type(*constraint_receiver).kind == TypeKind::TypeParameter)
        sub.push_back({type(*constraint_receiver).declaration,
                       *constraint_receiver});
    }
    const bool trait_constraint_inout =
        constraint && constraint_receiver &&
        (receiver ? constraint_method_source(target, *constraint,
                                             *constraint_receiver)
                  : constraint_static_method_source(target, *constraint,
                                                    *constraint_receiver));
    const bool trait_constraint_move =
        constraint && constraint_receiver &&
        (receiver ? constraint_method_move_source(target, *constraint,
                                                  *constraint_receiver)
                  : constraint_static_method_move_source(
                        target, *constraint, *constraint_receiver));
    const bool allow_move =
        free_move || inherent || inherent_static || trait_impl_move ||
        trait_constraint_move;
    const bool allow_inout =
        free_inout || inherent || inherent_static || trait_impl_inout ||
        trait_constraint_inout;
    // A value-form actual-self mode was rejected above, before arity. An own
    // type parameter that happens to be named self, or later concretizes to
    // Self, is not an actual self; a type-qualified call passes parameter zero
    // explicitly and uses the same ordinary/move/inout checks below.
    for (size_t pass = 0; pass < 2; ++pass)
      for (size_t i = 0; i < values.size(); ++i) {
        auto ar = child(r, i + 1), er = child(ar, 0);
        if (values[i] || (pass == 0 && contextual(er)))
          continue;
        auto pt =
            substitute(original_type.parameters[indices[i]].type, sub, m.types);
        const auto parameter_mode =
            original_type.parameters[indices[i]].mode;
        const auto argument = argument_mode(ar);
        er = argument.value;
        auto pm = argument.requested;
        if (parameter_mode == ParamMode::move && allow_move) {
          if (pm == ParamMode::ordinary) {
            auto tmp = er;
            while (ast(tmp).kind == AstKind::Parenthesized)
              tmp = child(tmp, 0);
            if (ast(tmp).kind == AstKind::Name ||
                ast(tmp).kind == AstKind::Member ||
                ast(tmp).kind == AstKind::NeutralBracket)
              reject(ar, "existing binding requires explicit move",
                     "E_CALL_ARGUMENT");
            pm = ParamMode::move;
          }
        } else if (parameter_mode != ParamMode::inout || !allow_inout) {
          if (parameter_mode != ParamMode::ordinary)
            unavailable(
                ar, "generic/method inout and move calls are not implemented");
        }
        if (parameter_mode == ParamMode::ordinary && argument.has_marker)
          reject(argument.marked,
                 "generic/method argument mode differs from ordinary parameter",
                 "E_CALL_ARGUMENT");
        if (pm != parameter_mode)
          reject(ar, "argument mode differs from parameter mode",
                 "E_CALL_ARGUMENT");
        if (argument.has_marker && argument.requested == ParamMode::move)
          er = argument.marked;
        modes[i] = pm;

        auto e = pm == ParamMode::inout
                     ? place(er, true)
                     : expression(er, contains_parameter(pt)
                                          ? std::nullopt
                                          : std::optional(pt));
        infer(ar, original_type.parameters[indices[i]].type, e.value.type, sub);
        values[i] = e;
      }
    // The constrained receiver, Self and every ordered trait argument belong
    // to the environment in which method-own bounds are satisfied. Seed them
    // before inference completes the own suffix, including phantom parameters.
    if (constraint) {
      const auto *tr = trait_decl(constraint->declaration);
      if (tr && constraint_receiver) {
        auto bind = [&](SymbolId symbol, TypeId value) {
          auto it = std::find_if(sub.begin(), sub.end(), [&](auto entry) {
            return entry.first == symbol;
          });
          if (it == sub.end())
            sub.push_back({symbol, value});
          else
            it->second = value;
        };
        bind(tr->self, *constraint_receiver);
        for (size_t i = 0; i < tr->params.size(); ++i)
          bind(tr->params[i].symbol, constraint->args.at(i));
      }
    }
    std::vector<TypeId> type_args;
    if (constraint)
      (void)finish_arguments(r, generic_params[target.value], sub);
    else
      type_args = finish_arguments(r, generic_params[target.value], sub);
    auto sig = substitute(original, sub, m.types);
    check_inout_graph(r, sig, true);
    auto ft = type(sig);
    for (size_t i = 0; i < values.size(); ++i) {
      auto e = *values[i];
      match(r, e.value.type, ft.parameters[indices[i]].type);
      args.push_back({uint32_t(i + offset), uint32_t(indices[i]), labels[i],
                      modes[i], e.id});
      children.push_back(e.id);
    }
    check_call_borrows(r, args, children);
    if (constraint)
      return {node(r, ft.result,
                   ConstraintCall{*constraint, target, *constraint_receiver,
                                  sig, args},
                   children),
              {ft.result, {}, {}}};
    Call c;
    c.kind = CalleeKind::direct;
    c.signature = sig;
    c.direct_symbol = target;
    c.args = std::move(args);
    c.type_args = std::move(type_args);
    return {node(r, ft.result, std::move(c), children), {ft.result, {}, {}}};
  }

  Expr scalar_call(Ref r, std::optional<Expr> checked_callee = {}) {
    auto callee_ref = child(r, 0);
    auto s = occurrence(callee_ref);
    if (s && symbol(*s).kind == modules::SymbolKind::builtin_type) {
      auto target = parse_type(callee_ref);
      const auto target_kind = type(target).kind;
      if (!integer(target) && target_kind != TypeKind::Float)
        unavailable(r,
                    "only explicit integer/float numeric conversions are "
                    "implemented");
      if (ast(r).children.size() != 2)
        reject(r, "conversion requires one argument", "E_CALL_ARGUMENT");
      auto arg = child(r, 1);
      if (ast(arg).token_begin + 1 < ast(arg).token_end && token(arg, 1) == ":")
        reject(arg, "conversion does not accept labels", "E_CALL_ARGUMENT");
      auto xr = child(arg, 0);
      // Only a pure integer literal may take an integer destination as its
      // context. A floating operand is never parsed as an integer, and an
      // integer source in an integer-to-float conversion keeps its own type.
      auto x = expression(xr, integer(target) && literal_integer(xr)
                                  ? std::optional(target)
                                  : std::nullopt);
      const bool source_integer = integer(x.value.type);
      const bool source_float = type(x.value.type).kind == TypeKind::Float;
      if (!source_integer && !source_float)
        reject(arg, "conversion operand must be an integer or float");
      if (source_float && target_kind == TypeKind::Float)
        unavailable(r, "float-to-float conversions are not implemented");
      Value v;
      v.type = target;
      if (source_float) {
        if (x.value.floating) {
          // Truncate towards zero from the already-rounded IEEE value, then
          // check the mathematical integer range. Only the magnitude is
          // shifted; the sign is applied afterwards, so -0.9 -> u8 truncates
          // to 0 instead of -1.
          const auto ieee = decode_ieee(*x.value.floating, width(x.value.type));
          if (ieee.kind != Ieee::Kind::finite)
            reject(arg,
                   "conversion of a NaN or infinity constant is a source "
                   "error");
          mpz_class truncated = ieee.significand;
          if (ieee.shift >= 0)
            truncated <<= ieee.shift;
          else
            truncated >>= -ieee.shift;
          if (ieee.negative)
            truncated = -truncated;
          range(arg, truncated, target);
          v.integer = std::move(truncated);
        }
      } else if (target_kind == TypeKind::Float) {
        // The source integer is rounded once, directly to the destination
        // precision. No decimal/host-double step is involved.
        if (x.value.integer) {
          mpz_class magnitude = *x.value.integer < 0 ? mpz_class(-*x.value.integer)
                                                     : *x.value.integer;
          v.floating = round_ratio(*x.value.integer < 0, magnitude,
                                   mpz_class(1), target);
        }
      } else if (x.value.integer) {
        range(arg, *x.value.integer, target);
        v.integer = *x.value.integer;
      }
      return {node(r, target, IntegerConvert{}, {x.id}), v};
    }
    Call c;
    std::vector<HirNodeId> children;
    if (s && symbol(*s).kind == modules::SymbolKind::function &&
        signatures.count(s->value)) {
      c.kind = CalleeKind::direct;
      c.direct_symbol = s;
      c.signature = signatures.at(s->value);
    } else {
      auto callee = checked_callee ? *checked_callee : expression(callee_ref);
      if (type(callee.value.type).kind == TypeKind::ForeignOpaque) {
        auto candidates = foreign_members(callee.value.type, "", "callable");
        if (candidates.empty()) reject(r, "external object is not callable", "E_CALL_ARGUMENT");
        return foreign_call(r, callee_ref, candidates, callee);
      }
      if (type(callee.value.type).kind != TypeKind::Function)
        reject(callee_ref, "callee is not a function", "E_CALL_ARGUMENT");
      c.kind = CalleeKind::function_value;
      c.callee = callee.id;
      c.signature = callee.value.type;
      children.push_back(callee.id);
    }
    const auto sig = type(c.signature);
    std::vector<bool> seen(sig.parameters.size());
    if (ast(r).children.size() - 1 != seen.size())
      reject(r, "wrong number of call arguments", "E_CALL_ARGUMENT");
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto ar = child(r, i);
      auto er = child(ar, 0);
      size_t pi = i - 1;
      std::optional<std::string> label;
      if (token(ar, 1) == ":") {
        if (!c.direct_symbol)
          reject(ar, "function values do not preserve parameter labels",
                 "E_CALL_ARGUMENT");
        label = std::string(token(ar));
        const auto &ps = parameters.at(c.direct_symbol->value);
        auto found = std::find_if(ps.begin(), ps.end(), [&](const auto &p) {
          return p.label == *label;
        });
        if (found == ps.end())
          reject(ar, "unknown argument label", "E_CALL_ARGUMENT");
        pi = size_t(found - ps.begin());
      }
      if (pi >= seen.size() || seen[pi])
        reject(ar, "parameter provided more than once", "E_CALL_ARGUMENT");
      seen[pi] = true;
      while (ast(er).kind == AstKind::Parenthesized)
        er = child(er, 0);
      const auto argument = argument_mode(ar);
      er = argument.value;
      auto pm = argument.requested;
      if (sig.parameters[pi].mode == ParamMode::move &&
          pm == ParamMode::ordinary) {
        auto tmp = er;
        while (ast(tmp).kind == AstKind::Parenthesized)
          tmp = child(tmp, 0);
        if (ast(tmp).kind == AstKind::Name ||
            ast(tmp).kind == AstKind::Member ||
            ast(tmp).kind == AstKind::NeutralBracket)
          reject(ar, "existing binding requires explicit move",
                 "E_CALL_ARGUMENT");
        pm = ParamMode::move;
      }
      if (pm != sig.parameters[pi].mode)
        reject(ar, "argument mode differs from parameter mode",
               "E_CALL_ARGUMENT");
      if (argument.has_marker && argument.requested == ParamMode::move)
        er = argument.marked;
      auto e = pm == ParamMode::inout ? place(er, true)
                                      : expression(er, sig.parameters[pi].type);
      match(ar, e.value.type, sig.parameters[pi].type);
      c.args.push_back({uint32_t(i - 1), uint32_t(pi), label, pm, e.id});
      children.push_back(e.id);
    }
    check_call_borrows(r, c.args, children);
    return {node(r, sig.result, std::move(c), std::move(children)),
            {sig.result, {}, {}}};
  }
  Constant constant(SymbolId s) {
    if (auto it = constants.find(s.value); it != constants.end())
      return it->second;
    if (!evaluating.insert(s.value).second)
      reject(*symbol(s).definition, "constant cycle", "E_CONSTANT_CYCLE");
    auto r = *symbol(s).definition;
    // Temporarily emitted evaluation nodes are discarded; constants are
    // materialized at their use sites with the use's Source/Scope.
    auto begin = m.nodes.size();
    std::optional<TypeId> expected;
    if (ast(r).children.size() > 1)
      expected = parse_type(child(r, 0));
    auto e = expression(child(r, ast(r).children.size() - 1), expected);
    Constant c{e.value, {}};
    if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data)) {
      c.function = f->symbol;
      c.type_args = f->type_args;
    }
    if (!c.value.integer && !c.value.floating && !c.value.boolean &&
        !c.value.string && !c.function)
      unavailable(
          r, "const initializer must be a non-Panic scalar compile-time value");
    m.nodes.resize(begin);
    evaluating.erase(s.value);
    constants[s.value] = c;
    return c;
  }
  ScopeId new_scope(Ref r, std::optional<ScopeId> parent) {
    ScopeId id{m.scopes.size()};
    m.scopes.push_back({function, parent, source(r)});
    return id;
  }
  struct Flow {
    bool falls = true;
    std::set<uint64_t> errors, breaks, continues;
  };
  // This is reachability analysis, separate from the conservative typed effect
  // records. A template handler remains possible if finite substitution can
  // make its error type equal to an error produced by the protected body.
  Flow flow(HirNodeId id, size_t depth = 0) const {
    check_cancelled(stop);
    if (depth > 512)
      throw std::invalid_argument("flow nesting limit");
    const auto &n = m.nodes.at(id.value);
    Flow out;
    auto part = [&](HirNodeId ch) { return flow(ch, depth + 1); };
    auto merge = [&](const Flow &f) {
      out.errors.insert(f.errors.begin(), f.errors.end());
      out.breaks.insert(f.breaks.begin(), f.breaks.end());
      out.continues.insert(f.continues.begin(), f.continues.end());
    };
    switch (n.kind()) {
    case NodeKind::defer_:
      return out; // registration itself cannot throw or leave this block
    case NodeKind::short_circuit: {
      merge(part(n.children[0]));
      const auto *known =
          std::get_if<BoolLiteral>(&m.nodes.at(n.children[0].value).data);
      const auto op = std::get<ShortCircuit>(n.data).op;
      if (!known || (op == LogicalOp::and_ ? known->value : !known->value))
        merge(part(n.children[1]));
      return out;
    }
    case NodeKind::block:
      for (auto ch : n.children) {
        if (!out.falls)
          break;
        auto f = part(ch);
        merge(f);
        out.falls = f.falls;
      }
      return out;
    case NodeKind::if_: {
      merge(part(n.children[0]));
      const auto *known =
          std::get_if<BoolLiteral>(&m.nodes.at(n.children[0].value).data);
      out.falls = !known && n.children.size() == 2;
      for (size_t i = 1; i < n.children.size(); ++i) {
        if (known && (known->value ? i != 1 : i != 2))
          continue;
        auto f = part(n.children[i]);
        merge(f);
        out.falls |= f.falls;
      }
      if (known && !known->value && n.children.size() == 2)
        out.falls = true;
      return out;
    }
    case NodeKind::try_: {
      const auto body = part(n.children[0]);
      out = body;
      const auto &catches = std::get<Try>(n.data).catches;
      for (const auto &c : catches)
        out.errors.erase(c.error_type.value);
      for (const auto &c : catches) {
        bool reachable = false;
        for (auto e : body.errors)
          reachable |= type_patterns_overlap(TypeId{e}, c.error_type, m.types);
        if (!reachable)
          continue;
        auto handler = part(c.body);
        merge(handler);
        out.falls |= handler.falls;
      }
      return out;
    }
    case NodeKind::switch_:
      merge(part(n.children[0]));
      out.falls = false;
      for (size_t i = 1; i < n.children.size(); ++i) {
        auto arm = part(n.children[i]);
        merge(arm);
        out.falls |= arm.falls || arm.breaks.contains(id.value);
      }
      out.breaks.erase(id.value);
      return out;
    case NodeKind::for_: {
      merge(part(n.children[0]));
      merge(part(n.children[1]));
      const auto *known =
          std::get_if<BoolLiteral>(&m.nodes.at(n.children[1].value).data);
      if (known && !known->value)
        return out;
      const auto body = part(n.children[3]);
      merge(body);
      if (body.falls || body.continues.contains(id.value))
        merge(part(n.children[2]));
      out.falls = !known || body.breaks.contains(id.value);
      out.breaks.erase(id.value);
      out.continues.erase(id.value);
      return out;
    }
    default:
      for (auto ch : n.children)
        merge(part(ch));
      if (auto c = std::get_if<Call>(&n.data))
        if (auto e = type(c->signature).throws_type)
          out.errors.insert(e->value);
      if (auto c = std::get_if<ConstraintCall>(&n.data))
        if (auto e = type(c->signature).throws_type)
          out.errors.insert(e->value);
      if (auto r = std::get_if<ForeignRead>(&n.data))
        out.errors.insert(m.foreign_bindings.at(r->binding.value).read_error.value);
      if (n.kind() == NodeKind::throw_) {
        out.errors.insert(m.nodes.at(n.children[0].value).type.value);
        out.falls = false;
      }
      if (n.kind() == NodeKind::return_)
        out.falls = false;
      if (auto b = std::get_if<Break>(&n.data)) {
        out.breaks.insert(b->loop.value);
        out.falls = false;
      }
      if (auto c = std::get_if<Continue>(&n.data)) {
        out.continues.insert(c->loop.value);
        out.falls = false;
      }
      return out;
    }
  }
  struct Statement {
    HirNodeId id;
    bool falls = true;
    bool breaks = false;
  };
  Statement block(Ref r, bool outer = false) {
    auto saved = scope;
    if (!outer)
      scope = new_scope(r, saved);
    std::vector<HirNodeId> children;
    bool falls = true, breaks = false;
    for (auto c : ast(r).children) {
      auto s = statement(at(r, c));
      children.push_back(s.id);
      if (falls) {
        breaks = breaks || s.breaks;
        falls = s.falls;
      }
    }
    auto id = node(r, unit, Block{}, std::move(children));
    scope = saved;
    return {id, falls, breaks};
  }
  Pattern pattern(Ref r, TypeId t) {
    Pattern p{t, source(r), {}, {}, {}};
    if (ast(r).kind == AstKind::BindPattern) {
      if (token(r) != "_") {
        auto s = definition(r);
        p.binding = s;
        locals[s.value] = {t, false};
      }
      return p;
    }
    if (ast(r).kind == AstKind::LiteralPattern) {
      try {
        p.literal = decode_pattern_literal(*m.resolved->project, m.types, r, t,
                                           m.config);
      } catch (const LiteralError &e) {
        reject(r, e.what());
      }
      return p;
    }
    if (ast(r).kind != AstKind::VariantPattern)
      unavailable(
          r,
          "only literal, enum variant, binding and wildcard patterns are implemented");
    if (type(t).kind != TypeKind::NominalEnum)
      reject(r, "variant pattern requires an enum");
    auto base = occurrence(r);
    if (!base || *base != type(t).declaration)
      reject(r, "pattern belongs to a different enum");
    std::string_view name;
    unsigned depth = 0;
    for (size_t i = ast(r).token_begin; i < ast(r).token_end; ++i) {
      auto tok = file(r).tokens[i].spelling;
      if (tok == "(" && !depth)
        break;
      if (tok == "[")
        ++depth;
      if (tok == "]")
        --depth;
      if (tok == "." && !depth)
        name = file(r).tokens[i + 1].spelling;
    }
    const VariantDecl *v = nullptr;
    for (const auto &candidate : nominal(t).variants)
      if (symbol(candidate.symbol).name == name)
        v = &candidate;
    if (!v)
      reject(r, "unknown enum pattern variant");
    p.variant = v->symbol;
    size_t pi = 0, ti = 0;
    for (auto id : ast(r).children) {
      auto cr = at(r, id);
      if (ast(cr).kind == AstKind::NamedType ||
          ast(cr).kind == AstKind::FunctionType ||
          ast(cr).kind == AstKind::ParenthesizedType) {
        if (ti >= type(t).type_args.size() ||
            parse_type(cr) != type(t).type_args[ti++])
          reject(cr, "enum pattern type arguments mismatch");
        continue;
      }
      if (pi >= v->payload.size())
        reject(r, "wrong variant pattern payload count");
      p.payload.push_back(pattern(cr, field_type(t, v->payload[pi++])));
    }
    if (pi != v->payload.size())
      reject(r, "wrong variant pattern payload count");
    return p;
  }
  Statement switch_statement(Ref r) {
    auto value = expression(child(r, 0));
    auto id = node(r, unit, Switch{});
    PatternCoverage coverage(
        value.value.type,
        [this](TypeId t) { return pattern_domain(m, t, m.types); },
        stop);
    break_targets.push_back(id);
    std::vector<HirNodeId> children{value.id};
    std::vector<Arm> arms;
    bool falls = false;
    for (size_t i = 1; i < ast(r).children.size(); ++i) {
      auto cr = child(r, i);
      auto saved = scope;
      scope = new_scope(cr, saved);
      bool def = token(cr) == "default";
      Pattern p = def ? Pattern{value.value.type, source(cr), {}, {}, {}}
                      : pattern(child(cr, 0), value.value.type);
      bool useful = false;
      try {
        useful = coverage.add(p);
      } catch (const PatternAnalysisLimit &e) {
        unavailable(cr, std::string("cannot complete switch pattern coverage: ") +
                            e.what());
      }
      if (!useful)
        reject(cr, "unreachable switch arm");
      std::vector<HirNodeId> statements;
      bool arm_falls = true, arm_breaks = false;
      for (size_t j = def ? 0 : 1; j < ast(cr).children.size(); ++j) {
        auto st = statement(child(cr, j));
        statements.push_back(st.id);
        if (arm_falls) {
          arm_breaks |= st.breaks;
          arm_falls = st.falls;
        }
      }
      auto body = node(cr, unit, Block{}, statements);
      arms.push_back({std::move(p), scope, body, source(cr)});
      children.push_back(body);
      scope = saved;
      falls |= arm_falls || arm_breaks;
    }
    bool exhaustive = false;
    try {
      exhaustive = coverage.exhaustive();
    } catch (const PatternAnalysisLimit &e) {
      unavailable(r, std::string("cannot complete switch pattern coverage: ") +
                          e.what());
    }
    if (!exhaustive)
      reject(r, "switch is not exhaustive");
    m.nodes[id.value].children = std::move(children);
    m.nodes[id.value].data = Switch{std::move(arms), exhaustive};
    break_targets.pop_back();
    return {id, falls, false};
  }
  Statement statement(Ref r) {
    check_cancelled(stop);
    const auto &n = ast(r);
    switch (n.kind) {
    case AstKind::Block:
      return block(r);
    case AstKind::Unsafe:
      if (n.children.size() != 1 ||
          ast(child(r, 0)).kind != AstKind::Block)
        unavailable(r, "unsafe requires exactly one lexical block");
      return block(child(r, 0));
    case AstKind::Switch:
      return switch_statement(r);
    case AstKind::Binding: {
      auto s = definition(r);
      std::optional<TypeId> expected;
      if (n.children.size() > 1)
        expected = parse_type(child(r, 0));
      auto e = expression(child(r, n.children.size() - 1), expected);
      bool writable = first(r) == "var";
      if (first(r) == "const" && !e.value.integer && !e.value.floating &&
          !e.value.boolean && !e.value.string &&
          !std::holds_alternative<FunctionValue>(m.nodes[e.id.value].data))
        unavailable(
            r,
            "const initializer must be a non-Panic scalar compile-time value");
      locals[s.value] = {e.value.type, writable};
      if (first(r) == "const") {
        Constant c{e.value, {}};
        if (auto f = std::get_if<FunctionValue>(&m.nodes[e.id.value].data)) {
          c.function = f->symbol;
          c.type_args = f->type_args;
        }
        constants[s.value] = c;
      }
      return {node(r, unit, Binding{s, writable}, {e.id})};
    }
    case AstKind::Assign: {
      auto p = place(child(r, 0), false);
      auto op = binary_token(r);
      bool shift = op == "<<=" || op == ">>=";
      auto rhs = expression(child(r, 1),
                            shift ? std::nullopt : std::optional(p.value.type));
      std::optional<BinaryOp> compound;
      if (op != "=") {
        compound = binary_op(op.substr(0, op.size() - 1));
        bool floating =
            type(p.value.type).kind == TypeKind::Float &&
            (*compound == BinaryOp::add || *compound == BinaryOp::subtract ||
             *compound == BinaryOp::multiply || *compound == BinaryOp::divide);
        if (!floating && (!integer(p.value.type) || !integer(rhs.value.type)))
          reject(r, "compound assignment requires integer operands");
        if (!shift)
          match(r, rhs.value.type, p.value.type);
      }
      return {node(r, unit, Assign{compound}, {p.id, rhs.id})};
    }
    case AstKind::ExpressionStatement: {
      auto e = expression(child(r, 0));
      return {node(r, unit, ExpressionStatement{}, {e.id})};
    }
    case AstKind::Try: {
      const auto id = node(r, unit, Try{});
      std::vector<Catch> catches;
      std::set<uint64_t> types;
      // Reserve the Try identity before checking its protected body.
      for (size_t i = 1; i < n.children.size(); ++i) {
        auto cr = child(r, i);
        auto t = parse_type(child(cr, 0));
        if (!types.insert(t.value).second)
          unavailable(cr, "duplicate catch types are not implemented");
        catches.push_back(
            {t, definition(cr), ScopeId{0}, HirNodeId{0}, source(cr)});
      }
      auto body = block(child(r, 0));
      std::vector<HirNodeId> children{body.id};
      for (size_t i = 0; i < catches.size(); ++i) {
        auto cr = child(r, i + 1);
        auto saved = scope;
        scope = new_scope(cr, saved);
        locals[catches[i].binding.value] = {catches[i].error_type, false};
        auto handler = block(child(cr, 1), true);
        catches[i].scope = scope;
        catches[i].body = handler.id;
        children.push_back(handler.id);
        scope = saved;
      }
      m.nodes[id.value].children = std::move(children);
      m.nodes[id.value].data = Try{std::move(catches)};
      const auto summary = flow(id);
      return {id, summary.falls, !summary.breaks.empty()};
    }
    case AstKind::Defer: {
      auto owner = scope;
      ++cleanup_depth;
      auto body = block(child(r, 0));
      --cleanup_depth;
      return {node(r, unit, Defer{owner, {}}, {body.id})};
    }
    case AstKind::Throw: {
      auto e = expression(child(r, 0));
      return {node(r, unit, Throw{}, {e.id}), false};
    }
    case AstKind::Return: {
      if (cleanup_depth)
        reject(r, "return escapes defer body", "E_CLEANUP_ESCAPE");
      auto result = type(signatures.at(function.value)).result;
      std::vector<HirNodeId> children;
      if (n.children.empty())
        match(r, unit, result);
      else
        children.push_back(expression(child(r, 0), result).id);
      return {node(r, unit, Return{function}, std::move(children)), false};
    }
    case AstKind::If: {
      auto cond = expression(child(r, 0), boolean);
      auto yes = block(child(r, 1));
      std::vector<HirNodeId> children{cond.id, yes.id};
      bool falls = true, breaks = yes.breaks;
      bool else_falls = true, else_breaks = false;
      if (n.children.size() > 2) {
        auto er = child(r, 2);
        Statement no{HirNodeId{0}};
        if (ast(er).kind == AstKind::Block)
          no = block(er);
        else {
          auto saved = scope;
          scope = new_scope(er, saved);
          auto branch = statement(er);
          no = {node(er, unit, Block{}, {branch.id}), branch.falls,
                branch.breaks};
          scope = saved;
        }
        children.push_back(no.id);
        falls = yes.falls || no.falls;
        breaks = breaks || no.breaks;
        else_falls = no.falls;
        else_breaks = no.breaks;
      }
      if (auto b = std::get_if<BoolLiteral>(&m.nodes[cond.id.value].data)) {
        if (b->value) {
          falls = yes.falls;
          breaks = yes.breaks;
        } else if (n.children.size() == 2) {
          falls = true;
          breaks = false;
        } else {
          // Branch summaries are needed after their scope has been restored.
          // The selected else summary is the combined result minus the then
          // arm; keep it directly rather than inferring from bool algebra.
          falls = else_falls;
          breaks = else_breaks;
        }
      }
      return {node(r, unit, If{}, std::move(children)), falls, breaks};
    }
    case AstKind::ForIn: {
      auto collection = expression(child(r, 0));
      if (type(collection.value.type).kind != TypeKind::Vec)
        reject(r, "for-in requires Vec");
      auto saved = scope;
      scope = new_scope(r, saved);
      auto id = node(r, unit, ForIn{definition(r), ScopeId{0}, HirNodeId{0}});
      loops.push_back(id);
      break_targets.push_back(id);
      auto bodyref = child(r, n.children.size() - 1);
      auto loop_scope = scope;
      scope = new_scope(bodyref, loop_scope);
      auto bs = scope;
      locals[definition(r).value] = {type(collection.value.type).type_args[0],
                                     false};
      auto body = block(bodyref, true);
      scope = loop_scope;
      m.nodes[id.value].children = {collection.id, body.id};
      m.nodes[id.value].data = ForIn{definition(r), bs, body.id};
      loops.pop_back();
      break_targets.pop_back();
      scope = saved;
      // Collection is evaluated before entering the loop's owning scope.
      m.nodes[collection.id.value].scope = loop_scope;
      std::vector<HirNodeId> todo = m.nodes[collection.id.value].children;
      while (!todo.empty()) {
        auto c = todo.back();
        todo.pop_back();
        m.nodes[c.value].scope = loop_scope;
        auto ch = m.nodes[c.value].children;
        todo.insert(todo.end(), ch.begin(), ch.end());
      }
      return {id, true, false};
    }
    case AstKind::For: {
      auto saved = scope;
      scope = new_scope(r, saved);
      auto id = node(r, unit, For{});
      loops.push_back(id);
      break_targets.push_back(id);
      auto bodyref = child(r, n.children.size() - 1);
      size_t first_semicolon = ast(bodyref).token_begin,
             second_semicolon = first_semicolon;
      // Separators at nesting depth zero, since calls/function types may have
      // punctuation in their own token ranges.
      unsigned nesting = 0;
      for (size_t i = n.token_begin + 1; i < ast(bodyref).token_begin; ++i) {
        auto t = file(r).tokens[i].spelling;
        if (t == "(" || t == "[")
          ++nesting;
        if (t == ")" || t == "]")
          --nesting;
        if (t == ";" && !nesting) {
          if (first_semicolon == ast(bodyref).token_begin)
            first_semicolon = i;
          else
            second_semicolon = i;
        }
      }
      std::optional<Ref> init, condition, step;
      for (size_t i = 0; i + 1 < n.children.size(); ++i) {
        auto cr = child(r, i);
        auto start = ast(cr).token_begin;
        if (first_semicolon == ast(bodyref).token_begin) {
          if (ast(cr).kind != AstKind::ExpressionStatement)
            reject(cr, "invalid loop condition");
          condition = child(cr, 0);
        } else if (start < first_semicolon)
          init = cr;
        else if (start < second_semicolon)
          condition = cr;
        else
          step = cr;
      }
      auto ini = init ? statement(*init).id : node(r, unit, Noop{});
      auto cond = condition ? expression(*condition, boolean)
                            : Expr{node(r, boolean, BoolLiteral{true}),
                                   {boolean, {}, true}};
      auto st = step ? statement(*step).id : node(r, unit, Noop{});
      auto body = block(bodyref);
      m.nodes[id.value].children = {ini, cond.id, st, body.id};
      loops.pop_back();
      break_targets.pop_back();
      scope = saved;
      bool forever =
          std::holds_alternative<BoolLiteral>(m.nodes[cond.id.value].data) &&
          std::get<BoolLiteral>(m.nodes[cond.id.value].data).value;
      return {id, !forever || body.breaks, false};
    }
    case AstKind::Break:
    case AstKind::Continue:
      if (n.kind == AstKind::Break) {
        if (break_targets.empty())
          reject(r, "break has no enclosing loop or switch",
                 cleanup_depth ? "E_CLEANUP_ESCAPE" : "E_TYPE_MISMATCH");
        return {node(r, unit, Break{break_targets.back()}), false, true};
      }
      if (loops.empty())
        reject(r, "continue has no enclosing loop",
               cleanup_depth ? "E_CLEANUP_ESCAPE" : "E_TYPE_MISMATCH");
      return {node(r, unit, Continue{loops.back()}), false, false};
    default:
      unavailable(r, "switch/aggregate, typed errors, cleanup, or ownership "
                     "statement requires a later task");
    }
  }

  SymbolId self_parameter(Ref r) const {
    for (auto [owner, s] : m.resolved->self_parameters)
      if (key(owner) == key(r))
        return s;
    throw std::invalid_argument("missing resolver Self owner");
  }
  void register_function(
      Ref r, std::vector<GenericParam> inherited, bool prototype = false,
      std::optional<SymbolId> trait_self = std::nullopt,
      const std::vector<GenericParam> *environment = nullptr) {
    auto id = definition(r);
    // A trait prototype's own bound may name that trait's ordered parameters.
    // They belong to the surrounding trait, not to the method's own list or
    // the instance key, so expose them only while parsing the declaration.
    auto saved_environment = signature_parameters;
    if (environment)
      for (const auto &p : *environment)
        signature_parameters.insert(p.symbol.value);
    auto own = generics(r);
    inherited.insert(inherited.end(), own.begin(), own.end());
    generic_params[id.value] = inherited;
    if (trait_self) {
      auto self = self_types.find(trait_self->value);
      if (self == self_types.end() ||
          !trait_self_type(self->second, *trait_self))
        throw std::invalid_argument("invalid trait Self registration");
    }
    bool trait_self_callback = false;
    if (trait_self)
      for (auto c : ast(r).children) {
        if (ast(at(r, c)).kind != AstKind::Parameter)
          continue;
        auto first = at(r, c);
        if (mode(first) == ParamMode::ordinary) {
          // The existing F1 ordinary owner parameter is also Self even when
          // its label is not `self`; the callback context follows the real
          // Self type, not the spelling. A trait-static prototype whose first
          // parameter is a callback type has no such receiver and stays
          // rejected.
          auto first_type = parse_type(child(first, 0), trait_self);
          trait_self_callback =
              trait_self_type(first_type, *trait_self);
        }
        break;
      }
    signature_parameters.clear();
    for (const auto &p : inherited)
      signature_parameters.insert(p.symbol.value);
    if (environment)
      for (const auto &p : *environment)
        signature_parameters.insert(p.symbol.value);
    signatures[id.value] =
        signature(r, trait_self, false, trait_self_callback);
    check_inout_graph(r, signatures[id.value], true, trait_self,
                      trait_self_callback);
    if (trait_self)
      trait_signature_context[id.value] = {*trait_self, trait_self_callback};
    signature_parameters = std::move(saved_environment);
    for (auto c : ast(r).children)
      if (ast(at(r, c)).kind == AstKind::Parameter) {
        auto p = definition(at(r, c));
        parameters[id.value].push_back(
            {p, symbol(p).name == "_" ? "" : symbol(p).name});
      }
    parameters.try_emplace(id.value);
    if (prototype)
      return;
    bodies[id.value] = r;
    std::function<void(Ref)> nested = [&](Ref cr) {
      if (ast(cr).kind == AstKind::FunctionLiteral) {
        register_function(cr, inherited);
        return;
      }
      for (auto c : ast(cr).children)
        nested(at(cr, c));
    };
    for (auto c : ast(r).children)
      if (ast(at(r, c)).kind == AstKind::Block)
        nested(at(r, c));
  }
  bool overlap(TypeId a, TypeId b) {
    return type_patterns_overlap(a, b, m.types);
  }
  // Same-trait impls overlap only when Self and every ordered trait argument
  // admit one common substitution. Interning a synthetic function type lets
  // the existing unifier solve all columns with a shared binding table; a
  // per-column check would wrongly accept Match[X,bool] and Match[bool,Y].
  bool impls_overlap(const ImplDecl &a, const ImplDecl &b) {
    if (!a.trait || !b.trait)
      return overlap(a.self_type, b.self_type);
    if (a.trait->args.size() != b.trait->args.size())
      return false;
    Type fa;
    fa.kind = TypeKind::Function;
    fa.result = unit;
    fa.parameters.push_back({ParamMode::ordinary, a.self_type});
    for (auto t : a.trait->args)
      fa.parameters.push_back({ParamMode::ordinary, t});
    Type fb = fa;
    fb.parameters[0].type = b.self_type;
    for (size_t i = 0; i < b.trait->args.size(); ++i)
      fb.parameters[i + 1].type = b.trait->args[i];
    return type_patterns_overlap(m.types.intern(std::move(fa)),
                                 m.types.intern(std::move(fb)), m.types);
  }
  void declarations() {
    const auto &project = *m.resolved->project;
    const bool has_read_error = std::any_of(project.foreign_bindings.begin(), project.foreign_bindings.end(),
      [](const auto &d) { auto t = std::get_if<modules::BindingType>(&d.detail);
        return t && t->kind == "runtime_error" && d.identity == "lang.py.Error"; });
    if (!project.foreign_bindings.empty() && !has_read_error)
      for (const auto &owner : project.modules)
        for (const auto &im : owner.imports)
          if (project.module(im.target).binding)
            unavailable({owner.id, im.file, im.node},
              "foreign reads require the declared lang.py.Error type (toolchain std binding)");
    m.foreign_bindings = hir::detail::binding_records(project, m.types);
    std::vector<Ref> top;
    for (const auto &mod : m.resolved->project->modules) {
      if (mod.binding) {
        const auto &backends = m.resolved->project->bindings.at(mod.binding->bundle).allowed_backends;
        const std::string backend = m.config.backend == Backend::python ? "python" : "native";
        if (std::find(backends.begin(), backends.end(), backend) == backends.end())
          for (const auto &owner : m.resolved->project->modules)
            for (const auto &im : owner.imports)
              if (im.target == mod.id)
                unavailable({owner.id, im.file, im.node}, "external module is unavailable for this backend");
        continue;
      }
      for (size_t f = 0; f < mod.files.size(); ++f)
        for (auto id : mod.files[f]->node(mod.files[f]->root).children)
          top.push_back({mod.id, f, id});
    }
    // Nominal declarations and their parameter positions must be visible
    // before any bound is parsed: a bound may name a nominal declared later in
    // the same package (SEMANTICS section 2 shares top-level package names).
    // This pass records only the real declaration kind and GenericParameter
    // symbols/Sources; kinds and parameters are never guessed from spelling.
    for (auto r : top)
      if (ast(r).kind == AstKind::Struct || ast(r).kind == AstKind::Enum) {
        auto s = definition(r);
        std::vector<GenericParam> shape;
        for (auto c : ast(r).children) {
          auto cr = at(r, c);
          if (ast(cr).kind != AstKind::GenericParameter)
            continue;
          shape.push_back({definition(cr), std::nullopt, source(cr)});
        }
        nominal_ids[s.value] = m.nominals.size();
        m.nominals.push_back({s,
                              ast(r).kind == AstKind::Struct
                                  ? TypeKind::NominalStruct
                                  : TypeKind::NominalEnum,
                              std::move(shape),
                              {},
                              {},
                              source(r)});
      }
    // Trait arity is needed before a struct/impl bound can validate its
    // ordered application, including when the trait is declared later in the
    // same package. This pass only records real GenericParameter symbols and
    // Sources; bounds are parsed in the ordered declaration pass below.
    for (auto r : top)
      if (ast(r).kind == AstKind::Trait) {
        std::vector<GenericParam> ps;
        for (auto c : ast(r).children) {
          auto cr = at(r, c);
          if (ast(cr).kind != AstKind::GenericParameter)
            continue;
          ps.push_back({definition(cr), std::nullopt, source(cr)});
        }
        trait_parameters[definition(r).value] = std::move(ps);
      }
    // All nominal declarations and trait arities are now visible. Parse the
    // real nominal parameter lists and their full bounds in declaration
    // order, then replace the shape placeholders. Symbols and order must
    // still be exactly those registered above.
    for (auto &d : m.nominals) {
      auto ps = generics(d.source.node);
      if (ps.size() != d.params.size())
        throw std::invalid_argument("nominal parameter registration mismatch");
      for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].symbol != d.params[i].symbol)
          throw std::invalid_argument(
              "nominal parameter registration order mismatch");
      d.params = std::move(ps);
    }
    for (auto r : top)
      if (ast(r).kind == AstKind::Trait) {
        // The real Self identity must be visible while parsing the ordered
        // trait parameter bounds, which may name this trait's Self.
        auto self = self_parameter(r);
        self_types[self.value] = parameter_type(self);
        auto ps = generics(r);
        auto found = trait_parameters.find(definition(r).value);
        if (found == trait_parameters.end() || found->second.size() != ps.size())
          throw std::invalid_argument("trait parameter registration mismatch");
        found->second = ps;
        std::vector<TypeId> self_args;
        for (const auto &p : ps)
          self_args.push_back(parameter_type(p.symbol));
        bounds[self.value] = TraitRef{definition(r), std::move(self_args)};
        TraitDecl tr{definition(r), self, std::move(ps), {}, source(r)};
        for (auto id : ast(r).children)
          if (ast(at(r, id)).kind == AstKind::Function) {
            auto cr = at(r, id);
            register_function(cr, {}, true, self, &tr.params);
            auto method = definition(cr);
            tr.methods.push_back({method, signatures.at(method.value),
                                  parameters.at(method.value), source(cr)});
          }
        // Check the real prototype now and again after nominal fields exist.
        if (!tr.params.empty())
          for (const auto &proto : tr.methods)
            check_generic_trait_method_modes(
                proto.source.node, method_own_parameters(proto.symbol),
                proto.signature, tr.self, true, &tr.params);
        m.traits.push_back(std::move(tr));
      }
    auto field_value_type = [&](Ref field) {
      auto t = parse_type(child(field, 0));
      if (type(t).kind == TypeKind::ForeignOpaque)
        unavailable(field, "external aggregate fields are not supported");
      return t;
    };
    for (auto &d : m.nominals) {
      auto r = d.source.node;
      auto saved_signature_parameters = signature_parameters;
      signature_parameters.clear();
      for (const auto &p : d.params)
        signature_parameters.insert(p.symbol.value);
      for (auto c : ast(r).children) {
        auto cr = at(r, c);
        if (ast(cr).kind == AstKind::Field)
          d.fields.push_back({definition(cr), field_value_type(cr),
                              token(cr) == "pub", source(cr)});
        if (ast(cr).kind == AstKind::Variant) {
          VariantDecl v{definition(cr), {}, source(cr)};
          for (auto f : ast(cr).children) {
            auto fr = at(cr, f);
            v.payload.push_back(
                {definition(fr), field_value_type(fr), true, source(fr)});
          }
          d.variants.push_back(std::move(v));
        }
      }
      signature_parameters = std::move(saved_signature_parameters);
    }
    for (auto r : top)
      if (ast(r).kind == AstKind::Impl) {
        std::vector<Ref> ts;
        for (auto c : ast(r).children)
          if (ast(at(r, c)).kind == AstKind::NamedType)
            ts.push_back(at(r, c));
        auto self = parse_type(ts.back());
        auto sp = self_parameter(r);
        self_types[sp.value] = self;
        // Impl-parameter bounds may name the impl's real Self; parse them
        // only after that concrete identity is in the type environment.
        auto ps = generics(r);
        std::optional<TraitRef> trait;
        if (ts.size() == 2) {
          auto declaration = occurrence(ts[0]);
          if (!declaration ||
              symbol(*declaration).kind != modules::SymbolKind::trait)
            reject(ts[0], "impl requires a nominal trait");
          std::vector<TypeId> args;
          for (auto c : ast(ts[0]).children)
            args.push_back(parse_type(at(ts[0], c)));
          trait = check_trait_application(ts[0], *declaration, std::move(args));
        }
        if (type(self).kind != TypeKind::NominalStruct &&
            type(self).kind != TypeKind::NominalEnum && !integer(self) &&
            type(self).kind != TypeKind::Float &&
            type(self).kind != TypeKind::Bool &&
            type(self).kind != TypeKind::Vec &&
            type(self).kind != TypeKind::String &&
            type(self).kind != TypeKind::Unit)
          unavailable(r, "impl target is not an implemented value type");
        ImplDecl im{ps, trait, sp, self, {}, source(r)};
        for (auto c : ast(r).children)
          if (ast(at(r, c)).kind == AstKind::Function) {
            auto cr = at(r, c);
            register_function(cr, ps);
            im.methods.push_back(definition(cr));
          }
        for (const auto &previous : m.impls) {
          const bool same_target =
              (!previous.trait && !im.trait) ||
              (previous.trait && im.trait &&
               previous.trait->declaration == im.trait->declaration);
          if (same_target && impls_overlap(previous, im))
            reject(r, "duplicate or overlapping impl");
        }
        m.impls.push_back(std::move(im));
      }
    for (const auto &im : m.impls)
      if (im.trait) {
        const TraitDecl *tr = trait_decl(im.trait->declaration);
        if (!tr)
          reject(im.source.node, "unknown trait implementation");
        if (im.methods.size() != tr->methods.size())
          reject(im.source.node,
                 "impl must define every trait method exactly once");
        for (const auto &proto : tr->methods) {
          std::optional<SymbolId> method;
          for (auto id : im.methods)
            if (symbol(id).name == symbol(proto.symbol).name)
              method = id;
          if (!method)
            reject(im.source.node, "impl is missing a trait method");
          if (!trait_impl_signature_matches(*tr, im, proto, *method))
            reject(im.source.node,
                   "impl method signature does not match trait");
        }
      }
    // Self-dependent callback data is no longer limited by a separate
    // scalar policy. The complete substituted application is checked below
    // with the common inout aggregate rule.
    // Method-own mode restrictions are checked after every trait/impl
    // signature has been matched, so an own arity/bound mismatch keeps its
    // precise source diagnostic instead of being hidden by a capability check.
    // Nominal fields are now populated, so generic traits repeat the full graph
    // check with real Self, ordered trait parameters and every own parameter.
    // The same helper covers uncalled declarations and hidden callbacks in
    // struct/enum fields, return types, throws and nested function types.
    for (const auto &tr : m.traits) {
      const bool generic_trait = !tr.params.empty();
      for (const auto &proto : tr.methods)
        check_generic_trait_method_modes(
            proto.source.node, method_own_parameters(proto.symbol),
            proto.signature, tr.self, generic_trait, &tr.params);
    }
    // Trait method signatures were parsed while nominal field lists were
    // still empty. With all declarations and substitutions now available,
    // recheck the complete graph so a struct/enum with an owning field is a
    // source capability error rather than a later HIR error.
    for (const auto &[method, context] : trait_signature_context) {
      auto found = symbol(SymbolId{method}).definition;
      if (!found)
        throw std::invalid_argument("trait method has no source definition");
      auto saved_signature_parameters = signature_parameters;
      signature_parameters.clear();
      for (const auto &p : method_own_parameters(SymbolId{method}))
        signature_parameters.insert(p.symbol.value);
      for (const auto &tr : m.traits)
        if (tr.self == context.first)
          for (const auto &p : tr.params)
            signature_parameters.insert(p.symbol.value);
      check_inout_graph(*found, signatures.at(method), true, context.first,
                        context.second);
      signature_parameters = std::move(saved_signature_parameters);
    }
    for (auto r : top)
      if (ast(r).kind == AstKind::Function)
        register_function(r, {});
    std::function<void(Ref)> const_functions = [&](Ref r) {
      if (ast(r).kind == AstKind::FunctionLiteral) {
        register_function(r, {});
        return;
      }
      for (auto c : ast(r).children)
        const_functions(at(r, c));
    };
    for (auto r : top)
      if (ast(r).kind == AstKind::Binding)
        const_functions(r);
  }
  // Produce cleanup/effect records from this function's structured HIR. The
  // independent verifier below the sema API never calls this producer.
  void annotate(const HirFunction &f) {
    std::vector<HirNodeId> handlers, active;
    std::map<uint64_t, ScopeId> visible;
    for (auto p : f.parameters)
      visible.emplace(p.symbol.value, f.scope);
    struct Cleanup {
      size_t handler_floor;
      ScopeId body;
      std::set<uint64_t> external, references;
    };
    std::vector<Cleanup> cleanups;
    auto inside = [&](ScopeId outer, ScopeId inner) {
      for (;;) {
        if (outer == inner)
          return true;
        auto parent = m.scopes.at(inner.value).parent;
        if (!parent)
          return false;
        inner = *parent;
      }
    };
    auto plan = [&](HirNodeId id, ExitKind kind, ExitTarget target,
                    std::optional<ScopeId> keep, std::optional<TypeId> error,
                    bool saves) {
      const auto &n = m.nodes.at(id.value);
      ExitPlan p{kind, error, target, {}, {}, saves};
      std::optional<ScopeId> s = n.scope;
      while (s != keep) {
        if (!s)
          throw std::invalid_argument("exit target is not an ancestor");
        p.exited_scopes.push_back(*s);
        s = m.scopes.at(s->value).parent;
      }
      for (auto it = active.rbegin(); it != active.rend(); ++it)
        if (std::find(p.exited_scopes.begin(), p.exited_scopes.end(),
                      m.nodes.at(it->value).scope) != p.exited_scopes.end())
          p.defers.push_back(*it);
      if (error || !p.defers.empty())
        m.nodes.at(id.value).exits.push_back(std::move(p));
    };
    auto error_exit = [&](HirNodeId id, TypeId error) {
      const auto floor = cleanups.empty() ? 0 : cleanups.back().handler_floor;
      for (size_t h = handlers.size(); h > floor; --h) {
        auto target = handlers[h - 1];
        const auto &cs = std::get<Try>(m.nodes.at(target.value).data).catches;
        for (size_t i = 0; i < cs.size(); ++i)
          if (cs[i].error_type == error) {
            plan(id, ExitKind::error, CatchTarget{target, uint32_t(i)},
                 m.nodes.at(target.value).scope, error, true);
            return;
          }
      }
      const auto ref = m.nodes.at(id.value).source.node;
      if (!cleanups.empty())
        reject(ref, "typed error escapes defer body", "E_CLEANUP_ESCAPE");
      if (type(f.signature).throws_type != error)
        reject(ref,
               "typed error is neither caught nor declared by this function",
               "E_EFFECT_UNHANDLED");
      plan(id, ExitKind::error, FunctionExit{f.symbol}, {}, error, true);
    };
    std::function<void(const Pattern &, ScopeId)> bind_pattern =
        [&](const Pattern &p, ScopeId s) {
          if (p.binding)
            visible[p.binding->value] = s;
          for (const auto &ch : p.payload)
            bind_pattern(ch, s);
        };
    std::function<void(HirNodeId, size_t)> visit = [&](HirNodeId id,
                                                       size_t depth) {
      check_cancelled(stop);
      if (depth > 512)
        throw std::invalid_argument("effect nesting limit");
      auto n = m.nodes.at(id.value); // child traversal must not retain aliases
      m.nodes.at(id.value).exits.clear();
      const bool scoped =
          n.kind() == NodeKind::block || n.kind() == NodeKind::for_ ||
          n.kind() == NodeKind::for_in || n.kind() == NodeKind::switch_ ||
          n.kind() == NodeKind::try_ || n.kind() == NodeKind::defer_;
      const auto saved_visible =
          scoped ? visible : std::map<uint64_t, ScopeId>{};
      const auto active_size = active.size();
      if (auto s = std::get_if<Switch>(&n.data)) {
        visit(n.children[0], depth + 1);
        for (const auto &a : s->arms) {
          visible = saved_visible;
          bind_pattern(a.pattern, a.scope);
          visit(a.body, depth + 1);
        }
        visible = saved_visible;
      } else if (auto fi = std::get_if<ForIn>(&n.data)) {
        visit(n.children[0], depth + 1);
        visible[fi->element.value] = fi->body_scope;
        visit(fi->body, depth + 1);
      } else if (auto t = std::get_if<Try>(&n.data)) {
        handlers.push_back(id);
        visit(n.children[0], depth + 1);
        handlers.pop_back(); // handlers' errors go to the outer try
        for (const auto &c : t->catches) {
          visible = saved_visible;
          visible[c.binding.value] = c.scope;
          visit(c.body, depth + 1);
        }
        visible = saved_visible;
      } else if (std::holds_alternative<Defer>(n.data)) {
        Cleanup c{
            handlers.size(), m.nodes.at(n.children[0].value).scope, {}, {}};
        for (const auto &[s, owner] : visible)
          c.external.insert(s);
        cleanups.push_back(std::move(c));
        visit(n.children[0], depth + 1);
        auto &d = std::get<Defer>(m.nodes.at(id.value).data);
        d.references.clear();
        for (auto s : cleanups.back().references)
          d.references.push_back(SymbolId{s});
        cleanups.pop_back();
        visible = saved_visible;
        active.push_back(id); // only after the registration point
      } else {
        for (auto ch : n.children)
          visit(ch, depth + 1);
      }
      if (auto b = std::get_if<Binding>(&n.data))
        visible[b->symbol.value] = n.scope;
      std::optional<SymbolId> read;
      if (auto r = std::get_if<Read>(&n.data))
        read = r->symbol;
      if (auto p = std::get_if<Place>(&n.data))
        read = p->symbol;
      if (auto p = std::get_if<Move>(&n.data))
        read = p->binding;
      if (read) {
        if (!visible.count(read->value))
          throw std::invalid_argument(
              "binding not visible at use/registration");
        for (auto &c : cleanups)
          if (c.external.count(read->value))
            c.references.insert(read->value);
      }
      std::set<uint64_t> errors;
      for (auto ch : n.children)
        for (auto e : m.nodes.at(ch.value).errors)
          errors.insert(e.value);
      if (auto t = std::get_if<Try>(&n.data)) {
        errors.clear();
        for (auto e : m.nodes.at(n.children[0].value).errors) {
          bool caught = false;
          for (const auto &c : t->catches)
            caught |= c.error_type == e;
          if (!caught)
            errors.insert(e.value);
        }
        for (const auto &c : t->catches)
          for (auto e : m.nodes.at(c.body.value).errors)
            errors.insert(e.value);
      }
      std::optional<TypeId> own_error;
      if (auto c = std::get_if<Call>(&n.data))
        own_error = type(c->signature).throws_type;
      if (auto c = std::get_if<ConstraintCall>(&n.data))
        own_error = type(c->signature).throws_type;
      if (auto r = std::get_if<ForeignRead>(&n.data))
        own_error = m.foreign_bindings.at(r->binding.value).read_error;
      if (std::holds_alternative<Throw>(n.data))
        own_error = m.nodes.at(n.children[0].value).type;
      if (own_error) {
        errors.insert(own_error->value);
        error_exit(id, *own_error);
      }
      if (std::holds_alternative<Defer>(n.data))
        errors.clear();
      auto &out = m.nodes.at(id.value);
      out.errors.clear();
      for (auto e : errors)
        out.errors.push_back(TypeId{e});
      if (n.kind() == NodeKind::return_) {
        if (!cleanups.empty())
          reject(n.source.node, "return escapes defer body",
                 "E_CLEANUP_ESCAPE");
        plan(id, ExitKind::return_, FunctionExit{f.symbol}, {}, {},
             !n.children.empty());
      }
      if (n.kind() == NodeKind::break_ || n.kind() == NodeKind::continue_) {
        auto target = n.kind() == NodeKind::break_
                          ? std::get<Break>(n.data).loop
                          : std::get<Continue>(n.data).loop;
        const auto &dest = m.nodes.at(target.value);
        if (!cleanups.empty() && !inside(cleanups.back().body, dest.scope))
          reject(n.source.node, "jump escapes defer body", "E_CLEANUP_ESCAPE");
        auto keep = std::optional(dest.scope);
        if (n.kind() == NodeKind::break_ &&
            (dest.kind() == NodeKind::for_ || dest.kind() == NodeKind::for_in))
          keep = m.scopes.at(dest.scope.value).parent;
        plan(id,
             n.kind() == NodeKind::break_ ? ExitKind::break_
                                          : ExitKind::continue_,
             JumpExit{target}, keep, {}, false);
      }
      if (n.kind() == NodeKind::block || n.kind() == NodeKind::for_ ||
          n.kind() == NodeKind::for_in) {
        plan(id, ExitKind::normal, ScopeExit{m.scopes.at(n.scope.value).parent},
             m.scopes.at(n.scope.value).parent, {}, false);
        visible = saved_visible;
        active.resize(active_size);
      }
    };
    visit(f.body, 0);
  }

  void check_body(SymbolId id) {
    function = id;
    auto r = bodies.at(id.value);
    auto sig = signatures.at(id.value);
    locals.clear();
    loops.clear();
    break_targets.clear();
    cleanup_depth = 0;
    active_params = generic_params[id.value];
    scope = new_scope(r, {});
    auto ps = parameters.at(id.value);
    auto ft = type(sig);
    for (size_t i = 0; i < ps.size(); ++i)
      locals[ps[i].symbol.value] = {
          ft.parameters[i].type, ft.parameters[i].mode == ParamMode::inout,
          ft.parameters[i].mode == ParamMode::ordinary};
    auto body = block(child(r, ast(r).children.size() - 1), true);
    if (ft.result != unit && flow(body.id).falls)
      reject(r, "non-Unit function has a reachable normal exit without return");
    HirFunction f{id, sig, ps, body.id, scope, source(r)};
    annotate(f);
    if (active_params.empty())
      m.functions.push_back(f);
    else {
      template_ids[id.value] = m.templates.size();
      m.templates.push_back({f, active_params});
    }
  }
  SymbolId duplicate_symbol(SymbolId origin) {
    auto s = symbol(origin);
    s.id = SymbolId{owned->symbols.size()};
    owned->symbols.push_back(s);
    return s.id;
  }
  std::optional<std::vector<Ref>> explicit_method_type_refs(
      const HirNode &n) const {
    auto r = n.source.node;
    const auto &call = ast(r);
    if (call.kind != AstKind::Call || call.children.empty())
      return std::nullopt;
    auto transparent = [&](Ref value) {
      while (ast(value).kind == AstKind::Parenthesized)
        value = child(value, 0);
      return value;
    };
    auto target = transparent(child(r, 0));
    if (ast(target).kind != AstKind::NeutralBracket)
      return std::nullopt;
    auto inner = transparent(child(target, 0));
    if (ast(inner).kind != AstKind::Member || occurrence(inner))
      return std::nullopt;
    std::vector<Ref> refs;
    for (size_t i = 1; i < ast(target).children.size(); ++i)
      refs.push_back(child(target, i));
    return refs;
  }
  // Complete the method-own actual arguments from the prototype. An explicit
  // suffix is read from the real Source and mapped through the current
  // instance replacement; without a suffix only the already determined
  // ConstraintCall signature can solve own parameters. A parameter absent from
  // the signature (phantom) therefore must have been written explicitly.
  std::vector<TypeId> constraint_method_arguments(
      const HirNode &n, const ConstraintCall &c,
      const Substitutions *instance_sub) {
    const TraitDecl *tr = trait_decl(c.trait.declaration);
    const MethodPrototype *prototype = nullptr;
    if (tr)
      for (const auto &p : tr->methods)
        if (p.symbol == c.method)
          prototype = &p;
    if (!tr || !prototype || c.trait.args.size() != tr->params.size())
      throw std::invalid_argument("constraint method has no real prototype");
    const auto ps = generic_params[c.method.value];
    Substitutions sub{{tr->self, c.receiver}};
    for (size_t i = 0; i < tr->params.size(); ++i)
      sub.push_back({tr->params[i].symbol, c.trait.args[i]});
    std::vector<TypeId> result;
    auto refs = explicit_method_type_refs(n);
    if (refs) {
      if (refs->size() != ps.size())
        reject(n.source.node, "wrong number of method type arguments");
      for (size_t i = 0; i < ps.size(); ++i) {
        auto t = parse_type((*refs)[i]);
        if (instance_sub)
          t = substitute(t, *instance_sub, m.types);
        result.push_back(t);
        sub.push_back({ps[i].symbol, t});
      }
      if (substitute(prototype->signature, sub, m.types) != c.signature)
        reject(n.source.node,
               "method type arguments differ from the checked signature");
    } else {
      Substitutions match = sub;
      infer(n.source.node, prototype->signature, c.signature, match);
      for (const auto &p : ps) {
        auto it = std::find_if(match.begin(), match.end(),
                               [&](auto entry) {
                                 return entry.first == p.symbol;
                               });
        if (it == match.end())
          reject(n.source.node,
                 "cannot infer method type argument; provide explicit type "
                 "arguments");
        result.push_back(it->second);
      }
    }
    // Bounds may name the trait Self, any ordered trait argument or a sibling
    // method-own parameter. Finish the same positional substitution before
    // asking for an implementation; a raw bound would leave those symbolic.
    for (size_t i = 0; i < ps.size(); ++i) {
      auto it = std::find_if(sub.begin(), sub.end(), [&](auto entry) {
        return entry.first == ps[i].symbol;
      });
      if (it == sub.end())
        sub.push_back({ps[i].symbol, result[i]});
      else
        it->second = result[i];
    }
    for (size_t i = 0; i < ps.size(); ++i) {
      if (!ps[i].trait)
        continue;
      TraitRef bound = *ps[i].trait;
      for (auto &arg : bound.args)
        arg = substitute(arg, sub, m.types);
      check_trait_parameter_bounds(n.source.node, bound, result[i]);
      if (!satisfies(n.source.node, result[i], bound))
        reject(n.source.node,
               "method type argument does not satisfy its trait constraint");
    }
    return result;
  }
  void transform_node(HirNodeId id, size_t depth,
                      const Substitutions *instance_sub = nullptr) {
    if (depth > 512)
      throw std::invalid_argument("HIR transform nesting limit");
    auto n = m.nodes.at(id.value);
    if (auto c = std::get_if<Call>(&n.data);
        c && c->direct_symbol && template_ids.count(c->direct_symbol->value)) {
      auto original = *c->direct_symbol;
      auto target = instantiate(original, c->type_args, n.source.node);
      c->direct_symbol = target;
      c->type_args.clear();
    }
    if (auto v = std::get_if<FunctionValue>(&n.data);
        v && template_ids.count(v->symbol.value)) {
      v->symbol = instantiate(v->symbol, v->type_args, n.source.node);
      v->type_args.clear();
    }
    if (auto c = std::get_if<ConstraintCall>(&n.data)) {
      auto name = symbol(c->method).name;
      auto selected = select_method(n.source.node, c->receiver, name, c->trait);
      auto method_args = constraint_method_arguments(n, *c, instance_sub);
      auto own = method_own_parameters(selected.method);
      if (own.size() != method_args.size())
        throw std::invalid_argument("constraint method own parameter count");
      for (size_t i = 0; i < own.size(); ++i)
        selected.substitutions.push_back({own[i].symbol, method_args[i]});
      auto args =
          finish_arguments(n.source.node, generic_params[selected.method.value],
                           selected.substitutions);
      auto target = selected.method;
      if (template_ids.count(target.value))
        target = instantiate(target, args, n.source.node);
      Call concrete;
      concrete.kind = CalleeKind::direct;
      concrete.signature = c->signature;
      concrete.direct_symbol = target;
      concrete.args = c->args;
      n.data = std::move(concrete);
    }
    m.nodes.at(id.value) = n;
    for (auto ch : n.children)
      transform_node(ch, depth + 1, instance_sub);
  }
  SymbolId instantiate(SymbolId origin, const std::vector<TypeId> &args,
                       Ref use) {
    const auto temp = m.templates.at(template_ids.at(origin.value));
    if (args.size() != temp.params.size())
      throw std::invalid_argument("instance type argument count");
    Substitutions sub;
    for (size_t i = 0; i < args.size(); ++i)
      sub.push_back({temp.params[i].symbol, args[i]});
    check_constraints(use, temp.params, args, &sub);
    std::vector<uint64_t> ids;
    for (auto t : args) {
      if (contains_parameter(t))
        throw std::invalid_argument("nonconcrete instance request");
      ids.push_back(t.value);
    }
    auto key = std::make_pair(origin.value, ids);
    if (auto it = instance_cache.find(key); it != instance_cache.end())
      return it->second;
    if (instance_cache.size() >= 128 || instantiation_depth >= 48)
      reject(use, "generic instantiation budget exceeded",
             "E_INSTANTIATION_LIMIT");
    ++instantiation_depth;
    auto f = temp.function;
    const auto result = duplicate_symbol(origin);
    instance_cache.emplace(key, result);
    std::map<uint64_t, SymbolId> renamed;
    renamed[origin.value] = result;
    auto rename = [&](SymbolId s) {
      auto it = renamed.find(s.value);
      if (it != renamed.end())
        return it->second;
      return renamed[s.value] = duplicate_symbol(s);
    };
    f.symbol = result;
    f.signature = substitute(f.signature, sub, m.types);
    // A generic declaration defers direct mode/type restrictions until the
    // concrete type is known; reject them before publishing invalid HIR. Free
    // generic templates and eligible inherent methods may carry scalar inout
    // into the instance whether or not either layer has a trait constraint; a
    // generic trait impl method is eligible only through the same real
    // prototype/self relation used by its direct call. Every other source
    // keeps the old refusal after check_inout_graph has checked the concrete
    // types unconditionally.
    const bool inout_source =
        !temp.params.empty() &&
        (free_generic_template(origin) || inherent_method_source(origin) ||
         trait_impl_inout_source(origin, true).has_value() ||
         trait_impl_inout_source(origin, false).has_value());
    for (const auto &p : type(f.signature).parameters) {
      if (p.mode != ParamMode::ordinary &&
          type(p.type).kind == TypeKind::ForeignOpaque)
        unavailable(use, "external move/inout parameters are not supported");
    }
    if (check_inout_graph(use, f.signature, false) && !inout_source)
      unavailable(use, "generic/method inout is not implemented");
    for (auto &p : f.parameters)
      p.symbol = rename(p.symbol);
    std::map<uint64_t, ScopeId> scopes;
    std::function<ScopeId(ScopeId)> clone_scope = [&](ScopeId old) {
      if (auto it = scopes.find(old.value); it != scopes.end())
        return it->second;
      auto sc = m.scopes.at(old.value);
      sc.function = result;
      if (sc.parent)
        sc.parent = clone_scope(*sc.parent);
      ScopeId sid{m.scopes.size()};
      m.scopes.push_back(sc);
      scopes[old.value] = sid;
      return sid;
    };
    std::map<uint64_t, HirNodeId> nodes;
    std::function<void(Pattern &)> clone_pattern = [&](Pattern &p) {
      p.type = substitute(p.type, sub, m.types);
      if (p.binding)
        p.binding = rename(*p.binding);
      for (auto &c : p.payload)
        clone_pattern(c);
    };
    std::function<HirNodeId(HirNodeId)> clone = [&](HirNodeId old) {
      if (++instantiated_nodes > 200000)
        reject(use, "generic instantiated node budget exceeded",
               "E_INSTANTIATION_LIMIT");
      auto n = m.nodes.at(old.value);
      n.type = substitute(n.type, sub, m.types);
      n.scope = clone_scope(n.scope);
      if (auto b = std::get_if<Binding>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Read>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Place>(&n.data))
        b->symbol = rename(b->symbol);
      if (auto b = std::get_if<Move>(&n.data); b && b->binding)
        b->binding = rename(*b->binding);
      if (auto b = std::get_if<ForIn>(&n.data))
        b->element = rename(b->element);
      if (auto b = std::get_if<Return>(&n.data))
        b->function = result;
      if (auto t = std::get_if<Try>(&n.data))
        for (auto &c : t->catches) {
          c.error_type = substitute(c.error_type, sub, m.types);
          c.binding = rename(c.binding);
        }
      // Matching can change after substitution (A and B may become equal).
      // Derive these records from the completed concrete tree, not the
      // template.
      n.errors.clear();
      n.exits.clear();
      HirNodeId id{m.nodes.size()};
      m.nodes.push_back(n);
      nodes[old.value] = id;
      for (auto &c : n.children)
        c = clone(c);
      auto update_args = [&](auto &as) {
        for (auto &a : as)
          a.expression = nodes.at(a.expression.value);
      };
      if (auto c = std::get_if<Call>(&n.data)) {
        c->signature = substitute(c->signature, sub, m.types);
        for (auto &t : c->type_args)
          t = substitute(t, sub, m.types);
        if (c->callee)
          c->callee = nodes.at(c->callee->value);
        update_args(c->args);
      }
      if (auto v = std::get_if<FunctionValue>(&n.data))
        for (auto &t : v->type_args)
          t = substitute(t, sub, m.types);
      if (auto c = std::get_if<ConstraintCall>(&n.data)) {
        c->signature = substitute(c->signature, sub, m.types);
        c->receiver = substitute(c->receiver, sub, m.types);
        for (auto &t : c->trait.args)
          t = substitute(t, sub, m.types);
        update_args(c->args);
      }
      if (auto c = std::get_if<VecSlice>(&n.data)) {
        if (c->start)
          c->start = nodes.at(c->start->value);
        if (c->end)
          c->end = nodes.at(c->end->value);
      }
      if (auto c = std::get_if<ForIn>(&n.data)) {
        c->body = nodes.at(c->body.value);
        c->body_scope = scopes.at(c->body_scope.value);
      }
      if (auto c = std::get_if<StructInit>(&n.data))
        update_args(c->fields);
      if (auto c = std::get_if<EnumInit>(&n.data))
        update_args(c->payload);
      if (auto c = std::get_if<Switch>(&n.data))
        for (auto &a : c->arms) {
          clone_pattern(a.pattern);
          a.scope = scopes.at(a.scope.value);
          a.body = nodes.at(a.body.value);
        }
      if (auto t = std::get_if<Try>(&n.data)) {
        std::set<uint64_t> caught;
        for (auto &c : t->catches) {
          if (!caught.insert(c.error_type.value).second)
            unavailable(use, "instance has duplicate catch types");
          c.scope = scopes.at(c.scope.value);
          c.body = nodes.at(c.body.value);
        }
      }
      if (auto d = std::get_if<Defer>(&n.data)) {
        d->owner_scope = clone_scope(d->owner_scope);
        for (auto &s : d->references)
          s = rename(s);
        std::sort(d->references.begin(), d->references.end(),
                  [](auto a, auto b) { return a.value < b.value; });
      }
      if (auto b = std::get_if<Break>(&n.data))
        b->loop = nodes.at(b->loop.value);
      if (auto b = std::get_if<Continue>(&n.data))
        b->loop = nodes.at(b->loop.value);
      m.nodes[id.value] = std::move(n);
      return id;
    };
    f.scope = clone_scope(f.scope);
    f.body = clone(f.body);
    FunctionInstance instance{result, origin, args, {}, m.config};
    for (auto [old, s] : renamed)
      instance.renamed.push_back({SymbolId{old}, s});
    m.instances.push_back(std::move(instance));
    m.functions.push_back(f);
    // Identity and signature are present before resolving recursive uses.
    transform_node(f.body, 0, &sub);
    annotate(f);
    if (type(f.signature).result != unit && flow(f.body).falls)
      reject(use,
             "instantiated non-Unit function has a normal exit without return");
    --instantiation_depth;
    return result;
  }

public:
  Checker(std::shared_ptr<const modules::ResolvedProject> r, Config config,
          std::stop_token s)
      : stop(s) {
    owned = std::make_shared<modules::ResolvedProject>(*r);
    m.resolved = owned;
    m.config = config;
    m.identity = m.resolved->project->entry;
    unit = scalar(TypeKind::Unit);
    boolean = scalar(TypeKind::Bool);
    i64 = scalar(TypeKind::SignedInt, 64);
    for (const auto &s : m.resolved->symbols)
      if (s.definition)
        definitions.emplace(key(*s.definition), s.id);
    for (const auto &o : m.resolved->occurrences)
      occurrences[key(o.source)] = o.symbol;
  }
  HirModule run() {
    declarations();
    // Bound function types may have been parsed before nominal fields existed.
    // Check each application and its substituted inout graph with complete
    // fields and the real surrounding template parameter identities.
    auto check_bounds = [&](const std::vector<GenericParam> &ps,
                            std::optional<SymbolId> self = std::nullopt,
                            const std::set<uint64_t> *environment = nullptr,
                            const ImplDecl *impl = nullptr) {
      auto saved = std::move(signature_parameters);
      signature_parameters.clear();
      for (const auto &p : ps)
        signature_parameters.insert(p.symbol.value);
      if (self)
        signature_parameters.insert(self->value);
      if (environment)
        signature_parameters.insert(environment->begin(), environment->end());
      for (const auto &p : ps)
        if (p.trait)
          check_trait_parameter_bounds(p.source.node, *p.trait,
                                       parameter_type(p.symbol));
      if (impl && impl->trait)
        check_trait_parameter_bounds(impl->source.node, *impl->trait,
                                     impl->self_type);
      signature_parameters = std::move(saved);
    };
    for (const auto &tr : m.traits)
      check_bounds(tr.params, tr.self);
    for (const auto &d : m.nominals)
      check_bounds(d.params);
    for (const auto &im : m.impls)
      check_bounds(im.params, im.self_parameter, nullptr, &im);
    for (const auto &[owner, ps] : generic_params) {
      auto context = trait_signature_context.find(owner);
      std::set<uint64_t> environment;
      for (const auto &tr : m.traits)
        for (const auto &method : tr.methods)
          if (method.symbol.value == owner) {
            for (const auto &p : tr.params)
              environment.insert(p.symbol.value);
            break;
          }
      check_bounds(ps,
                   context == trait_signature_context.end()
                       ? std::nullopt
                       : std::optional(context->second.first),
                   &environment);
    }
    for (const auto &mod : m.resolved->project->modules)
      for (size_t f = 0; f < mod.files.size(); ++f)
        for (auto id : mod.files[f]->node(mod.files[f]->root).children) {
          Ref r{mod.id, f, id};
          if (ast(r).kind == AstKind::Binding)
            constant(definition(r));
        }
    for (auto [id, r] : bodies) {
      check_cancelled(stop);
      check_body(SymbolId{id});
    }
    auto concrete = m.functions;
    for (const auto &f : concrete)
      transform_node(f.body, 0);
    const auto type_roots = m.types.size();
    for (size_t i = 1; i <= type_roots; ++i) {
      auto t = TypeId{i};
      if (type(t).kind == TypeKind::NominalStruct ||
          type(t).kind == TypeKind::NominalEnum) {
        auto d = nominal(t);
        Substitutions sub;
        for (size_t p = 0; p < d.params.size(); ++p)
          sub.push_back({d.params[p].symbol, type(t).type_args.at(p)});
        check_constraints(d.source.node, d.params, type(t).type_args, &sub);
      }
    }
    if (auto infinite = infinite_value_layout(m, stop))
      reject(m.nominals.at(nominal_ids.at(infinite->value)).source.node,
             "value type has an infinite recursive layout");
    auto ownership = detail::produce_ownership(m, stop);
    if (!ownership.success()) {
      if (ownership.diagnostics.empty())
        throw Cancelled{};
      auto d = ownership.diagnostics.front();
      auto kind = ownership.failure;
      throw Rejected{std::move(d), kind};
    }
    return std::move(m);
  }
};
template <class T> PhaseResult<T> internal(std::string message) {
  PhaseResult<T> r;
  Diagnostic d;
  d.code = "E_INTERNAL_ERROR";
  d.phase = "type";
  d.severity = "error";
  d.message = std::move(message);
  r.diagnostics.push_back(std::move(d));
  return r;
}
} // namespace
PhaseResult<HirModule>
check_types(std::shared_ptr<const modules::ResolvedProject> resolved,
            std::optional<CheckConfig> config, std::stop_token stop) {
  try {
    check_cancelled(stop);
    if (!resolved || !resolved->project)
      return internal<HirModule>(
          "checker requires an owning successfully resolved project");
    const auto &p = *resolved->project;
    if (p.backend != "native" && p.backend != "python")
      return internal<HirModule>("invalid project backend");
    Config c{p.backend == "native" ? Backend::native : Backend::python,
             p.word_bits};
    if (config && *config != c)
      return internal<HirModule>("CheckConfig does not match Project target");
    if (c.word_bits != 32 && c.word_bits != 64)
      return internal<HirModule>("target word_bits must be 32 or 64");
    Checker checker(std::move(resolved), c, stop);
    auto m = checker.run();
    auto verified = hir::verify_types(m, stop);
    PhaseResult<HirModule> result;
    result.state = verified.state;
    result.failure = verified.failure;
    result.diagnostics = std::move(verified.diagnostics);
    if (verified.success())
      result.value = std::move(m);
    return result;
  } catch (const Cancelled &) {
    PhaseResult<HirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  } catch (const Rejected &e) {
    PhaseResult<HirModule> r;
    r.failure = e.kind;
    r.diagnostics = {e.diagnostic};
    return r;
  } catch (const std::exception &e) {
    return internal<HirModule>(e.what());
  }
}
PhaseResult<HirModule>
check_types(const PhaseResult<modules::ResolvedProject> &resolved,
            std::optional<CheckConfig> config, std::stop_token stop) {
  if (stop.stop_requested()) {
    PhaseResult<HirModule> r;
    r.state = PhaseState::cancelled;
    return r;
  }
  if (!resolved.success()) {
    PhaseResult<HirModule> r;
    r.state = resolved.state;
    r.failure = resolved.failure;
    r.diagnostics = resolved.diagnostics;
    if (r.state == PhaseState::complete)
      r.state = PhaseState::failed;
    if (r.diagnostics.empty() && r.state != PhaseState::cancelled)
      return internal<HirModule>("resolve did not successfully complete");
    return r;
  }
  return check_types(
      std::make_shared<const modules::ResolvedProject>(*resolved.value), config,
      stop);
}
PhaseResult<VerifiedHirModule>
check_project(std::shared_ptr<const modules::ResolvedProject> r,
              std::optional<CheckConfig> c, std::stop_token stop, hir::VerifyMode mode) {
  auto typed = check_types(std::move(r), c, stop);
  if (typed.success())
    return hir::verify(*typed.value, stop, mode);
  PhaseResult<VerifiedHirModule> result;
  result.state = typed.state;
  result.failure = typed.failure;
  result.diagnostics = std::move(typed.diagnostics);
  return result;
}
PhaseResult<VerifiedHirModule>
check_project(const PhaseResult<modules::ResolvedProject> &r,
              std::optional<CheckConfig> c, std::stop_token stop, hir::VerifyMode mode) {
  auto typed = check_types(r, c, stop);
  if (typed.success())
    return hir::verify(*typed.value, stop, mode);
  PhaseResult<VerifiedHirModule> result;
  result.state = typed.state;
  result.failure = typed.failure;
  result.diagnostics = std::move(typed.diagnostics);
  return result;
}
} // namespace lang::sema

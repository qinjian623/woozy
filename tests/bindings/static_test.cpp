#include "lang/sema/check.hpp"
#include "lang/backend/python.hpp"
#ifdef LANG_T10_NATIVE
#include "lang/ir/native.hpp"
#endif
#include "../../src/hir/ownership.hpp"
#include "../../src/hir/foreign.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <json-c/json.h>
using namespace lang;
using namespace lang::hir;
namespace fs = std::filesystem;
namespace {
fs::path repo, output;
size_t probes = 0;
void need(bool b, const std::string &message) { if (!b) throw std::runtime_error(message); }
std::string diagnostics(const auto &r) {
  std::string out; for (const auto &d : r.diagnostics) out += d.code + ": " + d.message + "\n";
  return out;
}
bool has(const auto &r, std::string_view code) {
  return std::any_of(r.diagnostics.begin(), r.diagnostics.end(), [&](const auto &d){ return d.code == code; });
}
std::string read(const fs::path &path) {
  std::ifstream f(path); need(bool(f), "read " + path.string());
  return {std::istreambuf_iterator<char>(f), {}};
}
void write(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path()); std::ofstream f(path); f << text; need(bool(f), "write");
}
std::shared_ptr<modules::ResolvedProject> resolve(const std::string &text, const std::string &name,
                                                const std::string &bundle = "", std::string backend = "python") {
  auto root = output / name;
  write(root / "src/main.wz", text);
  write(root / "binding.json", bundle.empty() ? read(repo / "contracts/python_bindings.example.json") : bundle);
  write(root / "lang.toml", "schema_version=1\n[package]\nname=\"main\"\nsource_root=\"src\"\nentry=\"app\"\n[build]\nbackend=\"" + backend + "\"\n[dependencies.numpy]\nkind=\"python\"\nbinding=\"binding.json\"\nhost_package=\"numpy\"\n[dependencies.torch]\nkind=\"python\"\nbinding=\"binding.json\"\nhost_package=\"torch\"\n");
  SourceManager sm;
  auto p = modules::load_project({root / "lang.toml", {}, root / "binding.json"}, sm);
  need(p.success(), name + " load " + diagnostics(p));
  auto r = modules::resolve_project(std::make_shared<const modules::Project>(std::move(*p.value)));
  need(r.success(), name + " resolve " + diagnostics(r));
  return std::make_shared<modules::ResolvedProject>(std::move(*r.value));
}
HirModule accepted(const std::string &text, const std::string &name, const std::string &bundle = "") {
  auto r = sema::check_types(resolve(text, name, bundle));
  need(r.success(), name + " check " + diagnostics(r));
  auto v = verify_types(*r.value); need(v.success(), name + " verify " + diagnostics(v));
  auto frozen = verify(*r.value, {}, VerifyMode::static_only);
  need(frozen.success(), name + " freeze " + diagnostics(frozen));
  ++probes; std::cout << "passed " << name << '\n';
  return std::move(*r.value);
}
const std::string imports = "package main; import \"std/py\" as py; import \"numpy\" as np; import \"torch\" as torch; import \"torch/nn\" as nn; import \"torch/optim\" as optim; import \"torch/nn/functional\" as F; ";
void rejected(const std::string &text, const std::string &name, const std::string &code) {
  auto r = sema::check_types(resolve(text, name));
  need(!r.success() && !r.value && has(r, code), name + " expected " + code + " got " + diagnostics(r));
  ++probes; std::cout << "passed " << name << ' ' << code << '\n';
}
std::string_view m_name(const HirModule &m, SymbolId id) {
  return m.resolved->symbols.at(id.value).name;
}
const HirFunction &named_function(const HirModule &m, std::string_view name) {
  for (const auto &f : m.functions)
    if (m.resolved->symbols.at(f.symbol.value).name == name) return f;
  throw std::runtime_error("missing function " + std::string(name));
}
size_t foreign_call(const HirModule &m, const std::string &identity) {
  for (size_t i=0;i<m.nodes.size();++i)
    if (auto c=std::get_if<Call>(&m.nodes[i].data); c && c->binding &&
        m.resolved->project->foreign_binding(*c->binding).identity == identity) return i;
  throw std::runtime_error("missing call " + identity);
}
void bad(const HirModule &good, const std::string &name, const auto &edit, bool recompute = false) {
  need(verify_types(good).success(), name + " legal control");
  auto m = good; edit(m);
  if (recompute) { auto r = detail::produce_ownership(m, {}); need(r.success(), name + " recompute " + diagnostics(r)); }
  auto r = verify_types(m);
  need(!r.success() && has(r,"E_IR_INVALID"), name + " accepted malformed HIR " + diagnostics(r));
  auto frozen = verify(m, {}, VerifyMode::static_only);
  need(!frozen.success() && has(frozen,"E_IR_INVALID"), name + " published malformed HIR " + diagnostics(frozen));
  ++probes; std::cout << "passed bad-hir " << name << '\n';
}
void erase_foreign_call(HirModule &m, const std::string &identity) {
  SymbolId sink; TypeId sig;
  for (const auto &f : m.functions)
    if (m.resolved->symbols.at(f.symbol.value).name == "sink") {
      sink = f.symbol; sig = f.signature;
    }
  need(sink.value != 0, "erasure sink function");
  size_t index = m.nodes.size();
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (auto c = std::get_if<Call>(&m.nodes[i].data); c && c->binding &&
        m.resolved->project->foreign_binding(*c->binding).identity == identity)
      index = i;
  need(index < m.nodes.size(), "foreign call to erase");
  auto old = std::get<Call>(m.nodes[index].data);
  need(old.callee.has_value() && !m.nodes[old.callee->value].children.empty(),
       "foreign call target");
  auto callee = *old.callee;
  auto receiver = m.nodes[callee.value].children.at(0);
  std::vector<HirNodeId> leaves{receiver, callee};
  for (const auto &a : old.args) leaves.push_back(a.expression);
  for (size_t i = 0; i < leaves.size(); ++i)
    for (auto child : m.nodes[leaves[i].value].children)
      if (std::find(leaves.begin(), leaves.end(), child) == leaves.end())
        leaves.push_back(child);
  Type unit; unit.kind = TypeKind::Unit; auto u = m.types.intern(unit);
  for (auto id : leaves) {
    m.nodes[id.value].type = u;
    m.nodes[id.value].data = UnitLiteral{};
    m.nodes[id.value].children.clear();
  }
  Call replacement;
  replacement.kind = CalleeKind::direct;
  replacement.direct_symbol = sink;
  replacement.signature = sig;
  for (size_t i = 0; i < leaves.size(); ++i)
    replacement.args.push_back({uint32_t(i), uint32_t(i), {}, ParamMode::ordinary, leaves[i]});
  m.nodes[index].type = u;
  m.nodes[index].data = replacement;
  m.nodes[index].children = leaves;
  for (auto &n : m.nodes) { n.errors.clear(); n.exits.clear(); }
}
void erase_value_read(HirModule &m) {
  Type unit; unit.kind = TypeKind::Unit; auto u = m.types.intern(unit);
  bool changed = false;
  for (auto &n : m.nodes)
    if (auto r = std::get_if<ForeignRead>(&n.data);
        r && m.types.find(n.type)->kind == TypeKind::ForeignOpaque) {
      n.type = u; n.data = UnitLiteral{}; n.children.clear(); changed = true;
    }
  need(changed, "foreign value read to erase");
  for (auto &n : m.nodes) { n.errors.clear(); n.exits.clear(); }
}
}
int main(int argc, char **argv) try {
  need(argc==3,"usage root output"); repo=argv[1]; output=argv[2];
  static_assert(int(TypeKind::Vec)==14 && int(TypeKind::ForeignCallable)==15);
  static_assert(int(NodeKind::for_in)==38 && int(NodeKind::foreign_read)==39);
  static_assert(int(CalleeKind::function_value)==1 && int(CalleeKind::foreign)==2);
  auto baseline=accepted(read(repo/"baseline/samples/04_numpy_torch.wz"), "original-04");
  for (const auto &entry:fs::directory_iterator(repo/"tests/python_bindings/fixtures")) {
    auto name=entry.path().filename().string();
    if (name.substr(0,2)>="01" && name.substr(0,2)<="06") accepted(read(entry.path()),name);
  }
  auto error_type = baseline.foreign_bindings.at(0).type;
  need(baseline.types.find(error_type)->kind == TypeKind::ForeignOpaque &&
       baseline.types.find(error_type)->binding == ForeignBindingId{0}, "zero foreign binding");
  need(classify_value(baseline,error_type).kind == ValueKind::foreign_error &&
       classify_value(baseline,error_type).needs_drop && !classify_value(baseline,error_type).pod_inout, "error ownership");
  auto error_vec=accepted(imports+"func f(e py.Error) Unit { let values Vec[py.Error] = [e]; }", "py-error-vec");
  TypeId error_vec_type;
  for(const auto &n:error_vec.nodes)
    if(n.kind()==NodeKind::vec_init) error_vec_type=n.type;
  need(error_vec_type.value &&
       classify_value(error_vec,error_vec_type).kind==ValueKind::vec &&
       classify_value(error_vec,error_vec_type).needs_drop, "py.Error Vec ownership");
  auto error_struct=accepted(imports+"struct Saved { value py.Error; } func f(e py.Error) Unit { let saved = Saved(value: e); }", "py-error-struct");
  TypeId error_struct_type;
  for(const auto &n:error_struct.nodes)
    if(n.kind()==NodeKind::struct_init) error_struct_type=n.type;
  need(error_struct_type.value &&
       classify_value(error_struct,error_struct_type).kind==ValueKind::structure &&
       classify_value(error_struct,error_struct_type).needs_drop, "py.Error struct ownership");
  auto error_enum=accepted(imports+"enum Slot { One(value py.Error); Empty; } func f(e py.Error) Unit { let slot = Slot.One(value: e); }", "py-error-enum");
  TypeId error_enum_type;
  for(const auto &n:error_enum.nodes)
    if(n.kind()==NodeKind::enum_init) error_enum_type=n.type;
  need(error_enum_type.value &&
       classify_value(error_enum,error_enum_type).kind==ValueKind::enumeration &&
       classify_value(error_enum,error_enum_type).needs_drop, "py.Error enum ownership");
  auto error_generic=accepted(imports+"struct Box[T] { value T; } func f(e py.Error) Unit { let boxed = Box[py.Error](value: e); }", "py-error-generic");
  TypeId error_generic_type;
  for(const auto &n:error_generic.nodes)
    if(n.kind()==NodeKind::struct_init) error_generic_type=n.type;
  need(error_generic_type.value &&
       classify_value(error_generic,error_generic_type).kind==ValueKind::structure &&
       classify_value(error_generic,error_generic_type).needs_drop, "py.Error generic ownership");
  auto callable_probe=baseline;
  TypeId callable_type;
  for(const auto &n:callable_probe.nodes)
    if(callable_probe.types.find(n.type)->kind==TypeKind::ForeignCallable) {
      callable_type=n.type; break;
    }
  need(callable_type.value, "foreign callable type");
  Type callable_vec; callable_vec.kind=TypeKind::Vec;
  callable_vec.type_args={callable_type};
  auto callable_vec_type=callable_probe.types.intern(callable_vec);
  need(classify_value(callable_probe,callable_vec_type).kind==ValueKind::unsupported,
       "foreign callable Vec must stay unsupported");
  size_t reads=0, calls=0;
  for (size_t i=0;i<baseline.nodes.size();++i) {
    const auto &n=baseline.nodes[i];
    if (n.kind()==NodeKind::foreign_read || (std::get_if<Call>(&n.data) && std::get<Call>(n.data).binding)) {
      size_t errors=0;
      for (const auto &e:n.exits) if(e.kind==ExitKind::error) {
        ++errors; need(e.error_type==error_type && e.saves_value && !e.actions.empty() &&
          e.actions[0].kind==CleanupKind::save_output && e.actions[0].owner==OwnerRef{OwnerKind::error_output,i}, "own foreign error output");
      }
      need(errors==1,"independent read/call error exit");
    }
    if (n.kind()==NodeKind::foreign_read) {
      ++reads;
      if (baseline.types.find(n.type)->kind==TypeKind::ForeignCallable) {
        need(classify_value(baseline,n.type).kind==ValueKind::foreign_callable &&
             classify_value(baseline,n.type).needs_drop && !classify_value(baseline,n.type).pod_inout,"call target ownership");
        need(std::any_of(baseline.ownership.temporaries.begin(),baseline.ownership.temporaries.end(),[&](const auto &t){return t.producer==HirNodeId{i};}),"call target temporary");
      }
    }
    if (auto c=std::get_if<Call>(&n.data);c&&c->binding) {
      ++calls; need(c->callee && n.children[0]==*c->callee,"read precedes args");
      need(std::any_of(baseline.ownership.calls.begin(),baseline.ownership.calls.end(),[&](const auto &l){
        return l.node==HirNodeId{i} && std::any_of(l.on_return.begin(),l.on_return.end(),[&](const auto &step){return step.owner==OwnerRef{OwnerKind::temporary,c->callee->value} && step.kind==CleanupKind::end_borrow;});}),"target live until return");
    }
  }
  need(reads>calls && calls==18,"original 04 concrete reads/calls");
  auto ci=foreign_call(baseline,"torch.nn.Linear.call");
  auto cr=*std::get<Call>(baseline.nodes[ci].data).callee;
  need(baseline.nodes[cr.value].children.size()==1 &&
       std::get<modules::BindingCallable>(baseline.resolved->project->foreign_binding(*std::get<Call>(baseline.nodes[ci].data).binding).detail).call_kind=="callable", "model object call");
  auto copy=accepted(imports+"func keep(x torch.Tensor) torch.Tensor { let y=x; return y; } func twice(x torch.Tensor) torch.Tensor { var y=x; y=x; return x; } func drop(x torch.Tensor) Unit { let y=x; y; }", "ordinary-copy-return-drop");
  need(std::count_if(copy.ownership.uses.begin(),copy.ownership.uses.end(),[](const auto &u){return u.kind==UseKind::copy;})>=6,"copy records");
  auto optional=accepted(imports+"func f(a np.ndarray) np.ndarray throws py.Error { np.linspace(-1.0,1.0); return np.asarray(a); }", "optional-defaults");
  auto ai=foreign_call(optional,"numpy.asarray.array_dtype");
  need(std::get<Call>(optional.nodes[ai].data).omitted==std::vector<uint32_t>{1},"asarray omission");
  auto named=accepted(imports+"func mark(x i64) i64 throws py.Error { return x; } func f() nn.Linear throws py.Error { return nn.Linear(out_features:mark(2),in_features:mark(1)); }", "named-source-order");
  auto ni=foreign_call(named,"torch.nn.Linear.constructor");
  const auto &nc=std::get<Call>(named.nodes[ni].data);
  need(nc.args[0].source_index==0 && nc.args[0].parameter_index==1 && nc.args[1].parameter_index==0 && nc.omitted==std::vector<uint32_t>({2,3}),"named parameter indices");
  auto prefix=accepted(imports+"func later() f64 throws py.Error { np.linspace(-1.0,1.0); return 2.0; } func f() np.ndarray throws py.Error { return np.add(np.asarray(np.linspace(-1.0,1.0)), later()); }", "failure-prefix");
  auto pi=foreign_call(prefix,"numpy.add.array_scalar"); const auto pc=std::get<Call>(prefix.nodes[pi].data);
  const auto &target_error=prefix.nodes[pc.callee->value].exits[0];
  for(const auto &s:target_error.actions) need(!(s.owner.kind==OwnerKind::temporary && s.owner.identity==pc.args[0].expression.value),"read failure cannot clean unevaluated arg");
  const auto &late_error=prefix.nodes[pc.args[1].expression.value].exits[0];
  need(std::any_of(late_error.actions.begin(),late_error.actions.end(),[&](const auto &s){return s.kind==CleanupKind::destroy_if_live && s.owner==OwnerRef{OwnerKind::temporary,pc.args[0].expression.value};}),"late failure cleans completed prefix");
  accepted(imports+"func caught() Unit { try { np.linspace(-1.0,1.0); } catch e py.Error { let saved=e; throw_again(saved); } } func throw_again(e py.Error) Unit { let copy=e; }", "typed-error-catch-copy");
  accepted(imports+"func rethrow(e py.Error) Unit throws py.Error { throw e; }", "typed-error-throw");
  accepted(imports+"func f() nn.Linear throws py.Error { return nn.Linear(1,1,device:\"cuda:7\"); }", "device-string-runtime-rule");
  accepted(imports+"func f(x torch.Tensor) Unit throws py.Error { (x.backward)(); }", "parenthesized-method-call");
  accepted(imports+"func f() Unit throws py.Error { (np.linspace)(-1.0,1.0); }", "parenthesized-function-call");
  accepted(imports+"func f(m nn.Linear,x torch.Tensor) Unit throws py.Error { (m)(x); }", "parenthesized-callable");
  accepted(imports+"func get(x torch.Tensor) torch.Tensor { return x; } func f(x torch.Tensor) Unit throws py.Error { get(x).backward(); }", "computed-receiver-method");
  rejected(imports+"func f(x torch.Tensor) Unit { let v=(x.backward); }","parenthesized-method-escape","E_CAPABILITY_UNAVAILABLE");
  rejected(imports+"func f(x torch.Tensor) Unit throws py.Error { x.not_declared(); }","unknown-method","E_CAPABILITY_UNAVAILABLE");
  auto erased_method=accepted(imports+"func sink(a Unit,b Unit) Unit { } func f(x torch.Tensor) Unit throws py.Error { x.backward(); }", "erased-method-control");
  bad(erased_method,"erased-method-call",[&](auto &m){erase_foreign_call(m,"torch.Tensor.backward");},true);
  auto erased_callable=accepted(imports+"func sink(a Unit,b Unit,c Unit) Unit { } func f(m nn.Linear,x torch.Tensor) Unit throws py.Error { m(x); }", "erased-callable-control");
  bad(erased_callable,"erased-callable",[&](auto &m){erase_foreign_call(m,"torch.nn.Linear.call");},true);
  auto erased_read=accepted(imports+"func f() Unit throws py.Error { np.float32; }", "erased-value-read-control");
  bad(erased_read,"erased-value-read",[&](auto &m){erase_value_read(m);},true);
  auto factory_method=accepted(imports+"func sink(a Unit,b Unit,c Unit) Unit { } func f(g func() torch.Tensor) Unit throws py.Error { g().backward(); }", "function-value-method-control");
  need(check_backend_capabilities(factory_method).success(),"function value returning external object backend support");
  bad(factory_method,"erased-function-value-method",[&](auto &m){erase_foreign_call(m,"torch.Tensor.backward");},true);
  auto factory_callable=accepted(imports+"func sink(a Unit,b Unit,c Unit,d Unit) Unit { } func f(g func() nn.Linear,x torch.Tensor) Unit throws py.Error { g()(x); }", "function-value-callable-control");
  need(check_backend_capabilities(factory_callable).success(),"function value returning external object backend support 2");
  bad(factory_callable,"erased-function-value-callable",[&](auto &m){erase_foreign_call(m,"torch.nn.Linear.call");},true);
  std::vector<std::pair<std::string,std::string>> negatives={
    {"07", "E_CALL_ARGUMENT"},{"08","E_CAPABILITY_UNAVAILABLE"},{"09","E_TYPE_MISMATCH"},
    {"10","E_CALL_ARGUMENT"},{"11","E_CAPABILITY_UNAVAILABLE"},{"12","E_CAPABILITY_UNAVAILABLE"}};
  for(const auto &[prefix,code]:negatives)
    for(const auto &entry:fs::directory_iterator(repo/"tests/python_bindings/fixtures"))
      if(entry.path().filename().string().starts_with(prefix)) rejected(read(entry.path()),entry.path().filename().string(),code);
  const std::vector<std::pair<std::string,std::string>> bad_args={
    {"np.asarray()","E_CALL_ARGUMENT"}, {"np.linspace(-1.0,1.0,num:2,num:3)","E_CALL_ARGUMENT"},
    {"np.multiply(x1:np.linspace(-1.0,1.0),x2:2.0)","E_CALL_ARGUMENT"},
    {"torch.tensor(np.linspace(-1.0,1.0),torch.float32)","E_CALL_ARGUMENT"},
    {"np.asarray(np.linspace(-1.0,1.0),dtype:torch.float32)","E_TYPE_MISMATCH"}};
  for(size_t i=0;i<bad_args.size();++i) rejected(imports+"func f() Unit throws py.Error { "+bad_args[i].first+"; }","argument-reject-"+std::to_string(i),bad_args[i].second);
  rejected(imports+"func f() Unit { np.float32; }","unhandled-read","E_EFFECT_UNHANDLED");
  rejected(imports+"func f() Unit { np.linspace(-1.0,1.0); }","unhandled-call","E_EFFECT_UNHANDLED");
  // Behavior update: these three sources were formerly rejected before the
  // shared ownership rules handled declared Python host references.  Keep the
  // original source bytes and assert their now-defined copy/move behavior.
  auto foreign_vec = accepted(imports+"func f(x torch.Tensor) Unit { let v=[x]; }","foreign-vec");
  auto foreign_struct = accepted(imports+"struct S { value torch.Tensor; }","foreign-struct");
  auto foreign_move = accepted(imports+"func f(x torch.Tensor) Unit { let v=x; let z=move v; }","foreign-move-local");
  auto foreign_modes = accepted(imports+"func relay[T](value move T) T { return move value; } func keep(value move torch.Tensor) torch.Tensor { return value; } func apply(cb func(move torch.Tensor) torch.Tensor,value move torch.Tensor) torch.Tensor { return cb(move value); } func two(a move torch.Tensor,b move torch.Tensor) torch.Tensor { let a2=a; let b2=b; let x=move a2; return x; } func use() Unit throws py.Error { let x=torch.tensor(np.linspace(-1.0,1.0),dtype:torch.float32,device:\"cpu\"); let y=relay[torch.Tensor](move x); let z=keep(move y); let f=keep; let w=apply(f,move z); }","foreign-move-modes");
  auto foreign_generic_impl = accepted(imports+"struct Box[T]{value T;} impl[T] Box[T]{func keep[U](self Self,value move U)U{return move value;}} func use(x torch.Tensor)torch.Tensor{let local=x;let b=Box[torch.Tensor](value:local);return b.keep[torch.Tensor](move local);}","foreign-generic-impl");
  need(check_backend_capabilities(foreign_modes).success(),"Python/64 foreign move modes");
  need(check_backend_capabilities(foreign_generic_impl).success(),"Python/64 foreign generic impl move");
  auto native_foreign=foreign_vec;
  auto foreign_native_resolved=std::make_shared<modules::ResolvedProject>(*native_foreign.resolved);
  auto foreign_native_project=std::make_shared<modules::Project>(*foreign_native_resolved->project);
  foreign_native_project->backend="native";foreign_native_resolved->project=foreign_native_project;
  native_foreign.resolved=foreign_native_resolved;native_foreign.config.backend=Backend::native;
  auto rejected_native=check_backend_capabilities(native_foreign);
  need(!rejected_native.success()&&has(rejected_native,"E_CAPABILITY_UNAVAILABLE"),
       "native foreign Vec/move not rejected");
  auto native_error_vec=error_vec;
  auto native_error_resolved=std::make_shared<modules::ResolvedProject>(*native_error_vec.resolved);
  auto native_error_project=std::make_shared<modules::Project>(*native_error_resolved->project);
  native_error_project->backend="native";native_error_resolved->project=native_error_project;
  native_error_vec.resolved=native_error_resolved;native_error_vec.config.backend=Backend::native;
  auto rejected_native_error=check_backend_capabilities(native_error_vec);
  need(!rejected_native_error.success()&&has(rejected_native_error,"E_CAPABILITY_UNAVAILABLE"),
       "native py.Error Vec not rejected");
  auto profile32=foreign_vec;
  auto profile32_resolved=std::make_shared<modules::ResolvedProject>(*profile32.resolved);
  auto profile32_project=std::make_shared<modules::Project>(*profile32_resolved->project);
  profile32_project->word_bits=32;profile32_resolved->project=profile32_project;
  profile32.resolved=profile32_resolved;profile32.config.word_bits=32;
  auto rejected_32=check_backend_capabilities(profile32);
  need(!rejected_32.success()&&has(rejected_32,"E_CAPABILITY_UNAVAILABLE"),
       "32-bit foreign Vec/move not rejected: "+diagnostics(rejected_32));
  TypeId vec_type;
  for(const auto &n:foreign_vec.nodes)
    if(n.kind()==NodeKind::vec_init) vec_type=n.type;
  need(vec_type.value && classify_value(foreign_vec,vec_type).kind==ValueKind::vec,"foreign Vec ownership");
  need(classify_value(foreign_struct,foreign_struct.nominals.at(0).fields.at(0).type).kind==ValueKind::foreign_reference,"foreign field ownership");
  const auto &two = named_function(foreign_modes,"two");
  size_t two_move = foreign_modes.nodes.size();
  for(size_t i=0;i<foreign_modes.nodes.size();++i) {
    const auto &n=foreign_modes.nodes[i];
    if(n.kind()==NodeKind::move &&
       foreign_modes.scopes.at(n.scope.value).function==two.symbol) {
      two_move=i; break;
    }
  }
  need(two_move<foreign_modes.nodes.size(),"foreign local move node");
  bad(foreign_modes,"foreign-move-source",[&](auto &m){
    std::get<Move>(m.nodes[two_move].data).binding=named_function(m,"two").parameters.at(0).symbol;
  },true);
  size_t keep_call=foreign_modes.nodes.size();
  for(size_t i=0;i<foreign_modes.nodes.size();++i) {
    auto c=std::get_if<Call>(&foreign_modes.nodes[i].data);
    if(c && c->direct_symbol &&
       m_name(foreign_modes,*c->direct_symbol)=="keep" && !c->args.empty() &&
       c->args[0].mode==ParamMode::move) { keep_call=i; break; }
  }
  need(keep_call<foreign_modes.nodes.size(),"foreign move call node");
  bad(foreign_modes,"foreign-call-mode",[&](auto &m){
    std::get<Call>(m.nodes[keep_call].data).args[0].mode=ParamMode::ordinary;
  },true);
  size_t relay_instance=foreign_modes.instances.size();
  for(size_t i=0;i<foreign_modes.instances.size();++i)
    if(m_name(foreign_modes,foreign_modes.instances[i].origin)=="relay") {
      relay_instance=i; break;
    }
  need(relay_instance<foreign_modes.instances.size(),"foreign generic instance");
  bad(foreign_modes,"foreign-instance-substitution",[&](auto &m){
    Type i64; i64.kind=TypeKind::SignedInt; i64.bits=64;
    m.instances[relay_instance].type_args.at(0)=m.types.intern(i64);
  },true);
  bad(foreign_struct,"foreign-field-type",[&](auto &m){
    Type i64; i64.kind=TypeKind::SignedInt; i64.bits=64;
    m.nominals.at(0).fields.at(0).type=m.types.intern(i64);
  },true);
  bad(foreign_vec,"foreign-vec-element-type",[&](auto &m){
    Type i64; i64.kind=TypeKind::SignedInt; i64.bits=64;
    Type vec; vec.kind=TypeKind::Vec; vec.type_args={m.types.intern(i64)};
    for(auto &n:m.nodes)
      if(n.kind()==NodeKind::vec_init) { n.type=m.types.intern(vec); break; }
  },true);
  rejected(imports+"func f(x torch.Tensor) Unit { let v=move x; }","foreign-move-borrowed","E_CALL_ARGUMENT");
  rejected(imports+"func f() Unit throws py.Error { let v=np.linspace(-1.0,1.0); let z=move v; let q=v; }","foreign-read-after-move","E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(imports+"func f() Unit throws py.Error { let v=np.linspace(-1.0,1.0); let z=move v; let q=move v; }","foreign-double-move","E_OWNERSHIP_USE_AFTER_MOVE");
  rejected(imports+"struct S { value torch.Tensor; } func f(s S) Unit { let v=move s.value; }","foreign-partial-move","E_CAPABILITY_UNAVAILABLE");
  rejected(imports+"func f() Unit { let v=np.linspace; }","foreign-function-escape","E_CAPABILITY_UNAVAILABLE");
  rejected(imports+"func f(x torch.Tensor) Unit { x[0]; }","foreign-index","E_CAPABILITY_UNAVAILABLE");
  rejected(imports+"func f(x torch.Tensor) Unit { x.device; }","foreign-data-member","E_CAPABILITY_UNAVAILABLE");
  rejected(imports+"func f(x torch.Tensor) Unit { x = x; }","ordinary-parameter-reassign","E_TYPE_MISMATCH");
  auto native=sema::check_types(resolve(read(repo/"baseline/samples/04_numpy_torch.wz"),"native-original", "", "native"));
  need(!native.success()&&has(native,"E_CAPABILITY_UNAVAILABLE"),"native external source");
  auto resolved=resolve(read(repo/"baseline/samples/04_numpy_torch.wz"),"project-api");
  need(sema::check_project(resolved,{}, {}, VerifyMode::static_only).success(),"shared check_project static");
  auto default_checked=sema::check_project(resolved); need(default_checked.success(),"default backend check_project for supported Python 04");
  PhaseResult<modules::ResolvedProject> phase; phase.state=PhaseState::complete; phase.value=*resolved;
  need(sema::check_project(phase,{}, {}, VerifyMode::static_only).success(),"phase check_project static");
  need(sema::check_project(phase).success(),"phase default backend for supported Python 04");
  auto typed=sema::check_types(resolved); need(typed.success(),"freeze builder");
  auto frozen=verify(*typed.value,{},VerifyMode::static_only); need(frozen.success(),"freeze token");
  auto before=dump(*frozen.value);
  auto project_alias=std::make_shared<modules::Project>(*resolved->project);
  resolved->project=project_alias; typed.value->resolved=resolved;
  auto frozen_alias=verify(*typed.value,{},VerifyMode::static_only);need(frozen_alias.success(),"freeze retained project alias");
  auto alias_before=dump(*frozen_alias.value);
  typed.value->foreign_bindings[0].type=TypeId{0}; typed.value->nodes.clear();
  project_alias->foreign_bindings[0].identity="changed"; project_alias->bindings[0].json="changed";
  resolved->symbols.clear();
  need(dump(*frozen.value)==before && dump(*frozen_alias.value)==alias_before && verify_types(frozen_alias.value->module()).success(),"deep frozen foreign/project aliases");
  auto ai_good=ai;
  bad(optional,"binding-index",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).binding=ForeignBindingId{999}; });
  bad(optional,"wrong-binding",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).binding=ForeignBindingId{0}; });
  bad(optional,"signature",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).signature=m.functions[0].signature; });
  bad(optional,"missing-omitted",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).omitted.clear(); });
  bad(optional,"duplicate-omitted",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).omitted.push_back(1); });
  bad(optional,"source-index",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).args[0].source_index=1; },true);
  bad(optional,"parameter-index",[&](auto &m){ std::get<Call>(m.nodes[ai_good].data).args[0].parameter_index=1; },true);
  bad(named,"swapped-source-inputs",[&](auto &m){auto &c=std::get<Call>(m.nodes[ni].data); std::swap(c.args[0].expression,c.args[1].expression);std::swap(m.nodes[ni].children[1],m.nodes[ni].children[2]);},true);
  bad(named,"swapped-parameter-indices",[&](auto &m){auto &c=std::get<Call>(m.nodes[ni].data); std::swap(c.args[0].parameter_index,c.args[1].parameter_index);},true);
  bad(named,"target-after-argument",[&](auto &m){std::swap(m.nodes[ni].children[0],m.nodes[ni].children[1]);},true);
  bad(baseline,"owner-type",[&](auto &m){auto &b=m.foreign_bindings[std::get<ForeignRead>(m.nodes[cr.value].data).binding.value];b.owner=error_type;});
  bad(baseline,"read-binding",[&](auto &m){std::get<ForeignRead>(m.nodes[cr.value].data).binding=ForeignBindingId{0};});
  bad(baseline,"read-error-type",[&](auto &m){m.foreign_bindings[0].read_error=m.foreign_bindings[1].type;});
  bad(baseline,"missing-read-error",[&](auto &m){m.nodes[cr.value].exits.clear();});
  bad(baseline,"missing-read-effect",[&](auto &m){m.nodes[cr.value].errors.clear();},true);
  bad(baseline,"missing-call-error",[&](auto &m){m.nodes[ci].exits.clear();});
  bad(baseline,"missing-target-temporary",[&](auto &m){std::erase_if(m.ownership.temporaries,[&](auto &t){return t.producer==cr;});});
  bad(baseline,"missing-target-borrow-end",[&](auto &m){std::erase_if(m.ownership.calls,[&](auto &l){return l.node==HirNodeId{ci};});});
  bad(prefix,"wrong-failure-prefix",[&](auto &m){m.nodes[pc.callee->value].exits[0].actions=late_error.actions;});
  // Test-only declaration variants; the original bundle and fixtures stay untouched.
  auto variant = [&](bool unit, bool ambiguous) {
    auto text=read(repo/"contracts/python_bindings.example.json");
    auto json=json_tokener_parse(text.c_str()); need(json,"test JSON parse");
    auto list=json_object_object_get(json,"callables");
    for(size_t i=0;i<json_object_array_length(list);++i) {
      auto call=json_object_array_get_idx(list,i);
      if(std::string(json_object_get_string(json_object_object_get(call,"id")))!="torch.manual_seed")continue;
      if(unit) {
        json_object_object_add(call,"return_type",json_object_new_string("Unit"));
        json_object_object_add(call,"error_type",nullptr);
        auto own=json_object_object_get(call,"ownership");
        json_object_object_add(own,"return",json_object_new_string("unit"));
      } else {
        auto other=json_tokener_parse(json_object_to_json_string(call));
        json_object_object_add(other,"id",json_object_new_string("torch.manual_seed.alternative"));
        if(!ambiguous) {
          auto param=json_object_array_get_idx(json_object_object_get(other,"parameters"),0);
          json_object_object_add(param,"type",json_object_new_string("f64"));
        } else {
          auto param=json_tokener_parse(R"({"name":"extra","type":"bool","kind":"positional_or_keyword","optional":true,"mode":"ordinary","escapes":"no"})");
          json_object_array_add(json_object_object_get(other,"parameters"),param);
        }
        json_object_array_add(list,other);
      }
      break;
    }
    auto out=std::string(json_object_to_json_string_ext(json,JSON_C_TO_STRING_PRETTY));json_object_put(json);return out;
  };
  // Access declarations share the same binding projection table.  This checks
  // the extended foreign.hpp helper directly: property/indexer/slice records
  // occupy their declaration slots and keep the declared result type.
  auto access_bundle = [&] {
    auto text=read(repo/"contracts/python_bindings.example.json");
    auto json=json_tokener_parse(text.c_str()); need(json,"access JSON parse");
    auto property=json_tokener_parse(R"({"id":"py.torch.Tensor.device.projection","module":"torch","owner_type":"py.torch.Tensor","name":"device","type":"string","getter":["device"],"setter":null,"error_type":"lang.py.Error"})");
    auto properties=json_object_new_array(); json_object_array_add(properties,property);
    json_object_object_add(json,"properties",properties);
    auto indexer=json_tokener_parse(R"({"id":"py.torch.Tensor.index.projection","module":"torch","owner_type":"py.torch.Tensor","index_type":"i64","result_type":"py.torch.Tensor","readable":true,"writable":false,"error_type":"lang.py.Error"})");
    auto indexers=json_object_new_array(); json_object_array_add(indexers,indexer);
    json_object_object_add(json,"indexers",indexers);
    auto slice=json_tokener_parse(R"({"id":"py.torch.Tensor.slice.projection","module":"torch","owner_type":"py.torch.Tensor","start_type":"i64","end_type":"i64","result_type":"py.torch.Tensor","readable":true,"writable":false,"error_type":"lang.py.Error"})");
    auto slices=json_object_new_array(); json_object_array_add(slices,slice);
    json_object_object_add(json,"slices",slices);
    auto out=std::string(json_object_to_json_string_ext(json,JSON_C_TO_STRING_PRETTY));json_object_put(json);return out;
  };
  auto projection=accepted(imports+"func f(x torch.Tensor) Unit throws py.Error { let a=x[0]; let b=x[0:1]; let c=x.device; }","access-projection",access_bundle());
  auto projected=detail::binding_records(*projection.resolved->project,projection.types);
  need(projected.size()==projection.resolved->project->foreign_bindings.size(),"access projection size");
  size_t access_records=0;
  for(size_t i=0;i<projection.resolved->project->foreign_bindings.size();++i) {
    const auto &d=projection.resolved->project->foreign_bindings[i];
    if(std::holds_alternative<modules::BindingProperty>(d.detail) ||
       std::holds_alternative<modules::BindingIndexer>(d.detail) ||
       std::holds_alternative<modules::BindingSlice>(d.detail)) {
      need(projected[i].type==projection.foreign_bindings[i].type,"access projection type");
      need(!projected[i].signature && !projected[i].owner,"access projection signature/owner");
      ++access_records;
    }
  }
  need(access_records==3,"access projection record count");
  auto overload=accepted(imports+"func f() Unit throws py.Error { torch.manual_seed(7); }", "overload-integer",variant(false,false));
  need(overload.resolved->project->foreign_binding(*std::get<Call>(overload.nodes[foreign_call(overload,"torch.manual_seed")].data).binding).identity=="torch.manual_seed","selected overload identity");
  accepted(imports+"func f() Unit throws py.Error { torch.manual_seed(7.0); }", "overload-float",variant(false,false));
  auto ambiguous=sema::check_types(resolve(imports+"func f() Unit throws py.Error { torch.manual_seed(7); }","ambiguous-overload",variant(false,true)));
  need(!ambiguous.success()&&has(ambiguous,"E_CALL_ARGUMENT"),"ambiguous overload rejected");
  ++probes;std::cout<<"passed ambiguous-overload\n";
  bad(overload,"nonunique-candidate-recomputed",[&](auto &m){
    m.resolved=resolve(imports+"func f() Unit throws py.Error { torch.manual_seed(7); }","bad-ambiguous-hir",variant(false,true));
    m.foreign_bindings=detail::binding_records(*m.resolved->project,m.types);
  },true);
  auto unit=accepted(imports+"func main() Unit { try { torch.manual_seed(7); } catch e py.Error { } }","unit-scalar-call",variant(true,false));
  auto ui=foreign_call(unit,"torch.manual_seed");
  const auto &uc=std::get<Call>(unit.nodes[ui].data);
  need(unit.types.find(unit.nodes[ui].type)->kind==TypeKind::Unit &&
       !unit.types.find(uc.signature)->throws_type &&
       unit.types.find(unit.nodes[uc.args[0].expression.value].type)->kind==TypeKind::SignedInt,"pure scalar Unit foreign call");
  auto unsupported=check_backend_capabilities(unit);
  need(!unsupported.success()&&has(unsupported,"E_CAPABILITY_UNAVAILABLE")&&
       unsupported.diagnostics[0].message=="Python foreign callable requires declared lang.py.Error","Unit foreign call backend rejection");
  auto token=verify(unit,{},VerifyMode::static_only);need(token.success(),"Unit static token");
  auto emitted=backend::python::emit(*token.value,unit.functions[0].symbol);
  need(!emitted.success()&&!emitted.value&&has(emitted,"E_CAPABILITY_UNAVAILABLE"),"static token cannot emit foreign Unit");
#ifdef LANG_T10_NATIVE
  auto lowered=ir::lower(*token.value);
  need(!lowered.success()&&!lowered.value&&has(lowered,"E_CAPABILITY_UNAVAILABLE"),"static Python token cannot lower as native");
#endif
  auto native_builder=unit;
  auto native_resolved=std::make_shared<modules::ResolvedProject>(*unit.resolved);
  auto native_project=std::make_shared<modules::Project>(*native_resolved->project);
  native_project->backend="native";native_resolved->project=native_project;native_builder.resolved=native_resolved;native_builder.config.backend=Backend::native;
  auto native_static=verify(native_builder,{},VerifyMode::static_only);
  need(!native_static.success()&&has(native_static,"E_CAPABILITY_UNAVAILABLE"),"static native cannot bypass allowed_backends");
  auto owners=accepted(imports+"func f(a nn.Linear,b nn.Linear) Unit throws py.Error { a.parameters(); }","two-owner-controls");
  auto owner_call=foreign_call(owners,"torch.nn.Linear.parameters");
  auto owner_target=*std::get<Call>(owners.nodes[owner_call].data).callee;
  bad(owners,"receiver-owner-source",[&](auto &m){
    auto receiver=m.nodes[owner_target.value].children[0];
    std::get<Read>(m.nodes[receiver.value].data).symbol=m.functions[0].parameters[1].symbol;
  },true);
  bad(baseline,"callee-result-instead-of-read",[&](auto &m){m.nodes[cr.value].data=UnitLiteral{};});
  bad(baseline,"faked-ordinary-call",[&](auto &m){std::get<Call>(m.nodes[ci].data).kind=CalleeKind::function_value;});
  bad(baseline,"bad-call-error-target",[&](auto &m){m.nodes[ci].exits[0].target=FunctionExit{SymbolId{0}};});
  bad(baseline,"duplicated-owning-output",[&](auto &m){m.nodes[ci].exits[0].actions.insert(m.nodes[ci].exits[0].actions.begin(),m.nodes[ci].exits[0].actions[0]);});
  auto cancelled_source=resolve(imports+"func main() Unit {}","cancelled");
  std::stop_source stop;stop.request_stop();
  need(sema::check_project(cancelled_source,{},stop.get_token(),VerifyMode::static_only).state==PhaseState::cancelled,"static cancellation");
  PhaseResult<modules::ResolvedProject> failed;failed.state=PhaseState::failed;failed.value=*cancelled_source;
  need(!sema::check_project(failed,{}, {},VerifyMode::static_only).success(),"failed phase stays failed");
  ++probes;std::cout<<"passed public-api-backend-freeze-controls\n";
  std::cout << "passed probes=" << probes << '\n';
  return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }

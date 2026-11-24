#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace lang;
using namespace lang::hir;
namespace fs = std::filesystem;
namespace {
void need(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}
std::string diagnostics(const auto &result) {
  std::string text;
  for (const auto &d : result.diagnostics) {
    text += d.code + ": " + d.message;
    if (d.primary) text += " @" + std::to_string(d.primary->begin_byte);
    text += "\n";
  }
  return text;
}
bool has(const auto &result, std::string_view code) {
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &d) { return d.code == code; });
}
struct Fixture {
  fs::path path;
  SourceManager sources;
  Fixture() {
    char pattern[] = "/tmp/woozy-values-XXXXXX";
    auto *created = mkdtemp(pattern); need(created, "mkdtemp failed"); path = created;
    write("lang.toml", "schema_version=1\n[package]\nname=\"values\"\nsource_root=\"src\"\n"
                       "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
  }
  ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
  void write(const fs::path &name, const std::string &text) {
    fs::create_directories((path/name).parent_path());
    std::ofstream out(path/name); out << text; need(bool(out), "fixture write failed");
  }
  auto check(const std::string &source, const fs::path &stdlib, Backend backend=Backend::native) {
    write("lang.toml", "schema_version=1\n[package]\nname=\"values\"\nsource_root=\"src\"\n"
                       "entry=\"app\"\n[build]\nbackend=\"" +
                       std::string(backend==Backend::python?"python":"native") +
                       "\"\nword_bits=64\n");
    write("src/main.wz", source);
    auto loaded = modules::load_project({path/"lang.toml", stdlib, {}}, sources);
    if (!loaded.success()) {
      PhaseResult<HirModule> failed;
      failed.diagnostics = loaded.diagnostics;
      return failed;
    }
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*loaded.value)));
    return sema::check_types(resolved, Config{backend,64});
  }
};
const char *program = R"wz(package app;
import "std/algo" as algo;
import "std/text" as text;
import "std/vec" as vec;
import "std/option" as option;
import "std/result" as result;
struct Box[T] { value T; }
enum Error { Message(value string); }
func identity[T](value T) T { return value; }
func fail(value string) throws Error { throw Error.Message(value); }
func main() i32 throws text.Error {
    const label string = "literal";
    var s = "A中😀e\u{301}\0";
    s += "Z";
    let joined = s + identity[string](label);
    let eq = s == joined;
    let ne = s != joined;
    let lt = s < joined;
    let le = s <= joined;
    let gt = s > joined;
    let ge = s >= joined;
    let first u32 = joined[0];
    let second u32 = joined[1];
    let part = joined[1:4];
    let tail = joined[1:];
    let head = joined[:4];
    let full = joined[:];
    for rune in identity[string](part) { let point u32 = rune; }
    let values = vec.empty[string]();
    vec.reserve[string](values, 4);
    vec.push[string](values, part);
    let length = vec.len[string](values);
    vec.extend[string](values, values);
    let capacity = vec.capacity[string](values);
    let filled = vec.make[Box[string]](2, Box[string](value: joined));
    vec.resize[string](values, 3, joined);
    switch vec.pop[string](values) {
        case option.Option.None: {}
        case option.Option.Some(value): { let kept = identity[string](value); }
    }
    vec.clear[string](values);
    let bytes = text.bytes(joined);
    let decoded = text.from_bytes(bytes);
    let rune = text.from_rune(0x1f600);
    let codepoints = text.len(decoded);
    let byte_count = text.byte_len(decoded);
    let chunks = text.split(decoded,"中");
    let occurrences = text.count(decoded,"中");
    let folded = algo.fold[string,usize](chunks, 0,
        func(total usize, value string) usize { return total + text.len(value); });
    let result_value = result.Result[string,i32].Ok(decoded);
    switch result_value {
        case result.Result.Ok(value): { let kept = value; }
        case result.Result.Err(error): {}
    }
    try { fail(decoded); } catch error Error {
        switch error { case Error.Message(value): { let kept = value; } }
    }
    defer { let kept = decoded; }
    return 0;
}
)wz";
HirNode &find(HirModule &module, NodeKind kind, const auto &predicate) {
  for (auto &node : module.nodes)
    if (node.kind() == kind && predicate(node)) return node;
  throw std::runtime_error("wanted HIR node missing");
}
Call &system(HirModule &module, SystemOperation op) {
  for (auto &node : module.nodes)
    if (auto *call = std::get_if<Call>(&node.data); call && call->system == op) return *call;
  throw std::runtime_error("wanted system call missing");
}
void bad(const HirModule &good, const char *name, const auto &edit) {
  auto changed = good; edit(changed);
  auto verified = verify_types(changed);
  need(!verified.success() && has(verified,"E_IR_INVALID"),
       std::string(name) + " accepted\n" + diagnostics(verified));
  auto rebuilt = detail::produce_ownership(changed,{});
  if (rebuilt.success()) {
    verified = verify_types(changed);
    need(!verified.success() && has(verified,"E_IR_INVALID"),
         std::string(name) + " accepted after ownership rebuild\n" + diagnostics(verified));
  }
}
void deny(const fs::path &stdlib, const std::string &source, std::string_view code) {
  Fixture fixture; auto result=fixture.check(source,stdlib);
  need(!result.success() && has(result,code),
       "wrong source rejection\n"+diagnostics(result));
  need(!has(result,"E_INTERNAL_ERROR"),"source failure became internal error");
}
void suite(const fs::path &root) {
  Fixture fixture;
  auto checked=fixture.check(program,root/"stdlib");
  need(checked.success(),"value library check failed\n"+diagnostics(checked));
  auto good=*checked.value;
  auto verified=verify_types(good);
  need(verified.success(),"value library verify failed\n"+diagnostics(verified));
  need(check_backend_capabilities(good).success(),"Native value capabilities rejected");
  Fixture python_fixture;
  auto python=python_fixture.check(program,root/"stdlib",Backend::python);
  need(python.success(),"Python static checking failed\n"+diagnostics(python));
  auto python_capabilities=check_backend_capabilities(*python.value);
  need(python_capabilities.success(),"Python value capabilities rejected\n"+diagnostics(python_capabilities));
  std::set<SystemOperation> operations;
  for(const auto &node:good.nodes)
    if(auto *call=std::get_if<Call>(&node.data);call&&call->system) operations.insert(*call->system);
  for(const auto &spec:value_system_operations)
    if (spec.owner == "std/text" || spec.owner == "std/vec")
      need(operations.count(spec.operation),
           "text/Vec operation missing from typed HIR: " + std::string(spec.name));
  bool concrete_string=false;
  for(const auto &node:good.nodes)
    if(auto *call=std::get_if<Call>(&node.data);call&&call->system==SystemOperation::vec_push)
      for(auto type:call->type_args) concrete_string|=good.types.find(type)->kind==TypeKind::String;
  need(concrete_string,"generic Vec type arguments were not substituted");
  auto string_node=[&](const HirModule &m,const HirNode &n) {
    return m.types.find(m.nodes.at(n.children[0].value).type)->kind==TypeKind::String;
  };
  bad(good,"string multiplication",[&](auto &m){
    auto &n=find(m,NodeKind::binary,[&](auto &n){return string_node(m,n);});
    std::get<Binary>(n.data).op=BinaryOp::multiply;
  });
  bad(good,"invalid string operator",[&](auto &m){
    auto &n=find(m,NodeKind::binary,[&](auto &n){return string_node(m,n);});
    std::get<Binary>(n.data).op=static_cast<BinaryOp>(255);
  });
  bad(good,"string index result width",[&](auto &m){
    auto &n=find(m,NodeKind::index_read,[&](auto &n){return string_node(m,n);});
    Type type;type.kind=TypeKind::UnsignedInt;type.bits=64;n.type=m.types.intern(type);
  });
  bad(good,"mutable string index",[&](auto &m){
    auto &n=find(m,NodeKind::index_read,[&](auto &n){return string_node(m,n);});n.data=IndexPlace{};
  });
  bad(good,"index source substitution",[&](auto &m){
    std::vector<HirNode*> indices;
    for(auto &n:m.nodes) if(n.kind()==NodeKind::index_read&&string_node(m,n))indices.push_back(&n);
    need(indices.size()>=2,"two source indices missing");indices[0]->children[1]=indices[1]->children[1];
  });
  bad(good,"slice endpoint substitution",[&](auto &m){
    auto &n=find(m,NodeKind::vec_slice,[&](auto &n){auto &s=std::get<VecSlice>(n.data);return s.start&&s.end&&string_node(m,n);});
    auto &s=std::get<VecSlice>(n.data);std::swap(s.start,s.end);std::swap(n.children[1],n.children[2]);
  });
  bad(good,"same-signature system substitution",[](auto &m){
    auto &call=system(m,SystemOperation::text_len);
    call.system=SystemOperation::text_byte_len;
    call.direct_symbol=system(m,SystemOperation::text_byte_len).direct_symbol;
  });
  bad(good,"generic source type substitution",[](auto &m){
    auto &node=find(m,NodeKind::call,[](auto &n){return std::get<Call>(n.data).system==SystemOperation::vec_empty;});
    auto &call=std::get<Call>(node.data);Type type;type.kind=TypeKind::SignedInt;type.bits=32;
    call.type_args={m.types.intern(type)};
    call.signature=system_operation_signature(m.types,m,SystemOperation::vec_empty,call.type_args);
    node.type=m.types.find(call.signature)->result;
  });
  bad(good,"system builtin identity",[](auto &m){system(m,SystemOperation::text_len).direct_symbol=system(m,SystemOperation::text_byte_len).direct_symbol;});
  bad(good,"invalid decoded UTF-8",[](auto &m){
    auto &node=find(m,NodeKind::string_literal,[](auto &){return true;});
    std::get<StringLiteral>(node.data).utf8=std::string(1,static_cast<char>(0x80));
  });
  deny(root/"stdlib","package app; func main() i32 { let s=\"a\"-\"b\"; return 0; }","E_TYPE_MISMATCH");
  deny(root/"stdlib","package app; func main() i32 { let s=\"a\"+1; return 0; }","E_TYPE_MISMATCH");
  deny(root/"stdlib","package app; func main() i32 { var s=\"a\"; s[0]=65; return 0; }","E_TYPE_MISMATCH");
  deny(root/"stdlib","package app; func main() i32 { let s=\"a\"; let x=s[true]; return 0; }","E_TYPE_MISMATCH");
  deny(root/"stdlib","package app; func main() i32 { for x in \"a\" { let y i32=x; } return 0; }","E_TYPE_MISMATCH");
  deny(root/"stdlib","package app; func main() i32 { const s=\"abc\"[0:1]; return 0; }","E_CAPABILITY_UNAVAILABLE");
  deny(root/"stdlib","package app; func consume(value move string) string { return value; } func main() i32 { var s=\"a\"; s+=consume(move s); return 0; }","E_CALL_ARGUMENT");
  deny(root/"stdlib","package app; struct Box { value string; } func main() i32 { var b=Box(value:\"a\"); b.value+=\"b\"; return 0; }","E_CAPABILITY_UNAVAILABLE");
  deny(root/"stdlib","package app; func main() i32 { let n=__woozy_text_len(\"a\"); return 0; }","E_CAPABILITY_UNAVAILABLE");
  deny(root/"stdlib","package app; func main() i32 { let bits=__woozy_num_f32_bits(f32(1.0)); return 0; }","E_CAPABILITY_UNAVAILABLE");
  deny(root/"stdlib","package app; func main() i32 { let bits=__woozy_num_f64_bits(1.0); return 0; }","E_CAPABILITY_UNAVAILABLE");
  deny(root/"stdlib","package app; func main() i32 { let value=__woozy_math_f64_from_bits(u64(0)); return 0; }","E_CAPABILITY_UNAVAILABLE");
  Fixture malformed;
  malformed.write("stdlib/vec/vec.wz","package vec; pub func bad() { __woozy_vec_empty[i32,i32](); }");
  auto arity=malformed.check("package app; import \"std/vec\" as vec; func main() i32 { vec.bad(); return 0; }",malformed.path/"stdlib");
  need(!arity.success()&&has(arity,"E_CALL_ARGUMENT"),"generic system arity was accepted\n"+diagnostics(arity));
  Fixture literal;
  literal.write("src/main.wz","package app; func main() i32 { let s=\"valid\"; return 0; }");
  auto loaded=modules::load_project({literal.path/"lang.toml",root/"stdlib",{}},literal.sources);
  need(loaded.success(),"UTF-8 fixture load failed");
  auto project=std::make_shared<modules::Project>(std::move(*loaded.value));
  for(auto &module:project->modules) for(auto &file:module.files) {
    auto syntax=std::make_shared<AstFile>(*file);
    for(auto &token:syntax->tokens)
      if(token.kind==TokenKind::string_literal) token.string_value=std::string(1,static_cast<char>(0x80));
    file=syntax;
  }
  auto resolved=modules::resolve_project(project);
  auto invalid=sema::check_types(resolved);
  need(!invalid.success()&&has(invalid,"E_TYPE_MISMATCH")&&!has(invalid,"E_IR_INVALID")&&!has(invalid,"E_INTERNAL_ERROR"),
       "invalid decoded literal was not a source diagnostic\n"+diagnostics(invalid));
  Fixture missing;
  missing.write("stdlib/text/text.wz","package text; pub func bad(value u32) { __woozy_text_from_rune(value); }");
  auto absent=missing.check("package app; import \"std/text\" as text; func main() i32 { text.bad(65); return 0; }",missing.path/"stdlib");
  need(!absent.success()&&has(absent,"E_CAPABILITY_UNAVAILABLE")&&!has(absent,"E_INTERNAL_ERROR"),
       "missing standard nominal became an internal error\n"+diagnostics(absent));
  std::cout<<"passed: String language operations, all value stdlib operations, generic substitution, Native/Python capabilities and source/HIR rejection\n";
}
} // namespace
int main(int argc,char **argv) {
  try {need(argc==2,"usage: value_library_test <repository-root>");suite(argv[1]);}
  catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

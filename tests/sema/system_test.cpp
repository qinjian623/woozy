#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace lang;
using namespace lang::hir;
namespace fs = std::filesystem;
namespace {
void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
std::string diagnostics(const auto &result) {
  std::string text;
  for (const auto &d : result.diagnostics) text += d.code + ": " + d.message + "\n";
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
    char pattern[] = "/tmp/woozy-system-XXXXXX";
    auto *created = mkdtemp(pattern);
    need(created, "mkdtemp failed");
    path = created;
    write("lang.toml", "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\nentry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write("stdlib/result/result.wz", "package result;\npub enum Result[T,E] { Ok(value T); Err(error E); }\n");
  }
  ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
  void write(const fs::path &name, const std::string &text) {
    fs::create_directories((path / name).parent_path());
    std::ofstream out(path / name);
    out << text;
    need(bool(out), "fixture write failed");
  }
  auto resolve(const std::string &main, const std::string &os = {}) {
    write("src/main.wz", main);
    if (!os.empty()) write("stdlib/os/os.wz", os);
    auto loaded = modules::load_project({path / "lang.toml", path / "stdlib", {}}, sources);
    need(loaded.success(), "load failed: " + diagnostics(loaded));
    auto resolved = modules::resolve_project(std::make_shared<const modules::Project>(std::move(*loaded.value)));
    need(resolved.success(), "resolve failed: " + diagnostics(resolved));
    return resolved;
  }
};
size_t operation(const HirModule &m, SystemOperation wanted) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (auto *call = std::get_if<Call>(&m.nodes[i].data); call && call->system == wanted)
      return i;
  throw std::runtime_error("system call missing");
}
void bad(const HirModule &good, const std::string &name, const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = verify_types(changed);
  need(!checked.success() && has(checked, "E_IR_INVALID"), name + " direct verify accepted\n" + diagnostics(checked));
  auto published = verify(changed, {}, VerifyMode::static_only);
  need(!published.success() && has(published, "E_IR_INVALID"), name + " publish accepted");
  auto rebuilt = detail::produce_ownership(changed, {});
  if (rebuilt.success()) {
    auto rechecked = verify_types(changed);
    need(!rechecked.success() && has(rechecked, "E_IR_INVALID"), name + " recomputed ownership bypassed verification");
  }
}
void suite() {
  Fixture fixture;
  // Appending the raw byte-path stat must not renumber or redirect any
  // existing operation entry.
  need(static_cast<unsigned>(SystemOperation::read) == 0, "read ID changed");
  need(static_cast<unsigned>(SystemOperation::fs_stat) == 43, "fs_stat ID changed");
  need(static_cast<unsigned>(SystemOperation::getrandom) == 76, "getrandom ID changed");
  need(static_cast<unsigned>(SystemOperation::net_socket) == 85, "net_socket ID changed");
  need(static_cast<unsigned>(SystemOperation::process_args) == 138, "process_args ID changed");
  need(static_cast<unsigned>(SystemOperation::ffi_buffer) == 193, "ffi_buffer ID changed");
  need(static_cast<unsigned>(SystemOperation::weak_empty) == 201, "weak_empty ID changed");
  need(static_cast<unsigned>(SystemOperation::async_shutdown) == 212, "async_shutdown ID changed");
  need(static_cast<unsigned>(SystemOperation::fs_stat_bytes) == 213, "fs_stat_bytes ID changed");
  const auto *stat_bytes = system_operation_spec("__woozy_os_stat_bytes");
  need(stat_bytes && stat_bytes->operation == SystemOperation::fs_stat_bytes &&
           stat_bytes->result == "Result[std/os.FileInfo,i32]" &&
           stat_bytes->count == 1 && stat_bytes->parameters[0] == "Vec[u8]" &&
           stat_bytes->owner == "std/os" && stat_bytes->type_arity == 0,
       "stat_bytes spec changed");
  need(system_operation_spec(SystemOperation::fs_stat)->parameters[0] == "string",
       "string stat entry changed");
  std::string os = "package os;\nimport \"std/result\" as result;\n" + std::string(R"(pub struct Timespec { pub seconds i64; pub nanoseconds i64; }
pub struct FileInfo {
    pub device u64; pub inode u64; pub links u64;
    pub mode u32; pub uid u32; pub gid u32; pub rdev u64;
    pub size i64; pub block_size i64; pub blocks i64;
    pub accessed Timespec; pub modified Timespec; pub changed Timespec;
}
)") +
                   "pub func exercise() i64 { let bytes Vec[u8] = [0,1];\n";
  for (const auto &spec : system_operations) {
    os += std::string(spec.name) + "(";
    for (size_t i = 0; i < spec.count; ++i) {
      if (i) os += ',';
      os += spec.parameters[i] == "Vec[u8]" ? "bytes" : spec.parameters[i] == "string" ? "\"path\"" : "0";
    }
    os += ");\n";
  }
  os += "__woozy_os_stat_bytes(bytes);\n";
  os += "return 0; } pub enum Error { System(code i32); }\n"
        "pub func checked() i64 throws Error { let result = __woozy_os_close(0); if result < 0 { throw Error.System(code: i32(-result)); } return result; }";
  auto resolved = fixture.resolve("package app; import \"std/os\" as os; func main() i32 throws os.Error { os.exercise(); os.checked(); return 0; }", os);
  auto checked = sema::check_types(resolved);
  need(checked.success(), "valid system HIR failed: " + diagnostics(checked));
  const auto good = *checked.value;
  auto published = sema::check_project(resolved);
  need(published.success(), "valid system publication failed: " + diagnostics(published));
  std::set<SystemOperation> found;
  for (const auto &node : good.nodes)
    if (auto *call = std::get_if<Call>(&node.data); call && call->kind == CalleeKind::system) {
      found.insert(*call->system);
      need(call->direct_symbol && !call->callee, "system direct builtin target");
      need(!good.types.find(call->signature)->throws_type, "raw result must not gain a typed error");
    }
  need(found.size() == system_operations.size() + 1, "not all operations reached HIR");
  need(found.count(SystemOperation::fs_stat_bytes) == 1,
       "byte-path stat was not checked as a system call");
  const auto read = operation(good, SystemOperation::read);
  need(std::any_of(good.ownership.uses.begin(), good.ownership.uses.end(), [&](const auto &use) {
    return use.consumer == HirNodeId{read} && use.kind == UseKind::borrow && use.operand == 1;
  }), "Vec system argument did not use ordinary borrow records");
  for (const auto config : {Config{Backend::python,64}, Config{Backend::native,32}}) {
    auto target = good;
    target.config = config;
    auto resolved_target = std::make_shared<modules::ResolvedProject>(*target.resolved);
    auto project_target = std::make_shared<modules::Project>(*resolved_target->project);
    project_target->backend = config.backend == Backend::python ? "python" : "native";
    project_target->word_bits = config.word_bits;
    resolved_target->project = project_target;
    target.resolved = resolved_target;
    auto result = check_backend_capabilities(target);
    need(!result.success() && has(result,"E_CAPABILITY_UNAVAILABLE"), "unsupported system profile accepted");
  }
  auto call_at = [&](auto &m, SystemOperation op) -> Call & { return std::get<Call>(m.nodes[operation(m,op)].data); };
  bad(good,"missing operation",[&](auto &m){call_at(m,SystemOperation::read).system.reset();});
  bad(good,"unknown operation",[&](auto &m){call_at(m,SystemOperation::read).system=SystemOperation(255);});
  bad(good,"same-signature other operation",[&](auto &m){call_at(m,SystemOperation::unlink).system=SystemOperation::rmdir;});
  bad(good,"wrong builtin",[&](auto &m){call_at(m,SystemOperation::read).direct_symbol=call_at(m,SystemOperation::write).direct_symbol;});
  bad(good,"generic arguments",[&](auto &m){auto &c=call_at(m,SystemOperation::read);c.type_args.push_back(m.nodes[read].type);});
  bad(good,"argument label",[&](auto &m){call_at(m,SystemOperation::read).args[0].label="fd";});
  bad(good,"argument mode",[&](auto &m){call_at(m,SystemOperation::read).args[1].mode=ParamMode::move;});
  bad(good,"source index",[&](auto &m){call_at(m,SystemOperation::read).args[0].source_index=1;});
  bad(good,"same-type source argument reorder",[&](auto &m){
    auto id=operation(m,SystemOperation::write); auto &c=std::get<Call>(m.nodes[id].data);
    std::swap(c.args[2].expression,c.args[3].expression); std::swap(m.nodes[id].children[2],m.nodes[id].children[3]);
  });
  bad(good,"fixed signature",[&](auto &m){
    auto &c=call_at(m,SystemOperation::read);auto t=*m.types.find(c.signature);
    t.parameters[0].type=m.nodes[read].type;c.signature=m.types.intern(t);
  });
  bad(good,"non-system metadata",[&](auto &m){
    for(auto &n:m.nodes) if(auto *c=std::get_if<Call>(&n.data);c&&c->kind==CalleeKind::direct){c->system=SystemOperation::getpid;return;}
    throw std::runtime_error("ordinary call missing");
  });
  bad(good,"source module spoof",[&](auto &m){
    auto r=std::make_shared<modules::ResolvedProject>(*m.resolved);
    auto p=std::make_shared<modules::Project>(*r->project);
    p->modules[m.nodes[read].source.node.module.value].logical_path="app/os";
    r->project=p;m.resolved=r;
  });
  bad(good,"resolved builtin occurrence",[&](auto &m){
    auto r=std::make_shared<modules::ResolvedProject>(*m.resolved);
    auto ref=m.nodes[read].source.node;
    const auto &file=*r->project->module(ref.module).files.at(ref.file);
    ref.node=file.node(ref.node).children[0];
    for(auto &o:r->occurrences) if(o.source.module==ref.module&&o.source.file==ref.file&&o.source.node==ref.node)
      o.symbol=*call_at(m,SystemOperation::write).direct_symbol;
    m.resolved=r;
  });
  auto denied = [&](const std::string &body, const char *code, bool standard) {
    Fixture f;
    auto r=standard?f.resolve("package app; import \"std/os\" as os; func main() {}", "package os; pub func invalid() {"+body+"}")
                   :f.resolve("package os; func main() {"+body+"}");
    auto result=sema::check_types(r);
    need(!result.success()&&has(result,code),body+" wrong denial: "+diagnostics(result));
  };
  denied("__woozy_os_getpid();","E_CAPABILITY_UNAVAILABLE",false);
  denied("let values Vec[i32]=[1]; __woozy_vec_pin_begin[i32](values);","E_CAPABILITY_UNAVAILABLE",false);
  denied("let values Vec[i32]=[1]; __woozy_vec_try_move_trivial[i32](values,0,0,1);","E_CAPABILITY_UNAVAILABLE",false);
  denied("let operation = __woozy_os_getpid;","E_CAPABILITY_UNAVAILABLE",true);
  denied("__woozy_os_getpid[i32]();","E_CALL_ARGUMENT",true);
  denied("__woozy_os_close(fd: 0);","E_CALL_ARGUMENT",true);
  denied("__woozy_os_close();","E_CALL_ARGUMENT",true);
  denied("__woozy_os_close(true);","E_TYPE_MISMATCH",true);
  denied("var fd i32=0; __woozy_os_close(inout fd);","E_CALL_ARGUMENT",true);
  denied("var fd i32=0; __woozy_os_close(+(move fd));","E_CALL_ARGUMENT",true);
  denied("let bytes Vec[u8]=[0]; __woozy_os_read(0,move bytes,1);","E_CALL_ARGUMENT",true);
  Fixture shadow;
  auto same_name=shadow.resolve("package app; func __woozy_os_getpid() i64 {return 17;} func main() i32 {let pid=__woozy_os_getpid();return 0;}");
  auto ordinary=sema::check_project(same_name);
  need(ordinary.success(),"user function with same spelling rejected: "+diagnostics(ordinary));
  for(const auto &n:ordinary.value->module().nodes) if(auto *c=std::get_if<Call>(&n.data))
    need(c->kind==CalleeKind::direct&&!c->system,"user function gained system privilege");
  std::cout << "passed: 13 system operations, typed errors/Vec borrow, profiles, source denials, and 13 HIR mutations\n";
}
} // namespace
int main() {
  try { suite(); }
  catch(const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

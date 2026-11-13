#include "lang/backend/native.hpp"
#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace lang;
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
struct Fixture {
  fs::path path;
  Fixture() {
    char pattern[] = "/tmp/woozy-system-ir-XXXXXX";
    auto *created = mkdtemp(pattern);
    need(created, "mkdtemp failed");
    path = created;
    write("lang.toml", "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\nentry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write("stdlib/result/result.wz", "package result;\npub enum Result[T,E] { Ok(value T); Err(error E); }\n");
    write("src/main.wz", "package app; import \"std/os\" as os; func main() i32 { os.write(); os.order(); os.nested(); os.deferred(); os.looped(); os.stat_bytes(); return 0; }");
    write("stdlib/os/os.wz", R"(package os;
import "std/result" as result;
pub struct Timespec { pub seconds i64; pub nanoseconds i64; }
pub struct FileInfo {
    pub device u64; pub inode u64; pub links u64;
    pub mode u32; pub uid u32; pub gid u32; pub rdev u64;
    pub size i64; pub block_size i64; pub blocks i64;
    pub accessed Timespec; pub modified Timespec; pub changed Timespec;
}
pub func stat_bytes() i64 {
    let bytes Vec[u8] = [46];
    __woozy_os_stat_bytes(bytes);
    return 0;
}
pub func write() i64 {
    let bytes Vec[u8] = [65,66,67,68];
    return __woozy_os_write(1,bytes,1,2);
}
pub func order() {
    __woozy_os_unlink("alpha");
    __woozy_os_rmdir("beta");
}
pub func nested() i64 {
    return __woozy_os_write_text(1,"abc",__woozy_os_text_length(""),__woozy_os_text_length("ab"));
}
pub func deferred() i64 {
    defer { __woozy_os_unlink("first"); }
    defer { __woozy_os_rmdir("last"); }
    {
        defer { __woozy_os_write_text(1,"block",0,__woozy_os_text_length("block")); }
        __woozy_os_getpid();
    }
    return __woozy_os_getpid();
}
pub func looped() {
    for var index i64 = 0; __woozy_os_getpid() > 0 && index < 1; index = __woozy_os_getpid() {
        defer { __woozy_os_getpid(); }
        __woozy_os_getpid();
    }
}
)");
  }
  ~Fixture() { std::error_code ignored; fs::remove_all(path, ignored); }
  void write(const fs::path &name, const std::string &text) {
    fs::create_directories((path / name).parent_path());
    std::ofstream out(path / name);
    out << text;
    need(bool(out), "fixture write failed");
  }
};
ir::Instruction &system(ir::Module &module, hir::SystemOperation operation) {
  for (auto &function : module.functions)
    for (auto &block : function.blocks)
      for (auto &instruction : block.instructions)
        if (instruction.op == ir::Op::system_call && instruction.system == operation)
          return instruction;
  throw std::runtime_error("system instruction missing");
}
void rejected(const ir::Module &good, const std::string &name, const auto &edit) {
  auto changed = good;
  edit(changed);
  auto checked = ir::verify(changed);
  need(!checked.success() && !checked.diagnostics.empty() && checked.diagnostics.front().code == "E_IR_INVALID",
       name + " accepted\n" + diagnostics(checked));
}
void suite() {
  Fixture fixture;
  SourceManager sources;
  auto loaded = modules::load_project({fixture.path / "lang.toml", fixture.path / "stdlib", {}}, sources);
  need(loaded.success(), "load failed: " + diagnostics(loaded));
  auto resolved = modules::resolve_project(std::make_shared<const modules::Project>(std::move(*loaded.value)));
  auto checked = sema::check_project(resolved);
  need(checked.success(), "check failed: " + diagnostics(checked));
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), "lower failed: " + diagnostics(lowered));
  auto good = lowered.value->module();
  need(ir::verify(good).success(), "unchanged IR rejected");
  using hir::SystemOperation;
  auto &stat_call = system(good, SystemOperation::fs_stat_bytes);
  need(stat_call.inputs.size() == 1, "stat_bytes input count changed");
  SymbolId entry;
  for (auto &symbol : resolved.value->symbols)
    if (symbol.name == "main" && symbol.kind == modules::SymbolKind::function)
      entry = symbol.id;
  auto published = ir::verify(good);
  need(published.success(), "stat_bytes IR publication failed");
  for (unsigned optimization : {0u, 2u}) {
    backend::native::Options options;
    options.optimization = optimization;
    auto generated = backend::native::emit(*published.value, entry, options);
    need(generated.success(), "stat_bytes LLVM emit failed: " +
         diagnostics(generated));
    const auto &llvm = generated.value->llvm_ir;
    need(llvm.find("lang_sys_stat_v1") != std::string::npos,
         "stat_bytes runtime symbol missing");
    const auto &before = generated.value->llvm_before_optimization;
    need(before.find(", i32 0, i32 5") != std::string::npos &&
         before.find(", i32 0, i32 1") != std::string::npos,
         "stat_bytes did not read Vec data/length fields\n" + before);
  }
  rejected(good,"operation change",[](auto &m){ system(m,SystemOperation::write).system=SystemOperation::read; });
  rejected(good,"missing operation",[](auto &m){ system(m,SystemOperation::write).system.reset(); });
  rejected(good,"offset/count swap",[](auto &m){ auto &i=system(m,SystemOperation::write);std::swap(i.inputs[2],i.inputs[3]); });
  rejected(good,"origin/operation swap",[](auto &m){
    auto &a=system(m,SystemOperation::unlink); auto &b=system(m,SystemOperation::rmdir);
    std::swap(a.origin,b.origin);std::swap(a.system,b.system);
  });
  // Move complete statements, including the String argument's borrow/drop.
  auto groups = [](const auto &instructions) -> std::pair<size_t,size_t> {
    std::vector<size_t> calls;
    for (size_t i=0;i<instructions.size();++i)
      if (instructions[i].op==ir::Op::system_call) calls.push_back(i);
    if (calls.size()!=2 || instructions[calls[0]].system!=SystemOperation::unlink ||
        instructions[calls[1]].system!=SystemOperation::rmdir) return {0,0};
    need(instructions.front().op==ir::Op::string_literal, "first argument missing");
    size_t middle=calls[0]+1;
    while (middle<instructions.size() && instructions[middle].op!=ir::Op::string_literal) ++middle;
    need(middle<calls[1] && instructions.back().op==ir::Op::constant,
         "statement group prerequisite changed");
    return {middle,instructions.size()-1};
  };
  rejected(good,"system instruction order",[&](auto &m){
    for(auto &function:m.functions) for(auto &block:function.blocks) {
      auto [middle,end]=groups(block.instructions);
      if (!middle) continue;
      auto &instructions=block.instructions;
      std::rotate(instructions.begin(),instructions.begin()+middle,instructions.begin()+end);
      return;
    }
    throw std::runtime_error("order mutation missing");
  });
  rejected(good,"system order across a split block",[&](auto &m){
    for(auto &function:m.functions) {
      if(function.blocks.size()!=1) continue;
      auto &block=function.blocks.front();
      auto &instructions=block.instructions;
      auto [middle,end]=groups(instructions);
      if (!middle) continue;
      const auto first_size=end-middle;
      std::rotate(instructions.begin(),instructions.begin()+middle,instructions.begin()+end);
      ir::BasicBlock tail;
      tail.instructions.assign(instructions.begin()+first_size,instructions.end());
      instructions.erase(instructions.begin()+first_size,instructions.end());
      tail.terminator=block.terminator;
      block.terminator={};
      block.terminator.kind=ir::Exit::branch;
      block.terminator.targets={BlockId{1}};
      block.terminator.source=function.source;
      function.blocks.push_back(std::move(tail));
      return;
    }
    throw std::runtime_error("split order mutation missing");
  });
  std::cout << "passed: system NativeIR source arguments, operations, and call order mutations rejected\n";
}
} // namespace
int main() {
  try { suite(); }
  catch(const std::exception &error) { std::cerr << error.what() << '\n';return 1; }
}

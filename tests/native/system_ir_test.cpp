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
    write("src/main.wz", "package app; import \"std/os\" as os; func main() i32 { os.write(); os.order(); os.nested(); os.deferred(); os.looped(); return 0; }");
    write("stdlib/os/os.wz", R"(package os;
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
  rejected(good,"operation change",[](auto &m){ system(m,SystemOperation::write).system=SystemOperation::read; });
  rejected(good,"missing operation",[](auto &m){ system(m,SystemOperation::write).system.reset(); });
  rejected(good,"offset/count swap",[](auto &m){ auto &i=system(m,SystemOperation::write);std::swap(i.inputs[2],i.inputs[3]); });
  rejected(good,"origin/operation swap",[](auto &m){
    auto &a=system(m,SystemOperation::unlink); auto &b=system(m,SystemOperation::rmdir);
    std::swap(a.origin,b.origin);std::swap(a.system,b.system);
  });
  rejected(good,"system instruction order",[](auto &m){
    for(auto &function:m.functions) for(auto &block:function.blocks) {
      auto &instructions=block.instructions;
      for(size_t i=1;i+2<instructions.size();++i)
        if(instructions[i].op==ir::Op::system_call&&instructions[i].system==SystemOperation::unlink) {
          need(instructions[i-1].op==ir::Op::string_literal&&instructions[i+1].op==ir::Op::string_literal&&
                   instructions[i+2].op==ir::Op::system_call&&instructions[i+2].system==SystemOperation::rmdir,
               "order mutation prerequisite changed");
          std::rotate(instructions.begin()+i-1,instructions.begin()+i+1,instructions.begin()+i+3);
          return;
        }
    }
    throw std::runtime_error("order mutation missing");
  });
  rejected(good,"system order across a split block",[](auto &m){
    for(auto &function:m.functions) {
      if(function.blocks.size()!=1) continue;
      auto &block=function.blocks.front();
      auto &instructions=block.instructions;
      if(instructions.size()<4||instructions[1].system!=SystemOperation::unlink||
         instructions[3].system!=SystemOperation::rmdir) continue;
      need(instructions[0].op==ir::Op::string_literal&&instructions[2].op==ir::Op::string_literal,
           "split order mutation prerequisite changed");
      std::rotate(instructions.begin(),instructions.begin()+2,instructions.begin()+4);
      ir::BasicBlock tail;
      tail.instructions.assign(instructions.begin()+2,instructions.end());
      instructions.erase(instructions.begin()+2,instructions.end());
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

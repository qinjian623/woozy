#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace lang;
namespace {
void need(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
std::string diagnostics(const auto &result) {
  std::string text;
  for (const auto &d : result.diagnostics) text += d.code + ": " + d.message + "\n";
  return text;
}
void suite(const std::filesystem::path &root) {
  SourceManager sources;
  auto project = modules::load_project(
      {root/"tests/native/value_library/lang.toml", root/"stdlib", {}}, sources);
  need(project.success(), diagnostics(project));
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*project.value)));
  auto checked = sema::check_project(resolved);
  need(checked.success(), diagnostics(checked));
  auto lowered = ir::lower(*checked.value);
  need(lowered.success(), diagnostics(lowered));
  const auto original = lowered.value->module();
  auto regression_project = modules::load_project(
      {root/"tests/native/fixtures/throwing-switch/lang.toml", root/"stdlib", {}}, sources);
  need(regression_project.success(), diagnostics(regression_project));
  auto regression_resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*regression_project.value)));
  auto regression_checked = sema::check_project(regression_resolved);
  need(regression_checked.success(), diagnostics(regression_checked));
  auto regression_lowered = ir::lower(*regression_checked.value);
  need(regression_lowered.success(), diagnostics(regression_lowered));
  auto missing_cleanup = regression_lowered.value->module();
  bool removed = false;
  for (auto &function : missing_cleanup.functions)
    for (auto &block : function.blocks)
      if (!removed && block.terminator.kind == ir::Exit::error_return)
        for (auto it = block.instructions.begin(); it != block.instructions.end(); ++it)
          if (it->op == ir::Op::end_borrow) {
            block.instructions.erase(it); removed = true; break;
          }
  need(removed, "throwing switch/condition did not release its argument borrow");
  const auto cleanup_check = ir::verify(missing_cleanup);
  need(!cleanup_check.success() && !cleanup_check.diagnostics.empty() &&
           cleanup_check.diagnostics.front().code == "E_IR_INVALID",
       "missing failing-call borrow cleanup was accepted");
  auto kind = [&](TypeId id) { return original.hir.module().types.find(id)->kind; };
  auto text_binary = [&](const auto &i) {
    return i.op == ir::Op::binary && kind(i.type) == hir::TypeKind::String;
  };
  auto text_slice = [&](const auto &i) {
    return i.op == ir::Op::vec_slice && kind(i.type) == hir::TypeKind::String && i.bits == 3;
  };
  auto text_index = [&](const auto &i) {
    return i.op == ir::Op::vec_read && i.origin &&
        kind(original.hir.module().nodes.at(
            original.hir.module().nodes.at(i.origin->value).children[0].value).type)
            == hir::TypeKind::String;
  };
  size_t rejected = 1;
  auto bad = [&](const char *name, const auto &predicate, const auto &edit) {
    auto changed = original;
    bool found = false;
    for (auto &function : changed.functions)
      for (auto &block : function.blocks)
        for (auto &instruction : block.instructions)
          if (!found && predicate(instruction)) { edit(instruction); found = true; }
    need(found, std::string("missing mutation target: ") + name);
    const auto verified = ir::verify(changed);
    need(!verified.success() && !verified.value && !verified.diagnostics.empty() &&
             verified.diagnostics.front().code == "E_IR_INVALID",
         std::string("invalid value IR accepted: ") + name + "\n" + diagnostics(verified));
    ++rejected;
  };
  bad("string subtraction", text_binary, [](auto &i) { i.binary = hir::BinaryOp::subtract; });
  bad("string missing origin", text_binary, [](auto &i) { i.origin.reset(); });
  bad("string argument order", text_binary, [](auto &i) { std::swap(i.inputs[0], i.inputs[1]); });
  bad("string index origin", text_index, [](auto &i) { i.origin.reset(); });
  bad("string index operands", text_index, [](auto &i) { std::swap(i.inputs[0], i.inputs[1]); });
  bad("string slice origin", text_slice, [](auto &i) { i.origin.reset(); });
  bad("string slice endpoints", text_slice, [](auto &i) { std::swap(i.inputs[1], i.inputs[2]); });
  bad("string slice presence", text_slice, [](auto &i) { i.bits = 1; });
  bad("text length operation", [](const auto &i) {
    return i.op == ir::Op::system_call && i.system == hir::SystemOperation::text_len;
  }, [](auto &i) { i.system = hir::SystemOperation::text_byte_len; });
  bad("Vec push operation", [](const auto &i) {
    return i.op == ir::Op::system_call && i.system == hir::SystemOperation::vec_push;
  }, [](auto &i) { i.system = hir::SystemOperation::vec_resize; });
  bad("string copy replaced by borrow", [&](const auto &i) {
    return i.op == ir::Op::copy && kind(i.type) == hir::TypeKind::String;
  }, [](auto &i) { i.op = ir::Op::borrow; });
  std::cout << "passed: value library lowering and " << rejected << " invalid Native IR cases\n";
}
}
int main(int argc, char **argv) {
  try { need(argc == 2, "repository root required"); suite(argv[1]); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

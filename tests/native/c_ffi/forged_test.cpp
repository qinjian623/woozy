#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lang;
namespace {
void require(bool value, const std::string &message) {
  if (!value) throw std::runtime_error(message);
}
bool hir_rejected(const hir::HirModule &module) {
  auto result = hir::verify(module);
  return !result.success() && !result.value &&
         result.failure == FailureKind::internal && !result.diagnostics.empty() &&
         result.diagnostics.front().code == "E_IR_INVALID";
}
bool ir_rejected(const ir::Module &module) {
  auto result = ir::verify(module);
  return !result.success() && !result.value &&
         result.failure == FailureKind::internal && !result.diagnostics.empty() &&
         result.diagnostics.front().code == "E_IR_INVALID";
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "project manifest argument");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    require(project.success(), "C FFI project load");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    require(resolved.success(), "C FFI project resolve");
    auto checked = sema::check_project(resolved);
    require(checked.success(), "C FFI project check");
    const auto &hir = checked.value->module();
    size_t foreign_calls = 0, fields = 0, ptr_writes = 0;
    for (const auto &node : hir.nodes) {
      if (auto call = std::get_if<hir::Call>(&node.data);
          call && call->kind == hir::CalleeKind::c_foreign) ++foreign_calls;
      if (std::holds_alternative<hir::CFieldRead>(node.data)) ++fields;
    }
    require(foreign_calls > 0 && fields > 0, "C FFI fixture coverage");
    size_t rejected = 0;
    auto mutate_hir = [&](auto change) {
      auto copy = hir;
      change(copy);
      require(hir_rejected(copy), "forged C HIR accepted");
      ++rejected;
    };
    mutate_hir([](hir::HirModule &m) {
      for (auto &node : m.nodes)
        if (auto call = std::get_if<hir::Call>(&node.data);
            call && call->kind == hir::CalleeKind::c_foreign) {
          call->c_callable = 999999;
          return;
        }
      throw std::runtime_error("no C call");
    });
    mutate_hir([](hir::HirModule &m) {
      for (auto &node : m.nodes)
        if (auto call = std::get_if<hir::Call>(&node.data);
            call && call->kind == hir::CalleeKind::c_foreign) {
          call->signature = TypeId{999999};
          return;
        }
      throw std::runtime_error("no C call");
    });
    mutate_hir([](hir::HirModule &m) {
      for (auto &node : m.nodes)
        if (auto call = std::get_if<hir::Call>(&node.data);
            call && call->kind == hir::CalleeKind::c_foreign &&
            !call->args.empty()) {
          std::swap(call->args[0].source_index, call->args[0].parameter_index);
          call->args[0].parameter_index = 7;
          return;
        }
      throw std::runtime_error("no C arguments");
    });
    mutate_hir([](hir::HirModule &m) {
      for (auto &node : m.nodes)
        if (auto field = std::get_if<hir::CFieldRead>(&node.data)) {
          field->field = 999999;
          return;
        }
      throw std::runtime_error("no C field");
    });
    auto lowered = ir::lower(*checked.value);
    require(lowered.success(), "C FFI lower");
    const auto &cfg = lowered.value->module();
    for (const auto &f : cfg.functions)
      for (const auto &b : f.blocks)
        for (const auto &i : b.instructions) {
          if (i.op == ir::Op::foreign_call) ++foreign_calls;
          if (i.op == ir::Op::c_field_read) ++fields;
          if (i.op == ir::Op::c_ptr_write) ++ptr_writes;
        }
    require(ptr_writes > 0, "C pointer write coverage");
    auto mutate_ir = [&](auto change) {
      auto copy = cfg;
      change(copy);
      require(ir_rejected(copy), "forged C IR accepted");
      ++rejected;
    };
    mutate_ir([](ir::Module &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::foreign_call) {
              i.c_callable = 999999;
              return;
            }
      throw std::runtime_error("no IR C call");
    });
    mutate_ir([](ir::Module &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::foreign_call) {
              i.origin.reset();
              return;
            }
      throw std::runtime_error("no IR C call");
    });
    mutate_ir([](ir::Module &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::c_field_read) {
              i.c_field = 999999;
              return;
            }
      throw std::runtime_error("no IR C field");
    });
    mutate_ir([](ir::Module &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::c_ptr_write) {
              i.origin.reset();
              return;
            }
      throw std::runtime_error("no IR C pointer write");
    });
    std::cout << "C FFI forged HIR/IR rejected: " << rejected << "\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

#include "lang/backend/native.hpp"
#include "lang/sema/check.hpp"
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>

using namespace lang;

namespace {
bool inject_bad_alloc = false;

void need(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace

// The linked archive defines this C++ symbol, including the cxx11 ABI tag.
// These test-only wrappers are selected by -Wl,--wrap in CMakeLists.txt.
std::string real_dump(const ir::Module &)
    asm("__real__ZN4lang2ir4dumpB5cxx11ERKNS0_6ModuleE");
std::string wrap_dump(const ir::Module &)
    asm("__wrap__ZN4lang2ir4dumpB5cxx11ERKNS0_6ModuleE");

__attribute__((used)) std::string wrap_dump(const ir::Module &module) {
  if (inject_bad_alloc)
    throw std::bad_alloc();
  return real_dump(module);
}

int main(int argc, char **argv) {
  try {
    need(argc == 2, "manifest required");
    SourceManager sources;
    auto project = modules::load_project({argv[1], {}, {}}, sources);
    need(project.success(), "load failed");
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(std::move(*project.value)));
    auto checked = sema::check_project(resolved);
    need(checked.success(), "check failed");
    auto lowered = ir::lower(*checked.value);
    need(lowered.success() && lowered.value, "lower failed");
    SymbolId entry;
    for (const auto &symbol : resolved.value->symbols)
      if (symbol.name == "main" &&
          symbol.kind == modules::SymbolKind::function)
        entry = symbol.id;
    need(entry.value != 0, "main entry missing");

    backend::native::Options options;
    auto normal = backend::native::emit(*lowered.value, entry, options);
    need(normal.success() && normal.value && !normal.value->object.empty(),
         "normal emit failed");

    inject_bad_alloc = true;
    auto injected = backend::native::emit(*lowered.value, entry, options);
    inject_bad_alloc = false;
    need(!injected.success() && !injected.value,
         "injected bad_alloc produced output");
    need(injected.state == PhaseState::failed,
         "bad_alloc did not leave the phase failed");
    need(injected.failure == FailureKind::internal,
         "bad_alloc was not classified as internal");
    need(injected.exit_code() == ExitCode::internal_error,
         "bad_alloc did not map to the internal exit code");
    need(injected.status() == Status::internal_error,
         "bad_alloc did not map to the internal status");
    const std::string code = injected.diagnostics.empty()
                                 ? "<none>"
                                 : injected.diagnostics.at(0).code;
    const std::string phase = injected.diagnostics.empty()
                                  ? "<none>"
                                  : injected.diagnostics.at(0).phase;
    need(code == "E_INTERNAL_ERROR" && phase == "codegen",
         "bad_alloc diagnostic is " + phase + "/" + code);

    auto recovered = backend::native::emit(*lowered.value, entry, options);
    need(recovered.success() && recovered.value &&
             !recovered.value->object.empty(),
         "emit failed after restoring the wrapped function");

    auto no_entry =
        backend::native::emit(*lowered.value, SymbolId{999999}, options);
    need(!no_entry.success() && !no_entry.value &&
             no_entry.failure == FailureKind::internal &&
             !no_entry.diagnostics.empty() &&
             no_entry.diagnostics.at(0).code == "E_IR_INVALID",
         "explicit entry failure lost its E_IR_INVALID classification");

    std::cout << "passed: bad_alloc is internal E_INTERNAL_ERROR; explicit "
                 "entry failure remains E_IR_INVALID\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

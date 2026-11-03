#include "lang/modules/resolve.hpp"
#include <iostream>
int main(int argc, char **argv) {
  if (argc != 2) return 2;
  lang::SourceManager sources;
  try {
    auto loaded = lang::modules::load_project({argv[1], {}, {}}, sources);
    if (!loaded.success()) {
      std::cout << lang::diagnostic_json(sources, "module-test", "test",
                                        loaded.status(), loaded.diagnostics) << '\n';
      return static_cast<int>(loaded.exit_code());
    }
    auto resolved = lang::modules::resolve_project(
        std::make_shared<const lang::modules::Project>(std::move(*loaded.value)));
    std::cout << lang::diagnostic_json(sources, "module-test", "test",
                                      resolved.status(), resolved.diagnostics) << '\n';
    return static_cast<int>(resolved.exit_code());
  } catch (const std::exception &e) {
    std::cout << lang::diagnostic_json(sources, "module-test", "test",
        lang::Status::internal_error, {{"E_INTERNAL_ERROR", "resolve", "error", e.what()}}) << '\n';
    return 3;
  }
}

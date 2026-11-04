#include "lang/backend/python.hpp"
#include "lang/sema/check.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace lang;
namespace {
void require(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
auto checked(const std::filesystem::path &root, const char *backend,
             unsigned bits,
             const char *manifest = "tests/fixtures/core/lang.toml") {
  SourceManager sources;
  auto p = modules::load_project(
      {root / manifest, {}, {}}, sources);
  require(p.success(), "real project load");
  p.value->backend = backend;
  p.value->word_bits = bits;
  auto r = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*p.value)));
  require(r.success(), "real project resolve");
  auto c = sema::check_project(r);
  require(c.success(), "real project check/verify");
  return std::move(*c.value);
}
SymbolId entry(const hir::VerifiedHirModule &m) {
  for (const auto &f : m.module().functions)
    if (m.module().resolved->symbols.at(f.symbol.value).name == "main")
      return f.symbol;
  throw std::runtime_error("no main");
}
} // namespace
int main(int argc, char **argv) {
  try {
    static_assert(
        !std::is_constructible_v<hir::VerifiedHirModule, hir::HirModule>);
    require(argc == 2, "source root argument");
    auto m = checked(argv[1], "python", 64);
    auto a = backend::python::emit(m, entry(m));
    auto b = backend::python::emit(m, entry(m));
    require(a.success() && b.success(), "emit real verified HIR");
    require(a.value->code == b.value->code &&
                a.value->runtime == b.value->runtime,
            "deterministic output");
    require(a.value->runtime.find(backend::python::runtime_version) !=
                std::string::npos,
            "embedded runtime version matches public API");
    require(!a.value->source_map.empty() && !a.value->symbols.empty(),
            "source/symbol maps");
    for (const auto &line : a.value->source_map) {
      require(line.line > 0 && line.source.begin_byte <= line.source.end_byte,
              "valid line mapping");
    }
    auto imported = backend::python::emit(m, entry(m), {false});
    require(imported.success() &&
                imported.value->code.find("__main__") == std::string::npos,
            "no runner option");
    auto absent = backend::python::emit(m, SymbolId{9999999});
    require(!absent.success() && !absent.value &&
                absent.failure == FailureKind::internal &&
                absent.diagnostics.at(0).code == "E_IR_INVALID",
            "invalid entry diagnosed");
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = backend::python::emit(m, entry(m), {}, stop.get_token());
    require(cancelled.state == PhaseState::cancelled && !cancelled.value &&
                cancelled.exit_code() == ExitCode::cancelled,
            "cancel has no partial output");
    auto shared_cancel =
        hir::check_backend_capabilities(m.module(), stop.get_token());
    require(cancelled.state == shared_cancel.state &&
                cancelled.failure == shared_cancel.failure &&
                cancelled.diagnostics.size() == shared_cancel.diagnostics.size(),
            "shared capability result preserved");
    auto generic = checked(argv[1], "python", 64,
                           "tests/generics/fixtures/multi-instance/lang.toml");
    require(!generic.module().templates.empty() &&
                !generic.module().instances.empty(),
            "real template and concrete instance input");
    auto concrete = backend::python::emit(generic, entry(generic));
    require(concrete.success(), "emit concrete scalar instance");
    for (const auto &instance : generic.module().instances)
      for (const auto &renamed : instance.renamed)
        for (const auto &s : concrete.value->symbols)
          require(s.symbol != renamed.first, "template-owned symbol excluded");
    auto enumeration = checked(argv[1], "python", 64,
                               "tests/generics/fixtures/enum-payload/lang.toml");
    auto patterns = backend::python::emit(enumeration, entry(enumeration));
    require(patterns.success(), "emit real enum/pattern input");
    bool saw_binding = false;
    for (const auto &n : enumeration.module().nodes)
      if (const auto *s = std::get_if<hir::Switch>(&n.data))
        for (const auto &arm : s->arms)
          for (const auto &p : arm.pattern.payload)
            if (p.binding) {
              saw_binding = true;
              bool named = false, mapped = false;
              for (const auto &s : patterns.value->symbols)
                named |= s.symbol == *p.binding &&
                         s.name == "_s" + std::to_string(p.binding->value);
              for (const auto &line : patterns.value->source_map)
                mapped |= line.source.snapshot == p.source.span.snapshot &&
                          line.source.begin_byte == p.source.span.begin_byte &&
                          line.source.end_byte == p.source.span.end_byte;
              require(named && mapped, "pattern SymbolId and exact SourceSpan");
            }
    require(saw_binding, "pattern mapping test exercised");
    auto finite = checked(argv[1], "python", 64,
                          "tests/python/fixtures/finite-callback/lang.toml");
    auto count = finite.module().types.size();
    auto finite_check = hir::check_backend_capabilities(finite.module());
    auto finite_output = backend::python::emit(finite, entry(finite));
    require(finite_check.success() && finite_output.success() &&
                finite.module().types.size() == count,
            "function field finite compilation without changing public TypeContext");
    for (const auto &[backend, bits] :
         {std::pair{"native", 64U}, {"python", 32U}}) {
      auto unsupported = checked(argv[1], backend, bits);
      auto r = backend::python::emit(unsupported, entry(unsupported));
      require(!r.success() && !r.value &&
                  r.failure == FailureKind::unsupported &&
                  r.diagnostics.at(0).code == "E_CAPABILITY_UNAVAILABLE",
              "wrong profile rejected");
    }
    // A valid HIR function with parameters cannot be used as the OS entry.
    auto f = m.module().functions.front().symbol;
    auto bad_signature = backend::python::emit(m, f);
    require(!bad_signature.success() &&
                bad_signature.failure == FailureKind::unsupported &&
                bad_signature.diagnostics.at(0).primary.has_value(),
            "unsupported entry signature");
    std::cout << "python API: passed (real load/resolve/check/verify; "
                 "NDEBUG-safe checks)\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

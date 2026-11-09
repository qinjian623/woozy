#include "../../src/hir/ownership.hpp"
#include "lang/sema/check.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <json-c/json.h>
#include <set>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace lang;
namespace fs = std::filesystem;
void need(bool b, const std::string &msg) {
  if (!b)
    throw std::runtime_error(msg);
}
std::string messages(const auto &r) {
  std::string s;
  for (const auto &d : r.diagnostics)
    s += d.code + ": " + d.message + "\n";
  return s;
}
auto check(const fs::path &p) {
  SourceManager sm;
  auto loaded = modules::load_project({p, {}, {}}, sm);
  if (!loaded.success()) {
    PhaseResult<hir::HirModule> r;
    r.diagnostics = loaded.diagnostics;
    r.failure = loaded.failure;
    return r;
  }
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(*loaded.value));
  return sema::check_types(resolved);
}
struct Temp {
  fs::path path;
  Temp() {
    char p[] = "/tmp/lang-t07-XXXXXX";
    auto t = mkdtemp(p);
    need(t, "mkdtemp");
    path = t;
    fs::create_directories(path / "src");
  }
  ~Temp() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};
void write(const fs::path &p, const std::string &s) {
  std::ofstream f(p);
  f << s;
  need(bool(f), "write test fixture");
}
auto source(const std::string &s, std::string backend = "native",
            unsigned width = 64) {
  Temp t;
  write(t.path / "lang.toml",
        "schema_version = "
        "1\n[package]\nname=\"test\"\nsource_root=\"src\"\nentry=\"app\"\n["
        "build]\nbackend=\"" +
            backend + "\"\nword_bits=" + std::to_string(width) + "\n");
  write(t.path / "src/main.lang", "package app; " + s);
  SourceManager sm;
  auto loaded = modules::load_project({t.path / "lang.toml", {}, {}}, sm);
  if (!loaded.success()) {
    PhaseResult<hir::HirModule> r;
    r.diagnostics = loaded.diagnostics;
    r.failure = loaded.failure;
    return r;
  }
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(*loaded.value));
  if (!resolved.success())
    return sema::check_types(resolved);
  auto input = std::make_shared<modules::ResolvedProject>(*resolved.value);
  auto size = input->symbols.size(), occurrences = input->occurrences.size();
  auto result = sema::check_types(input);
  need(input->symbols.size() == size &&
           input->occurrences.size() == occurrences,
       "checker mutated input ResolvedProject");
  if (result.success())
    need(result.value->resolved.get() != input.get(),
         "checker did not own its symbol additions");
  return result;
}
bool has(const auto &r, std::string_view code) {
  for (const auto &d : r.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
void invalid(hir::HirModule m, const std::string &name, const auto &change) {
  change(m);
  auto r = hir::verify_types(m);
  need(!r.success() && !r.value && r.failure == FailureKind::internal &&
           has(r, "E_IR_INVALID"),
       "corruption accepted: " + name + "\n" + messages(r));
  auto published = hir::verify(m);
  need(!published.success() && has(published, "E_IR_INVALID"),
       "corrupt module published: " + name);
}
void invalid_recomputed(hir::HirModule m, const std::string &name,
                        const auto &change) {
  change(m);
  auto derived = hir::detail::produce_ownership(m, {});
  need(derived.success(),
       "recomputed ownership failed before verification: " + name + "\n" +
           messages(derived));
  auto r = hir::verify_types(m);
  need(!r.success() && !r.value && r.failure == FailureKind::internal &&
           has(r, "E_IR_INVALID"),
       "recomputed corruption accepted: " + name + "\n" + messages(r));
}
void invalid_recomputed_published(hir::HirModule m, const std::string &name,
                                  const auto &change) {
  change(m);
  auto derived = hir::detail::produce_ownership(m, {});
  need(derived.success(),
       "recomputed ownership failed before verification: " + name + "\n" +
           messages(derived));
  auto r = hir::verify_types(m);
  need(!r.success() && !r.value && r.failure == FailureKind::internal &&
           has(r, "E_IR_INVALID"),
       "recomputed corruption accepted: " + name + "\n" + messages(r));
  auto published = hir::verify(m);
  need(!published.success() && has(published, "E_IR_INVALID"),
       "recomputed corrupt module published: " + name);
}
void repairs(const fs::path &root) {
  auto good = [](const std::string &s) {
    auto r = source(s);
    need(r.success(), s + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [](const std::string &s, const std::string &code) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         s + "\n" + messages(r));
  };
  auto packages = [](const std::string &lib, const std::string &app) {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang", "package lib; " + lib);
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; " + app);
    return check(t.path / "lang.toml");
  };
  size_t failures = 0;
  auto test = [&](const char *id, const std::function<void()> &f,
                  bool bounded = false) {
    try {
      if (bounded) {
        std::cout.flush();
        auto pid = fork();
        need(pid >= 0, "fork resource-limited layout test");
        if (!pid) {
          rlimit memory{384 * 1024 * 1024, 384 * 1024 * 1024};
          rlimit cpu{3, 3};
          // ASan reserves a large address range; CPU/wall limits still apply.
#if defined(__SANITIZE_ADDRESS__)
          constexpr bool limit_address_space = false;
#elif defined(__has_feature)
          constexpr bool limit_address_space = !__has_feature(address_sanitizer);
#else
          constexpr bool limit_address_space = true;
#endif
          if ((limit_address_space && setrlimit(RLIMIT_AS, &memory)) ||
              setrlimit(RLIMIT_CPU, &cpu))
            _exit(2);
          alarm(10);
          try {
            f();
            _exit(0);
          } catch (const std::exception &e) {
            std::cerr << e.what() << "\n";
            _exit(1);
          }
        }
        int status = 0;
        need(waitpid(pid, &status, 0) == pid, "wait layout child");
        need(WIFEXITED(status) && WEXITSTATUS(status) == 0,
             "layout child failed (exit/signal status=" +
                 std::to_string(status) + "); kill/timeout is a test failure");
      } else
        f();
      std::cout << id << ": passed\n";
    } catch (const std::exception &e) {
      ++failures;
      std::cout << id << ": FAILED: " << e.what() << "\n";
    }
  };
  test("repair-1-finite-layout", [&] {
    auto m = good("struct S[T]{f func() (S[S[T]]);} func main() i32{return 0;}");
    need(m.types.size() < 32, "function field expanded unused types");
    good("enum E[T]{A(f func(E[E[T]]) E[E[T]]);} func main() i32{return 0;}");
    good("struct Ignore[T]{x i32;} struct S[T]{x Ignore[S[S[T]]];} "
         "func main() i32{return 0;}");
    good("struct A[T]{x B[T];} struct B[T]{f func() A[A[T]];} "
         "func main() i32{return 0;}");
    good("trait V{func v(self Self) i32;} struct S[T V]{f func() S[S[T]];} "
         "impl[T V] V for S[T]{func v(self Self) i32{return 0;}} "
         "func main() i32{return 0;}");
    good("struct Box[T]{x T;} struct P{x Box[Box[i32]];} func main() i32{return 0;}");
    std::string deep;
    for (size_t i = 0; i < 80; ++i)
      deep += "struct S" + std::to_string(i) + "{x " +
              (i == 79 ? "i32" : "S" + std::to_string(i + 1)) + ";}";
    good(deep + "func main() i32{return 0;}");
    bad("struct Box[T]{x T;} struct S[T]{x Box[S[Box[T]]];} "
        "func main() i32{return 0;}", "E_TYPE_MISMATCH");
    bad("enum E[T]{A(x E[E[T]]);} func main() i32{return 0;}", "E_TYPE_MISMATCH");
  }, true);
  test("repair-2-qualified-enum-pattern", [&] {
    auto r = packages("pub enum E{A;}",
                      "func main() i32{switch lib.E.A{case lib.E.A:return 0;}}");
    need(r.success(), messages(r));
    r = packages("enum E{A;} pub func marker(){}",
                 "func main() i32{switch lib.E.A{case lib.E.A:return 0;}}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    r = packages("pub enum E{A;} pub enum F{A;}",
                 "func main() i32{switch lib.E.A{case lib.F.A:return 0;}}");
    need(!r.success() && has(r, "E_TYPE_MISMATCH"), messages(r));
    r = packages("pub enum E{A;}",
                 "func main() i32{switch lib.E.A{case lib.Missing.A:return 0;}}");
    need(!r.success() && has(r, "E_NAME_NOT_FOUND"), messages(r));
    r = packages("pub enum E{A;} pub const N=1;",
                 "func main() i32{switch lib.E.A{case lib.N.A:return 0;}}");
    need(!r.success() && has(r, "E_NAME_KIND"), messages(r));
  });
  test("repair-3-callable-field", [&] {
    good("struct C{cb func() i32;} func invoke(c C)i32{return c.cb();} "
         "func main() i32{return 0;}");
    good("struct C{cb func() i32;} func invoke(c C)i32{return (c.cb)();} "
         "func main() i32{return 0;}");
    auto m = good("struct C{cb func(i32) i32;} func id(x i32)i32{return x;} "
                  "func make() C{return C(cb:id);} func arg() i32{return 2;} "
                  "func main() i32{return make().cb(arg());}");
    auto once = [&](const std::string &name) {
      return std::count_if(m.nodes.begin(), m.nodes.end(), [&](const auto &n) {
        auto c = std::get_if<hir::Call>(&n.data);
        return c && c->direct_symbol &&
               m.resolved->symbols[c->direct_symbol->value].name == name;
      });
    };
    need(once("make") == 1 && once("arg") == 1, "callee/arg evaluated twice");
    bool saw = false;
    for (const auto &n : m.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->callee) {
        const auto &field = m.nodes[c->callee->value];
        need(field.kind() == hir::NodeKind::field_read && n.children.size() == 2 &&
                 n.children[0] == *c->callee &&
                 m.nodes[field.children[0].value].kind() == hir::NodeKind::call &&
                 n.children[1] == c->args[0].expression,
             "field callee/argument HIR order");
        saw = true;
      }
    need(saw, "function field call missing");
    bad("struct C{cb i32;} func invoke(c C)i32{return c.cb();} "
        "func main() i32{return 0;}", "E_CALL_ARGUMENT");
    good("struct C{cb func() i32;} impl C{func cb(self Self)i32{return 9;}} "
         "func invoke(c C)i32{return c.cb();} func main() i32{return 0;}");
  });
  test("repair-4-pattern-type-arguments", [&] {
    good("enum E[T]{A(x T);} func main() i32{let e=E[i32].A(x:1); "
         "switch e{case E[(i32)].A(x):return x;}}");
    good("enum E[T]{A(x T);} struct Box[T]{x T;} "
         "func main() i32{let e=E[Box[i32]].A(x:Box[i32](x:1)); "
         "switch e{case E[Box[(i32)]].A(x):return x.x;}}");
    good("enum E[T]{A(x T);} func id(x i32)i32{return x;} "
         "func main() i32{let e=E[func(i32) i32].A(x:id); "
         "switch e{case E[func((i32)) (i32)].A(f):return f(1);}}");
    bad("enum E[T]{A(x T);} func main() i32{let e=E[i32].A(x:1); "
        "switch e{case E[(Missing)].A(x):return x;}}", "E_NAME_NOT_FOUND");
    bad("enum E[T]{A(x T);} func id(x i32)i32{return x;} "
        "func main() i32{let e=E[func(i32)i32].A(x:id); "
        "switch e{case E[func(Missing)i32].A(f):return f(1);}}", "E_NAME_NOT_FOUND");
    bad("enum E[T]{A(x T);} const N=1; func main() i32{let e=E[i32].A(x:1); "
        "switch e{case E[(N)].A(x):return x;}}", "E_NAME_KIND");
    auto r = packages("pub enum E[T]{A(x T);} pub struct P{pub x i32;}",
        "func main() i32{let e=lib.E[lib.P].A(x:lib.P(x:1)); "
        "switch e{case lib.E[(lib.P)].A(x):return x.x;}}");
    need(r.success(), messages(r));
    r = packages("pub enum E[T]{A(x T);} struct Hidden{x i32;}",
        "func main() i32{let e=lib.E[i32].A(x:1); "
        "switch e{case lib.E[(lib.Hidden)].A(x):return x;}}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    r = packages("pub enum E[T]{A(x T);} struct Hidden{x i32;}",
        "func id(x i32)i32{return x;} "
        "func main() i32{let e=lib.E[func(i32)i32].A(x:id); "
        "switch e{case lib.E[func(lib.Hidden)i32].A(f):return 0;}}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
  });
  test("repair-5-constructor-visibility", [&] {
    auto r = packages("pub struct P{secret i32;}",
        "func main() i32{let p=lib.P(secret:1);return 0;}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    r = packages("pub struct P{pub x i32;}",
        "func main() i32{return lib.P(x:1).x;}");
    need(r.success(), messages(r));
    good("struct P{x i32;} func main() i32{return P(x:1).x;}");
    r = packages("pub enum E{A(x i32);}",
        "func main() i32{switch lib.E.A(x:1){case lib.E.A(x):return x;}}");
    need(r.success(), messages(r));
  });
  test("positional-enum-constructors", [&] {
    auto shared = check(root / "tests/generics/positional/payload/lang.toml");
    need(shared.success() && hir::verify(*shared.value).success(),
         messages(shared));
    auto r = packages("pub enum E[T]{A(x T);}",
        "func main() i32{switch lib.E[i32].A(42){case lib.E.A(x):return x;}}");
    need(r.success(), messages(r));
    r = packages("enum E[T]{A(x T);} pub func marker(){}",
        "func main(){let e=lib.E[i32].A(42);}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    good("enum E[T]{A(x T);} struct Box[T]{x T;} "
         "func main(){let e=E[Box[i32]].A(Box[i32](x:42));}");
    const std::string prefix = "enum E[T]{A(x T,y T);} func main(){";
    for (const auto *call : {"E[i32].A(1)", "E[i32].A(1,2,3)",
                            "E[i32].A(1,x:2)", "E[i32].A(x:1,x:2)",
                            "E[i32].A(1,unknown:2)"})
      bad(prefix + "let e=" + call + ";}", "E_CALL_ARGUMENT");
    bad(prefix + "let e=E[i32].A(1,false);}", "E_TYPE_MISMATCH");
    bad(prefix + "let e=E[i32].A(y:1,2);}", "E_PARSE_EXPECTED_TOKEN");
    for (const auto *call : {"E[i32].A(inout n,2)", "E[i32].A(move n,2)",
                            "E[i32].A(1,y:inout n)", "E[i32].A(1,y:move n)",
                            "E[i32].A((move n),2)"})
      bad(prefix + "var n i32=0;let e=" + call + ";}", "E_CALL_ARGUMENT");
    bad("struct S{x i32;} func main(){let s=S(1);}", "E_CALL_ARGUMENT");
    bad("struct S{x i32;y i32;} func main(){let s=S(1,y:2);}", "E_CALL_ARGUMENT");
    bad("struct S{x i32;} func main(){var n i32=0;let s=S(x:move n);}",
        "E_CALL_ARGUMENT");
    bad("struct S{x i32;} func main(){var n i32=0;let s=S(x:inout n);}",
        "E_CALL_ARGUMENT");
    for (bool generic : {false, true}) {
      const std::string declaration = generic ? "enum E[T]{A(value T);} "
                                               : "enum E{A(value i32);} ";
      const std::string callee = generic ? "E[i32].A[bool]" : "E.A[i64]";
      for (const auto *argument : {"42", "value:42"})
        bad(declaration + "func main(){let e=" + callee + "(" + argument + ");}",
            "E_CALL_ARGUMENT");
      bad((generic ? "enum E[T]{A;} " : "enum E{A;} ") +
              std::string("func main(){let e=") + callee + "();}",
          "E_CALL_ARGUMENT");
    }

    auto m = good("enum E{A(x i32,y i32);B(x i32,y i32);} "
                  "func main(){let e=E.A(1,2);}");
    auto init = [](auto &x) -> hir::EnumInit & {
      for (auto &n : x.nodes)
        if (auto e = std::get_if<hir::EnumInit>(&n.data))
          return *e;
      throw std::runtime_error("missing enum constructor");
    };
    invalid(m, "positional out-of-range index", [&](auto &x) {
      init(x).payload[0].parameter_index = 2;
    });
    invalid(m, "positional duplicate index", [&](auto &x) {
      init(x).payload[1].parameter_index = 0;
    });
    invalid(m, "positional reordered indices", [&](auto &x) {
      auto &args = init(x).payload;
      std::swap(args[0].parameter_index, args[1].parameter_index);
    });
    invalid(m, "positional source index", [&](auto &x) {
      init(x).payload[0].source_index = 1;
    });
    invalid(m, "positional invented label", [&](auto &x) {
      init(x).payload[0].label = "x";
    });
    invalid(m, "same-shaped foreign variant", [&](auto &x) {
      auto &e = init(x);
      for (const auto &d : x.nominals)
        for (const auto &v : d.variants)
          if (v.symbol != e.variant) {
            e.variant = v.symbol;
            return;
          }
    });
    for (auto mode : {hir::ParamMode::inout, hir::ParamMode::move})
      invalid(m, "positional mode", [&](auto &x) {
        init(x).payload[0].mode = mode;
      });
    invalid(m, "positional expression order", [&](auto &x) {
      for (auto &n : x.nodes)
        if (auto e = std::get_if<hir::EnumInit>(&n.data)) {
          std::swap(e->payload[0].expression, e->payload[1].expression);
          std::swap(n.children[0], n.children[1]);
          return;
        }
    });
    auto named = good("enum E{A(x i32,y i32);} func main(){let e=E.A(y:2,x:1);}");
    invalid(named, "named label removed", [&](auto &x) {
      init(x).payload[0].label.reset();
    });
    invalid(named, "named field identity changed", [&](auto &x) {
      auto &args = init(x).payload;
      std::swap(args[0].parameter_index, args[1].parameter_index);
      std::swap(args[0].label, args[1].label);
    });
    invalid(named, "named source order changed", [&](auto &x) {
      for (auto &n : x.nodes)
        if (auto e = std::get_if<hir::EnumInit>(&n.data)) {
          std::swap(e->payload[0], e->payload[1]);
          e->payload[0].source_index = 0;
          e->payload[1].source_index = 1;
          std::swap(n.children[0], n.children[1]);
          return;
        }
    });
    auto wrapped = good("enum E{A(x i32);} "
                        "func main(){var n i32=1;let e=E.A((+n));}");
    for (const auto *mode : {"inout", "move"})
      invalid(wrapped, "erased source argument mode", [&](auto &x) {
        auto names = std::make_shared<modules::ResolvedProject>(*x.resolved);
        auto project = std::make_shared<modules::Project>(*names->project);
        auto file = std::make_shared<AstFile>(*project->modules[0].files[0]);
        bool changed = false;
        for (const auto &n : file->nodes)
          if (n.kind == AstKind::Unary &&
              file->tokens[n.token_begin].spelling == "+") {
            file->tokens[n.token_begin].spelling = mode;
            changed = true;
          }
        need(changed, "missing source mode corruption target");
        project->modules[0].files[0] = file;
        names->project = project;
        x.resolved = names;
      });
    auto mixed = good("enum E{A(x i32,y i32,z i32);} "
                      "func main(){let e=E.A(1,z:3,y:2);}");
    for (const auto &n : mixed.nodes)
      if (auto e = std::get_if<hir::EnumInit>(&n.data))
        need(e->payload.size() == 3 && !e->payload[0].label &&
                 e->payload[0].parameter_index == 0 &&
                 e->payload[1].parameter_index == 2 &&
                 e->payload[2].parameter_index == 1,
             "mixed constructor lost source order/field indices");
    auto structure = good("struct S{x i32;} func main(){let s=S(x:1);}");
    invalid(structure, "struct positional HIR", [](auto &x) {
      for (auto &n : x.nodes)
        if (auto s = std::get_if<hir::StructInit>(&n.data)) {
          s->fields[0].label.reset();
          return;
        }
    });
  });
  test("repair-6-method-visibility", [&] {
    auto r = packages("impl i32{func hidden(self Self)i32{return 42;}} "
                      "pub func marker(){}",
        "func main() i32{let x i32=0;return x.hidden();}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    r = packages("impl i32{pub func value(self Self)i32{return 42;}} "
                 "pub func marker(){}",
        "func main() i32{let x i32=0;return x.value();}");
    need(r.success(), messages(r));
    r = packages("pub struct P{pub x i32;} "
                 "impl P{func hidden(self Self)i32{return self.x;}}",
        "func main() i32{return lib.P(x:1).hidden();}");
    need(!r.success() && has(r, "E_PRIVATE_ACCESS"), messages(r));
    r = packages("pub struct P{pub x i32;} "
                 "impl P{pub func value(self Self)i32{return self.x;}}",
        "func main() i32{return lib.P(x:1).value();}");
    need(r.success(), messages(r));
    good("impl i32{func hidden(self Self)i32{return 42;}} "
         "func main() i32{let x i32=0;return x.hidden();}");
    r = packages("pub trait V{func value(self Self)i32;} "
                 "impl V for i32{func value(self Self)i32{return 7;}}",
        "func get[T lib.V](x T)i32{return x.value();} "
        "func main() i32{let x i32=0;return get(x)+x.value();}");
    need(r.success(), messages(r));
  });
  test("repair-7-instance-symbol-origins", [&] {
    auto r = check(root / "tests/generics/fixtures/multi-instance/lang.toml");
    need(r.success(), messages(r));
    auto m = *r.value;
    size_t other = 1;
    while (other < m.instances.size() &&
           m.instances[other].origin != m.instances[0].origin)
      ++other;
    need(other < m.instances.size(), "second instance missing");
    for (size_t j = 0; j < m.instances[0].renamed.size(); ++j)
      invalid(m, "foreign instance source " + std::to_string(j), [=](auto &x) {
        auto from = x.instances[0].renamed[j].first;
        for (auto [original, concrete] : x.instances[other].renamed)
          if (original == from) {
            x.instances[0].renamed[j].first = concrete;
            return;
          }
        throw std::runtime_error("matching foreign symbol missing");
      });
    invalid(m, "missing original local plus target remapped", [](auto &x) {
      auto &i = x.instances[0];
      auto original = i.renamed.back().first, target = i.renamed.back().second;
      i.renamed.pop_back();
      for (auto &n : x.nodes)
        if (x.scopes[n.scope.value].function == i.origin)
          if (auto b = std::get_if<hir::Binding>(&n.data); b && b->symbol == original)
            b->symbol = target;
    });
    need(hir::verify_types(m).success(), "valid multi-instance HIR rejected");
    auto patterns = good("enum E[T]{A(x T);} func unwrap[T](e E[T])T{"
        "switch e{case E.A(x):return x;}} func main()i32{"
        "let a=unwrap(E[i32].A(x:1));let b=unwrap(E[i64].A(x:2));return a;}");
    invalid(patterns, "foreign instance pattern origin", [](auto &x) {
      auto &a = x.instances[0];
      const auto &b = x.instances[1];
      for (auto &entry : a.renamed) {
        auto original = x.resolved->symbols[entry.first.value].definition;
        if (original && x.resolved->project->module(original->module)
                            .files[original->file]->node(original->node).kind ==
                            AstKind::BindPattern)
          for (auto [from, to] : b.renamed)
            if (from == entry.first) { entry.first = to; return; }
      }
      throw std::runtime_error("pattern symbol missing");
    });
    invalid(patterns, "missing template pattern symbol", [](auto &x) {
      x.instances[0].renamed.pop_back();
    });
  });
  need(!failures, std::to_string(failures) + " T07 repair groups failed");
}
void generic_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported && has(r, code),
         text + "\n" + messages(r));
  };
  auto call_named = [](const hir::HirModule &m, const std::string &name) {
    std::vector<const hir::HirNode *> result;
    for (const auto &n : m.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          m.resolved->symbols[c->direct_symbol->value].name == name)
        result.push_back(&n);
    return result;
  };

  const auto positive =
      check(root / "tests/generics/move/positive/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(), "generic move HIR rejected");
  const auto &m = *positive.value;
  auto calls = call_named(m, "relay");
  need(calls.size() == 4, "generic move calls missing");
  size_t binding_moves = 0, temporary_moves = 0;
  for (const auto *call_node : calls) {
    const auto &call = std::get<hir::Call>(call_node->data);
    need(call.args.size() == 1, "generic move arity");
    const auto &arg = call.args[0];
    need(arg.mode == hir::ParamMode::move, "generic move argument mode");
    const auto &value = m.nodes[arg.expression.value];
    if (value.kind() == hir::NodeKind::move) {
      need(std::get<hir::Move>(value.data).binding.has_value(),
           "generic binding move lacks its source binding");
      ++binding_moves;
    } else {
      need(value.kind() == hir::NodeKind::vec_init ||
               value.kind() == hir::NodeKind::integer_literal,
           "generic temporary move shape");
      ++temporary_moves;
    }
  }
  need(binding_moves == 2 && temporary_moves == 2,
       "generic move source kinds changed");
  need(m.templates.size() == 1, "generic relay template missing");
  need(m.instances.size() == 3, "generic relay instance count");
  need(m.templates[0].function.parameters.size() == 1,
       "generic relay template parameter missing");
  need(m.types.find(m.templates[0].function.signature)->parameters[0].mode ==
           hir::ParamMode::move,
       "generic template parameter mode");
  for (const auto &instance : m.instances) {
    auto function = std::find_if(
        m.functions.begin(), m.functions.end(),
        [&](const auto &f) { return f.symbol == instance.symbol; });
    need(function != m.functions.end(), "generic move instance function");
    need(m.types.find(function->signature)->parameters[0].mode ==
             hir::ParamMode::move,
         "generic instance parameter mode");
    for (const auto *call_node : calls)
      if (auto c = std::get_if<hir::Call>(&call_node->data))
        need(!c->direct_symbol || *c->direct_symbol != instance.origin,
             "generic template entered execution");
  }

  auto named = check(root / "tests/generics/move/error-before/lang.toml");
  need(named.success(), messages(named));
  need(hir::verify(*named.value).success(), "generic named move HIR rejected");
  auto consumed = call_named(*named.value, "consume");
  need(consumed.size() == 1, "generic named call missing");
  const auto &named_call = std::get<hir::Call>(consumed[0]->data);
  need(named_call.args.size() == 3 &&
           named_call.args[0].parameter_index == 2 &&
           named_call.args[1].parameter_index == 1 &&
           named_call.args[2].parameter_index == 0,
       "generic named parameters were not reordered");
  for (size_t i = 1; i < named_call.args.size(); ++i)
    need(named.value->nodes[named_call.args[i - 1].expression.value]
                 .source.span.begin_byte <
             named.value->nodes[named_call.args[i].expression.value]
                 .source.span.begin_byte,
         "generic named argument evaluation order changed");
  need(named_call.args[2].mode == hir::ParamMode::move &&
           named.value->nodes[named_call.args[2].expression.value].kind() ==
               hir::NodeKind::move,
       "generic named move mode/source");

  bad("func relay[T](value move T) T{return move value;} "
      "func main() i32{let values Vec[i32]=[1];let result=relay(values);"
      "return result[0];}",
      "E_CALL_ARGUMENT");
  bad("func relay[T](value move T) T{return move value;} "
      "func forward[T](value T) T{return relay(move value);} "
      "func main() i32{return 0;}",
      "E_CALL_ARGUMENT");
  bad("func relay[T](value move T) T{return move value;} "
      "func main() i32{let values Vec[i32]=[1];let result=relay(move "
      "values);return result[0]+values[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad("func relay[T](value move T) T{return move value;} "
      "func main() i32{let values Vec[i32]=[1];let a=relay(move values);"
      "let b=relay(move values);return a[0]+b[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad("func use[T](first T,second move T){} "
      "func main() i32{let values Vec[i32]=[1];use(values,move values);"
      "return 0;}",
      "E_CALL_ARGUMENT");
  bad("func relay[T](value move T) T{return move value;} "
      "func main() i32{var values Vec[i32]=[1];defer{let x=values[0];}"
      "let result=relay(move values);return result[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad("enum E{Bad;} func bad() i32 throws E{throw E.Bad;} "
      "func consume[T](value move T,first i32,second i32)i32{return first;} "
      "func main() i32{var values Vec[i32]=[1];try{let r=consume(value:move "
      "values,first:0,second:bad());}catch e E{return values[0];}"
      "return 0;}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  good("func relay[T](value move T) T{return move value;} "
       "func main() i32{var values Vec[i32]=[1];let a=relay(move "
       "values);values=[7,8];return values[0]-7;}");
  good("func pair[T](a move T,b T) T{return move a;} "
       "func main() i32{let x i32=2;let r=pair(move 1,x);"
       "return i32(r)-1;}");
  good(
      "impl i32{func take[T](self Self,value move T)T{return move value;}} "
      "func main() i32{let x i32=1;let v Vec[i32]=[2];let r=x.take(move v);"
      "return r[0];}");
  const std::string static_move =
      "impl i32{func take[T](value move T)T{return move value;}} ";
  good(static_move +
       "func main() i32{let values Vec[i32]=[7];"
       "let result=i32.take(move values);return result[0];}");
  good(static_move +
       "func main() i32{let values Vec[i32]=[7];"
       "let result=i32.take[Vec[i32]](move values);"
       "return result[0];}");
  good("struct Box[T]{value T;} impl[T] Box[T]{"
       "func take[U](value move U)U{return move value;}} "
       "func main() i32{let values Vec[i32]=[7];"
       "let result=Box[i32].take(move values);return result[0]-7;}");
  good("impl i32{func id[T](value T)T{return value;}} "
       "func main() i32{return i32.id(7)-7;}");
  good("impl i32{func id[T](value T)T{return value;}} "
       "func main() i32{return i32.id[i32](7)-7;}");
  good("struct Box[T]{value T;} impl[T] Box[T]{"
       "func id[U](self Self,value U)U{return value;}} "
       "func main() i32{let box=Box[i32](value:1);return box.id(7)-7;}");
  good("func swap[T](a inout T,b inout T){} "
       "func main() i32{var x i32=1;var y i32=2;"
       "swap(inout x,inout y);return x;}");
  good("trait V{} impl V for i32{} "
       "func take[T V](value move T)T{return move value;} "
       "func main() i32{var x i32=1;let r=take(move x);return r-1;}");
  good("func relay[T](value move T)T{return move value;} "
       "func main(){let r=relay[string](move \"x\");}");
  good("func relay[T](value move T)T{return move value;} "
       "func main(){let r=relay[string](\"x\");}");

  auto move_hir = good(
      "func relay[T](value move T) T{return move value;} "
      "func main() i32{var values Vec[i32]=[1];let result=relay(move "
      "values);return 0;}");
  auto relay_call = [](auto &x) -> hir::Call & {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          x.resolved->symbols[c->direct_symbol->value].name == "relay")
        return *c;
    throw std::runtime_error("generic move call missing");
  };
  invalid(move_hir, "generic move argument mode", [&](auto &x) {
    auto &call = relay_call(x);
    need(call.args.size() == 1, "generic move arity");
    call.args[0].mode = hir::ParamMode::ordinary;
  });
  invalid(move_hir, "generic move source marker", [&](auto &x) {
    auto &call = relay_call(x);
    const auto &value = x.nodes[call.args[0].expression.value];
    const auto &move = std::get<hir::Move>(value.data);
    need(move.binding.has_value(), "generic move binding missing");
    auto names = std::make_shared<modules::ResolvedProject>(*x.resolved);
    auto project = std::make_shared<modules::Project>(*names->project);
    auto file = std::make_shared<AstFile>(
        *project->modules[value.source.node.module.value].files.at(
            value.source.node.file));
    auto &ast = file->node(value.source.node.node);
    need(ast.kind == AstKind::Unary &&
             file->tokens.at(ast.token_begin).spelling == "move",
         "generic move source token missing");
    file->tokens[ast.token_begin].spelling = "+";
    project->modules[value.source.node.module.value]
        .files[value.source.node.file] = file;
    names->project = project;
    x.resolved = names;
  });
  invalid(move_hir, "generic instance parameter mode", [](auto &x) {
    need(!x.instances.empty(), "generic move instance missing");
    for (auto &f : x.functions)
      if (f.symbol == x.instances[0].symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].mode = hir::ParamMode::ordinary;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("generic move instance function missing");
  });
  invalid(move_hir, "generic template parameter mode", [](auto &x) {
    need(!x.templates.empty(), "generic move template missing");
    auto &f = x.templates[0].function;
    auto sig = *x.types.find(f.signature);
    sig.parameters[0].mode = hir::ParamMode::ordinary;
    f.signature = x.types.intern(sig);
  });
}


void constrained_free_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "constrained free move HIR rejected: " + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  auto positive =
      check(root / "tests/generics/move/constrained-positive/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(),
       "constrained free move HIR rejected");
  const auto &m = *positive.value;

  auto symbol_name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return symbol_name(t->declaration);
  };
  auto find_template = [&](const std::string &name)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (symbol_name(t.function.symbol) == name)
        return &t;
    return nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto has_parameter = [&](TypeId root_id) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root_id);
  };
  auto find_trait = [&](const std::string &name) -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (symbol_name(t.symbol) == name)
        return &t;
    return nullptr;
  };
  auto item_trait_method = [&](const hir::TraitDecl &trait,
                               const std::string &name) -> SymbolId {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait.symbol && nominal_name(im.self_type) == "Item")
        for (auto method : im.methods)
          if (symbol_name(method) == name)
            return method;
    throw std::runtime_error("Item trait method missing: " + name);
  };

  const auto *read = find_template("read");
  const auto *pair = find_template("pair");
  const auto *forward = find_template("forward");
  const auto *right = find_template("right");
  const auto *left = find_template("left");
  const auto *value_trait = find_trait("Value");
  const auto *right_trait = find_trait("Right");
  const auto *left_trait = find_trait("Left");
  need(read && pair && forward && right && left && value_trait &&
           right_trait && left_trait,
       "constrained free move templates or traits missing");

  const auto read_instances = instances_of(read);
  const auto pair_instances = instances_of(pair);
  const auto forward_instances = instances_of(forward);
  const auto right_instances = instances_of(right);
  const auto left_instances = instances_of(left);
  need(read_instances.size() == 3 && pair_instances.size() == 1 &&
           forward_instances.size() == 1 && right_instances.size() == 1 &&
           left_instances.size() == 1,
       "constrained free move instance counts changed");
  need(right_instances[0]->symbol != left_instances[0]->symbol &&
           right_instances[0]->origin != left_instances[0]->origin,
       "same-named methods from different traits shared an instance");
  need(pair_instances[0]->type_args.size() == 2 &&
           nominal_name(pair_instances[0]->type_args[0]) == "Item" &&
           nominal_name(pair_instances[0]->type_args[1]) == "Item",
       "pair complete type argument key changed");

  auto instance_category = [&](const hir::FunctionInstance *i)
      -> std::string {
    if (i->type_args.size() != 1)
      return {};
    auto t = m.types.find(i->type_args[0]);
    if (!t)
      return {};
    if (t->kind == hir::TypeKind::NominalStruct &&
        symbol_name(t->declaration) == "Item")
      return "Item";
    if (t->kind == hir::TypeKind::SignedInt && t->bits == 32)
      return "i32";
    if (t->kind == hir::TypeKind::Bool)
      return "bool";
    return {};
  };
  std::map<std::string, SymbolId> read_by_type;
  for (const auto *i : read_instances) {
    auto category = instance_category(i);
    need(!category.empty(), "unexpected read instance type");
    need(read_by_type.emplace(category, i->symbol).second,
         "read instance type combination was not unique");
  }
  need(read_by_type.size() == 3 && read_by_type.count("Item") &&
           read_by_type.count("i32") && read_by_type.count("bool"),
       "read concrete type combinations missing");

  auto pairs_for_read = [&](SymbolId target) -> bool {
    for (const auto *i : read_instances)
      if (i->symbol == target)
        return true;
    return false;
  };

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  std::map<uint64_t, size_t> read_calls;
  std::map<uint64_t, size_t> pair_calls;
  std::map<uint64_t, size_t> right_calls;
  std::map<uint64_t, size_t> left_calls;
  for (const auto &n : m.nodes) {
    if (template_symbols.count(m.scopes[n.scope.value].function.value))
      continue;
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol)
      continue;
    auto name = symbol_name(*c->direct_symbol);
    if (name == "read") {
      need(pairs_for_read(*c->direct_symbol),
           "concrete read call still targets its template");
      need(c->type_args.empty(), "concrete read call retained type arguments");
      need(c->args.size() == 1 &&
               c->args[0].mode == hir::ParamMode::move,
           "concrete read call lost its move argument");
      ++read_calls[c->direct_symbol->value];
    } else if (name == "pair") {
      need(pair_instances[0]->symbol == *c->direct_symbol,
           "pair call selected a wrong instance");
      need(c->type_args.empty(), "concrete pair call retained type arguments");
      need(c->args.size() == 2 &&
               c->args[0].mode == hir::ParamMode::move &&
               c->args[1].mode == hir::ParamMode::move,
           "concrete pair call lost a move argument");
      ++pair_calls[c->direct_symbol->value];
    } else if (name == "right") {
      need(right_instances[0]->symbol == *c->direct_symbol,
           "right call selected a wrong instance");
      ++right_calls[c->direct_symbol->value];
    } else if (name == "left") {
      need(left_instances[0]->symbol == *c->direct_symbol,
           "left call selected a wrong instance");
      ++left_calls[c->direct_symbol->value];
    }
  }
  need(read_calls.size() == 3 && pair_calls.size() == 1 &&
           right_calls.size() == 1 && left_calls.size() == 1 &&
           pair_calls.begin()->second == 2 &&
           right_calls.begin()->second == 2 &&
           left_calls.begin()->second == 1,
       "constrained free move call reuse changed");
  need(read_calls[read_by_type["Item"].value] == 4 &&
           read_calls[read_by_type["i32"].value] == 1 &&
           read_calls[read_by_type["bool"].value] == 1,
       "read same-combination reuse changed");

  size_t template_constraints = 0;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == read->function.symbol)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(c->trait.declaration == value_trait->symbol, "read template trait changed");
        need(symbol_name(c->method) == "get",
             "read template method changed");
        ++template_constraints;
      }
  need(template_constraints == 1,
       "read template did not retain its ordinary trait ConstraintCall");

  for (const auto &f : m.functions) {
    auto sig = m.types.find(f.signature);
    need(sig && !has_parameter(f.signature),
         "concrete constrained free move signature retained T");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete constrained free move node retained T");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete constrained free move retained ConstraintCall");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete constrained free move retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete constrained free move call targets a template");
      }
    }
  }

  auto right_pick = item_trait_method(*right_trait, "pick");
  auto left_pick = item_trait_method(*left_trait, "pick");
  need(right_pick != left_pick,
       "same-named methods from different traits shared a symbol");
  auto concrete_function = [&](const hir::FunctionInstance *i) {
    auto f = function_for(i->symbol);
    need(f, "constrained free move instance function missing");
    return f;
  };
  auto method_calls = [&](const hir::HirFunction *f) {
    std::vector<SymbolId> result;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == f->symbol)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->direct_symbol && symbol_name(*c->direct_symbol) == "pick")
          result.push_back(*c->direct_symbol);
    return result;
  };
  const auto *right_function = concrete_function(right_instances[0]);
  const auto *left_function = concrete_function(left_instances[0]);
  auto right_selected = method_calls(right_function);
  auto left_selected = method_calls(left_function);
  need(right_selected.size() == 1 && right_selected[0] == right_pick,
       "Right constraint did not select Right.pick");
  need(left_selected.size() == 1 && left_selected[0] == left_pick,
       "Left constraint did not select Left.pick");

  const auto *forward_function = concrete_function(forward_instances[0]);
  std::map<uint64_t, SymbolId> renamed;
  for (auto [from, to] : forward_instances[0]->renamed)
    renamed[from.value] = to;
  need(renamed.at(forward->function.parameters[0].symbol.value) ==
               forward_function->parameters[0].symbol,
       "forward instance lost its renamed move parameter");
  size_t forward_moves = 0;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == forward_function->symbol) {
      if (auto move = std::get_if<hir::Move>(&n.data);
          move && move->binding) {
        need(*move->binding == forward_function->parameters[0].symbol,
             "forward concrete Move kept a template binding");
        ++forward_moves;
      }
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && symbol_name(*c->direct_symbol) == "read") {
        need(*c->direct_symbol == read_by_type["Item"],
             "forward forwarding call did not reuse read[Item]");
        need(c->args.size() == 1 &&
                 c->args[0].mode == hir::ParamMode::move &&
                 c->type_args.empty(),
             "forward forwarding call lost its concrete move argument");
      }
    }
  need(forward_moves == 1,
       "forward concrete instance lost its renamed Move");

  good("trait V{} impl V for i32{} "
       "func take[T V](value move T)T{return move value;} "
       "func main()i32{var x i32=1;let f=take[i32];"
       "let r=f(move x);return r-1;}");
  bad("trait V{func get(self Self)i32;} struct S{x i32;} "
      "func read[T V](value move T)i32{return value.get();} "
      "func main()i32{var s S=S(x:1);let r=read(move s);return r;}",
      "E_TYPE_MISMATCH");
  bad("trait A{} trait B{} impl B for i32{} "
      "func take[T A](value move T)T{return move value;} "
      "func main()i32{var x i32=1;let r=take(move x);return r-1;}",
      "E_TYPE_MISMATCH");
  bad("trait V{} impl V for i32{} "
      "func take[T V](value move T)T{return move value;} "
      "func main()i32{let x i32=1;let r=take(x);return i32(r)-1;}",
      "E_CALL_ARGUMENT");
  bad("trait V{} impl V for i32{} "
      "func take[T V](value move T)T{return move value;} "
      "func main()i32{var x i32=1;let r=take(move x);return r+x;}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad("trait V{} impl V for i32{} "
      "func pair[T V](first T,second move T)i32{return 0;} "
      "func main()i32{var x i32=1;return pair(x,move x);}",
      "E_CALL_ARGUMENT");
  good("trait V{} impl V for i32{} "
       "func bump[T V](x inout T){} "
       "func main()i32{var x i32=1;bump(inout x);return x-1;}");
  good("trait V{func take(self Self,value move Vec[i32])Vec[i32];} "
       "struct S{x i32;} impl V for S{"
       "func take(self Self,value move Vec[i32])Vec[i32]{"
       "return move value;}} "
       "func main()i32{let s=S(x:1);var v Vec[i32]=[1];"
       "let r=s.take(move v);return r[0]-1;}");
  good("trait V{} impl V for i32{} struct S{x i32;} impl S{"
       "func take[T V](self Self,value move T)T{return move value;}} "
       "func main()i32{let s=S(x:1);var x i32=1;"
       "let r=s.take(move x);return r-1;}");
}

void constrained_free_inout(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "constrained free inout HIR rejected: " + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const auto positive =
      check(root / "tests/generics/inout/constrained/positive/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(),
       "constrained free inout fixture HIR rejected");
  const auto &m = *positive.value;

  auto symbol_name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto find_template = [&](const std::string &name)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (symbol_name(t.function.symbol) == name)
        return &t;
    return nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_i64 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 64;
  };
  auto type_label = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t)
      return {};
    switch (t->kind) {
    case hir::TypeKind::SignedInt:
      return "i" + std::to_string(t->bits);
    case hir::TypeKind::UnsignedInt:
      return "u" + std::to_string(t->bits);
    case hir::TypeKind::Float:
      return "f" + std::to_string(t->bits);
    case hir::TypeKind::Bool:
      return "bool";
    default:
      return {};
    }
  };
  auto has_parameter = [&](TypeId root_id) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root_id);
  };
  auto find_trait = [&](const std::string &name) -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (symbol_name(t.symbol) == name)
        return &t;
    return nullptr;
  };
  auto trait_method_for = [&](const hir::TraitDecl &trait,
                              const std::string &type,
                              const std::string &method) -> SymbolId {
    for (const auto &im : m.impls) {
      if (!im.trait || im.trait->declaration != trait.symbol)
        continue;
      auto t = m.types.find(im.self_type);
      if (t && t->kind == hir::TypeKind::SignedInt && type == "i32" &&
          t->bits == 32)
        for (auto candidate : im.methods)
          if (symbol_name(candidate) == method)
            return candidate;
    }
    throw std::runtime_error("trait impl method missing: " + type + "." +
                             method);
  };

  const auto *swap = find_template("swap");
  const auto *replace = find_template("replace");
  const auto *pair = find_template("pair");
  const auto *left = find_template("left");
  const auto *right = find_template("right");
  const auto *mix = find_template("mix");
  const auto *value_trait = find_trait("Value");
  const auto *left_trait = find_trait("Left");
  const auto *right_trait = find_trait("Right");
  need(swap && replace && pair && left && right && mix && value_trait &&
           left_trait && right_trait,
       "constrained free inout templates or traits missing");

  const auto swap_instances = instances_of(swap);
  const auto replace_instances = instances_of(replace);
  const auto pair_instances = instances_of(pair);
  const auto left_instances = instances_of(left);
  const auto right_instances = instances_of(right);
  const auto mix_instances = instances_of(mix);
  need(swap_instances.size() == 7 && replace_instances.size() == 1 &&
           pair_instances.size() == 1 && left_instances.size() == 1 &&
           right_instances.size() == 1 && mix_instances.size() == 1,
       "constrained free inout instance counts changed");

  std::set<std::string> swap_types;
  for (const auto *instance : swap_instances) {
    need(instance->type_args.size() == 1,
         "swap instance lacks its complete type key");
    auto label = type_label(instance->type_args[0]);
    need(!label.empty(), "swap instance has an unexpected type");
    need(swap_types.insert(label).second, "swap type combination duplicated");
    auto f = function_for(instance->symbol);
    need(f, "swap concrete function missing");
    auto sig = m.types.find(f->signature);
    need(sig && sig->parameters.size() == 2 &&
             sig->parameters[0].mode == hir::ParamMode::inout &&
             sig->parameters[1].mode == hir::ParamMode::inout &&
             sig->parameters[0].type == instance->type_args[0] &&
             sig->parameters[1].type == instance->type_args[0] &&
             !has_parameter(f->signature),
         "swap concrete signature retained a template or wrong mode");
  }
  need(swap_types.size() == 7 && swap_types.count("i32") &&
           swap_types.count("i64") && swap_types.count("u32") &&
           swap_types.count("u64") && swap_types.count("f32") &&
           swap_types.count("f64") && swap_types.count("bool"),
       "swap concrete data types changed");

  need(pair_instances[0]->type_args.size() == 2 &&
           is_i32(pair_instances[0]->type_args[0]) &&
           is_i32(pair_instances[0]->type_args[1]),
       "pair full type key changed");
  const auto *pair_function = function_for(pair_instances[0]->symbol);
  need(pair_function, "pair concrete function missing");
  auto pair_sig = m.types.find(pair_function->signature);
  need(pair_sig && pair_sig->parameters.size() == 5 &&
           pair_sig->parameters[0].mode == hir::ParamMode::inout &&
           pair_sig->parameters[1].mode == hir::ParamMode::inout &&
           pair_sig->parameters[2].type == pair_instances[0]->type_args[0] &&
           pair_sig->parameters[3].type == pair_instances[0]->type_args[1] &&
           pair_sig->parameters[4].mode == hir::ParamMode::inout &&
           is_i64(pair_sig->parameters[4].type),
       "pair constraint/mode signature changed");

  const auto *replace_instance = replace_instances[0];
  const auto *replace_function = function_for(replace_instance->symbol);
  need(replace_instance->type_args.size() == 1 &&
           is_i32(replace_instance->type_args[0]) && replace_function,
       "replace [i32] instance missing");
  auto replace_sig = m.types.find(replace_function->signature);
  need(replace_sig && replace_sig->parameters.size() == 2 &&
           replace_sig->parameters[0].mode == hir::ParamMode::inout &&
           is_i32(replace_sig->parameters[0].type) &&
           replace_sig->parameters[1].mode == hir::ParamMode::ordinary &&
           is_i32(replace_sig->parameters[1].type) &&
           is_i32(replace_sig->result) &&
           !has_parameter(replace_function->signature),
       "replace concrete signature retained T or lost its mode");

  size_t template_constraints = 0;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == replace->function.symbol)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(c->trait.declaration == value_trait->symbol,
             "replace template constraint trait changed");
        need(symbol_name(c->method) == "get",
             "replace template constraint method changed");
        ++template_constraints;
      }
  need(template_constraints == 1,
       "replace template did not retain its ordinary trait ConstraintCall");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  size_t function_values = 0, swap_values = 0, replace_values = 0;
  std::set<uint64_t> swap_instance_symbols;
  for (const auto *i : swap_instances)
    swap_instance_symbols.insert(i->symbol.value);
  for (const auto &f : m.functions) {
    need(!has_parameter(f.signature),
         "concrete constrained free inout signature retained a parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete constrained free inout node retained a parameter");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete constrained free inout retained ConstraintCall");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete constrained free inout call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete constrained free inout call targets a template");
      }
      if (auto v = std::get_if<hir::FunctionValue>(&n.data)) {
        need(v->type_args.empty(),
             "concrete function value retained type arguments");
        need(!template_symbols.count(v->symbol.value),
             "concrete function value targets a template");
        ++function_values;
        if (symbol_name(v->symbol) == "swap") {
          need(swap_instance_symbols.count(v->symbol.value),
               "swap function value targets a foreign instance");
          ++swap_values;
        }
        if (symbol_name(v->symbol) == "replace")
          ++replace_values;
      }
    }
  }
  need(function_values >= 4 && swap_values >= 1 && replace_values >= 3,
       "constrained free inout function values changed");

  size_t inout_arguments = 0;
  for (const auto &n : m.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data))
      for (const auto &a : c->args)
        if (a.mode == hir::ParamMode::inout) {
          auto &value = m.nodes[a.expression.value];
          need(value.kind() == hir::NodeKind::place,
               "constrained inout argument is not a Place");
          auto owner = m.scopes[n.scope.value].function;
          auto f = function_for(owner);
          if (f) {
            auto place = std::get<hir::Place>(value.data);
            auto sig = m.types.find(f->signature);
            for (size_t i = 0; i < f->parameters.size(); ++i)
              if (f->parameters[i].symbol == place.symbol) {
                need(sig && i < sig->parameters.size() &&
                         m.types.find(sig->parameters[i].type)->kind !=
                             hir::TypeKind::TypeParameter,
                     "constrained inout Place kept a template parameter");
                break;
              }
          }
          ++inout_arguments;
        }
  need(inout_arguments >= 20, "constrained free inout calls missing");

  auto calls_named = [&](const hir::HirFunction &f, const std::string &name) {
    std::vector<SymbolId> result;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == f.symbol)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->direct_symbol && symbol_name(*c->direct_symbol) == name)
          result.push_back(*c->direct_symbol);
    return result;
  };
  auto left_pick = trait_method_for(*left_trait, "i32", "pick");
  auto right_pick = trait_method_for(*right_trait, "i32", "pick");
  need(left_pick != right_pick,
       "same-named methods from different traits shared a symbol");
  const auto *left_function = function_for(left_instances[0]->symbol);
  const auto *right_function = function_for(right_instances[0]->symbol);
  need(left_function && right_function, "left/right concrete function missing");
  auto left_selected = calls_named(*left_function, "pick");
  auto right_selected = calls_named(*right_function, "pick");
  need(left_selected.size() == 1 && left_selected[0] == left_pick,
       "Left constraint did not select Left.pick");
  need(right_selected.size() == 1 && right_selected[0] == right_pick,
       "Right constraint did not select Right.pick");

  std::map<uint64_t, SymbolId> renamed;
  for (auto [from, to] : replace_instance->renamed)
    renamed[from.value] = to;
  need(renamed.at(replace->function.symbol.value) ==
               replace_function->symbol &&
           renamed.at(replace->function.parameters[0].symbol.value) ==
               replace_function->parameters[0].symbol &&
           renamed.at(replace->function.parameters[1].symbol.value) ==
               replace_function->parameters[1].symbol,
       "replace renamed map lost the function or an inout/ordinary parameter");
  size_t template_locals = 0, concrete_locals = 0;
  for (const auto &n : m.nodes) {
    if (m.scopes[n.scope.value].function == replace->function.symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        need(renamed.count(b->symbol.value),
             "replace renamed map lost a template local");
        ++template_locals;
      }
    if (m.scopes[n.scope.value].function == replace_function->symbol)
      if (std::holds_alternative<hir::Binding>(n.data))
        ++concrete_locals;
  }
  need(template_locals == 2 && concrete_locals == 2,
       "replace template/instance locals changed");
  bool concrete_place = false;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == replace_function->symbol)
      if (auto p = std::get_if<hir::Place>(&n.data); p) {
        need(p->symbol == replace_function->parameters[0].symbol,
             "replace concrete Place kept a template parameter");
        concrete_place = true;
      }
  need(concrete_place, "replace concrete inout Place missing");

  invalid(m, "constrained inout call mode", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          x.resolved->symbols[c->direct_symbol->value].name == "swap")
        for (auto &a : c->args)
          if (a.mode == hir::ParamMode::inout) {
            a.mode = hir::ParamMode::ordinary;
            return;
          }
    throw std::runtime_error("constrained inout call missing");
  });
  invalid(m, "constrained inout place read", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          x.resolved->symbols[c->direct_symbol->value].name == "swap")
        for (auto &a : c->args)
          if (a.mode == hir::ParamMode::inout) {
            auto &value = x.nodes[a.expression.value];
            value.data = hir::Read{std::get<hir::Place>(value.data).symbol};
            return;
          }
    throw std::runtime_error("constrained inout Place missing");
  });
  invalid(m, "constrained function value target", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto v = std::get_if<hir::FunctionValue>(&n.data);
          v && x.resolved->symbols[v->symbol.value].name == "replace") {
        v->symbol = replace->function.symbol;
        return;
      }
    throw std::runtime_error("constrained function value missing");
  });
  invalid_recomputed(m, "constrained instance parameter mode", [&](auto &x) {
    for (auto &f : x.functions)
      if (f.symbol == replace_instance->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].mode = hir::ParamMode::ordinary;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("constrained replace instance missing");
  });

  bad("trait V{func get(self Self)i32;} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x i32=1;var y i32=2;swap(inout x,inout y);}",
      "E_TYPE_MISMATCH");
  bad("trait A{func get(self Self)i32;} trait B{} impl B for i32{} "
      "func swap[T A](a inout T,b inout T){} "
      "func main(){var x i32=1;var y i32=2;swap(inout x,inout y);}",
      "E_TYPE_MISMATCH");
  auto constrained_aggregate = good(
      "trait V{func get(self Self)i32;} struct S{x i32;} "
      "impl V for S{func get(self Self)i32{return self.x;}} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x S=S(x:1);var y S=S(x:2);"
      "swap(inout x,inout y);}");
  need(hir::verify(constrained_aggregate).success(),
       "constrained POD aggregate inout did not publish");
  good("trait V{func get(self Self)i32;} "
       "impl V for i32{func get(self Self)i32{return self;}} "
       "struct B[T]{x T;} impl[T] B[T]{"
       "func swap[X V](self Self,a inout X,b inout X){}} "
       "func main(){let b=B[i32](x:1);var x i32=1;var y i32=2;"
       "b.swap(inout x,inout y);}");
  good("trait V{func bump(self Self,slot inout i32);} "
       "struct S{x i32;} impl V for S{"
       "func bump(self Self,slot inout i32){}} "
       "func main(){let s=S(x:1);var x i32=1;s.bump(inout x);}");
  unsupported("trait V{} impl V for i32{} "
              "impl i32{func bump(self inout i32,slot inout i32){}} "
              "func main(){var x i32=1;x.bump(inout x);}");
  bad("trait V{func get(self Self)i32;} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x i32=1;var y i32=2;let f=swap[i32];"
      "f(inout x,inout y);}",
      "E_TYPE_MISMATCH");
  auto constrained_aggregate_value = good(
      "trait V{func get(self Self)i32;} struct S{x i32;} "
      "impl V for S{func get(self Self)i32{return self.x;}} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x S=S(x:1);var y S=S(x:2);let f=swap[S];"
      "f(inout x,inout y);}");
  need(hir::verify(constrained_aggregate_value).success(),
       "constrained POD aggregate function value did not publish");
  bad("trait V{} impl V for i32{} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x i32=1;swap(inout x,inout x);}",
      "E_CALL_ARGUMENT");
  bad("trait V{} impl V for i32{} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(){var x i32=1;let f=swap[i32];f(inout x,inout x);}",
      "E_CALL_ARGUMENT");
  unsupported("trait V{} impl V for i32{} struct S{x i32;} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var s S=S(x:1);var y i32=2;"
              "swap(inout s.x,inout y);}");
  unsupported("trait V{} impl V for i32{} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var v Vec[i32]=[1];var y i32=2;"
              "swap(inout v[0],inout y);}");
  bad("trait V{} impl V for i32{} "
      "func swap[T V](a inout T,b inout T){} "
      "func main(x i32){var y i32=2;swap(inout x,inout y);}",
      "E_CALL_ARGUMENT");
  bad("trait V{} impl V for i32{} func g(x i32)i32{return x;} "
      "func use[T V](a inout T,b T){} "
      "func main(){var x i32=1;use(inout x,g(x));}",
      "E_CALL_ARGUMENT");
  bad("trait V{} impl V for i32{} func g(x inout i32)i32{x+=1;return x;} "
      "func use[T V](a inout T,b T){} "
      "func main(){var x i32=1;use(inout x,g(inout x));}",
      "E_CALL_ARGUMENT");
  unsupported("trait V{} impl V for i32{} "
              "struct Box[T]{cb func(inout T);} "
              "func keep[T V](cb func() Box[Vec[i32]])i32{return 0;} "
              "func main()i32{let f=keep[i32];return 0;}");
  auto name_expected = good("trait V{func get(self Self)i32;} "
      "impl V for i32{func get(self Self)i32{return self;}} "
      "func replace[T V](slot inout T,value T)T{"
      "let old=slot;slot=value;return old;} "
      "func main()i32{var x i32=1;"
      "let f func(inout i32,i32)i32=replace;"
      "let old=f(inout x,2);return old+i32(x)-3;}");
  need(hir::verify(name_expected).success(),
       "Name expected function type constrained inout HIR rejected");
}

void generic_inout(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported && has(r, code),
         text + "\n" + messages(r));
  };
  const std::string swap =
      "func swap[T](a inout T,b inout T){let tmp=a;a=b;b=tmp;} ";

  good(swap + "func main()i32{var x i32=1;var y i32=2;"
              "swap(inout x,inout y);return x-2;}");
  good(swap + "func main()i32{var x bool=true;var y bool=false;"
              "swap(b:inout y,a:inout x);if x{return 1;}return 0;}");
  good(swap + "func main()i32{var x f32=1.5;var y f32=2.5;"
              "swap(inout x,inout y);if x>2.0{return 0;}return 1;}");
  good(swap + "func main()i32{var x f64=1.5;var y f64=2.5;"
              "swap[f64](inout x,inout y);return 0;}");
  good(swap + "func relay[T](a inout T,b inout T){"
              "swap(inout a,inout b);} func main()i32{var x i32=1;"
              "var y i32=2;relay(inout x,inout y);return x-2;}");
  good("func swap[T](a inout T,b inout T){let tmp=a;a=b;b=tmp;} "
       "func inner(a inout i32,b inout i32){swap(inout a,inout b);} "
       "func main()i32{var x i32=1;var y i32=2;"
       "inner(inout x,inout y);return x-2;}");
  good(swap + "func main()i32{var x i32=1;var y i32=2;"
              "let f=swap[i32];f(inout x,inout y);return x-2;}");
  good(swap + "func main()i32{var x i32=1;var y i32=2;"
              "let f func(inout i32,inout i32)=swap;f(inout x,inout y);"
              "return x-2;}");
  good(swap + "func main()i32{var x i32=7;var y i32=7;"
              "swap(inout x,inout y);return x-7;}");
  good(swap + "func main()i32{var x i32=1;var y i32=2;"
              "swap(b:inout y,a:inout x);return x-2;}");

  bad(swap + "func main(){var x i32=1;swap(inout x,inout x);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;swap(inout x,x);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;swap(x,inout x);}",
      "E_CALL_ARGUMENT");
  bad("func mix[T](a inout T,b T){let tmp=a;a=b;} "
      "func main(){var x i32=1;mix(inout x,x);}",
      "E_CALL_ARGUMENT");
  bad("func mix[T](a inout T,b T){let tmp=a;a=b;} "
      "func main(){var x i32=1;mix(x,inout x);}",
      "E_CALL_ARGUMENT");
  bad("func g(x inout i32)i32{x+=1;return x;} "
      "func use[T](a inout T,b T){} func main(){var x i32=1;"
      "use(inout x,g(inout x));}",
      "E_CALL_ARGUMENT");
  bad("func g(x i32)i32{return x;} func use[T](a inout T,b T){} "
      "func main(){var x i32=1;use(inout x,g(x));}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;var y i32=2;"
              "swap(b:inout x,a:inout x);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;var y i32=2;swap(x,y);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;var y i32=2;"
              "swap(move x,inout y);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){let x i32=1;var y i32=2;"
              "swap(inout x,inout y);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){const x i32=1;var y i32=2;"
              "swap(inout x,inout y);}",
      "E_CALL_ARGUMENT");
  bad("func swap[T](a inout T,b inout T){} func main(x i32){var y i32=2;"
      "swap(inout x,inout y);}",
      "E_CALL_ARGUMENT");
  bad(swap + "func main(){var x i32=1;var y i64=2;"
              "swap(inout x,inout y);}",
      "E_TYPE_MISMATCH");

  unsupported(swap + "func main(){swap(inout 1,inout 2);}",
              "E_CAPABILITY_UNAVAILABLE");
  auto aggregate_struct = good("struct S { x i32; } " + swap +
                  "func main(){var s S=S(x:1);var t S=S(x:2);"
                  "swap(inout s,inout t);}");
  need(hir::verify(aggregate_struct).success(),
       "POD struct inout did not publish");
  auto aggregate_enum = good("enum E { A; } " + swap +
                  "func main(){var x E=E.A;var y E=E.A;"
                  "swap(inout x,inout y);}");
  need(hir::verify(aggregate_enum).success(),
       "empty-enum inout did not publish");
  unsupported(swap + "func main(){var x Vec[i32]=[1];var y Vec[i32]=[2];"
              "swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported(swap + "func main(){var x string=\"a\";var y string=\"b\";"
              "swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("func inc(x i32)i32{return x;} " + swap +
                  "func main(){var f func(i32)i32=inc;"
                  "var g func(i32)i32=inc;swap(inout f,inout g);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("func make(){} " + swap +
                  "func main(){var x Unit=make();var y Unit=make();"
                  "swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("struct S { x i32; } " + swap +
                  "func main(){var s S=S(x:1);var y i32=2;"
                  "swap(inout s.x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("func swap[T](a inout T,b inout T){} "
              "func main(){var v Vec[i32]=[1];var y i32=2;"
              "swap(inout v[0],inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  auto constrained_direct = good("trait V{} impl V for i32{} "
       "func swap[T V](a inout T,b inout T){} "
       "func main(){var x i32=1;var y i32=2;"
       "swap(inout x,inout y);}");
  need(hir::verify(constrained_direct).success(),
       "constrained free inout HIR rejected");
  auto constrained_fv = good("trait V{} impl V for i32{} "
       "func swap[T V](a inout T,b inout T){} "
       "func main(){var x i32=1;var y i32=2;"
       "let f=swap[i32];f(inout x,inout y);}");
  need(hir::verify(constrained_fv).success(),
       "constrained free inout function-value HIR rejected");
  good("impl i32{func swap[T](a inout T,b inout T){} } "
       "func main(){var x i32=1;var y i32=2;"
       "i32.swap(inout x,inout y);}");
  good("struct Box[T]{value T;} impl[T] Box[T]{"
       "func swap[U](a inout U,b inout U){} } "
       "func main(){var x i32=1;var y i32=2;"
       "Box[i32].swap(inout x,inout y);}");
  good("impl i32{func bump(self Self,x inout i32){x+=1;}} "
       "func main()i32{var x i32=1;var y i32=1;x.bump(inout y);"
       "return y-2;}");
  bad("impl i32{func bump(self Self,x inout i32){x+=1;}} "
      "func main(){var x i32=1;x.bump(inout x);}",
      "E_CALL_ARGUMENT");
  unsupported("impl i32{func bump(self inout i32,x inout i32){x+=1;}} "
              "func main(){var x i32=1;x.bump(inout x);}",
              "E_CAPABILITY_UNAVAILABLE");
  // The explicit Type form now reaches the ordinary duplicate-storage
  // source check instead of the old actual-self capability rejection.
  bad("impl i32{func bump(self inout i32,x inout i32){x+=1;}} "
      "func main(){var x i32=1;i32.bump(inout x,inout x);}",
      "E_CALL_ARGUMENT");

  auto unsupported_span = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
    need(!r.diagnostics.empty() && r.diagnostics.front().primary &&
             r.diagnostics.front().primary->begin_byte <
                 r.diagnostics.front().primary->end_byte,
         "nested inout diagnostic lacks a source span: " + text + "\n" +
             messages(r));
  };
  auto unsupported_budget = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE") &&
             messages(r).find("exceeds checking budget") != std::string::npos,
         "growing inout graph did not stop at its explicit budget: " + text +
             "\n" + messages(r));
  };
  const std::string keep =
      "func keep[T](cb func(inout T))i32{return 0;} ";
  unsupported_span(keep + "func main()i32{let f=keep[Vec[i32]];return 0;}");
  unsupported_span("struct Box[T]{cb func(inout T);} "
                   "func keep(value Box[Vec[i32]])i32{return 0;} "
                   "func main()i32{return 0;}");
  unsupported_span("func keep[T](cb func() (func(inout T)))i32{return 0;} "
                   "func main()i32{let f=keep[Vec[i32]];return 0;}");
  unsupported_span("func keep[T](cb func(func(inout T)))i32{return 0;} "
                   "func main()i32{let f=keep[Vec[i32]];return 0;}");
  unsupported_span("struct Box[T]{cb func(inout T);} "
                   "func keep[T](cb func() Box[T])i32{return 0;} "
                   "func main()i32{let f=keep[Vec[i32]];return 0;}");
  unsupported_span("func make[T]() (func(inout T)){"
                   "return func(x inout T){};} "
                   "func main()i32{let f=make[Vec[i32]];return 0;}");
  unsupported_span("enum E[T]{A(cb func(inout T));} "
                   "func keep(value E[Vec[i32]])i32{return 0;} "
                   "func main()i32{return 0;}");
  // S[i32] contains function fields, so the first concrete callback is not POD.
  unsupported_span("struct S[T]{f func() S[S[T]];cb func(inout T);} "
                   "func take(value S[S[i32]])i32{return 0;} "
                   "func main()i32{return 0;}");
  // With only scalar callback data, unbounded applications hit the budget.
  unsupported_budget("struct S[T]{f func() S[S[T]];cb func(inout i32);} "
                     "func take(value S[i32])i32{return 0;} "
                     "func main()i32{return 0;}");
  unsupported_span("struct A[T]{b B[T];} "
                   "struct B[T]{f func() A[T];cb func(inout T);} "
                   "func take(value A[Vec[i32]])i32{return 0;} "
                   "func main()i32{return 0;}");

  good(keep + "func main()i32{let f=keep[i32];return 0;}");
  good(keep + "func main()i32{let f=keep[bool];return 0;}");
  good(keep + "func main()i32{let f=keep[f32];return 0;}");
  good(keep + "func main()i32{let f=keep[f64];return 0;}");
  good(keep + "func main()i32{return 0;}");
  good("struct Box[T]{cb func(inout T);} "
       "func keep(value Box[i32])i32{return 0;} "
       "func main()i32{return 0;}");
  good("struct S[T]{f func() S[S[T]];} "
       "func take(value S[S[i32]])i32{return 0;} "
       "func main()i32{return 0;}");
  good("struct A[T]{b B[T];} "
       "struct B[T]{f func() A[T];cb func(inout T);} "
       "func take(value A[i32])i32{return 0;} "
       "func main()i32{return 0;}");

  // Existing concrete function-value inout calls and generic move calls keep
  // their behavior; only the newly available generic inout route is added.
  good("func bump(x inout i32){x+=1;} "
       "func main()i32{var x i32=0;let op=bump;op(inout x);return x-1;}");
  good("func relay[T](value move T)T{return move value;} "
       "func main()i32{let v Vec[i32]=[1];let r=relay(move v);"
       "return r[0]-1;}");
  good("trait V{} impl V for i32{} "
       "func take[T V](value move T)T{return move value;} "
       "func main()i32{var x i32=1;let f=take[i32];"
       "let r=f(move x);return r-1;}");

  const std::string ordinary_callback =
      "trait V{} impl V for i32{} func bump(x inout i32){} "
      "func keep[T V](cb func(inout i32),value T)T{return value;} ";
  good(ordinary_callback +
       "func main()i32{return keep(bump,7)-7;}");
  good(ordinary_callback +
       "func main()i32{let f=keep[i32];return f(bump,7)-7;}");
  good("func bump(x inout i32){} struct Box[T]{value T;} "
       "impl[T] Box[T]{func keep(cb func(inout i32),value T)T{return value;}} "
       "func main()i32{return Box[i32].keep(bump,7)-7;}");

  auto positive = check(root / "tests/generics/inout/positive/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(), "generic inout HIR rejected");
  const auto &m = *positive.value;
  size_t inout_arguments = 0;
  for (const auto &n : m.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data))
      for (const auto &a : c->args)
        if (a.mode == hir::ParamMode::inout) {
          need(m.nodes[a.expression.value].kind() == hir::NodeKind::place,
               "generic inout argument is not a Place");
          ++inout_arguments;
        }
  need(inout_arguments >= 20, "generic inout calls missing");
  need(!m.templates.empty() && !m.instances.empty(),
       "generic inout template/instance missing");
  for (const auto &instance : m.instances) {
    auto f = std::find_if(
        m.functions.begin(), m.functions.end(),
        [&](const auto &candidate) {
          return candidate.symbol == instance.symbol;
        });
    need(f != m.functions.end(), "generic inout instance function missing");
    for (const auto &p : m.types.find(f->signature)->parameters)
      if (p.mode == hir::ParamMode::inout)
        need(m.types.find(p.type)->kind != hir::TypeKind::TypeParameter,
             "concrete generic inout signature retains T");
  }

  auto type_parameter = [](auto &x, const std::string &name) {
    for (uint64_t id = 1; id <= x.types.size(); ++id) {
      auto t = x.types.find(TypeId{id});
      if (t->kind == hir::TypeKind::TypeParameter &&
          x.resolved->symbols[t->declaration.value].name == name)
        return TypeId{id};
    }
    throw std::runtime_error("type parameter missing: " + name);
  };
  auto swap_call = [](auto &x) -> hir::Call & {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          x.resolved->symbols[c->direct_symbol->value].name == "swap")
        return *c;
    throw std::runtime_error("generic inout call missing");
  };
  invalid(*positive.value, "generic inout argument mode", [&](auto &x) {
    auto &call = swap_call(x);
    need(call.args.size() == 2, "generic inout call arity");
    call.args[0].mode = hir::ParamMode::ordinary;
  });
  invalid(*positive.value, "generic inout read as place", [&](auto &x) {
    auto &call = swap_call(x);
    auto &node = x.nodes[call.args[0].expression.value];
    auto symbol = std::get<hir::Place>(node.data).symbol;
    node.data = hir::Read{symbol};
  });
  invalid(*positive.value, "generic inout duplicate storage", [&](auto &x) {
    auto &call = swap_call(x);
    auto first = std::get<hir::Place>(
        x.nodes[call.args[0].expression.value].data).symbol;
    x.nodes[call.args[1].expression.value].data = hir::Place{first};
  });
  invalid(*positive.value, "generic inout foreign template parameter",
          [&](auto &x) {
            auto foreign = type_parameter(x, "U");
            for (auto &t : x.templates)
              if (x.resolved->symbols[t.function.symbol.value].name == "swap") {
                auto sig = *x.types.find(t.function.signature);
                sig.parameters[0].type = foreign;
                t.function.signature = x.types.intern(sig);
                return;
              }
            throw std::runtime_error("swap template missing");
          });
  invalid(*positive.value, "generic inout instance signature",
          [&](auto &x) {
            auto parameter = type_parameter(x, "T");
            for (auto &f : x.functions)
              for (const auto &instance : x.instances)
                if (f.symbol == instance.symbol) {
                  auto sig = *x.types.find(f.signature);
                  for (auto &p : sig.parameters)
                    if (p.mode == hir::ParamMode::inout) {
                      p.type = parameter;
                      f.signature = x.types.intern(sig);
                      return;
                    }
                }
            throw std::runtime_error("generic inout instance missing");
          });
}


void method_inout(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(), "method inout HIR rejected");
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string positive = R"(
struct Box[T]{value T;items Vec[i32];marker i32;}
struct Pair[A,B]{first A;second B;items Vec[i32];}
enum Maybe[T]{None;Some(value T);}
func swap[T](a inout T,b inout T){let tmp=a;a=b;b=tmp;}
impl[T] Box[T]{
func replace[U](self Self,slot inout U,value U)U{let old=slot;slot=value;return old;}
func replace_static[U](slot inout U,value U)U{let old=slot;slot=value;return old;}
func forward[U](self Self,slot inout U,value U)U{return self.replace(inout slot,value);}
func static_self[U](self U,slot inout i32,value i32)i32{let old=slot;slot=value;return old;}
}
impl[A,B] Pair[A,B]{
func replace_pair[U,V](self Self,first inout U,value U,second inout V,other V)i32{first=value;second=other;return 1;}
}
impl[T] Maybe[T]{
func replace_static[U](slot inout U,value U)U{let old=slot;slot=value;return old;}
}
func main()i32{
let b=Box[i32](value:1,items:[1],marker:2);
var x i64=3;let old=b.replace(inout x,4);
var y i64=5;let old2=Box[i32].replace_static[i64](inout y,6);
var z i64=7;let old3=b.forward(inout z,8);
var w i32=9;let old4=Box[i32].static_self(b,inout w,10);
let p=Pair[i32,Vec[i32]](first:1,second:[1],items:[2]);
var pf u32=11;var ps bool=false;let pv u32=12;let po bool=true;
let r=p.replace_pair(value:pv,other:po,second:inout ps,first:inout pf);
var mb bool=false;let old5=Maybe[i32].replace_static[bool](inout mb,true);
let total=i32(old)+i32(old2)+i32(old3)+old4+r+i32(pf);
if !old5 || total!=37{return 1;}
return 0;}
)";
  auto m = good(positive);

  auto type_named = [&](TypeId id, const std::string &name) {
    auto t = m.types.find(id);
    return t && (t->kind == hir::TypeKind::NominalStruct ||
                 t->kind == hir::TypeKind::NominalEnum) &&
           m.resolved->symbols.at(t->declaration.value).name == name;
  };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_i64 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 64;
  };
  auto is_u32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::UnsignedInt && t->bits == 32;
  };
  auto is_bool = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Bool;
  };
  auto is_vec_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Vec && t->type_args.size() == 1 &&
           is_i32(t->type_args[0]);
  };
  auto is_box_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return type_named(id, "Box") && t->type_args.size() == 1 &&
           is_i32(t->type_args[0]);
  };
  auto is_pair_i32_vec = [&](TypeId id) {
    auto t = m.types.find(id);
    return type_named(id, "Pair") && t->type_args.size() == 2 &&
           is_i32(t->type_args[0]) && is_vec_i32(t->type_args[1]);
  };
  auto has_parameter = [&](TypeId root) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root);
  };
  auto find_impl = [&](const std::string &name) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (!im.trait && type_named(im.self_type, name))
        return &im;
    return nullptr;
  };
  auto template_for = [&](SymbolId method) -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (t.function.symbol == method)
        return &t;
    return nullptr;
  };
  auto method_template = [&](const hir::ImplDecl &im,
                             const std::string &name)
      -> const hir::FunctionTemplate * {
    for (auto method : im.methods)
      if (m.resolved->symbols[method.value].name == name)
        return template_for(method);
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto same_prefix = [&](const hir::FunctionTemplate *t,
                         const hir::ImplDecl &im, size_t suffix) {
    if (!t || t->params.size() != im.params.size() + suffix)
      return false;
    for (size_t i = 0; i < im.params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.symbol != im.params[i].symbol ||
          p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte >= t->function.source.span.begin_byte)
        return false;
    }
    for (size_t i = im.params.size(); i < t->params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte < t->function.source.span.begin_byte ||
          p.source.span.end_byte > t->function.source.span.end_byte)
        return false;
      if (std::any_of(im.params.begin(), im.params.end(),
                      [&](const hir::GenericParam &q) {
                        return q.symbol == p.symbol;
                      }))
        return false;
    }
    return true;
  };

  const auto *box_impl = find_impl("Box");
  const auto *pair_impl = find_impl("Pair");
  const auto *maybe_impl = find_impl("Maybe");
  need(box_impl && pair_impl && maybe_impl, "method inout impls missing");
  const auto *box_replace = method_template(*box_impl, "replace");
  const auto *box_static = method_template(*box_impl, "replace_static");
  const auto *box_forward = method_template(*box_impl, "forward");
  const auto *box_static_self = method_template(*box_impl, "static_self");
  const auto *pair_replace = method_template(*pair_impl, "replace_pair");
  const auto *maybe_static = method_template(*maybe_impl, "replace_static");
  need(box_replace && box_static && box_forward && box_static_self &&
           pair_replace && maybe_static,
       "method inout templates missing");
  need(same_prefix(box_replace, *box_impl, 1) &&
           same_prefix(box_static, *box_impl, 1) &&
           same_prefix(box_forward, *box_impl, 1) &&
           same_prefix(box_static_self, *box_impl, 1) &&
           same_prefix(pair_replace, *pair_impl, 2) &&
           same_prefix(maybe_static, *maybe_impl, 1),
       "method inout template impl/method parameter split changed");
  need(box_replace->params.size() == 2 &&
           box_replace->params[0].symbol == box_impl->params[0].symbol &&
           box_replace->params[1].symbol != box_impl->params[0].symbol,
       "Box replace template lost real impl/own symbols");
  need(pair_replace->params.size() == 4 &&
           pair_replace->params[0].symbol == pair_impl->params[0].symbol &&
           pair_replace->params[1].symbol == pair_impl->params[1].symbol &&
           pair_replace->params[2].symbol != pair_replace->params[0].symbol &&
           pair_replace->params[3].symbol != pair_replace->params[1].symbol,
       "Pair replace_pair template argument order changed");

  const auto replace_instances = instances_of(box_replace);
  const auto static_instances = instances_of(box_static);
  const auto forward_instances = instances_of(box_forward);
  const auto static_self_instances = instances_of(box_static_self);
  const auto pair_instances = instances_of(pair_replace);
  const auto maybe_instances = instances_of(maybe_static);
  need(replace_instances.size() == 1 && static_instances.size() == 1 &&
           forward_instances.size() == 1 &&
           static_self_instances.size() == 1 &&
           pair_instances.size() == 1 && maybe_instances.size() == 1,
       "method inout instance full keys collapsed or duplicated");
  const auto *replace_instance = replace_instances[0];
  const auto *static_instance = static_instances[0];
  const auto *forward_instance = forward_instances[0];
  const auto *static_self_instance = static_self_instances[0];
  const auto *pair_instance = pair_instances[0];
  const auto *maybe_instance = maybe_instances[0];
  need(replace_instance->type_args.size() == 2 &&
           is_i32(replace_instance->type_args[0]) &&
           is_i64(replace_instance->type_args[1]),
       "Box replace [i32,i64] full key changed");
  need(static_instance->type_args.size() == 2 &&
           is_i32(static_instance->type_args[0]) &&
           is_i64(static_instance->type_args[1]),
       "Box static [i32,i64] full key changed");
  need(static_self_instance->type_args.size() == 2 &&
           is_i32(static_self_instance->type_args[0]) &&
           is_box_i32(static_self_instance->type_args[1]),
       "static self U=Self changed the impl/own key");
  need(pair_instance->type_args.size() == 4 &&
           is_i32(pair_instance->type_args[0]) &&
           is_vec_i32(pair_instance->type_args[1]) &&
           is_u32(pair_instance->type_args[2]) &&
           is_bool(pair_instance->type_args[3]),
       "Pair replace_pair [A,B,U,V] key order changed");
  need(maybe_instance->type_args.size() == 2 &&
           is_i32(maybe_instance->type_args[0]) &&
           is_bool(maybe_instance->type_args[1]),
       "Maybe static [i32,bool] key changed");

  const auto *replace_function = function_for(replace_instance->symbol);
  const auto *static_function = function_for(static_instance->symbol);
  const auto *forward_function = function_for(forward_instance->symbol);
  const auto *static_self_function = function_for(static_self_instance->symbol);
  const auto *pair_function = function_for(pair_instance->symbol);
  const auto *maybe_function = function_for(maybe_instance->symbol);
  need(replace_function && static_function && forward_function &&
           static_self_function && pair_function && maybe_function,
       "method inout concrete functions missing");
  auto replace_sig = m.types.find(replace_function->signature);
  auto static_sig = m.types.find(static_function->signature);
  auto self_sig = m.types.find(static_self_function->signature);
  auto pair_sig = m.types.find(pair_function->signature);
  auto maybe_sig = m.types.find(maybe_function->signature);
  need(replace_sig && static_sig && self_sig && pair_sig && maybe_sig,
       "method inout concrete signatures missing");
  need(replace_sig->parameters.size() == 3 &&
           is_box_i32(replace_sig->parameters[0].type) &&
           replace_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_i64(replace_sig->parameters[1].type) &&
           replace_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i64(replace_sig->parameters[2].type) &&
           replace_sig->parameters[2].mode == hir::ParamMode::ordinary &&
           is_i64(replace_sig->result),
       "Box replace Self/inout/mode signature mismatch");
  need(static_sig->parameters.size() == 2 &&
           is_i64(static_sig->parameters[0].type) &&
           static_sig->parameters[0].mode == hir::ParamMode::inout &&
           is_i64(static_sig->parameters[1].type) &&
           static_sig->parameters[1].mode == hir::ParamMode::ordinary,
       "Box static inout signature gained a receiver");
  need(self_sig->parameters.size() == 3 &&
           is_box_i32(self_sig->parameters[0].type) &&
           self_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_i32(self_sig->parameters[1].type) &&
           self_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(self_sig->parameters[2].type) &&
           self_sig->parameters[2].mode == hir::ParamMode::ordinary,
       "static ordinary self U=Self was classified as a receiver");
  need(pair_sig->parameters.size() == 5 &&
           is_pair_i32_vec(pair_sig->parameters[0].type) &&
           is_u32(pair_sig->parameters[1].type) &&
           pair_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_u32(pair_sig->parameters[2].type) &&
           is_bool(pair_sig->parameters[3].type) &&
           pair_sig->parameters[3].mode == hir::ParamMode::inout &&
           is_bool(pair_sig->parameters[4].type) &&
           is_i32(pair_sig->result),
       "Pair replace_pair inout order or Self mismatch");
  need(maybe_sig->parameters.size() == 2 &&
           is_bool(maybe_sig->parameters[0].type) &&
           maybe_sig->parameters[0].mode == hir::ParamMode::inout &&
           is_bool(maybe_sig->parameters[1].type),
       "Maybe static inout signature mismatch");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &f : m.functions) {
    need(!has_parameter(f.signature),
         "concrete method inout signature retained a parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete method inout node retained a parameter");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete method inout call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete method inout call targets a template");
        for (const auto &a : c->args)
          if (a.mode == hir::ParamMode::inout)
            need(m.nodes[a.expression.value].kind() == hir::NodeKind::place,
                 "method inout argument is not a Place");
      }
    }
  }
  bool forward_call = false;
  for (const auto &n : m.nodes) {
    if (m.scopes[n.scope.value].function != forward_function->symbol)
      continue;
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol ||
        m.resolved->symbols[c->direct_symbol->value].name != "replace")
      continue;
    need(c->direct_symbol->value == replace_instance->symbol.value,
         "method-to-method inout call missed the concrete instance");
    need(c->args.size() == 3 &&
             c->args[1].mode == hir::ParamMode::inout,
         "method-to-method inout call lost its mode");
    const auto &place = std::get<hir::Place>(
        m.nodes[c->args[1].expression.value].data);
    need(place.symbol == forward_function->parameters[1].symbol,
         "method-to-method inout Place kept a template parameter");
    forward_call = true;
  }
  need(forward_call, "method-to-method inout forwarding call missing");

  std::map<uint64_t, SymbolId> replace_renamed;
  for (auto [from, to] : replace_instance->renamed)
    replace_renamed[from.value] = to;
  auto template_sig = m.types.find(box_replace->function.signature);
  need(template_sig && template_sig->parameters.size() == 3 &&
           template_sig->parameters[1].mode == hir::ParamMode::inout &&
           m.types.find(template_sig->parameters[1].type)->kind ==
               hir::TypeKind::TypeParameter &&
           m.types.find(template_sig->parameters[1].type)->declaration ==
               box_replace->params[1].symbol,
       "method inout template lost its real inout type parameter");
  SymbolId template_local{0}, instance_local{0};
  for (const auto &n : m.nodes) {
    if (m.scopes[n.scope.value].function == box_replace->function.symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data); b)
        template_local = b->symbol;
    if (m.scopes[n.scope.value].function == replace_function->symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data); b)
        instance_local = b->symbol;
  }
  need(template_local.value && instance_local.value &&
           template_local != instance_local,
       "method inout instance local was not renamed");
  need(replace_renamed.at(box_replace->function.symbol.value) ==
               replace_function->symbol &&
           replace_renamed.at(
               box_replace->function.parameters[0].symbol.value) ==
               replace_function->parameters[0].symbol &&
           replace_renamed.at(
               box_replace->function.parameters[1].symbol.value) ==
               replace_function->parameters[1].symbol &&
           replace_renamed.at(
               box_replace->function.parameters[2].symbol.value) ==
               replace_function->parameters[2].symbol &&
           replace_renamed.at(template_local.value) == instance_local,
       "Box replace renamed map lost Self, an inout parameter, or a local");
  bool concrete_place = false;
  for (const auto &n : m.nodes) {
    if (m.scopes[n.scope.value].function != replace_function->symbol)
      continue;
    if (auto p = std::get_if<hir::Place>(&n.data); p) {
      need(p->symbol == replace_function->parameters[1].symbol,
           "replace body Place kept a template parameter");
      concrete_place = true;
    }
  }
  need(concrete_place, "replace body concrete inout Place missing");

  invalid_recomputed(m, "method inout instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == replace_instance->symbol) {
        i.origin = box_static->function.symbol;
        return;
      }
    throw std::runtime_error("method inout replace instance missing");
  });
  invalid_recomputed(m, "method inout full key order", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == pair_instance->symbol) {
        std::swap(i.type_args[2], i.type_args[3]);
        return;
      }
    throw std::runtime_error("method inout pair instance missing");
  });
  invalid_recomputed(m, "method inout Self substitution", [&](auto &x) {
    auto template_sig = *x.types.find(box_replace->function.signature);
    for (auto &f : x.functions)
      if (f.symbol == replace_function->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].type = template_sig.parameters[1].type;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("method inout replace function missing");
  });
  invalid_recomputed(m, "method inout Place rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == forward_function->symbol)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->direct_symbol &&
            x.resolved->symbols[c->direct_symbol->value].name == "replace") {
          auto &place = std::get<hir::Place>(
              x.nodes[c->args[1].expression.value].data);
          place.symbol = box_forward->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("method inout forwarding call missing");
  });

  const std::string method_prefix =
      "struct Box[T]{value T;items Vec[i32];marker i32;} ";
  good("trait V{} impl V for i32{} " + method_prefix +
       "impl[T V] Box[T]{func replace[U](self Self,slot inout U){"
       "slot=slot;}} "
       "func main(){let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;b.replace(inout x);}");
  good("trait V{} impl V for i32{} " + method_prefix +
       "impl[T] Box[T]{func replace[U V](self Self,slot inout U){"
       "slot=slot;}} "
       "func main(){let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;b.replace(inout x);}");
  good("trait V{func replace(self Self,slot inout i32);} "
       "struct H{marker i32;} impl V for H{"
       "func replace(self Self,slot inout i32){slot=slot;}} "
       "func main(){let h=H(marker:0);var x i32=1;"
       "h.replace(inout x);}");
  unsupported(method_prefix +
              "impl[T] Box[T]{func replace[U](self Self,slot inout U,value U)U{"
              "let old=slot;slot=value;return old;}} "
              "func main(){let b=Box[i32](value:1,items:[1],marker:0);"
              "var v Vec[i32]=[1];b.replace(inout v,[2]);}");
  bad(method_prefix +
          "impl[T] Box[T]{func replace[U](self Self,slot inout U,value U)U{"
          "let old=slot;slot=value;return old;}} "
          "func main(){let b=Box[i32](value:1,items:[1],marker:0);"
          "let x i32=1;b.replace(inout x,3);}",
      "E_CALL_ARGUMENT");
  bad(method_prefix +
          "impl[T] Box[T]{func replace[U](self Self,slot inout U,value U)U{"
          "let old=slot;slot=value;return old;}} "
          "func main(){let b=Box[i32](value:1,items:[1],marker:0);"
          "var x i32=1;b.replace(inout x,inout x);}",
      "E_CALL_ARGUMENT");
  bad("impl i32{func swap[T](a inout T,b inout T){}} "
      "func main(){var x i32=1;i32.swap(inout x,x);}",
      "E_CALL_ARGUMENT");
  bad("impl i32{func swap[T](a inout T,b inout T){}} "
      "func g(x i32)i32{return x;} "
      "func main(){var x i32=1;i32.swap(inout x,g(x));}",
      "E_CALL_ARGUMENT");
  unsupported("impl i32{func bump(self Self,slot inout i32){slot+=1;}} "
              "func main(){var x i32=1;x.bump(inout 1);}");
  unsupported("struct H{marker i32;} impl H{"
              "func bump(self Self,slot inout i32){slot+=1;}} "
              "func main(){let h=H(marker:1);h.bump(inout h.marker);}");
  unsupported("struct H{items Vec[i32];} impl H{"
              "func bump(self Self,slot inout i32){slot+=1;}} "
              "func main(){let h=H(items:[1]);h.bump(inout h.items[0]);}");
  bad("struct H{marker i32;} impl H{"
      "func bump(self Self,slot inout i32){slot+=1;}} "
      "func use(h H,x i32){h.bump(inout x);} "
      "func main(){let h=H(marker:1);use(h,2);}",
      "E_CALL_ARGUMENT");
  good("impl i32{func swap[T](a inout T,b inout T){}} "
       "func main()i32{var x i32=1;var y i32=2;"
       "i32.swap(inout x,inout y);return x-2;}");
  good("struct Box[T]{value T;} impl[T] Box[T]{"
       "func swap[U](a inout U,b inout U){let tmp=a;a=b;b=tmp;}} "
       "func main()i32{var x f64=1.0;var y f64=2.0;"
       "Box[bool].swap(inout x,inout y);return 0;}");
  unsupported("impl i32{func bump(self inout i32,x inout i32){x=x;}} "
              "func main(){var x i32=1;var y i32=2;x.bump(inout y);}");
}

void extra(const fs::path &root) {
  auto good = [&](const std::string &s) {
    auto r = source(s);
    need(r.success(), s + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &s, std::string code = "E_TYPE_MISMATCH") {
    auto r = source(s);
    need(!r.success() && has(r, code) && r.failure == FailureKind::source,
         s + "\n" + messages(r));
  };
  good("func keep[T](x T,cb func(i32) i32) T{return x;} "
       "func main() i32{return 0;}");
  good("func keep[T](x T,cb func(i32) i32) T{return x;} "
       "func id(x i32) i32{return x;} func main() i32{return keep[i32](3,id);}");
  auto generic_calls = good("func g[T](){} func h[U](){g[U]();let f=g[U];} "
                            "func other[X](x X){} func main() i32{return 0;}");
  for (auto invalid_id : {uint64_t{0}, uint64_t{999999}}) {
    invalid(generic_calls, "invalid generic call type argument", [=](auto &m) {
      for (auto &n : m.nodes)
        if (auto c = std::get_if<hir::Call>(&n.data); c && !c->type_args.empty()) {
          c->type_args[0] = TypeId{invalid_id}; return;
        }
      throw std::runtime_error("missing generic call");
    });
    invalid(generic_calls, "invalid generic function value type argument", [=](auto &m) {
      for (auto &n : m.nodes)
        if (auto v = std::get_if<hir::FunctionValue>(&n.data); v && !v->type_args.empty()) {
          v->type_args[0] = TypeId{invalid_id}; return;
        }
      throw std::runtime_error("missing generic function value");
    });
  }
  invalid(generic_calls, "foreign template type parameter", [](auto &m) {
    TypeId foreign{0};
    for (uint64_t id=1; id<=m.types.size(); ++id) {
      auto t=m.types.find(TypeId{id});
      if (t->kind==hir::TypeKind::TypeParameter &&
          m.resolved->symbols[t->declaration.value].name=="X") foreign=TypeId{id};
    }
    need(foreign.value != 0, "foreign type parameter missing");
    for (auto &n:m.nodes)
      if (auto c=std::get_if<hir::Call>(&n.data); c && !c->type_args.empty()) {
        c->type_args[0]=foreign; return;
      }
  });
  auto declarations = good("struct Pair[A,B]{first A;second B;} enum E{None;Some(x i32);} "
                           "enum F{Other;} func main() i32{return 0;}");
  invalid(declarations, "missing nominal field", [](auto &m){m.nominals[0].fields.pop_back();});
  invalid(declarations, "reordered nominal fields", [](auto &m){std::swap(m.nominals[0].fields[0],m.nominals[0].fields[1]);});
  invalid(declarations, "missing enum variant", [](auto &m){m.nominals[1].variants.pop_back();});
  invalid(declarations, "missing enum payload", [](auto &m){m.nominals[1].variants[1].payload.clear();});
  invalid(declarations, "foreign enum variant owner", [](auto &m){m.nominals[1].variants.push_back(m.nominals[2].variants[0]);});
  invalid(declarations, "reordered nominal parameters", [](auto &m){std::swap(m.nominals[0].params[0],m.nominals[0].params[1]);});
  auto foreign_parameter = good("struct A[X]{} struct B[Y]{} func main() i32{return 0;}");
  invalid(foreign_parameter, "foreign nominal parameter owner", [](auto &m){m.nominals[0].params[0]=m.nominals[1].params[0];});
  auto multi = check(root / "tests/generics/fixtures/multi-instance/lang.toml");
  need(multi.success(), messages(multi));
  const auto m = *multi.value;
  need(m.templates.size() == 2 && m.instances.size() == 4,
       "missing independent local/lambda template instances");
  std::set<uint64_t> generated, scopes, nodes;
  for (const auto &instance : m.instances) {
    for (auto [from, to] : instance.renamed)
      need(from != to && generated.insert(to.value).second,
           "shared instantiated symbol");
    auto f = std::find_if(
        m.functions.begin(), m.functions.end(),
        [&](const auto &f) { return f.symbol == instance.symbol; });
    need(f != m.functions.end(), "instance has no concrete function");
    need(scopes.insert(f->scope.value).second, "instances share a root scope");
    std::vector<HirNodeId> todo{f->body};
    while (!todo.empty()) {
      auto id = todo.back();
      todo.pop_back();
      need(nodes.insert(id.value).second, "instances share a node");
      const auto &n = m.nodes[id.value];
      need(m.types.find(n.type)->kind != hir::TypeKind::TypeParameter,
           "unsubstituted concrete type");
      todo.insert(todo.end(), n.children.begin(), n.children.end());
    }
  }
  invalid(m, "signature replacement", [](auto &x) {
    for (auto &f : x.functions)
      if (f.symbol == x.instances[0].symbol) {
        auto sig = *x.types.find(f.signature);
        sig.result = TypeId{1};
        f.signature = x.types.intern(sig);
        break;
      }
  });
  invalid(m, "unresolved concrete template call", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
        for (const auto &i : x.instances)
          if (*c->direct_symbol == i.symbol) {
            c->direct_symbol = i.origin;
            c->type_args = i.type_args;
            return;
          }
      }
  });
  invalid(m, "missing instance record",
          [](auto &x) { x.instances.erase(x.instances.begin()); });
  invalid(m, "mapping missing local",
          [](auto &x) { x.instances[0].renamed.pop_back(); });
  invalid(m, "forged symbol origin", [](auto &x) {
    x.instances[0].renamed[0].first = x.instances[0].renamed[0].second;
  });
  invalid(m, "wrong target",
          [](auto &x) { x.instances[0].target.word_bits = 32; });
  invalid(m, "unsubstituted type", [](auto &x) {
    auto sid = x.instances[0].symbol;
    TypeId t{0};
    for (size_t i = 1; i <= x.types.size(); ++i)
      if (x.types.find(TypeId{i})->kind == hir::TypeKind::TypeParameter) {
        t = TypeId{i};
        break;
      }
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == sid &&
          n.kind() == hir::NodeKind::read) {
        n.type = t;
        break;
      }
  });
  auto two = good("func a[T](x T) T { return x; } func b[T](x T) T { return x; "
                  "} func main() i32 { return a[i32](1)+b[i32](2); }");
  invalid(two, "same-type cross-instance local read", [](auto &x) {
    SymbolId from = x.instances[0].symbol, foreign = x.instances[1].symbol;
    SymbolId parameter{0};
    for (auto &f : x.functions)
      if (f.symbol == foreign)
        parameter = f.parameters[0].symbol;
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == from &&
          n.kind() == hir::NodeKind::read) {
        std::get<hir::Read>(n.data).symbol = parameter;
        break;
      }
  });
  // The sole publisher deep-copies generated symbols, templates, instances,
  // source ASTs and project state as well as the executable arena.
  auto frozen = hir::verify(m);
  need(frozen.success(), messages(frozen));
  auto before = hir::dump(*frozen.value);
  auto mutable_builder = m;
  mutable_builder.instances.clear();
  mutable_builder.templates.clear();
  mutable_builder.nodes.clear();
  auto mutable_resolved = std::const_pointer_cast<modules::ResolvedProject>(
      mutable_builder.resolved);
  mutable_resolved->symbols.clear();
  need(hir::dump(*frozen.value) == before &&
           hir::verify(frozen.value->module()).success(),
       "published generic HIR changed through builder aliases");
  auto repeat = good("func id[T](x T) T {return x;} func main() i32 {return "
                     "id[i32](1)+id[i32](2);}");
  need(repeat.instances.size() == 1, "repeat calls were not deduplicated");
  auto unused = good("struct Box[T] {value T;} func id[T](x T) T{return x;} "
                     "func main() i32{return 0;}");
  need(unused.instances.empty() && hir::verify(unused).success(),
       "unused declarations treated as backend operations");
  good("func id[T](x T) T{return x;} func main() i32 {let f func(i32) i32=id; "
       "return f(3);}");
  good("func id[T](x T) T{return x;} const f=id[i32]; func main() i32{return "
       "f(3);}");
  good("struct Box[T] {value T;} enum Option[T] {None;Some(value T);} func "
       "main() i32 {let c=Option[Box[i32]].Some(value:Box[i32](value:5)); "
       "switch c {case Option.Some(x):return x.value;case Option.None:return "
       "0;}}");
  good("struct P {x i32;} impl P {func value(self Self) i32{return self.x;}} "
       "func main() i32{return P(x:4).value();}");
  good("trait Value{func value(self Self) i32;} struct Box[T]{x T;} impl[T] "
       "Value for Box[T]{func value(self Self) i32{return 1;}} func get[T "
       "Value](x T) i32{return x.value();} func main() i32{return "
       "get(Box[i32](x:3));}");
  good("enum E {A;B(x i32);} func main() i32 {let e=E.B(x:5);switch e {case "
       "x:return 0;}}");
  good("enum E {A;B;} func main() i32 {var n=0;for n<2 {switch E.A {case "
       "E.A:n+=1;continue;case E.B:break;} break;}return 0;}");
  good("trait Ordered{func less(self Self,other Self) bool;} impl Ordered for "
       "i32{func less(self i32,other i32) bool{return self<other;}} func "
       "less[T Ordered](a T,b T) bool{return a.less(b);} func main() i32{if "
       "less[i32](1,2){return 0;}return 1;}");
  good("struct Box[T]{x T;} impl[T] Box[T]{func wrap[U](self Self,y U) "
       "U{return y;}} func main() i32{return Box[i64](x:3).wrap[i32](4);}");
  good("func f(x f32) f32{var y=x; y+=x;return y;} func main() i32{return 0;}");
  bad("trait V{func v(self Self) i32;} trait W{func w(self Self) i32;} struct "
      "Box[T]{x T;} impl[T V] W for Box[T]{func w(self Self) i32{return 1;}} "
      "func use[T W](x T) i32{return 0;} func main() i32{return "
      "use(Box[i32](x:3));}");
  good("trait V{func v(self Self) i32;} struct Pair[A,B]{a A;b B;} impl[T] V "
       "for Pair[T,Pair[T,T]]{func v(self Self) i32{return 1;}} impl[U] V for "
       "Pair[Pair[U,U],U]{func v(self Self) i32{return 2;}} func main() "
       "i32{return 0;}");
  bad("trait V{func v(self Self) i32;} struct Pair[A,B]{a A;b B;} impl[T] V "
      "for Pair[T,i32]{func v(self Self) i32{return 1;}} impl[U] V for "
      "Pair[i32,U]{func v(self Self) i32{return 2;}} func main() i32{return "
      "0;}");
  bad("struct Pair[A,B]{a A;b B;} func grow[T](x T) i32{return "
      "grow[Pair[T,T]](Pair[T,T](a:x,b:x));} func main() i32{return "
      "grow[i32](0);}",
      "E_INSTANTIATION_LIMIT");
  bad("func f[T](x T) T {return x;} func main() i32 {let a=f[3](1);return 0;}",
      "E_NAME_KIND");
  bad("func f[T]() i32 {return 0;} func main() i32 {return f();}");
  bad("trait V{func v(self Self) i32;} func bad[T V](x T) T{return x+x;} func "
      "main() i32{return 0;}");
  bad("trait V{func v(self Self) i32;} struct P{x i32;} impl V for P{func "
      "v(self P) i64{return 0;}} func main() i32{return 0;}");
  bad("trait V{func v(self Self) i32;} struct P{x i32;} impl V for P{} func "
      "main() i32{return 0;}");
  bad("trait V{func v(self Self) i32;} struct P{x i32;} impl V for P{func "
      "v(other P) i32{return 0;}} func main() i32{return 0;}");
  bad("struct A{b B;} struct B{a A;} func main() i32{return 0;}");
  bad("struct Box[T]{x T;} struct A[T]{next A[Box[T]];} func main() i32{return "
      "0;}");
  bad("enum E{A;B;} func main() i32{switch E.A{case E.A:return 0;case "
      "E.A:return 1;case E.B:return 2;}}");
  bad("enum E{A;B;} func main() i32{switch E.A{default:return 0;case "
      "E.A:return 1;}}");
  auto fields =
      check(root / "tests/generics/fixtures/fields-reordered/lang.toml");
  need(fields.success(), messages(fields));
  invalid(*fields.value, "foreign field", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto f = std::get_if<hir::FieldRead>(&n.data)) {
        f->field = x.functions[0].symbol;
        break;
      }
  });
  invalid(*fields.value, "duplicate constructor mapping", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::StructInit>(&n.data)) {
        c->fields[1].parameter_index = c->fields[0].parameter_index;
        break;
      }
  });
  auto order =
      check(root / "tests/generics/fixtures/field-evaluation-order/lang.toml");
  need(order.success(), messages(order));
  bool saw = false;
  for (const auto &n : order.value->nodes)
    if (auto c = std::get_if<hir::StructInit>(&n.data)) {
      need(c->fields.size() == 2 && c->fields[0].parameter_index == 1 &&
               c->fields[1].parameter_index == 0,
           "constructor reordered named fields");
      need(order.value->nodes[n.children[0].value].source.span.begin_byte <
               order.value->nodes[n.children[1].value].source.span.begin_byte,
           "constructor changed evaluation order");
      saw = true;
    }
  need(saw, "constructor missing");
  auto enumeration =
      check(root / "tests/generics/fixtures/enum-payload/lang.toml");
  need(enumeration.success(), messages(enumeration));
  invalid(*enumeration.value, "invalid variant", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::EnumInit>(&n.data)) {
        c->variant = x.functions[0].symbol;
        break;
      }
  });
  invalid(*enumeration.value, "nonexhaustive switch", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Switch>(&n.data)) {
        c->exhaustive = false;
        break;
      }
  });
  invalid(*enumeration.value, "pattern scope", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Switch>(&n.data)) {
        c->arms[1].scope = c->arms[0].scope;
        break;
      }
  });
  invalid(*enumeration.value, "Unsupported corruption", [](auto &x) {
    x.nodes[0].data = hir::Unsupported{"NeutralBracket"};
  });
  auto method = good("struct P{x i32;} impl P{func value(self Self) i32{return "
                     "self.x;}} func main() i32{return P(x:5).value();}");
  bool saw_receiver = false;
  for (const auto &n : method.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->direct_symbol &&
        method.resolved->symbols[c->direct_symbol->value].name == "value") {
      need(c->args.size() == 1 && n.children.size() == 1 &&
               c->args[0].expression == n.children[0] &&
               method.nodes[n.children[0].value].kind() ==
                   hir::NodeKind::struct_init,
           "method receiver not evaluated once before args");
      saw_receiver = true;
    }
  need(saw_receiver, "method call missing");
  for (const auto *backend : {"native", "python"})
    for (unsigned width : {32u, 64u}) {
      auto scalar = source(
          "func id[T](x T) T{return x;} func main() i32{return id(7)-7;}",
          backend, width);
      need(scalar.success() && hir::verify(*scalar.value).success(),
           "generic scalar profile failed");
      auto aggregate = source(
          "struct P{x i32;} func main() i32{return P(x:5).x;}", backend, width);
      need(aggregate.success(), messages(aggregate));
      auto capability = hir::check_backend_capabilities(*aggregate.value);
      need(capability.success() && hir::verify(*aggregate.value).success(),
           "POD aggregate profile rejected");
      auto owned = source(
          "struct S{s string;} func main(){let x=S(s:\"owned\");}",
          backend, width);
      need(owned.success(), messages(owned));
      need(hir::verify_types(*owned.value).success() &&
               hir::check_backend_capabilities(*owned.value).success(),
           "string aggregate backend support is not E_CAP-free");
    }
  Temp baseline;
  write(baseline.path / "lang.toml",
        "schema_version=1\n[package]\nname=\"app\"\nsource_root=\"src\"\nentry="
        "\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
  fs::copy_file(root / "baseline/samples/05_traits.lang",
                baseline.path / "src/main.lang");
  auto traits = check(baseline.path / "lang.toml");
  need(traits.success(),
       "full 05_traits static check failed: " + messages(traits));
  need(hir::verify_types(*traits.value).success(), "05_traits invalid HIR");
  auto cap = hir::verify(*traits.value);
  need(cap.success(), "native 05_traits floating execution unavailable");
  auto floating = good("func f(x f64) f64{return -x*x+1.0;} func main() i32{if "
                       "f(2.0)<0.0{return 1;}return 0;}");
  need(hir::verify(floating).success(), "native floating operations unavailable");
  auto python_float = source(
      "func f(x f64) f64{return -x*x+1.0;} func main() i32{if "
      "f(2.0)<0.0{return 1;}return 0;}", "python");
  need(python_float.success() && hir::verify(*python_float.value).success(),
       "Python strict floating operations rejected");
  invalid(method, "Self owner missing", [](auto &x) {
    auto r = std::make_shared<modules::ResolvedProject>(*x.resolved);
    r->self_parameters.clear();
    x.resolved = r;
  });
  std::stop_source stop;
  stop.request_stop();
  need(hir::verify_types(floating, stop.get_token()).state ==
           PhaseState::cancelled,
       "static verifier ignored cancellation");
  std::cout << "T07 identities, corruption, constraints, layout, scopes, "
               "order, full traits, freezing and capability checks passed\n";
}

void method_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(), "method move HIR rejected");
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  auto calls_named = [](const hir::HirModule &m, const std::string &name) {
    std::vector<const hir::HirNode *> result;
    for (const auto &n : m.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          m.resolved->symbols[c->direct_symbol->value].name == name)
        result.push_back(&n);
    return result;
  };

  const std::string positive =
      "struct Holder{items Vec[i32];marker i32;} "
      "struct Box{items Vec[i32];marker i32;} "
      "impl Holder{"
      "func take(self Self,value move Vec[i32])Vec[i32]{return move value;}"
      "func take_static(value move Vec[i32])Vec[i32]{return move value;}"
      "func accept(self Self,value move Holder)i32{"
      "return self.marker+value.marker;}"
      "func scalar(self Self,value move i64)i64{return value;}} "
      "func main()i32{let h=Holder(items:[1],marker:2);"
      "var v Vec[i32]=[3];let a=v;let r=h.take(move v);"
      "var sv Vec[i32]=[6];let s0=Holder.take_static([4]);"
      "let s1=Holder.take_static(move sv);let c=h;"
      "let n=h.accept(move c);let q=h.scalar(5);"
      "return h.marker+a[0]+r[0]+s0[0]+s1[0]+n+i32(q)-37;}";
  auto method_hir = good(positive);
  need(method_hir.templates.empty() && method_hir.instances.empty(),
       "non-generic method move created templates");
  auto take = calls_named(method_hir, "take");
  need(take.size() == 1, "instance method move call missing");
  const auto &take_node = *take[0];
  const auto &take_call = std::get<hir::Call>(take_node.data);
  need(take_call.args.size() == 2 &&
           take_call.args[0].parameter_index == 0 &&
           take_call.args[0].mode == hir::ParamMode::ordinary &&
           take_call.args[1].parameter_index == 1 &&
           take_call.args[1].mode == hir::ParamMode::move,
       "instance method parameter modes");
  need(take_node.children.size() == 2 &&
           take_node.children[0] == take_call.args[0].expression &&
           take_node.children[1] == take_call.args[1].expression,
       "instance method receiver/argument source order");
  need(method_hir.nodes[take_node.children[0].value].source.span.begin_byte <
           method_hir.nodes[take_node.children[1].value].source.span.begin_byte,
       "receiver did not precede arguments");
  const auto &take_value =
      method_hir.nodes[take_call.args[1].expression.value];
  need(take_value.kind() == hir::NodeKind::move &&
           std::get<hir::Move>(take_value.data).binding.has_value(),
       "instance move lacks its real binding source");
  for (const auto &n : method_hir.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->direct_symbol &&
        method_hir.resolved->symbols[c->direct_symbol->value].name == "take") {
      const auto &sig =
          *method_hir.types.find(c->signature);
      need(sig.parameters.size() == 2 &&
               sig.parameters[0].mode == hir::ParamMode::ordinary &&
               sig.parameters[1].mode == hir::ParamMode::move,
           "instance method signature lost self/move modes");
    }

  auto statics = calls_named(method_hir, "take_static");
  need(statics.size() == 2, "static method move calls missing");
  bool static_binding = false, static_temporary = false;
  for (const auto *node : statics) {
    const auto &call = std::get<hir::Call>(node->data);
    need(call.args.size() == 1 &&
             call.args[0].parameter_index == 0 &&
             call.args[0].mode == hir::ParamMode::move &&
             node->children.size() == 1 &&
             node->children[0] == call.args[0].expression,
         "static method move mode/source order");
    const auto &value = method_hir.nodes[call.args[0].expression.value];
    if (value.kind() == hir::NodeKind::move) {
      need(std::get<hir::Move>(value.data).binding.has_value(),
           "static binding move lacks source binding");
      static_binding = true;
    } else {
      need(value.kind() == hir::NodeKind::vec_init,
           "static temporary move shape");
      static_temporary = true;
    }
    const auto &sig = *method_hir.types.find(call.signature);
    need(sig.parameters.size() == 1 &&
             sig.parameters[0].mode == hir::ParamMode::move,
         "static method signature gained a receiver");
  }
  need(static_binding && static_temporary,
       "static method explicit/temporary move coverage");
  auto accepted = calls_named(method_hir, "accept");
  need(accepted.size() == 1, "shared-copy receiver call missing");
  const auto &accepted_call = std::get<hir::Call>(accepted[0]->data);
  need(accepted_call.args.size() == 2 &&
           accepted_call.args[0].mode == hir::ParamMode::ordinary &&
           accepted_call.args[1].mode == hir::ParamMode::move,
       "shared-copy owned argument mode");
  auto scalar = calls_named(method_hir, "scalar");
  need(scalar.size() == 1, "scalar method move missing");
  const auto &scalar_call = std::get<hir::Call>(scalar[0]->data);
  need(scalar_call.args.size() == 2 &&
           scalar_call.args[1].mode == hir::ParamMode::move &&
           method_hir.nodes[scalar_call.args[1].expression.value].kind() ==
               hir::NodeKind::integer_literal,
       "scalar temporary move mode");

  const std::string prefix =
      "struct H{items Vec[i32];marker i32;} "
      "impl H{func take(self Self,value move Vec[i32])Vec[i32]{"
      "return move value;}} ";
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "let v Vec[i32]=[1];let r=h.take(v);return r[0];}",
      "E_CALL_ARGUMENT");
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "var v Vec[i32]=[1];let r=h.take(inout v);return r[0];}",
      "E_CALL_ARGUMENT");
  const std::string accept_prefix =
      "struct H{marker i32;} "
      "impl H{func accept(self Self,value move H)i32{"
      "return self.marker+value.marker;}} ";
  bad(accept_prefix + "func main()i32{let h=H(marker:1);"
                      "return h.accept(move h);}",
      "E_CALL_ARGUMENT");
  bad("struct H{marker i32;} func id(value move H)H{return move value;} "
      "impl H{func accept(self Self,value move H)i32{"
      "return self.marker+value.marker;}} "
      "func main()i32{let h=H(marker:1);return h.accept(id(move h));}",
      "E_CALL_ARGUMENT");
  bad("struct H{marker i32;} impl H{"
      "func pair(self Self,a move Vec[i32],b move Vec[i32])i32{return 0;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "return h.pair(move v,move v);}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  unsupported(prefix + "func main()i32{let h=H(items:[1],marker:0);"
                       "let r=h.take(move h.items);return r[0];}");
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "let f=h.take;return 0;}",
      "E_TYPE_MISMATCH");

  unsupported(
      "struct H{marker i32;} impl H{"
      "func take(self move Self,value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  good(
      "struct H{marker i32;} impl H{"
      "func take(self move Self,value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=H.take(move h,move v);return r[0];}");
  unsupported(
      "impl i32{func take(self inout i32,value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func main()i32{var h i32=1;var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  good(
      "struct B[T]{x T;} impl[T] B[T]{"
      "func take(self Self,value move Vec[i32])Vec[i32]{return move value;}} "
      "func main()i32{let b=B[i32](x:1);var v Vec[i32]=[1];"
      "let r=b.take(move v);return r[0];}");
  good(
      "struct H{marker i32;} impl H{"
      "func take[U](self Self,value move U)U{return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  good(
      "trait V{} impl V for i32{} struct H{marker i32;} impl H{"
      "func take[U V](self Self,value move U)U{return move value;}} "
      "func main()i32{let h=H(marker:0);var v i32=1;"
      "let r=h.take(move v);return r-1;}");
  good(
      "trait V{func take(self Self,value move Vec[i32])Vec[i32];} "
      "struct H{items Vec[i32];marker i32;} impl V for H{"
      "func take(self Self,value move Vec[i32])Vec[i32]{return move value;}} "
      "func main()i32{let h=H(items:[1],marker:0);var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  unsupported(
      "trait V{func take(self move Self)Self;} "
      "struct H{marker i32;} impl V for H{"
      "func take(self move Self)Self{return move self;}} "
      "func use[T V](x T)T{return x.take();} "
      "func main()i32{let h=H(marker:0);let r=use(h);return r.marker;}");
  good(
      "struct H{marker i32;} impl H{"
      "func swap(a inout i32,b inout i32){let t=a;a=b;b=t;}} "
      "func main()i32{var x i32=1;var y i32=2;"
      "H.swap(inout x,inout y);return x-2;}");
  good(
      "struct H{marker i32;} impl H{"
      "func bump(self Self,a inout i32){a+=1;}} "
      "func main()i32{let h=H(marker:0);var x i32=1;"
      "h.bump(inout x);return x-2;}");
  good(
      "struct H{marker i32;} impl H{"
      "func take(self Self,value move string)string{return move value;}} "
      "func main()i32{let h=H(marker:0);let r=h.take(move \"x\");return 0;}");

  const std::string enum_positive =
      "enum E{A;B(x i32);} enum Payload{Items(items Vec[i32]);Empty;} "
      "struct Box{items Vec[i32];marker i32;} "
      "impl E{func marker()i32{return 4;}"
      "func take(value move Vec[i32])Vec[i32]{return move value;}"
      "func take_box(value move Box)Box{return move value;}"
      "func take_payload(value move Payload)Payload{return move value;}} "
      "func main()i32{var v Vec[i32]=[1];let a=v;let r=E.take(move v);"
      "r[0]=7;if a[0]!=7{return 1;}let t=E.take([2]);"
      "if t[0]!=2{return 2;}var b=Box(items:[3],marker:0);"
      "let ba=b;let br=E.take_box(move b);br.items[0]=9;"
      "if ba.items[0]!=9{return 3;}var p=Payload.Items([8]);"
      "let pa=p;let pr=E.take_payload(move p);"
      "switch pr{case Payload.Items(items):items[0]=10;"
      "case Payload.Empty:return 4;}"
      "switch pa{case Payload.Items(items):if items[0]!=10{return 5;}"
      "case Payload.Empty:return 6;}"
      "if E.marker()!=4{return 7;}let variant=E.B(x:5);"
      "switch variant{case E.B(x):if x!=5{return 8;}case E.A:return 9;}"
      "return 0;}";
  auto enum_hir = good(enum_positive);
  need(enum_hir.templates.empty(), "enum static method move created templates");
  auto enum_take = calls_named(enum_hir, "take");
  need(enum_take.size() == 2, "enum static method move calls missing");
  for (const auto *node : enum_take) {
    const auto &call = std::get<hir::Call>(node->data);
    need(call.args.size() == 1 &&
             call.args[0].parameter_index == 0 &&
             call.args[0].mode == hir::ParamMode::move &&
             node->children.size() == 1 &&
             node->children[0] == call.args[0].expression,
         "enum static move mode/source order");
    const auto &sig = *enum_hir.types.find(call.signature);
    need(sig.parameters.size() == 1 &&
             sig.parameters[0].mode == hir::ParamMode::move,
         "enum static method signature gained a receiver");
  }
  auto enum_box = calls_named(enum_hir, "take_box");
  need(enum_box.size() == 1, "enum static owned-struct method move missing");
  auto enum_payload = calls_named(enum_hir, "take_payload");
  need(enum_payload.size() == 1, "enum static enum-data method move missing");

  auto variant_priority = good(
      "enum E{A(x i32);} impl E{func A()i32{return 9;}} "
      "func main()i32{let v=E.A(1);"
      "switch v{case E.A(x):return x-1;}}");
  bool saw_variant = false;
  for (const auto &n : variant_priority.nodes) {
    if (std::holds_alternative<hir::EnumInit>(n.data))
      saw_variant = true;
  }
  need(saw_variant, "same-name variant lost constructor priority");
  for (const auto &n : variant_priority.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->direct_symbol &&
        variant_priority.resolved->symbols[c->direct_symbol->value].name == "A")
      throw std::runtime_error("same-name variant became a method call");
  bad("enum E{A(x i32);} impl E{func A()i32{return 9;}} "
      "func main()i32{let v=E.A();return 0;}",
      "E_CALL_ARGUMENT");
  bad("enum E{A;} func main()i32{return E.missing();}", "E_TYPE_MISMATCH");

  auto packages = [&](const std::string &lib, const std::string &app) {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang", "package lib; " + lib);
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; " + app);
    return check(t.path / "lang.toml");
  };
  auto private_enum = packages(
      "pub enum E{A;} impl E{func hidden()i32{return 1;}}",
      "func main()i32{return lib.E.hidden();}");
  need(!private_enum.success() && has(private_enum, "E_PRIVATE_ACCESS"),
       "private enum static method accepted\n" + messages(private_enum));
  auto public_enum = packages(
      "pub enum E{A;} impl E{pub func value()i32{return 7;}}",
      "func main()i32{return lib.E.value()-7;}");
  need(public_enum.success(), messages(public_enum));

  good(
      "enum E{A;} impl E{func take[U](value move U)U{return move value;}} "
      "func main()i32{var v Vec[i32]=[1];let r=E.take(move v);return r[0];}");
  good(
      "enum E[T]{A(x T);} impl[T] E[T]{"
      "func take(value move Vec[i32])Vec[i32]{return move value;}} "
      "func main()i32{var v Vec[i32]=[1];let r=E[i32].take(move v);"
      "return r[0];}");
  // Reclassified after opening non-self move on a type-qualified trait
  // method: the same complete Source now names a real trait impl and passes
  // the move at explicit parameter zero. A genuine call and its execution are
  // covered by the trait-generic-move fixture below.
  good(
      "trait V{func take(value move Vec[i32])Vec[i32];} enum E{A;} "
      "impl V for E{func take(value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func main()i32{var v Vec[i32]=[1];let r=E.take(move v);return r[0];}");
  good(
      "enum E{A;} impl E{func bump(x inout i32){x+=1;}} "
      "func main()i32{var x i32=1;E.bump(inout x);return x-2;}");
  unsupported(
      "enum E{A;} impl E{func take(self move Self)Self{return move self;}} "
      "func main()i32{let e=E.A;let r=e.take();return 0;}");

  auto invalid_call = [](auto &x, const std::string &name) -> hir::Call & {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          x.resolved->symbols[c->direct_symbol->value].name == name)
        return *c;
    throw std::runtime_error("method move call missing: " + name);
  };
  invalid_recomputed(method_hir, "method move argument mode", [&](auto &x) {
    auto &call = invalid_call(x, "take");
    need(call.args.size() == 2, "method move arity");
    call.args[1].mode = hir::ParamMode::ordinary;
  });
  invalid_recomputed(method_hir, "method move source marker", [&](auto &x) {
    auto &call = invalid_call(x, "take");
    const auto &value = x.nodes[call.args[1].expression.value];
    const auto &move = std::get<hir::Move>(value.data);
    need(move.binding.has_value(), "method move binding missing");
    auto names = std::make_shared<modules::ResolvedProject>(*x.resolved);
    auto project = std::make_shared<modules::Project>(*names->project);
    auto file = std::make_shared<AstFile>(
        *project->modules[value.source.node.module.value].files.at(
            value.source.node.file));
    auto &ast = file->node(value.source.node.node);
    need(ast.kind == AstKind::Unary &&
             file->tokens.at(ast.token_begin).spelling == "move",
         "method move source token missing");
    file->tokens[ast.token_begin].spelling = "+";
    project->modules[value.source.node.module.value]
        .files[value.source.node.file] = file;
    names->project = project;
    x.resolved = names;
  });
  invalid_recomputed(method_hir, "method move source order", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto call = std::get_if<hir::Call>(&n.data);
          call && call->direct_symbol &&
          x.resolved->symbols[call->direct_symbol->value].name == "take") {
        need(call->args.size() == 2 && n.children.size() == 2,
             "method move source order arity");
        std::swap(call->args[0], call->args[1]);
        std::swap(n.children[0], n.children[1]);
        return;
      }
    throw std::runtime_error("method move source order call missing");
  });
}


void generic_method_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "generic method move HIR rejected: " + messages(hir::verify(*r.value)));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string positive =
      "struct Holder{items Vec[i32];marker i32;} "
      "struct Box{items Vec[i32];marker i32;} "
      "enum E{A;B(x i32);} "
      "func relay[T](value move T)T{return move value;} "
      "impl Holder{"
      "func take[T](self Self,value move T)T{return move value;}"
      "func take_static[T](value move T)T{return move value;}"
      "func pair[T](self Self,value move T,hint T)T{return move value;}"
      "func two[A,B](self Self,first move A,second move B)B{return move second;}"
      "func forward[T](self Self,value move T)T{return relay(move value);}"
      "func nested[T](self Self,value move T)T{return self.take(move value);}"
      "func keep_with_defer[T](self Self,value move T)T{"
      "let local Vec[i32]=[8];defer{let read=local[0];}return move value;}"
      "func accept_box(self Self,value move Box)Box{return move value;}"
      "func named_self[T](self T,value move Vec[i32])i32{return value[0];}"
      "} "
      "impl E{func take[T](value move T)T{return move value;}} "
      "func main()i32{"
      "let h=Holder(items:[1],marker:2);"
      "var a Vec[i32]=[3];let alias=a;let r=h.take(move a);"
      "r[0]=7;if alias[0]!=7{return 1;}"
      "var b Vec[i32]=[4];let sr=Holder.take_static(move b);"
      "var c Vec[i32]=[5];let er=h.take[Vec[i32]](move c);"
      "var d Vec[i32]=[6];let es=Holder.take_static[Vec[i32]](move d);"
      "var e Vec[i32]=[7];let ee=E.take(move e);"
      "var f Vec[i32]=[8];let ex=E.take[Vec[i32]](move f);"
      "let hint i32=0;let scalar=h.pair(move 2,hint);"
      "var i i32=1;var g Vec[i32]=[9];let multi=h.two(move i,move g);"
      "var j Vec[i32]=[10];let fw=h.forward(move j);"
      "var k Vec[i32]=[11];let ne=h.nested(move k);"
      "let copy=h;let cr=h.take(move copy);"
      "var box=Box(items:[12],marker:1);let ba=box;"
      "let br=h.accept_box(move box);br.items[0]=13;"
      "if ba.items[0]!=13{return 2;}"
      "var kb=Box(items:[14],marker:2);let ka=kb;"
      "let kr=h.keep_with_defer(move kb);kr.items[0]=15;"
      "if ka.items[0]!=15{return 3;}"
      "var nv Vec[i32]=[16];"
      "let ns=Holder.named_self[Holder](h,move nv);"
      "return sr[0]+er[0]+es[0]+ee[0]+ex[0]+scalar+multi[0]+fw[0]+"
      "ne[0]+cr.marker+ns-"
      "(4+5+6+7+8+2+9+10+11+2+16);}";
  auto m = good(positive);

  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t ||
        (t->kind != hir::TypeKind::NominalStruct &&
         t->kind != hir::TypeKind::NominalEnum))
      return {};
    return m.resolved->symbols.at(t->declaration.value).name;
  };
  auto is_vec_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    if (!t || t->kind != hir::TypeKind::Vec || t->type_args.size() != 1)
      return false;
    auto e = m.types.find(t->type_args[0]);
    return e && e->kind == hir::TypeKind::SignedInt && e->bits == 32;
  };
  SymbolId main_symbol{0};
  for (const auto &f : m.functions)
    if (m.resolved->symbols[f.symbol.value].name == "main")
      main_symbol = f.symbol;
  need(main_symbol.value != 0, "generic method main symbol missing");

  const hir::FunctionTemplate *h_take = nullptr, *e_take = nullptr;
  for (const auto &t : m.templates) {
    const auto &name = m.resolved->symbols[t.function.symbol.value].name;
    auto sig = m.types.find(t.function.signature);
    need(sig && sig->kind == hir::TypeKind::Function,
         "template signature kind");
    if (name == "take" && sig->parameters.size() == 2 &&
        nominal_name(sig->parameters[0].type) == "Holder")
      h_take = &t;
    if (name == "take" && sig->parameters.size() == 1)
      e_take = &t;
  }
  need(h_take && e_take, "generic method templates missing");
  need(h_take->params.size() == 1 && e_take->params.size() == 1,
       "method generic parameter arity");

  const hir::FunctionInstance *h_vec_instance = nullptr;
  const hir::FunctionInstance *e_vec_instance = nullptr;
  size_t h_vec_instances = 0, e_vec_instances = 0;
  for (const auto &i : m.instances) {
    if (i.origin == h_take->function.symbol) {
      if (i.type_args.size() == 1 && is_vec_i32(i.type_args[0])) {
        ++h_vec_instances;
        h_vec_instance = &i;
      }
    }
    if (i.origin == e_take->function.symbol) {
      ++e_vec_instances;
      e_vec_instance = &i;
    }
  }
  need(h_vec_instances == 1,
       "same concrete method combination did not deduplicate");
  need(e_vec_instances == 1, "enum static method instance count");
  need(h_vec_instance && e_vec_instance &&
           h_vec_instance->symbol != e_vec_instance->symbol,
       "same-name methods lost distinct origins");

  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  const auto *hf = function_for(h_vec_instance->symbol);
  const auto *ef = function_for(e_vec_instance->symbol);
  need(hf && ef, "generic method concrete functions missing");
  auto hs = m.types.find(hf->signature);
  auto es = m.types.find(ef->signature);
  need(hs && hs->parameters.size() == 2 && es && es->parameters.size() == 1,
       "generic method instance signature arity");
  need(hs->parameters[0].mode == hir::ParamMode::ordinary &&
           nominal_name(hs->parameters[0].type) == "Holder" &&
           hs->parameters[1].mode == hir::ParamMode::move &&
           is_vec_i32(hs->parameters[1].type) &&
           hs->result == hs->parameters[1].type,
       "generic method instance signature substitution/modes");
  need(es->parameters[0].mode == hir::ParamMode::move &&
           is_vec_i32(es->parameters[0].type) && es->result == es->parameters[0].type,
       "enum static generic method signature");
  auto ht = m.types.find(h_take->function.signature);
  need(ht && ht->parameters.size() == 2 &&
           m.types.find(ht->parameters[1].type)->kind ==
               hir::TypeKind::TypeParameter,
       "method template signature lost its type parameter");

  std::map<uint64_t, SymbolId> renamed;
  for (auto [from, to] : h_vec_instance->renamed)
    renamed[from.value] = to;
  need(renamed.at(h_take->function.symbol.value) == hf->symbol &&
           renamed.at(h_take->function.parameters[0].symbol.value) ==
               hf->parameters[0].symbol &&
           renamed.at(h_take->function.parameters[1].symbol.value) ==
               hf->parameters[1].symbol,
       "generic method instance did not rename self/move parameter");

  size_t instance_moves = 0, template_moves = 0;
  for (const auto &n : m.nodes) {
    const auto owner = m.scopes[n.scope.value].function;
    if (owner == hf->symbol) {
      need(m.types.find(n.type) &&
               m.types.find(n.type)->kind != hir::TypeKind::TypeParameter,
           "generic method instance retained a type parameter");
      if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
        need(*mv->binding == hf->parameters[1].symbol,
             "instance Move binding was not renamed to its concrete parameter");
        ++instance_moves;
      }
    }
    if (owner == h_take->function.symbol)
      if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
        need(*mv->binding == h_take->function.parameters[1].symbol,
             "template Move binding changed");
        ++template_moves;
      }
  }
  need(instance_moves == 1 && template_moves == 1,
       "generic method Move binding count");

  size_t main_h_calls = 0, main_e_calls = 0;
  std::set<uint64_t> template_symbols, concrete_functions;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &f : m.functions)
    concrete_functions.insert(f.symbol.value);
  for (const auto &n : m.nodes) {
    if (!concrete_functions.count(m.scopes[n.scope.value].function.value))
      continue;
    need(!std::holds_alternative<hir::ConstraintCall>(n.data),
         "concrete generic method retained a constraint call");
    if (auto c = std::get_if<hir::Call>(&n.data)) {
      need(c->type_args.empty(),
           "concrete generic method call retained type arguments");
      if (c->direct_symbol)
        need(!template_symbols.count(c->direct_symbol->value),
             "concrete call still targets a method template");
      if (m.scopes[n.scope.value].function == main_symbol && c->direct_symbol) {
        if (*c->direct_symbol == h_vec_instance->symbol)
          ++main_h_calls;
        if (*c->direct_symbol == e_vec_instance->symbol)
          ++main_e_calls;
      }
    }
  }
  need(main_h_calls == 2 && main_e_calls == 2,
       "inferred/explicit instance and enum static calls missing");

  invalid_recomputed(m, "generic method instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == h_vec_instance->symbol) {
        i.origin = e_take->function.symbol;
        return;
      }
    throw std::runtime_error("generic method instance record missing");
  });
  const auto h_args = h_vec_instance->type_args;
  invalid_recomputed(m, "generic method unresolved instance call", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          *c->direct_symbol == h_vec_instance->symbol) {
        c->direct_symbol = h_take->function.symbol;
        c->type_args = h_args;
        return;
      }
    throw std::runtime_error("generic method concrete call missing");
  });
  invalid_recomputed(m, "generic method call argument mode", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          *c->direct_symbol == h_vec_instance->symbol && c->args.size() == 2) {
        c->args[1].mode = hir::ParamMode::ordinary;
        return;
      }
    throw std::runtime_error("generic method move call missing");
  });
  invalid(m, "generic method move binding rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (m.scopes[n.scope.value].function == h_vec_instance->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          *mv->binding = h_take->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("generic method instance Move missing");
  });

  const std::string prefix =
      "struct H{items Vec[i32];marker i32;} impl H{"
      "func take[T](self Self,value move T)T{return move value;}"
      "func both[T](self Self,a move T,b move T)i32{return 0;}} ";
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "var v Vec[i32]=[1];let r=h.take(v);return r[0];}",
      "E_CALL_ARGUMENT");
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "let r=h.take(move h);return 0;}",
      "E_CALL_ARGUMENT");
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "var v Vec[i32]=[1];let r=h.take(move v);return v[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(prefix + "func main()i32{let h=H(items:[1],marker:0);"
               "var v Vec[i32]=[1];return h.both(move v,move v);}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  unsupported(prefix + "func main()i32{let h=H(items:[1],marker:0);"
                       "let r=h.take(move h.items);return r[0];}");
  // Type.method supplies the declared ordinary Self explicitly; the later
  // move parameter uses the same source method rules.
  good(prefix + "func main()i32{let h=H(items:[1],marker:0);"
                "var v Vec[i32]=[1];"
                "let r=H.take(h,move v);return r[0];}");
  unsupported(
      "struct H{marker i32;} impl H{"
      "func take[T](self move Self,value move T)T{return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  good(
      "struct H{marker i32;} impl H{"
      "func take[T](self Self,value move T)T{return move value;}} "
      "func main()i32{let h=H(marker:0);"
      "let r=h.take[string](move \"x\");return 0;}");
  good(
      "struct H{marker i32;} func bump(x inout i32){x+=1;} "
      "impl H{func keep[T](self Self,value move T,cb func(inout i32))T{"
      "return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=h.keep(move v,bump);return r[0]-1;}");

  auto foreign_check = [&](const std::string &text) {
    Temp t;
    fs::copy_file(root / "contracts/python_bindings.example.json",
                  t.path / "binding.json");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"main\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"python\"\n"
          "[dependencies.numpy]\nkind=\"python\"\nbinding=\"binding.json\"\n"
          "host_package=\"numpy\"\n");
    write(t.path / "src/main.lang", "package app; " + text);
    SourceManager sm;
    auto loaded = modules::load_project(
        {t.path / "lang.toml", {}, t.path / "binding.json"}, sm);
    need(loaded.success(), messages(loaded));
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(*loaded.value));
    need(resolved.success(), messages(resolved));
    return sema::check_types(resolved);
  };
  auto foreign = foreign_check(
      "import \"std/py\" as py; import \"numpy\" as np; "
      "struct H{marker i32;} impl H{"
      "func take[T](self Self,value move T)T{return move value;}} "
      "func use[T](h H,value T)T{let local=value;return h.take(move local);} "
      "func make() np.ndarray throws py.Error{return np.linspace(-1.0,1.0);} "
      "func main()i32 throws py.Error{let h=H(marker:0);"
      "let r=use(h,make());return 0;}");
  need(!foreign.success() && foreign.failure == FailureKind::unsupported &&
           has(foreign, "E_CAPABILITY_UNAVAILABLE"),
       "foreign generic method move was not rejected at its concrete instance\n" +
           messages(foreign));
}

void generic_impl_move() {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "generic impl move HIR rejected: " + messages(hir::verify(*r.value)));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string positive = R"(
struct Box[T]{value T;items Vec[i32];marker i32;}
struct Pair[A,B]{first A;second B;items Vec[i32];}
enum Maybe[T]{None;Some(value T);}
func relay[T](value move T)T{return move value;}
impl[T] Box[T]{
func take(self Self,value move T)T{return move value;}
func take_static(value move T)T{return move value;}
func take_named(self Self,first i32,value move T,second i32)T{return move value;}
func forward(self Self,value move T)T{return relay(move value);}
func nested(self Self,value move T)T{return self.take(move value);}
func keep(self Self,value move T)T{
let local Vec[i32]=[8];defer{let read=local[0];}return move value;}
func choose(self Self,other Self)Self{return other;}
func accept(self Self,value move Self)i32{return self.marker+value.marker;}
}
impl[A,B] Pair[A,B]{
func move_second(self Self,first move A,second move B)B{return move second;}
}
impl[T] Maybe[T]{
func take(value move T)T{return move value;}
}
func main()i32{
let b=Box[Vec[i32]](value:[1],items:[2],marker:3);
var values Vec[i32]=[4];let alias=values;let result=b.take(move values);
result[0]=7;if alias[0]!=7{return 1;}
let temp=b.take([5]);if temp[0]!=5{return 2;}
var static_values Vec[i32]=[6];let static_alias=static_values;
let static_result=Box[Vec[i32]].take_static(move static_values);
static_result[0]=9;if static_alias[0]!=9{return 3;}
let static_temp=Box[Vec[i32]].take_static([10]);
if static_temp[0]!=10{return 4;}
var forwarded Vec[i32]=[11];let forwarded_result=b.forward(move forwarded);
var nested Vec[i32]=[13];let nested_result=b.nested(move nested);
var kept Vec[i32]=[14];let kept_alias=kept;
let kept_result=b.keep(move kept);kept_result[0]=15;
if kept_alias[0]!=15{return 5;}
let scalar_box=Box[i32](value:1,items:[16],marker:4);
var scalar i32=5;let scalar_result=scalar_box.take(move scalar);
if scalar_result!=5{return 6;}
let scalar_static=Box[i32].take_static(17);
if scalar_static!=17{return 7;}
let inner=Box[i32](value:18,items:[19],marker:8);
let inner_alias=inner;
let outer=Box[Box[i32]](value:inner,items:[20],marker:9);
let moved_box=outer.take(move inner_alias);moved_box.items[0]=21;
if inner.items[0]!=21{return 8;}
let pair=Pair[i32,Vec[i32]](first:1,second:[22],items:[23]);
var pair_values Vec[i32]=[24];let pair_result=pair.move_second(move 2,move pair_values);
if pair_result[0]!=24{return 9;}
let maybe_temp=Maybe[Vec[i32]].take([25]);
var maybe_values Vec[i32]=[26];let maybe_alias=maybe_values;
let maybe_result=Maybe[Vec[i32]].take(move maybe_values);maybe_result[0]=27;
if maybe_alias[0]!=27{return 10;}
let maybe_again=Maybe[Vec[i32]].take([25]);
if maybe_again[0]!=25{return 11;}
let chosen=b.choose(b);if chosen.marker!=3{return 12;}
var accepted=Box[Vec[i32]](value:[29],items:[30],marker:12);
let accepted_result=b.accept(move accepted);
if accepted_result!=15{return 13;}
var named_values Vec[i32]=[31];
let named=b.take_named(second:0,value:move named_values,first:0);
if named[0]!=31{return 14;}
let total=i32(forwarded_result[0])+i32(nested_result[0])+
i32(maybe_temp[0])+i32(maybe_again[0]);
if total!=11+13+25+25{return 15;}
return 0;}
)";
  auto m = good(positive);

  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return m.resolved->symbols.at(t->declaration.value).name;
  };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_vec_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    if (!t || t->kind != hir::TypeKind::Vec || t->type_args.size() != 1)
      return false;
    return is_i32(t->type_args[0]);
  };
  auto is_box_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Box" && t->type_args.size() == 1 &&
           is_i32(t->type_args[0]);
  };
  auto has_parameter = [&](TypeId root) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root);
  };
  auto find_impl = [&](const std::string &name) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (!im.trait && nominal_name(im.self_type) == name)
        return &im;
    return nullptr;
  };
  auto template_for = [&](SymbolId method) -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (t.function.symbol == method)
        return &t;
    return nullptr;
  };
  auto method_template = [&](const hir::ImplDecl &im, const std::string &name,
                             size_t arity) -> const hir::FunctionTemplate * {
    for (auto method : im.methods)
      if (m.resolved->symbols[method.value].name == name) {
        const auto *t = template_for(method);
        if (t && t->function.parameters.size() == arity)
          return t;
      }
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto calls_named = [&](const std::string &name) {
    std::vector<const hir::HirNode *> result;
    for (const auto &n : m.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          m.resolved->symbols[c->direct_symbol->value].name == name)
        result.push_back(&n);
    return result;
  };
  auto same_impl_params = [&](const hir::FunctionTemplate *t,
                              const hir::ImplDecl &im) {
    if (!t || t->params.size() != im.params.size())
      return false;
    for (size_t i = 0; i < im.params.size(); ++i)
      if (t->params[i].symbol != im.params[i].symbol)
        return false;
    return true;
  };

  const auto *box_impl = find_impl("Box");
  const auto *pair_impl = find_impl("Pair");
  const auto *maybe_impl = find_impl("Maybe");
  need(box_impl && pair_impl && maybe_impl, "generic impl declarations missing");
  const auto *box_take = method_template(*box_impl, "take", 2);
  const auto *box_static = method_template(*box_impl, "take_static", 1);
  const auto *box_named = method_template(*box_impl, "take_named", 4);
  const auto *box_choose = method_template(*box_impl, "choose", 2);
  const auto *pair_second = method_template(*pair_impl, "move_second", 3);
  const auto *maybe_take = method_template(*maybe_impl, "take", 1);
  need(box_take && box_static && box_named && box_choose && pair_second &&
           maybe_take,
       "generic impl method templates missing");
  need(same_impl_params(box_take, *box_impl) &&
           same_impl_params(box_static, *box_impl) &&
           same_impl_params(pair_second, *pair_impl) &&
           same_impl_params(maybe_take, *maybe_impl),
       "generic impl method did not inherit exactly its impl parameters");
  need(pair_second->params.size() == 2 &&
           pair_second->params[0].symbol == pair_impl->params[0].symbol &&
           pair_second->params[1].symbol == pair_impl->params[1].symbol,
       "multiple impl parameters changed order");

  const auto box_take_instances = instances_of(box_take);
  need(box_take_instances.size() == 3,
       "Box take instance combinations were not deduplicated");
  const hir::FunctionInstance *box_vec = nullptr, *box_box = nullptr,
                             *box_scalar = nullptr;
  for (const auto *i : box_take_instances) {
    need(i->type_args.size() == 1, "Box take instance arity");
    if (is_vec_i32(i->type_args[0]))
      box_vec = i;
    else if (is_box_i32(i->type_args[0]))
      box_box = i;
    else if (is_i32(i->type_args[0]))
      box_scalar = i;
  }
  need(box_vec && box_box && box_scalar &&
           box_vec->symbol != box_box->symbol &&
           box_vec->symbol != box_scalar->symbol &&
           box_box->symbol != box_scalar->symbol,
       "Box concrete type instances missing");
  const auto box_static_instances = instances_of(box_static);
  const auto maybe_instances = instances_of(maybe_take);
  const auto pair_instances = instances_of(pair_second);
  const hir::FunctionInstance *box_static_vec = nullptr;
  for (const auto *i : box_static_instances) {
    need(i->type_args.size() == 1, "Box static instance arity");
    if (is_vec_i32(i->type_args[0]))
      box_static_vec = i;
  }
  need(box_static_instances.size() == 2 && box_static_vec,
       "Box static instance/type argument mismatch");
  need(maybe_instances.size() == 1 &&
           maybe_instances[0]->type_args.size() == 1 &&
           is_vec_i32(maybe_instances[0]->type_args[0]),
       "generic enum static instance mismatch");
  need(pair_instances.size() == 1 &&
           pair_instances[0]->type_args.size() == 2 &&
           is_i32(pair_instances[0]->type_args[0]) &&
           is_vec_i32(pair_instances[0]->type_args[1]),
       "multiple impl type arguments lost their order");
  need(box_vec->origin == box_take->function.symbol &&
           box_box->origin == box_take->function.symbol &&
           box_static_vec->origin == box_static->function.symbol &&
           maybe_instances[0]->origin == maybe_take->function.symbol &&
           box_static_vec->origin != maybe_instances[0]->origin,
       "generic impl instances lost their method origin");

  const auto *box_vec_function = function_for(box_vec->symbol);
  const auto *box_box_function = function_for(box_box->symbol);
  const auto *box_static_function = function_for(box_static_vec->symbol);
  const auto *maybe_function = function_for(maybe_instances[0]->symbol);
  const auto *pair_function = function_for(pair_instances[0]->symbol);
  need(box_vec_function && box_box_function && box_static_function &&
           maybe_function && pair_function,
       "generic impl instance functions missing");
  auto box_vec_sig = m.types.find(box_vec_function->signature);
  auto box_box_sig = m.types.find(box_box_function->signature);
  auto box_static_sig = m.types.find(box_static_function->signature);
  auto maybe_sig = m.types.find(maybe_function->signature);
  auto pair_sig = m.types.find(pair_function->signature);
  need(box_vec_sig && box_box_sig && box_static_sig && maybe_sig && pair_sig,
       "generic impl instance signatures missing");
  need(box_vec_sig->parameters.size() == 2 &&
           nominal_name(box_vec_sig->parameters[0].type) == "Box" &&
           m.types.find(box_vec_sig->parameters[0].type)->type_args.size() == 1 &&
           is_vec_i32(m.types.find(box_vec_sig->parameters[0].type)->type_args[0]) &&
           box_vec_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           box_vec_sig->parameters[1].mode == hir::ParamMode::move &&
           is_vec_i32(box_vec_sig->parameters[1].type) &&
           box_vec_sig->result == box_vec_sig->parameters[1].type,
       "Box[Vec] Self/move signature was not fully substituted");
  need(box_box_sig->parameters.size() == 2 &&
           nominal_name(box_box_sig->parameters[0].type) == "Box" &&
           is_box_i32(m.types.find(box_box_sig->parameters[0].type)->type_args[0]) &&
           is_box_i32(box_box_sig->parameters[1].type) &&
           box_box_sig->result == box_box_sig->parameters[1].type,
       "nested nominal Self substitution mismatch");
  need(box_static_sig->parameters.size() == 1 &&
           box_static_sig->parameters[0].mode == hir::ParamMode::move &&
           is_vec_i32(box_static_sig->parameters[0].type),
       "type-qualified generic impl method gained a self parameter");
  need(maybe_sig->parameters.size() == 1 &&
           maybe_sig->parameters[0].mode == hir::ParamMode::move &&
           is_vec_i32(maybe_sig->parameters[0].type),
       "generic enum static signature mismatch");
  need(pair_sig->parameters.size() == 3 &&
           pair_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           pair_sig->parameters[1].mode == hir::ParamMode::move &&
           pair_sig->parameters[2].mode == hir::ParamMode::move &&
           is_i32(pair_sig->parameters[1].type) &&
           is_vec_i32(pair_sig->parameters[2].type) &&
           pair_sig->result == pair_sig->parameters[2].type,
       "two impl parameters substituted out of order");

  const auto choose_inst = instances_of(box_choose);
  need(choose_inst.size() == 1, "ordinary generic impl method instance missing");
  auto choose_sig =
      m.types.find(function_for(choose_inst[0]->symbol)->signature);
  need(choose_sig->parameters.size() == 2 &&
           choose_sig->parameters[0].type == choose_sig->parameters[1].type &&
           choose_sig->result == choose_sig->parameters[0].type &&
           nominal_name(choose_sig->result) == "Box" &&
           is_vec_i32(m.types.find(choose_sig->result)->type_args[0]),
       "ordinary Self parameter/result was not fully substituted");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &i : m.instances) {
    const auto *f = function_for(i.symbol);
    need(f, "instance function missing while checking concrete types");
    auto sig = m.types.find(f->signature);
    need(sig && !has_parameter(f->signature),
         "concrete instance signature retained a type parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f->symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete instance node retained a type parameter");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete generic impl node retained a constraint call");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete generic impl call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete generic impl call targets a template");
      }
    }
  }

  size_t vec_moves = 0, pair_moves = 0, template_moves = 0;
  for (const auto &n : m.nodes) {
    const auto owner = m.scopes[n.scope.value].function;
    if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
      if (owner == box_vec_function->symbol) {
        need(*mv->binding == box_vec_function->parameters[1].symbol,
             "Box[Vec] Move binding was not renamed to the concrete parameter");
        ++vec_moves;
      }
      if (owner == pair_function->symbol) {
        need(*mv->binding == pair_function->parameters[2].symbol,
             "Pair Move binding did not follow the second impl parameter");
        ++pair_moves;
      }
      if (owner == box_take->function.symbol) {
        need(*mv->binding == box_take->function.parameters[1].symbol,
             "template Move binding changed");
        ++template_moves;
      }
    }
  }
  need(vec_moves == 1 && pair_moves == 1 && template_moves == 1,
       "generic impl Move bindings missing");
  std::map<uint64_t, SymbolId> renamed;
  for (auto [from, to] : box_vec->renamed)
    renamed[from.value] = to;
  need(renamed.at(box_take->function.symbol.value) == box_vec_function->symbol &&
           renamed.at(box_take->function.parameters[0].symbol.value) ==
               box_vec_function->parameters[0].symbol &&
           renamed.at(box_take->function.parameters[1].symbol.value) ==
               box_vec_function->parameters[1].symbol,
       "generic impl instance did not rename Self/move parameters");

  auto named_calls = calls_named("take_named");
  need(named_calls.size() == 1, "generic impl named call missing");
  const auto &named_call = std::get<hir::Call>(named_calls[0]->data);
  need(named_call.args.size() == 4 &&
           named_call.args[0].parameter_index == 0 &&
           named_call.args[1].parameter_index == 3 &&
           named_call.args[2].parameter_index == 2 &&
           named_call.args[3].parameter_index == 1,
       "generic impl named parameters were not reordered");
  for (size_t i = 1; i < named_call.args.size(); ++i)
    need(m.nodes[named_call.args[i - 1].expression.value].source.span.begin_byte <
             m.nodes[named_call.args[i].expression.value].source.span.begin_byte,
         "generic impl named argument evaluation order changed");
  need(named_call.args[2].mode == hir::ParamMode::move &&
           m.nodes[named_call.args[2].expression.value].kind() ==
               hir::NodeKind::move,
       "generic impl named move mode/source");

  invalid_recomputed(m, "generic impl instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == box_vec->symbol) {
        i.origin = maybe_take->function.symbol;
        return;
      }
    throw std::runtime_error("generic impl instance record missing");
  });
  invalid(m, "generic impl fully substituted self", [&](auto &x) {
    auto template_sig = *x.types.find(box_take->function.signature);
    for (auto &f : x.functions)
      if (f.symbol == box_vec->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].type = template_sig.parameters[1].type;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("generic impl instance function missing");
  });
  invalid(m, "generic impl parameter order", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == pair_instances[0]->symbol) {
        std::swap(i.type_args[0], i.type_args[1]);
        return;
      }
    throw std::runtime_error("generic impl pair instance missing");
  });
  invalid(m, "generic impl move binding rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == box_vec->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          *mv->binding = box_take->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("generic impl instance Move missing");
  });

  const std::string box =
      "struct Box[T]{value T;items Vec[i32];marker i32;} ";
  good(box + "impl[T] Box[T]{"
            "func take[U](self Self,value move U)U{return move value;}} "
            "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
            "var x i32=1;let r=b.take(move x);return r-1;}");
  good(box + "impl[T] Box[T]{"
            "func take[U](value move U)U{return move value;}} "
            "func main()i32{var x i32=1;"
            "let r=Box[i32].take(move x);return r-1;}");
  good("trait V{} impl V for i32{} struct Box[T]{value T;} "
       "impl[T V] Box[T]{func take(value move T)T{return move value;}} "
       "func main()i32{var x i32=1;"
       "let r=Box[i32].take(move x);return r-1;}");
  good("trait V{func take(self Self,value move Vec[i32])Vec[i32];} "
       "struct Box[T]{value T;items Vec[i32];} "
       "impl[T] V for Box[T]{"
       "func take(self Self,value move Vec[i32])Vec[i32]{"
       "return move value;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1]);"
       "var v Vec[i32]=[2];let r=b.take(move v);return r[0];}");
  good(box + "impl[T] Box[T]{"
       "func bump(self Self,x inout i32){x+=1;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;b.bump(inout x);return x-2;}");
  good(box + "impl[T] Box[T]{"
       "func bump(x inout i32){x+=1;}} "
       "func main()i32{var x i32=1;"
       "Box[i32].bump(inout x);return x-2;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take(self move Self,value move T)T{return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=b.take(move x);return r-1;}");
  good(box + "impl[T] Box[T]{"
            "func take(self move Self,value move T)T{return move value;}} "
            "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
            "var x i32=1;let r=Box[i32].take(move b,move x);"
            "return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take(self inout Self,value move T)T{return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=b.take(move x);return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take(self Self,value move T)T{return move value;}} "
                    "func main()i32{var b=Box[Vec[i32]](value:[1],items:[2],"
                    "marker:0);let r=b.take(move b.items);return r[0];}");
  good(box + "impl[T] Box[T]{"
            "func take(value move string)string{return move value;}} "
            "func main()i32{let r=Box[i32].take(move \"x\");return 0;}");
  bad(box + "impl[T] Box[T]{"
           "func take(self Self,value move T)T{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "let x i32=1;let r=b.take(x);return r-1;}",
      "E_CALL_ARGUMENT");
  bad(box + "impl[T] Box[T]{"
           "func take(self Self,value move T)T{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "var x i32=1;let r=b.take(move x);return x-1;}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(box + "impl[T] Box[T]{"
           "func both(self Self,first move T,second move T)i32{return 0;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "var x i32=1;return b.both(move x,move x);}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(box + "impl[T] Box[T]{"
           "func accept(self Self,value move Self)i32{"
           "return self.marker+value.marker;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "return b.accept(move b);}",
      "E_CALL_ARGUMENT");
  bad(box + "func pass[T](value move T)T{return move value;} "
           "impl[T] Box[T]{func accept(self Self,value move Self)i32{"
           "return self.marker+value.marker;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "return b.accept(pass(move b));}",
      "E_CALL_ARGUMENT");
  bad(box + "impl[T] Box[T]{"
           "func take(self Self,value move T)T{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "let f=b.take;return 0;}",
      "E_TYPE_MISMATCH");
}

void generic_impl_own_method_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "generic impl own-method move HIR rejected: " + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string positive = R"(
struct Box[T]{value T;items Vec[i32];marker i32;}
struct Pair[A,B]{first A;second B;items Vec[i32];}
enum Maybe[T]{None;Some(value T);}
func relay[T](value move T)T{return move value;}
impl[T] Box[T]{
func take[U](self Self,value move U)U{return move value;}
func take_static[U](value move U)U{return move value;}
func mix[U](self Self,left move T,right move U)U{return move right;}
func forward[U](self Self,value move U)U{return relay(move value);}
func also[U](self Self,value move U)i32{return self.marker+1;}
func same_name[T](self Self,value move T)T{return move value;}
func static_self[U](self U,value move U)i32{return 1;}
}
impl[A,B] Pair[A,B]{
func move_second[U](self Self,first move A,second move B,value move U)U{return move value;}
func pair_take[U,V](self Self,first move U,second move V)V{return move second;}
}
impl[T] Maybe[T]{
func take[U](value move U)U{return move value;}
}
func main()i32{
let b=Box[i32](value:1,items:[1],marker:2);
let bv=Box[Vec[i32]](value:[2],items:[3],marker:3);
var x i32=4;let rx=b.take(move x);
var xs i32=25;let rxs=b.take[i32](move xs);
var y Vec[i32]=[5];let ry=b.take(move y);
var z Vec[i32]=[6];let rz=bv.take(move z);
var s0 Vec[i32]=[7];let rs0=Box[i32].take_static[Vec[i32]](move s0);
var s1 Vec[i32]=[8];let rs1=Box[i32].take_static(move s1);
var s2 Vec[i32]=[9];let rs2=Box[Vec[i32]].take_static[Vec[i32]](move s2);
var m1 i32=10;var m2 Vec[i32]=[11];let rm=b.mix(move m1,move m2);
var m3 Vec[i32]=[12];var m4 i32=13;let rm2=bv.mix(move m3,move m4);
var f Vec[i32]=[14];let rf=b.forward(move f);
let copy=b;let acc=b.also(move copy);
var sn Vec[i32]=[24];let rsn=b.same_name(move sn);
let copy3=b;let rss=Box[i32].static_self[Box[i32]](b,move copy3);
let pair=Pair[i32,Vec[i32]](first:1,second:[15],items:[16]);
var p1 i32=17;var p2 Vec[i32]=[18];
var p3 Box[i32]=Box[i32](value:1,items:[19],marker:4);
let rp=pair.move_second(move p1,move p2,move p3);
var q1 Vec[i32]=[20];var q2 i32=21;
let rq=pair.pair_take[Vec[i32],i32](move q1,move q2);
let e0=Maybe[i32].take[Vec[i32]]([22]);
var e1 Vec[i32]=[23];let re=Maybe[Vec[i32]].take(move e1);
return rx+rxs+ry[0]+rz[0]+rs0[0]+rs1[0]+rs2[0]+rm[0]+rm2+rf[0]+acc+
rsn[0]+rss+rp.items[0]+rq+e0[0]+re[0];}
)";
  auto m = good(positive);

  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return m.resolved->symbols.at(t->declaration.value).name;
  };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_vec_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    if (!t || t->kind != hir::TypeKind::Vec || t->type_args.size() != 1)
      return false;
    return is_i32(t->type_args[0]);
  };
  auto is_box_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Box" && t->type_args.size() == 1 &&
           is_i32(t->type_args[0]);
  };
  auto is_box_vec = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Box" && t->type_args.size() == 1 &&
           is_vec_i32(t->type_args[0]);
  };
  auto is_pair_i32_vec = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Pair" && t->type_args.size() == 2 &&
           is_i32(t->type_args[0]) && is_vec_i32(t->type_args[1]);
  };
  auto has_parameter = [&](TypeId root) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root);
  };
  auto find_impl = [&](const std::string &name) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (!im.trait && nominal_name(im.self_type) == name)
        return &im;
    return nullptr;
  };
  auto template_for = [&](SymbolId method) -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (t.function.symbol == method)
        return &t;
    return nullptr;
  };
  auto method_template = [&](const hir::ImplDecl &im,
                             const std::string &name)
      -> const hir::FunctionTemplate * {
    for (auto method : im.methods)
      if (m.resolved->symbols[method.value].name == name)
        return template_for(method);
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto same_prefix = [&](const hir::FunctionTemplate *t,
                         const hir::ImplDecl &im, size_t suffix) {
    if (!t || t->params.size() != im.params.size() + suffix)
      return false;
    for (size_t i = 0; i < im.params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.symbol != im.params[i].symbol ||
          p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte >= t->function.source.span.begin_byte)
        return false;
    }
    for (size_t i = im.params.size(); i < t->params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte < t->function.source.span.begin_byte ||
          p.source.span.end_byte > t->function.source.span.end_byte)
        return false;
    }
    return true;
  };

  const auto *box_impl = find_impl("Box");
  const auto *pair_impl = find_impl("Pair");
  const auto *maybe_impl = find_impl("Maybe");
  need(box_impl && pair_impl && maybe_impl,
       "generic impl own-method declarations missing");
  const auto *box_take = method_template(*box_impl, "take");
  const auto *box_static = method_template(*box_impl, "take_static");
  const auto *box_mix = method_template(*box_impl, "mix");
  const auto *box_forward = method_template(*box_impl, "forward");
  const auto *box_also = method_template(*box_impl, "also");
  const auto *box_same_name = method_template(*box_impl, "same_name");
  const auto *box_static_self = method_template(*box_impl, "static_self");
  const auto *pair_second = method_template(*pair_impl, "move_second");
  const auto *pair_take = method_template(*pair_impl, "pair_take");
  const auto *maybe_take = method_template(*maybe_impl, "take");
  need(box_take && box_static && box_mix && box_forward && box_also &&
           box_same_name && box_static_self && pair_second && pair_take &&
           maybe_take,
       "generic impl own-method templates missing");
  need(same_prefix(box_take, *box_impl, 1) &&
           same_prefix(box_static, *box_impl, 1) &&
           same_prefix(box_mix, *box_impl, 1) &&
           same_prefix(box_forward, *box_impl, 1) &&
           same_prefix(box_also, *box_impl, 1) &&
           same_prefix(box_same_name, *box_impl, 1) &&
           same_prefix(box_static_self, *box_impl, 1) &&
           same_prefix(pair_second, *pair_impl, 1) &&
           same_prefix(pair_take, *pair_impl, 2) &&
           same_prefix(maybe_take, *maybe_impl, 1),
       "generic impl own-method parameter prefix/suffix source mismatch");
  need(box_take->params.size() == 2 &&
           box_take->params[0].symbol == box_impl->params[0].symbol &&
           box_take->params[1].symbol != box_impl->params[0].symbol,
       "Box take did not keep impl T before its own U");
  need(box_same_name->params.size() == 2 &&
           box_same_name->params[0].symbol == box_impl->params[0].symbol &&
           box_same_name->params[1].symbol != box_impl->params[0].symbol &&
           m.resolved->symbols[box_same_name->params[0].symbol.value].name ==
               "T" &&
           m.resolved->symbols[box_same_name->params[1].symbol.value].name ==
               "T",
       "same-named method parameter impersonated its impl parameter");
  need(pair_take->params.size() == 4 &&
           pair_take->params[0].symbol == pair_impl->params[0].symbol &&
           pair_take->params[1].symbol == pair_impl->params[1].symbol &&
           pair_take->params[2].symbol != pair_take->params[0].symbol &&
           pair_take->params[3].symbol != pair_take->params[1].symbol &&
           pair_take->params[2].symbol != pair_take->params[3].symbol,
       "Pair pair_take parameter order or own identity changed");

  const auto box_take_instances = instances_of(box_take);
  need(box_take_instances.size() == 3,
       "Box take instance combinations were not exact");
  const hir::FunctionInstance *take_i32_i32 = nullptr,
                             *take_i32_vec = nullptr,
                             *take_vec_vec = nullptr;
  for (const auto *i : box_take_instances) {
    need(i->type_args.size() == 2, "Box take instance arity");
    if (is_i32(i->type_args[0]) && is_i32(i->type_args[1]))
      take_i32_i32 = i;
    else if (is_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      take_i32_vec = i;
    else if (is_vec_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      take_vec_vec = i;
    else
      throw std::runtime_error("unexpected Box take type combination");
  }
  need(take_i32_i32 && take_i32_vec && take_vec_vec,
       "Box take impl/own combinations missing");
  need(take_i32_i32->symbol != take_i32_vec->symbol &&
           take_i32_i32->symbol != take_vec_vec->symbol &&
           take_i32_vec->symbol != take_vec_vec->symbol,
       "Box take full combination keys collapsed");

  const auto box_static_instances = instances_of(box_static);
  need(box_static_instances.size() == 2,
       "Box take_static explicit/inferred combinations were not deduplicated");
  const hir::FunctionInstance *static_i32_vec = nullptr,
                             *static_vec_vec = nullptr;
  for (const auto *i : box_static_instances) {
    need(i->type_args.size() == 2, "Box static instance arity");
    if (is_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      static_i32_vec = i;
    else if (is_vec_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      static_vec_vec = i;
    else
      throw std::runtime_error("unexpected Box static type combination");
  }
  need(static_i32_vec && static_vec_vec,
       "Box static same-signature combinations missing");

  const auto mix_instances = instances_of(box_mix);
  const auto forward_instances = instances_of(box_forward);
  const auto also_instances = instances_of(box_also);
  const auto same_name_instances = instances_of(box_same_name);
  const auto static_self_instances = instances_of(box_static_self);
  const auto pair_second_instances = instances_of(pair_second);
  const auto pair_take_instances = instances_of(pair_take);
  const auto maybe_instances = instances_of(maybe_take);
  need(mix_instances.size() == 2 && forward_instances.size() == 1 &&
           also_instances.size() == 1 && same_name_instances.size() == 1 &&
           static_self_instances.size() == 1 &&
           pair_second_instances.size() == 1 &&
           pair_take_instances.size() == 1 && maybe_instances.size() == 2,
       "generic impl own-method instance counts changed");
  const hir::FunctionInstance *mix_i32_vec = nullptr,
                             *mix_vec_i32 = nullptr;
  for (const auto *i : mix_instances) {
    need(i->type_args.size() == 2, "Box mix instance arity");
    if (is_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      mix_i32_vec = i;
    else if (is_vec_i32(i->type_args[0]) && is_i32(i->type_args[1]))
      mix_vec_i32 = i;
    else
      throw std::runtime_error("unexpected Box mix type combination");
  }
  need(mix_i32_vec && mix_vec_i32,
       "Box mix impl/own order combinations missing");
  need(forward_instances[0]->type_args.size() == 2 &&
           is_i32(forward_instances[0]->type_args[0]) &&
           is_vec_i32(forward_instances[0]->type_args[1]),
       "Box forward full type arguments mismatch");
  need(also_instances[0]->type_args.size() == 2 &&
           is_i32(also_instances[0]->type_args[0]) &&
           is_box_i32(also_instances[0]->type_args[1]),
       "Box also copied-self type arguments mismatch");
  need(same_name_instances[0]->type_args.size() == 2 &&
           is_i32(same_name_instances[0]->type_args[0]) &&
           is_vec_i32(same_name_instances[0]->type_args[1]),
       "same-named own T replaced or reordered the impl T key");
  need(static_self_instances[0]->type_args.size() == 2 &&
           is_i32(static_self_instances[0]->type_args[0]) &&
           is_box_i32(static_self_instances[0]->type_args[1]),
       "static own U=Self changed the impl/own instance key");
  need(pair_second_instances[0]->type_args.size() == 3 &&
           is_i32(pair_second_instances[0]->type_args[0]) &&
           is_vec_i32(pair_second_instances[0]->type_args[1]) &&
           is_box_i32(pair_second_instances[0]->type_args[2]),
       "Pair move_second [A,B,U] order changed");
  need(pair_take_instances[0]->type_args.size() == 4 &&
           is_i32(pair_take_instances[0]->type_args[0]) &&
           is_vec_i32(pair_take_instances[0]->type_args[1]) &&
           is_vec_i32(pair_take_instances[0]->type_args[2]) &&
           is_i32(pair_take_instances[0]->type_args[3]),
       "Pair pair_take [A,B,U,V] order changed");
  const hir::FunctionInstance *maybe_i32_vec = nullptr,
                             *maybe_vec_vec = nullptr;
  for (const auto *i : maybe_instances) {
    need(i->type_args.size() == 2, "Maybe take instance arity");
    if (is_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      maybe_i32_vec = i;
    else if (is_vec_i32(i->type_args[0]) && is_vec_i32(i->type_args[1]))
      maybe_vec_vec = i;
  }
  need(maybe_i32_vec && maybe_vec_vec &&
           maybe_i32_vec->symbol != maybe_vec_vec->symbol,
       "generic enum static impl/own combinations missing");

  auto signature_of = [&](const hir::FunctionInstance *i) {
    return m.types.find(function_for(i->symbol)->signature);
  };
  auto take_i32_vec_sig = signature_of(take_i32_vec);
  auto take_vec_vec_sig = signature_of(take_vec_vec);
  auto static_i32_vec_sig = signature_of(static_i32_vec);
  auto static_vec_vec_sig = signature_of(static_vec_vec);
  auto mix_i32_vec_sig = signature_of(mix_i32_vec);
  auto mix_vec_i32_sig = signature_of(mix_vec_i32);
  auto pair_second_sig = signature_of(pair_second_instances[0]);
  auto pair_take_sig = signature_of(pair_take_instances[0]);
  auto same_name_sig = signature_of(same_name_instances[0]);
  auto static_self_sig = signature_of(static_self_instances[0]);
  auto maybe_sig = signature_of(maybe_i32_vec);
  need(take_i32_vec_sig && take_vec_vec_sig && static_i32_vec_sig &&
           static_vec_vec_sig && mix_i32_vec_sig && mix_vec_i32_sig &&
           same_name_sig && static_self_sig && pair_second_sig &&
           pair_take_sig && maybe_sig,
       "generic impl own-method signatures missing");
  need(take_i32_vec_sig->parameters.size() == 2 &&
           is_box_i32(take_i32_vec_sig->parameters[0].type) &&
           take_i32_vec_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_vec_i32(take_i32_vec_sig->parameters[1].type) &&
           take_i32_vec_sig->parameters[1].mode == hir::ParamMode::move &&
           take_i32_vec_sig->result == take_i32_vec_sig->parameters[1].type,
       "Box take [i32,Vec] Self/own signature mismatch");
  need(take_vec_vec_sig->parameters.size() == 2 &&
           is_box_vec(take_vec_vec_sig->parameters[0].type) &&
           is_vec_i32(take_vec_vec_sig->parameters[1].type) &&
           take_vec_vec_sig->result == take_vec_vec_sig->parameters[1].type,
       "Box take [Vec,Vec] Self substitution mismatch");
  need(static_i32_vec_sig->parameters.size() == 1 &&
           is_vec_i32(static_i32_vec_sig->parameters[0].type) &&
           static_i32_vec_sig->parameters[0].mode == hir::ParamMode::move &&
           static_vec_vec_sig->parameters.size() == 1 &&
           static_i32_vec_sig->parameters[0].type ==
               static_vec_vec_sig->parameters[0].type &&
           static_i32_vec_sig->result == static_vec_vec_sig->result,
       "same-signature static combinations did not remain independent");
  need(static_i32_vec->symbol != static_vec_vec->symbol &&
           static_i32_vec->type_args != static_vec_vec->type_args,
       "static same-signature combinations were deduplicated by signature");
  need(mix_i32_vec_sig->parameters.size() == 3 &&
           is_box_i32(mix_i32_vec_sig->parameters[0].type) &&
           is_i32(mix_i32_vec_sig->parameters[1].type) &&
           mix_i32_vec_sig->parameters[1].mode == hir::ParamMode::move &&
           is_vec_i32(mix_i32_vec_sig->parameters[2].type) &&
           mix_i32_vec_sig->parameters[2].mode == hir::ParamMode::move &&
           is_vec_i32(mix_i32_vec_sig->result),
       "Box mix [i32,Vec] left/right signature mismatch");
  need(mix_vec_i32_sig->parameters.size() == 3 &&
           is_box_vec(mix_vec_i32_sig->parameters[0].type) &&
           is_vec_i32(mix_vec_i32_sig->parameters[1].type) &&
           is_i32(mix_vec_i32_sig->parameters[2].type) &&
           is_i32(mix_vec_i32_sig->result),
       "Box mix [Vec,i32] Self/left/right signature mismatch");
  need(same_name_sig->parameters.size() == 2 &&
           is_box_i32(same_name_sig->parameters[0].type) &&
           is_vec_i32(same_name_sig->parameters[1].type) &&
           same_name_sig->result == same_name_sig->parameters[1].type,
       "same-named own T changed Self or the move data type");
  need(static_self_sig->parameters.size() == 2 &&
           is_box_i32(static_self_sig->parameters[0].type) &&
           static_self_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_box_i32(static_self_sig->parameters[1].type) &&
           static_self_sig->parameters[1].mode == hir::ParamMode::move &&
           is_i32(static_self_sig->result),
       "static own U=Self was misread as a hidden receiver");
  need(pair_second_sig->parameters.size() == 4 &&
           is_pair_i32_vec(pair_second_sig->parameters[0].type) &&
           is_i32(pair_second_sig->parameters[1].type) &&
           is_vec_i32(pair_second_sig->parameters[2].type) &&
           is_box_i32(pair_second_sig->parameters[3].type) &&
           is_box_i32(pair_second_sig->result),
       "Pair Self/A/B/U signature order mismatch");
  need(pair_take_sig->parameters.size() == 3 &&
           is_pair_i32_vec(pair_take_sig->parameters[0].type) &&
           is_vec_i32(pair_take_sig->parameters[1].type) &&
           is_i32(pair_take_sig->parameters[2].type) &&
           is_i32(pair_take_sig->result),
       "Pair U/V signature order mismatch");
  need(maybe_sig->parameters.size() == 1 &&
           is_vec_i32(maybe_sig->parameters[0].type) &&
           maybe_sig->parameters[0].mode == hir::ParamMode::move &&
           maybe_sig->result == maybe_sig->parameters[0].type,
       "generic enum Maybe[T].take[U] static signature mismatch");

  auto function_of = [&](const hir::FunctionInstance *i) {
    return function_for(i->symbol);
  };
  const auto *take_i32_vec_function = function_of(take_i32_vec);
  const auto *mix_i32_vec_function = function_of(mix_i32_vec);
  const auto *same_name_function = function_of(same_name_instances[0]);
  const auto *pair_second_function = function_of(pair_second_instances[0]);
  const auto *pair_take_function = function_of(pair_take_instances[0]);
  const auto *maybe_function = function_of(maybe_i32_vec);
  auto only_move_binding = [&](const hir::HirFunction *f,
                               SymbolId expected) {
    size_t count = 0;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == f->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          need(*mv->binding == expected,
               "concrete instance Move kept a template or wrong binding");
          ++count;
        }
    return count;
  };
  need(only_move_binding(take_i32_vec_function,
                         take_i32_vec_function->parameters[1].symbol) == 1,
       "Box take concrete Move binding missing");
  need(only_move_binding(mix_i32_vec_function,
                         mix_i32_vec_function->parameters[2].symbol) == 1,
       "Box mix concrete Move binding missing");
  need(only_move_binding(same_name_function,
                         same_name_function->parameters[1].symbol) == 1,
       "same-named own T concrete Move binding missing");
  need(only_move_binding(pair_second_function,
                         pair_second_function->parameters[3].symbol) == 1,
       "Pair move_second concrete Move binding missing");
  need(only_move_binding(pair_take_function,
                         pair_take_function->parameters[2].symbol) == 1,
       "Pair pair_take concrete Move binding missing");
  need(only_move_binding(maybe_function,
                         maybe_function->parameters[0].symbol) == 1,
       "Maybe take concrete Move binding missing");

  std::map<uint64_t, SymbolId> take_renamed;
  for (auto [from, to] : take_i32_vec->renamed)
    take_renamed[from.value] = to;
  need(take_renamed.at(box_take->function.symbol.value) ==
               take_i32_vec_function->symbol &&
           take_renamed.at(box_take->function.parameters[0].symbol.value) ==
               take_i32_vec_function->parameters[0].symbol &&
           take_renamed.at(box_take->function.parameters[1].symbol.value) ==
               take_i32_vec_function->parameters[1].symbol,
       "Box take renamed map lost Self or own move parameter");
  std::map<uint64_t, SymbolId> pair_renamed;
  for (auto [from, to] : pair_take_instances[0]->renamed)
    pair_renamed[from.value] = to;
  need(pair_renamed.at(pair_take->function.symbol.value) ==
               pair_take_function->symbol &&
           pair_renamed.at(pair_take->function.parameters[0].symbol.value) ==
               pair_take_function->parameters[0].symbol &&
           pair_renamed.at(pair_take->function.parameters[1].symbol.value) ==
               pair_take_function->parameters[1].symbol &&
           pair_renamed.at(pair_take->function.parameters[2].symbol.value) ==
               pair_take_function->parameters[2].symbol,
       "Pair pair_take renamed map lost a concrete parameter");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &f : m.functions) {
    auto sig = m.types.find(f.signature);
    need(sig && !has_parameter(f.signature),
         "concrete generic impl own-method signature retained a parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete generic impl own-method node retained a parameter");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete generic impl own-method node retained a constraint call");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete generic impl own-method call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete generic impl own-method call targets a template");
      }
    }
  }

  invalid_recomputed(m, "generic impl own-method instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == take_i32_vec->symbol) {
        i.origin = maybe_take->function.symbol;
        return;
      }
    throw std::runtime_error("generic impl own-method instance missing");
  });
  invalid_recomputed(m, "generic impl own-method full key order", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == pair_take_instances[0]->symbol) {
        std::swap(i.type_args[2], i.type_args[3]);
        return;
      }
    throw std::runtime_error("generic impl own-method pair instance missing");
  });
  invalid_recomputed(m, "generic impl own-method Self substitution",
                     [&](auto &x) {
    auto template_sig = *x.types.find(box_take->function.signature);
    for (auto &f : x.functions)
      if (f.symbol == take_i32_vec_function->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].type = template_sig.parameters[1].type;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("generic impl own-method take instance missing");
  });
  invalid(m, "generic impl own-method Move rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == take_i32_vec_function->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          *mv->binding = box_take->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("generic impl own-method concrete Move missing");
  });

  const std::string box =
      "struct Box[T]{value T;items Vec[i32];marker i32;} ";
  good("trait V{} impl V for i32{} " + box +
       "impl[T] Box[T]{"
       "func take[U V](self Self,value move U)U{return move value;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;let r=b.take(move x);return r-1;}");
  good("trait V{} impl V for i32{} " + box +
       "impl[T V] Box[T]{"
       "func take[U](self Self,value move U)U{return move value;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;let r=b.take(move x);return r-1;}");
  good("trait V{func take(self Self,value move Vec[i32])Vec[i32];} " +
       box +
       "impl[T] V for Box[T]{"
       "func take(self Self,value move Vec[i32])Vec[i32]{"
       "return move value;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var v Vec[i32]=[2];let r=b.take(move v);return r[0]-2;}");
  good(box + "impl[T] Box[T]{"
       "func bump[U](x inout i32){x+=1;}} "
       "func main()i32{var x i32=1;"
       "Box[i32].bump[i32](inout x);return x-2;}");
  good(box + "impl[T] Box[T]{"
            "func bump[U](self Self,x inout U){x=x;}} "
            "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
            "var x i32=1;"
            "Box[i32].bump[i32](b,inout x);return x-2;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take[U](self move Self,value move U)U{"
                    "return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=b.take(move x);return r-1;}");
  good(box + "impl[T] Box[T]{"
            "func take[U](self move Self,value move U)U{"
            "return move value;}} "
            "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
            "var x i32=1;let r=Box[i32].take(move b,move x);"
            "return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take[U](self inout Self,value move U)U{"
                    "return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=b.take(move x);return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take[U](self inout Self,value move U)U{"
                    "return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=Box[i32].take(inout b,move x);"
                    "return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take[U](self Self,value move U)U{return move value;}} "
                    "func main()i32{let b=Box[Vec[i32]](value:[1],items:[2],"
                    "marker:0);let r=b.take(move b.items);return r[0];}");
  good(box + "impl[T] Box[T]{"
            "func take[U](value move U)U{return move value;}} "
            "func main()i32{let r=Box[i32].take(move \"x\");return 0;}");
  bad(box + "impl[T] Box[T]{"
           "func also[U](self Self,value move U)i32{return self.marker;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "return b.also(move b);}",
      "E_CALL_ARGUMENT");
  bad(box + "func pass[T](value move T)T{return move value;} "
           "impl[T] Box[T]{"
           "func also[U](self Self,value move U)i32{return self.marker;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "return b.also(pass(move b));}",
      "E_CALL_ARGUMENT");
  bad(box + "impl[T] Box[T]{"
           "func take[U](self Self,value move U)U{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "var x i32=1;let r=b.take(x);return r-1;}",
      "E_CALL_ARGUMENT");
  bad(box + "impl[T] Box[T]{"
           "func both[U](self Self,first move U,second move U)i32{return 0;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "var x i32=1;return b.both(move x,move x);}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(box + "impl[T] Box[T]{"
           "func take[U](self Self,value move U)U{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "var x i32=1;let r=b.take(move x);return x-1;}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(box + "impl[T] Box[T]{"
           "func take[U](self Self,value move U)U{return move value;}} "
           "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
           "let f=b.take;return 0;}",
      "E_TYPE_MISMATCH");

  good("func bump(x inout i32){x+=1;} " + box +
       "impl[T] Box[T]{"
       "func keep[U](self Self,value move U,cb func(inout i32))U{"
       "return move value;}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var v Vec[i32]=[2];let r=b.keep(move v,bump);return r[0]-2;}");
  unsupported("func bump(x inout Vec[i32]){} " + box +
              "impl[T] Box[T]{"
              "func keep[U](self Self,value move U,"
              "cb func(inout Vec[i32]))U{return move value;}} "
              "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
              "var v Vec[i32]=[2];let r=b.keep(move v,bump);return r[0]-2;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func keep[U](value move U,"
                    "cb func() (func(inout Vec[i32])))U{"
                    "return move value;}} "
                    "func main()i32{return 0;}");

  good("enum Tag[T]{A(value T);} impl[T] Tag[T]{"
       "func A[U](value move U)U{return move value;}} "
       "func main()i32{let v=Tag[i32].A(1);"
       "switch v{case Tag.A(x):return x-1;}}");
  bad("enum Tag[T]{A(value T);} impl[T] Tag[T]{"
      "func A[U](value move U)U{return move value;}} "
      "func main()i32{let v=Tag[i32].A[bool](1);return 0;}",
      "E_CALL_ARGUMENT");

  auto foreign_check = [&](const std::string &text) {
    Temp t;
    fs::copy_file(root / "contracts/python_bindings.example.json",
                  t.path / "binding.json");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"main\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"python\"\n"
          "[dependencies.numpy]\nkind=\"python\"\nbinding=\"binding.json\"\n"
          "host_package=\"numpy\"\n");
    write(t.path / "src/main.lang", "package app; " + text);
    SourceManager sm;
    auto loaded = modules::load_project(
        {t.path / "lang.toml", {}, t.path / "binding.json"}, sm);
    need(loaded.success(), messages(loaded));
    auto resolved = modules::resolve_project(
        std::make_shared<const modules::Project>(*loaded.value));
    need(resolved.success(), messages(resolved));
    return sema::check_types(resolved);
  };
  auto foreign = foreign_check(
      "import \"std/py\" as py; import \"numpy\" as np; "
      "struct Box[T]{value T;} impl[T] Box[T]{"
      "func take[U](self Self,value move U)U{return move value;}} "
      "func use[T](value T)T{let local=value;"
      "let b=Box[i32](value:1);return b.take(move local);} "
      "func make() np.ndarray throws py.Error{return np.linspace(-1.0,1.0);} "
      "func main()i32 throws py.Error{let r=use(make());return 0;}");
  need(!foreign.success() && foreign.failure == FailureKind::unsupported &&
           has(foreign, "E_CAPABILITY_UNAVAILABLE"),
       "foreign generic impl own-method move was not rejected at its "
       "concrete instance\n" +
           messages(foreign));
}

void constrained_inherent_move(const fs::path &root) {
  auto positive =
      check(root /
            "tests/generics/method-move/constrained-inherent/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(),
       "constrained inherent move HIR rejected");
  const auto &m = *positive.value;

  auto symbol_name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return symbol_name(t->declaration);
  };
  auto is_nominal = [&](TypeId id, const std::string &name) {
    return !nominal_name(id).empty() && nominal_name(id) == name;
  };
  auto is_item = [&](TypeId id) { return is_nominal(id, "Item"); };
  auto is_bag = [&](TypeId id) { return is_nominal(id, "Bag"); };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_box_of = [&](TypeId id, const auto &element) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Box" && t->type_args.size() == 1 &&
           element(t->type_args[0]);
  };
  auto has_parameter = [&](TypeId root_id) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root_id);
  };
  auto find_impl = [&](const std::string &name) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (!im.trait && nominal_name(im.self_type) == name)
        return &im;
    return nullptr;
  };
  auto template_for = [&](SymbolId method) -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (t.function.symbol == method)
        return &t;
    return nullptr;
  };
  auto method_template = [&](const hir::ImplDecl &im,
                             const std::string &name)
      -> const hir::FunctionTemplate * {
    for (auto method : im.methods)
      if (symbol_name(method) == name)
        return template_for(method);
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto signature_for = [&](const hir::FunctionInstance *i)
      -> const hir::Type * {
    auto f = function_for(i->symbol);
    return f ? m.types.find(f->signature) : nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto find_trait = [&](const std::string &name) -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (symbol_name(t.symbol) == name)
        return &t;
    return nullptr;
  };
  auto trait_method = [&](const hir::TraitDecl &trait,
                          const std::string &type,
                          const std::string &method) -> SymbolId {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait.symbol && nominal_name(im.self_type) == type)
        for (auto candidate : im.methods)
          if (symbol_name(candidate) == method)
            return candidate;
    throw std::runtime_error("trait impl method missing: " + type + "." +
                             method);
  };
  auto prefix_ok = [&](const hir::FunctionTemplate *t,
                       const hir::ImplDecl &im, size_t suffix) {
    if (!t || t->params.size() != im.params.size() + suffix)
      return false;
    for (size_t i = 0; i < im.params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.symbol != im.params[i].symbol ||
          p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte >= t->function.source.span.begin_byte)
        return false;
    }
    for (size_t i = im.params.size(); i < t->params.size(); ++i) {
      const auto &p = t->params[i];
      if (p.source.span.snapshot != t->function.source.span.snapshot ||
          p.source.span.begin_byte < t->function.source.span.begin_byte ||
          p.source.span.end_byte > t->function.source.span.end_byte)
        return false;
    }
    return true;
  };

  const auto *box_impl = find_impl("Box");
  const auto *maybe_impl = find_impl("Maybe");
  need(box_impl && maybe_impl,
       "constrained inherent move impl declarations missing");
  need(box_impl->params.size() == 1 && box_impl->params[0].trait &&
           symbol_name(box_impl->params[0].trait->declaration) == "Value",
       "Box impl layer lost its Value constraint");
  const auto *box_take = method_template(*box_impl, "take");
  const auto *box_static = method_template(*box_impl, "take_static");
  const auto *box_mix = method_template(*box_impl, "mix");
  const auto *box_phantom = method_template(*box_impl, "phantom");
  const auto *box_left = method_template(*box_impl, "pick_left");
  const auto *box_right = method_template(*box_impl, "pick_right");
  const auto *box_same = method_template(*box_impl, "same_name");
  const auto *box_keep = method_template(*box_impl, "keep_self");
  const auto *box_static_self = method_template(*box_impl, "static_self");
  const auto *box_ordered = method_template(*box_impl, "ordered");
  const auto *maybe_take = method_template(*maybe_impl, "take");
  need(box_take && box_static && box_mix && box_phantom && box_left &&
           box_right && box_same && box_keep && box_static_self &&
           box_ordered && maybe_take,
       "constrained inherent move templates missing");
  need(prefix_ok(box_take, *box_impl, 1) &&
           prefix_ok(box_static, *box_impl, 1) &&
           prefix_ok(box_mix, *box_impl, 1) &&
           prefix_ok(box_phantom, *box_impl, 2) &&
           prefix_ok(box_left, *box_impl, 1) &&
           prefix_ok(box_right, *box_impl, 1) &&
           prefix_ok(box_same, *box_impl, 1) &&
           prefix_ok(box_keep, *box_impl, 1) &&
           prefix_ok(box_static_self, *box_impl, 1) &&
           prefix_ok(box_ordered, *box_impl, 1) &&
           prefix_ok(maybe_take, *maybe_impl, 1),
       "constrained inherent move impl prefix or own suffix changed");
  need(box_take->params[1].trait &&
           symbol_name(box_take->params[1].trait->declaration) == "Value" &&
           box_phantom->params[1].trait && box_phantom->params[2].trait &&
           box_left->params[1].trait && box_right->params[1].trait,
       "method own layer lost a declared constraint");
  need(box_same->params.size() == 2 &&
           box_same->params[0].symbol == box_impl->params[0].symbol &&
           box_same->params[1].symbol != box_same->params[0].symbol &&
           symbol_name(box_same->params[0].symbol) == "T" &&
           symbol_name(box_same->params[1].symbol) == "T",
       "same-named method T was merged with the impl T");

  const auto take_instances = instances_of(box_take);
  need(take_instances.size() == 3,
       "constrained Box take instance combinations were not exact");
  const hir::FunctionInstance *take_item_item = nullptr,
                             *take_item_bag = nullptr,
                             *take_bag_item = nullptr;
  for (const auto *i : take_instances) {
    need(i->type_args.size() == 2, "Box take instance arity");
    if (is_item(i->type_args[0]) && is_item(i->type_args[1]))
      take_item_item = i;
    else if (is_item(i->type_args[0]) && is_bag(i->type_args[1]))
      take_item_bag = i;
    else if (is_bag(i->type_args[0]) && is_item(i->type_args[1]))
      take_bag_item = i;
    else
      throw std::runtime_error("unexpected Box take type combination");
  }
  need(take_item_item && take_item_bag && take_bag_item &&
           take_item_item->symbol != take_item_bag->symbol &&
           take_item_item->symbol != take_bag_item->symbol &&
           take_item_bag->symbol != take_bag_item->symbol,
       "Box take impl/own layer changes shared an instance");

  const auto static_instances = instances_of(box_static);
  need(static_instances.size() == 2,
       "constrained take_static combinations were not deduplicated");
  const hir::FunctionInstance *static_item_bag = nullptr,
                             *static_item_item = nullptr;
  for (const auto *i : static_instances) {
    need(i->type_args.size() == 2, "Box take_static instance arity");
    if (is_item(i->type_args[0]) && is_bag(i->type_args[1]))
      static_item_bag = i;
    else if (is_item(i->type_args[0]) && is_item(i->type_args[1]))
      static_item_item = i;
    else
      throw std::runtime_error("unexpected take_static combination");
  }
  need(static_item_bag && static_item_item,
       "constrained take_static explicit/inferred instances missing");

  const auto phantom_instances = instances_of(box_phantom);
  need(phantom_instances.size() == 2,
       "phantom params were merged by identical concrete signatures");
  const hir::FunctionInstance *phantom_item = nullptr,
                             *phantom_bag = nullptr;
  for (const auto *i : phantom_instances) {
    need(i->type_args.size() == 3 && is_item(i->type_args[0]) &&
             is_bag(i->type_args[1]),
         "phantom instance complete key changed");
    if (is_item(i->type_args[2]))
      phantom_item = i;
    else if (is_bag(i->type_args[2]))
      phantom_bag = i;
  }
  need(phantom_item && phantom_bag &&
           phantom_item->symbol != phantom_bag->symbol &&
           phantom_item->type_args != phantom_bag->type_args,
       "same-signature phantom combinations collapsed");

  const auto mix_instances = instances_of(box_mix);
  const auto keep_instances = instances_of(box_keep);
  const auto same_instances = instances_of(box_same);
  const auto static_self_instances = instances_of(box_static_self);
  const auto left_instances = instances_of(box_left);
  const auto right_instances = instances_of(box_right);
  const auto ordered_instances = instances_of(box_ordered);
  const auto maybe_instances = instances_of(maybe_take);
  need(mix_instances.size() == 1 && keep_instances.size() == 1 &&
           same_instances.size() == 1 &&
           static_self_instances.size() == 1 &&
           left_instances.size() == 1 && right_instances.size() == 1 &&
           ordered_instances.size() == 1 && maybe_instances.size() == 1,
       "constrained inherent move instance counts changed");
  need(mix_instances[0]->type_args.size() == 2 &&
           is_item(mix_instances[0]->type_args[0]) &&
           is_bag(mix_instances[0]->type_args[1]),
       "Box mix impl/own key order changed");
  need(keep_instances[0]->type_args.size() == 2 &&
           is_item(keep_instances[0]->type_args[0]) &&
           is_bag(keep_instances[0]->type_args[1]),
       "Box keep_self impl/own key order changed");
  need(same_instances[0]->type_args.size() == 2 &&
           is_item(same_instances[0]->type_args[0]) &&
           is_bag(same_instances[0]->type_args[1]),
       "same-named own T replaced or reordered the impl key");
  need(static_self_instances[0]->type_args.size() == 2 &&
           is_item(static_self_instances[0]->type_args[0]) &&
           is_box_of(static_self_instances[0]->type_args[1], is_item),
       "static own U=Self changed the impl/own key");
  need(maybe_instances[0]->type_args.size() == 2 &&
           is_i32(maybe_instances[0]->type_args[0]) &&
           is_item(maybe_instances[0]->type_args[1]),
       "generic enum Maybe take key changed");

  auto take_item_bag_sig = signature_for(take_item_bag);
  auto take_bag_item_sig = signature_for(take_bag_item);
  auto static_bag_sig = signature_for(static_item_bag);
  auto static_item_sig = signature_for(static_item_item);
  auto phantom_item_sig = signature_for(phantom_item);
  auto phantom_bag_sig = signature_for(phantom_bag);
  auto mix_sig = signature_for(mix_instances[0]);
  auto keep_sig = signature_for(keep_instances[0]);
  auto same_sig = signature_for(same_instances[0]);
  auto static_self_sig = signature_for(static_self_instances[0]);
  auto ordered_sig = signature_for(ordered_instances[0]);
  need(take_item_bag_sig && take_bag_item_sig && static_bag_sig &&
           static_item_sig && phantom_item_sig && phantom_bag_sig &&
           mix_sig && keep_sig && same_sig && static_self_sig &&
           ordered_sig,
       "constrained inherent move signatures missing");
  need(take_item_bag_sig->parameters.size() == 2 &&
           is_box_of(take_item_bag_sig->parameters[0].type, is_item) &&
           take_item_bag_sig->parameters[0].mode ==
               hir::ParamMode::ordinary &&
           is_bag(take_item_bag_sig->parameters[1].type) &&
           take_item_bag_sig->parameters[1].mode == hir::ParamMode::move &&
           is_bag(take_item_bag_sig->result),
       "Box take [Item,Bag] Self/own signature mismatch");
  need(take_bag_item_sig->parameters.size() == 2 &&
           is_box_of(take_bag_item_sig->parameters[0].type, is_bag) &&
           is_item(take_bag_item_sig->parameters[1].type) &&
           take_bag_item_sig->parameters[1].mode == hir::ParamMode::move &&
           is_item(take_bag_item_sig->result),
       "Self followed the own U instead of the impl T");
  need(static_bag_sig->parameters.size() == 1 &&
           is_bag(static_bag_sig->parameters[0].type) &&
           static_bag_sig->parameters[0].mode == hir::ParamMode::move &&
           is_bag(static_bag_sig->result) &&
           static_item_sig->parameters.size() == 1 &&
           is_item(static_item_sig->parameters[0].type) &&
           static_item_sig->parameters[0].mode == hir::ParamMode::move &&
           is_item(static_item_sig->result),
       "constrained static signatures lost their concrete move types");
  need(phantom_item_sig->parameters.size() == 2 &&
           phantom_item_sig->parameters[0].type ==
               phantom_bag_sig->parameters[0].type &&
           phantom_item_sig->parameters[1].type ==
               phantom_bag_sig->parameters[1].type &&
           phantom_item_sig->result == phantom_bag_sig->result,
       "phantom instances did not have the same concrete signature");
  need(mix_sig->parameters.size() == 3 &&
           is_box_of(mix_sig->parameters[0].type, is_item) &&
           is_item(mix_sig->parameters[1].type) &&
           mix_sig->parameters[1].mode == hir::ParamMode::move &&
           is_bag(mix_sig->parameters[2].type) &&
           is_bag(mix_sig->result),
       "Box mix T/U owning signature changed");
  need(keep_sig->parameters.size() == 2 &&
           is_box_of(keep_sig->parameters[0].type, is_item) &&
           is_bag(keep_sig->parameters[1].type) &&
           is_box_of(keep_sig->result, is_item),
       "keep_self result Self did not follow only the impl type");
  need(same_sig->parameters.size() == 2 &&
           is_box_of(same_sig->parameters[0].type, is_item) &&
           is_bag(same_sig->parameters[1].type) &&
           is_i32(same_sig->result),
       "same-named own T changed Self or the move data type");
  need(static_self_sig->parameters.size() == 2 &&
           is_box_of(static_self_sig->parameters[0].type, is_item) &&
           static_self_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_box_of(static_self_sig->parameters[1].type, is_item) &&
           static_self_sig->parameters[1].mode == hir::ParamMode::move &&
           is_i32(static_self_sig->result),
       "static own U=Self was reclassified as a receiver");
  need(ordered_sig->parameters.size() == 4 &&
           is_box_of(ordered_sig->parameters[0].type, is_item) &&
           is_i32(ordered_sig->parameters[1].type) &&
           is_bag(ordered_sig->parameters[2].type) &&
           ordered_sig->parameters[2].mode == hir::ParamMode::move &&
           is_i32(ordered_sig->parameters[3].type) &&
           is_bag(ordered_sig->result),
       "ordered source-order signature changed");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &f : m.functions) {
    auto sig = m.types.find(f.signature);
    need(sig && !has_parameter(f.signature),
         "concrete constrained move signature retained a parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete constrained move node retained a parameter");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete constrained move retained ConstraintCall");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete constrained move call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete constrained move call targets a template");
      }
    }
  }

  const auto *value_trait = find_trait("Value");
  const auto *left_trait = find_trait("Left");
  const auto *right_trait = find_trait("Right");
  need(value_trait && left_trait && right_trait,
       "constrained inherent move traits missing");
  auto item_value_get = trait_method(*value_trait, "Item", "get");
  auto bag_value_get = trait_method(*value_trait, "Bag", "get");
  auto item_left_pick = trait_method(*left_trait, "Item", "pick");
  auto item_right_pick = trait_method(*right_trait, "Item", "pick");
  need(item_value_get != bag_value_get &&
           item_left_pick != item_right_pick,
       "same-named trait methods shared a symbol");

  auto calls_named = [&](SymbolId function, SymbolId target) {
    size_t count = 0;
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && *c->direct_symbol == target)
        ++count;
    }
    return count;
  };
  const auto *take_item_bag_function = function_for(take_item_bag->symbol);
  const auto *same_function = function_for(same_instances[0]->symbol);
  const auto *left_function = function_for(left_instances[0]->symbol);
  const auto *right_function = function_for(right_instances[0]->symbol);
  need(take_item_bag_function && same_function && left_function &&
           right_function,
       "constrained move concrete functions missing");
  need(calls_named(take_item_bag_function->symbol, item_value_get) == 1,
       "take did not resolve the impl-layer Value constraint to Item.get");
  need(calls_named(same_function->symbol, item_value_get) == 1 &&
           calls_named(same_function->symbol, bag_value_get) == 1,
       "same-named T did not resolve both distinct get implementations");
  need(calls_named(left_function->symbol, item_left_pick) == 1 &&
           calls_named(left_function->symbol, item_right_pick) == 0,
       "Left constraint did not select Item.Left.pick");
  need(calls_named(right_function->symbol, item_right_pick) == 1 &&
           calls_named(right_function->symbol, item_left_pick) == 0,
       "Right constraint did not select Item.Right.pick");

  size_t take_template_constraints = 0, same_template_constraints = 0;
  for (const auto &n : m.nodes) {
    if (m.scopes[n.scope.value].function == box_take->function.symbol)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(c->trait.declaration == value_trait->symbol,
             "take template constraint trait changed");
        ++take_template_constraints;
      }
    if (m.scopes[n.scope.value].function == box_same->function.symbol)
      if (std::holds_alternative<hir::ConstraintCall>(n.data))
        ++same_template_constraints;
  }
  need(take_template_constraints == 1 && same_template_constraints == 2,
       "constrained method templates lost ordinary ConstraintCall nodes");

  auto only_move_binding = [&](const hir::HirFunction *f, SymbolId expected) {
    size_t count = 0;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == f->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          need(*mv->binding == expected,
               "concrete Move kept a template or wrong binding");
          ++count;
        }
    return count;
  };
  need(only_move_binding(take_item_bag_function,
                         take_item_bag_function->parameters[1].symbol) == 1,
       "take concrete Move binding missing");

  std::map<uint64_t, SymbolId> take_renamed;
  for (auto [from, to] : take_item_bag->renamed)
    take_renamed[from.value] = to;
  need(take_renamed.at(box_take->function.symbol.value) ==
               take_item_bag_function->symbol &&
           take_renamed.at(box_take->function.parameters[0].symbol.value) ==
               take_item_bag_function->parameters[0].symbol &&
           take_renamed.at(box_take->function.parameters[1].symbol.value) ==
               take_item_bag_function->parameters[1].symbol,
       "take renamed map lost Self or its own move parameter");
  std::optional<SymbolId> template_local;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == box_take->function.symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        template_local = b->symbol;
        break;
      }
  need(template_local && take_renamed.count(template_local->value),
       "take template local was not renamed in its instance");
  need(take_renamed.at(template_local->value) !=
               *template_local,
       "take instance local kept the template SymbolId");

  auto count_instance_calls = [&](SymbolId target) {
    size_t count = 0;
    for (const auto &n : m.nodes) {
      auto c = std::get_if<hir::Call>(&n.data);
      if (c && c->direct_symbol && *c->direct_symbol == target)
        ++count;
    }
    return count;
  };
  need(count_instance_calls(take_item_bag->symbol) == 2,
       "same impl/own combination did not reuse one take instance");
  need(count_instance_calls(take_item_item->symbol) == 1 &&
           count_instance_calls(take_bag_item->symbol) == 1,
       "take call-to-instance mapping changed");

  invalid_recomputed(m, "constrained move instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == take_item_bag->symbol) {
        i.origin = maybe_take->function.symbol;
        return;
      }
    throw std::runtime_error("constrained move instance missing");
  });
  invalid_recomputed(m, "constrained move full key order", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == take_item_bag->symbol) {
        std::swap(i.type_args[0], i.type_args[1]);
        return;
      }
    throw std::runtime_error("constrained move instance missing");
  });
  invalid_recomputed(m, "constrained move Self substitution", [&](auto &x) {
    auto template_sig = *x.types.find(box_take->function.signature);
    for (auto &f : x.functions)
      if (f.symbol == take_item_bag_function->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].type = template_sig.parameters[1].type;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("constrained move take instance missing");
  });
  invalid(m, "constrained move Move rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == take_item_bag_function->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          *mv->binding = box_take->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("constrained move concrete Move missing");
  });
  invalid(m, "constrained move local rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == take_item_bag_function->symbol)
        if (auto b = std::get_if<hir::Binding>(&n.data); b && template_local) {
          b->symbol = *template_local;
          return;
        }
    throw std::runtime_error("constrained move concrete local missing");
  });

  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "constrained inherent move source HIR rejected");
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  const std::string box = "struct Box[T]{value T;items Vec[i32];marker i32;} ";
  bad("trait V{} struct S{x i32;} " + box +
          "impl[T V] Box[T]{func take[U V](self Self,value move U)U{"
          "return move value;}} "
          "func main()i32{let b=Box[S](value:S(x:1),items:[1],marker:0);"
          "var x S=S(x:2);let r=b.take(move x);return r.x;}",
      "E_TYPE_MISMATCH");
  bad("trait V{} impl V for i32{} struct S{x i32;} " + box +
          "impl[T V] Box[T]{func take[U V](self Self,value move U)U{"
          "return move value;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "var x S=S(x:2);let r=b.take(move x);return r.x;}",
      "E_TYPE_MISMATCH");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func take[U V](self move Self,value move U)U{"
              "return move value;}} "
              "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
              "var x i32=1;let r=b.take(move x);return r-1;}");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func take[U V](self inout Self,value move U)U{"
              "return move value;}} "
              "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
              "var x i32=1;let r=Box[i32].take(inout b,move x);return r-1;}");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func take[U V](self Self,value move U)U{"
          "return move value;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "var x i32=1;let f=b.take;return 0;}",
      "E_TYPE_MISMATCH");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func take[U V](self Self,value move U)U{"
          "return move value;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "var x i32=1;let r=b.take(x);return r-1;}",
      "E_CALL_ARGUMENT");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func take[U V](self Self,value move U)U{"
          "return move value;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "var x i32=1;let r=b.take(move x);return x-1;}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func take_items(self Self,"
              "value move Vec[i32])i32{return value[0];}} "
              "func main()i32{let b=Box[i32](value:1,items:[2],"
              "marker:0);let r=b.take_items(move b.items);return r;}");
  good(std::string("func bump(x inout i32){x+=1;} trait V{} "
                    "struct Item{items Vec[i32];} impl V for Item{} ") +
       "struct H{items Vec[i32];} impl H{"
       "func keep[U V](self Self,value move U,cb func(inout i32))U{"
       "return move value;}} "
       "func main()i32{let h=H(items:[1]);var v Item=Item(items:[2]);"
       "let r=h.keep(move v,bump);return r.items[0]-2;}");
  unsupported(std::string("func bump(x inout Vec[i32]){} trait V{} "
                           "struct Item{items Vec[i32];} impl V for Item{} ") +
              "struct H{items Vec[i32];} impl H{"
              "func keep[U V](self Self,value move U,"
              "cb func(inout Vec[i32]))U{return move value;}} "
              "func main()i32{let h=H(items:[1]);var v Item=Item(items:[2]);"
              "let r=h.keep(move v,bump);return r.items[0]-2;}");
}

void constrained_inherent_inout(const fs::path &root) {
  auto positive =
      check(root /
            "tests/generics/method-inout/constrained-inherent/lang.toml");
  need(positive.success(), messages(positive));
  need(hir::verify(*positive.value).success(),
       "constrained inherent inout HIR rejected");
  const auto &m = *positive.value;

  auto symbol_name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return symbol_name(t->declaration);
  };
  auto is_nominal = [&](TypeId id, const std::string &name) {
    return !nominal_name(id).empty() && nominal_name(id) == name;
  };
  auto is_item = [&](TypeId id) { return is_nominal(id, "Item"); };
  auto is_i32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_i64 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 64;
  };
  auto is_u32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::UnsignedInt && t->bits == 32;
  };
  auto is_u64 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::UnsignedInt && t->bits == 64;
  };
  auto is_f32 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Float && t->bits == 32;
  };
  auto is_f64 = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Float && t->bits == 64;
  };
  auto is_bool = [&](TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Bool;
  };
  auto is_box_of = [&](TypeId id, const auto &element) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::NominalStruct &&
           nominal_name(id) == "Box" && t->type_args.size() == 1 &&
           element(t->type_args[0]);
  };
  auto has_parameter = [&](TypeId root_id) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root_id);
  };
  auto find_impl = [&](const std::string &name) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (!im.trait && nominal_name(im.self_type) == name)
        return &im;
    return nullptr;
  };
  auto template_for = [&](SymbolId method) -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (t.function.symbol == method)
        return &t;
    return nullptr;
  };
  auto method_template = [&](const hir::ImplDecl &im,
                             const std::string &name)
      -> const hir::FunctionTemplate * {
    for (auto method : im.methods)
      if (symbol_name(method) == name)
        return template_for(method);
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto signature_for = [&](const hir::FunctionInstance *i)
      -> const hir::Type * {
    auto f = function_for(i->symbol);
    return f ? m.types.find(f->signature) : nullptr;
  };
  auto instances_of = [&](const hir::FunctionTemplate *t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t->function.symbol)
        result.push_back(&i);
    return result;
  };
  auto find_trait = [&](const std::string &name) -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (symbol_name(t.symbol) == name)
        return &t;
    return nullptr;
  };
  auto trait_method = [&](const hir::TraitDecl &trait,
                          const std::string &type,
                          const std::string &method) -> SymbolId {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait.symbol && nominal_name(im.self_type) == type)
        for (auto candidate : im.methods)
          if (symbol_name(candidate) == method)
            return candidate;
    throw std::runtime_error("trait impl method missing: " + type + "." +
                             method);
  };

  const auto *box_impl = find_impl("Box");
  const auto *crate_impl = find_impl("Crate");
  const auto *maybe_impl = find_impl("Maybe");
  need(box_impl && crate_impl && maybe_impl,
       "constrained inherent inout impl declarations missing");
  need(box_impl->params.size() == 1 && box_impl->params[0].trait &&
           symbol_name(box_impl->params[0].trait->declaration) == "Value",
       "Box impl layer lost its Value constraint");
  need(!crate_impl->params.empty() && !crate_impl->params[0].trait,
       "Crate impl unexpectedly gained a constraint");
  const auto *box_bump = method_template(*box_impl, "bump");
  const auto *box_bump_static = method_template(*box_impl, "bump_static");
  const auto *box_replace = method_template(*box_impl, "replace");
  const auto *box_replace_static = method_template(*box_impl, "replace_static");
  const auto *box_pair = method_template(*box_impl, "pair_replace");
  const auto *box_ordered = method_template(*box_impl, "ordered");
  const auto *box_forward = method_template(*box_impl, "forward");
  const auto *box_forward_free = method_template(*box_impl, "forward_free");
  const auto *box_apply = method_template(*box_impl, "apply");
  const auto *box_later = method_template(*box_impl, "later_write");
  const auto *box_left = method_template(*box_impl, "pick_left");
  const auto *box_right = method_template(*box_impl, "pick_right");
  const auto *box_same = method_template(*box_impl, "same_name");
  const auto *box_static_self = method_template(*box_impl, "static_self");
  const auto *box_fail = method_template(*box_impl, "fail_after");
  const auto *box_cleanup = method_template(*box_impl, "cleanup_fail");
  const auto *crate_own = method_template(*crate_impl, "replace_own");
  const auto *crate_own_static =
      method_template(*crate_impl, "replace_own_static");
  const auto *maybe_take = method_template(*maybe_impl, "take_static");
  const auto *maybe_some = method_template(*maybe_impl, "Some");
  const auto *maybe_bump = method_template(*maybe_impl, "bump_static");
  need(box_bump && box_bump_static && box_replace && box_replace_static &&
           box_pair && box_ordered && box_forward && box_forward_free &&
           box_apply && box_later && box_left && box_right && box_same &&
           box_static_self && box_fail && box_cleanup && crate_own &&
           crate_own_static && maybe_take && maybe_some && maybe_bump,
       "constrained inherent inout templates missing");
  need(box_replace->params.size() == 2 && box_replace->params[0].symbol ==
               box_impl->params[0].symbol &&
           box_replace->params[1].symbol != box_replace->params[0].symbol &&
           box_replace->params[1].trait &&
           symbol_name(box_replace->params[1].trait->declaration) == "Value" &&
           box_bump->params.size() == 1 &&
           box_bump->params[0].symbol == box_impl->params[0].symbol &&
           crate_own->params.size() == 2 && crate_own->params[1].trait,
       "constrained inout impl prefix or own suffix changed");
  need(box_same->params.size() == 2 &&
           box_same->params[0].symbol == box_impl->params[0].symbol &&
           box_same->params[1].symbol != box_same->params[0].symbol &&
           symbol_name(box_same->params[0].symbol) == "T" &&
           symbol_name(box_same->params[1].symbol) == "T",
       "same-named method T was merged with the impl T");

  const auto replace_instances = instances_of(box_replace);
  need(replace_instances.size() == 4,
       "constrained Box replace instance combinations were not exact");
  auto replace_prefix = [&](const hir::FunctionInstance *i) {
    return i->type_args.size() == 2 && is_item(i->type_args[0]);
  };
  const hir::FunctionInstance *replace_i32 = nullptr, *replace_i64 = nullptr,
                             *replace_u32 = nullptr, *replace_f64 = nullptr;
  for (const auto *i : replace_instances) {
    need(replace_prefix(i), "replace instance lost its impl key");
    if (is_i32(i->type_args[1]))
      replace_i32 = i;
    else if (is_i64(i->type_args[1]))
      replace_i64 = i;
    else if (is_u32(i->type_args[1]))
      replace_u32 = i;
    else if (is_f64(i->type_args[1]))
      replace_f64 = i;
    else
      throw std::runtime_error("unexpected replace own type");
  }
  need(replace_i32 && replace_i64 && replace_u32 && replace_f64,
       "replace scalar own combinations missing");
  const auto static_instances = instances_of(box_replace_static);
  need(static_instances.size() == 3,
       "replace_static combinations were not deduplicated");
  const hir::FunctionInstance *static_bool = nullptr, *static_f32 = nullptr,
                             *static_u64 = nullptr;
  for (const auto *i : static_instances) {
    need(i->type_args.size() == 2, "replace_static instance arity");
    if (is_bool(i->type_args[0]) && is_bool(i->type_args[1]))
      static_bool = i;
    else if (is_i32(i->type_args[0]) && is_f32(i->type_args[1]))
      static_f32 = i;
    else if (is_i64(i->type_args[0]) && is_u64(i->type_args[1]))
      static_u64 = i;
    else
      throw std::runtime_error("unexpected replace_static combination");
  }
  need(static_bool && static_f32 && static_u64,
       "replace_static explicit/inferred combinations missing");

  const auto bump_instances = instances_of(box_bump);
  const auto bump_static_instances = instances_of(box_bump_static);
  const auto pair_instances = instances_of(box_pair);
  const auto ordered_instances = instances_of(box_ordered);
  const auto forward_instances = instances_of(box_forward);
  const auto forward_free_instances = instances_of(box_forward_free);
  const auto apply_instances = instances_of(box_apply);
  const auto later_instances = instances_of(box_later);
  const auto left_instances = instances_of(box_left);
  const auto right_instances = instances_of(box_right);
  const auto same_instances = instances_of(box_same);
  const auto static_self_instances = instances_of(box_static_self);
  const auto fail_instances = instances_of(box_fail);
  const auto cleanup_instances = instances_of(box_cleanup);
  const auto crate_instances = instances_of(crate_own);
  const auto crate_static_instances = instances_of(crate_own_static);
  const auto maybe_take_instances = instances_of(maybe_take);
  const auto maybe_bump_instances = instances_of(maybe_bump);
  need(bump_instances.size() == 1 && bump_static_instances.size() == 1 &&
           pair_instances.size() == 1 && ordered_instances.size() == 1 &&
           forward_instances.size() == 1 &&
           forward_free_instances.size() == 1 && apply_instances.size() == 1 &&
           later_instances.size() == 1 && left_instances.size() == 1 &&
           right_instances.size() == 1 && same_instances.size() == 1 &&
           static_self_instances.size() == 1 && fail_instances.size() == 1 &&
           cleanup_instances.size() == 1 && crate_instances.size() == 1 &&
           crate_static_instances.size() == 1 &&
           maybe_take_instances.size() == 1 &&
           maybe_bump_instances.size() == 1,
       "constrained inherent inout instance counts changed");
  need(bump_instances[0]->type_args.size() == 1 &&
           is_i32(bump_instances[0]->type_args[0]) &&
           bump_static_instances[0]->type_args.size() == 1 &&
           is_i64(bump_static_instances[0]->type_args[0]),
       "impl-only constrained inout keys changed");
  need(pair_instances[0]->type_args.size() == 3 &&
           is_item(pair_instances[0]->type_args[0]) &&
           is_i32(pair_instances[0]->type_args[1]) &&
           is_bool(pair_instances[0]->type_args[2]),
       "pair_replace impl/own key order changed");
  need(same_instances[0]->type_args.size() == 2 &&
           is_item(same_instances[0]->type_args[0]) &&
           is_i32(same_instances[0]->type_args[1]),
       "same-named own T replaced or reordered the impl key");
  need(static_self_instances[0]->type_args.size() == 2 &&
           is_item(static_self_instances[0]->type_args[0]) &&
           is_box_of(static_self_instances[0]->type_args[1], is_item),
       "static own U=Self changed the impl/own key");
  need(crate_instances[0]->type_args.size() == 2 &&
           is_i32(crate_instances[0]->type_args[0]) &&
           is_i32(crate_instances[0]->type_args[1]) &&
           crate_static_instances[0]->type_args.size() == 2 &&
           is_i32(crate_static_instances[0]->type_args[0]) &&
           is_i64(crate_static_instances[0]->type_args[1]),
       "own-only constrained Crate inout keys changed");
  need(maybe_take_instances[0]->type_args.size() == 2 &&
           is_i32(maybe_take_instances[0]->type_args[0]) &&
           is_bool(maybe_take_instances[0]->type_args[1]) &&
           maybe_bump_instances[0]->type_args.size() == 1 &&
           is_i64(maybe_bump_instances[0]->type_args[0]),
       "generic enum static inout keys changed");

  auto replace_sig = signature_for(replace_i32);
  auto static_bool_sig = signature_for(static_bool);
  auto pair_sig = signature_for(pair_instances[0]);
  auto same_sig = signature_for(same_instances[0]);
  auto static_self_sig = signature_for(static_self_instances[0]);
  auto bump_sig = signature_for(bump_instances[0]);
  need(replace_sig && static_bool_sig && pair_sig && same_sig &&
           static_self_sig && bump_sig,
       "constrained inherent inout signatures missing");
  need(replace_sig->parameters.size() == 3 &&
           is_box_of(replace_sig->parameters[0].type, is_item) &&
           replace_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_i32(replace_sig->parameters[1].type) &&
           replace_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(replace_sig->parameters[2].type) &&
           replace_sig->parameters[2].mode == hir::ParamMode::ordinary &&
           is_i32(replace_sig->result),
       "Box replace Self/own inout signature mismatch");
  need(static_bool_sig->parameters.size() == 2 &&
           is_bool(static_bool_sig->parameters[0].type) &&
           static_bool_sig->parameters[0].mode == hir::ParamMode::inout &&
           is_bool(static_bool_sig->parameters[1].type) &&
           is_bool(static_bool_sig->result),
       "static replace_static constrained signature changed");
  need(pair_sig->parameters.size() == 5 &&
           is_box_of(pair_sig->parameters[0].type, is_item) &&
           is_i32(pair_sig->parameters[1].type) &&
           pair_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(pair_sig->parameters[2].type) &&
           is_bool(pair_sig->parameters[3].type) &&
           pair_sig->parameters[3].mode == hir::ParamMode::inout &&
           is_bool(pair_sig->parameters[4].type),
       "pair_replace impl/own inout order changed");
  need(same_sig->parameters.size() == 3 &&
           is_box_of(same_sig->parameters[0].type, is_item) &&
           is_i32(same_sig->parameters[1].type) &&
           same_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(same_sig->parameters[2].type) &&
           is_i32(same_sig->result),
       "same-named own T changed Self or the inout data type");
  need(static_self_sig->parameters.size() == 3 &&
           is_box_of(static_self_sig->parameters[0].type, is_item) &&
           static_self_sig->parameters[0].mode == hir::ParamMode::ordinary &&
           is_i32(static_self_sig->parameters[1].type) &&
           static_self_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(static_self_sig->parameters[2].type),
       "static own U=Self was reclassified as a receiver");
  need(bump_sig->parameters.size() == 3 &&
           is_box_of(bump_sig->parameters[0].type, is_i32) &&
           is_i32(bump_sig->parameters[1].type) &&
           bump_sig->parameters[1].mode == hir::ParamMode::inout &&
           is_i32(bump_sig->parameters[2].type) &&
           bump_sig->parameters[2].mode == hir::ParamMode::ordinary,
       "impl-only constrained inout signature changed");

  std::set<uint64_t> template_symbols;
  for (const auto &t : m.templates)
    template_symbols.insert(t.function.symbol.value);
  for (const auto &f : m.functions) {
    auto sig = m.types.find(f.signature);
    need(sig && !has_parameter(f.signature),
         "concrete constrained inout signature retained a parameter");
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f.symbol)
        continue;
      need(!has_parameter(n.type),
           "concrete constrained inout node retained a parameter");
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete constrained inout retained ConstraintCall");
      if (auto c = std::get_if<hir::Call>(&n.data)) {
        need(c->type_args.empty(),
             "concrete constrained inout call retained type arguments");
        if (c->direct_symbol)
          need(!template_symbols.count(c->direct_symbol->value),
               "concrete constrained inout call targets a template");
      }
    }
  }

  const auto *value_trait = find_trait("Value");
  const auto *left_trait = find_trait("Left");
  const auto *right_trait = find_trait("Right");
  need(value_trait && left_trait && right_trait,
       "constrained inherent inout traits missing");
  auto item_value_get = trait_method(*value_trait, "Item", "get");
  auto item_left_pick = trait_method(*left_trait, "Item", "pick");
  auto item_right_pick = trait_method(*right_trait, "Item", "pick");
  need(item_value_get != item_left_pick && item_left_pick != item_right_pick,
       "same-named inout trait methods shared a symbol");
  auto calls_named = [&](SymbolId function, SymbolId target) {
    size_t count = 0;
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && *c->direct_symbol == target)
        ++count;
    }
    return count;
  };
  const auto *replace_function = function_for(replace_i32->symbol);
  const auto *left_function = function_for(left_instances[0]->symbol);
  const auto *right_function = function_for(right_instances[0]->symbol);
  need(replace_function && left_function && right_function,
       "constrained inout concrete functions missing");
  need(calls_named(replace_function->symbol, item_value_get) == 1,
       "replace did not resolve its impl constraint to Item.get");
  need(calls_named(left_function->symbol, item_left_pick) == 1 &&
           calls_named(left_function->symbol, item_right_pick) == 0,
       "Left constraint did not select Item.Left.pick");
  need(calls_named(right_function->symbol, item_right_pick) == 1 &&
           calls_named(right_function->symbol, item_left_pick) == 0,
       "Right constraint did not select Item.Right.pick");
  size_t replace_template_constraints = 0;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == box_replace->function.symbol)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(c->trait.declaration == value_trait->symbol,
             "replace template constraint trait changed");
        ++replace_template_constraints;
      }
  need(replace_template_constraints == 1,
       "replace template lost its ordinary ConstraintCall");

  std::map<uint64_t, SymbolId> replace_renamed;
  for (auto [from, to] : replace_i32->renamed)
    replace_renamed[from.value] = to;
  need(replace_renamed.at(box_replace->function.symbol.value) ==
               replace_function->symbol &&
           replace_renamed.at(box_replace->function.parameters[0].symbol.value) ==
               replace_function->parameters[0].symbol &&
           replace_renamed.at(box_replace->function.parameters[1].symbol.value) ==
               replace_function->parameters[1].symbol,
       "replace renamed map lost Self or its inout parameter");
  std::optional<SymbolId> replace_local;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == box_replace->function.symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        replace_local = b->symbol;
        break;
      }
  need(replace_local && replace_renamed.count(replace_local->value),
       "replace template local was not renamed in its instance");
  bool concrete_place = false;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == replace_function->symbol)
      if (auto p = std::get_if<hir::Place>(&n.data); p) {
        need(p->symbol == replace_function->parameters[1].symbol,
             "replace concrete Place kept a template parameter");
        need(p->symbol != box_replace->function.parameters[1].symbol,
             "replace concrete Place impersonated its template parameter");
        concrete_place = true;
      }
  need(concrete_place, "replace concrete inout Place missing");
  std::set<uint64_t> template_locals;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == box_replace->function.symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data))
        template_locals.insert(b->symbol.value);
  bool concrete_local = false;
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == replace_function->symbol)
      if (auto b = std::get_if<hir::Binding>(&n.data); b && replace_local) {
        need(!template_locals.count(b->symbol.value),
             "replace concrete local kept a template SymbolId");
        if (b->symbol == replace_renamed.at(replace_local->value))
          concrete_local = true;
      }
  need(concrete_local, "replace concrete local binding missing");

  invalid_recomputed(m, "constrained inout instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == replace_i32->symbol) {
        i.origin = box_replace_static->function.symbol;
        return;
      }
    throw std::runtime_error("constrained inout replace instance missing");
  });
  invalid_recomputed(m, "constrained inout full key order", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == pair_instances[0]->symbol) {
        std::swap(i.type_args[1], i.type_args[2]);
        return;
      }
    throw std::runtime_error("constrained inout pair instance missing");
  });
  invalid_recomputed(m, "constrained inout Self substitution", [&](auto &x) {
    auto template_sig = *x.types.find(box_replace->function.signature);
    for (auto &f : x.functions)
      if (f.symbol == replace_function->symbol) {
        auto sig = *x.types.find(f.signature);
        sig.parameters[0].type = template_sig.parameters[1].type;
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("constrained inout replace function missing");
  });
  invalid(m, "constrained inout Place rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == replace_function->symbol)
        if (auto p = std::get_if<hir::Place>(&n.data); p) {
          p->symbol = box_replace->function.parameters[1].symbol;
          return;
        }
    throw std::runtime_error("constrained inout concrete Place missing");
  });
  invalid(m, "constrained inout local rename", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == replace_function->symbol)
        if (auto b = std::get_if<hir::Binding>(&n.data); b && replace_local) {
          b->symbol = *replace_local;
          return;
        }
    throw std::runtime_error("constrained inout concrete local missing");
  });

  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "constrained inherent inout source HIR rejected");
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  const std::string box = "struct Box[T]{value T;items Vec[i32];marker i32;} ";
  bad("trait V{} struct S{x i32;} " + box +
          "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
          "value U)U{let old=slot;slot=value;return old;}} "
          "func main()i32{let b=Box[S](value:S(x:0),items:[1],marker:0);"
          "var x i32=1;b.replace(inout x,2);return 0;}",
      "E_TYPE_MISMATCH");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
          "value U)U{let old=slot;slot=value;return old;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "var x i64=1;b.replace(inout x,2);return 0;}",
      "E_TYPE_MISMATCH");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
              "value U)U{let old=slot;slot=value;return old;}} "
              "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
              "var v Vec[i32]=[1];b.replace(inout v,[2]);return 0;}");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
              "value U)U{let old=slot;slot=value;return old;}} "
              "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
              "var x i32=1;b.replace(inout b.items[0],2);return 0;}");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
          "value U)U{let old=slot;slot=value;return old;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "let x i32=1;b.replace(inout x,2);return 0;}",
      "E_CALL_ARGUMENT");
  unsupported("trait V{} impl V for i32{} " + box +
              "impl[T V] Box[T]{func replace[U V](self inout Self,"
              "slot inout U,value U)U{let old=slot;slot=value;return old;}} "
              "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
              "var x i32=1;b.replace(inout x,2);return 0;}");
  bad("trait V{} impl V for i32{} " + box +
          "impl[T V] Box[T]{func replace[U V](self Self,slot inout U,"
          "value U)U{let old=slot;slot=value;return old;}} "
          "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
          "let f=b.replace[i32];return 0;}",
      "E_TYPE_MISMATCH");
  good("func bump[T V](slot inout T,value T)T{let old=slot;slot=value;"
       "return old;} trait V{} impl V for i32{} " + box +
       "impl[T V] Box[T]{func use[U V](self Self,f func(inout U,U)U,"
       "slot inout U,value U)U{return f(inout slot,value);}} "
       "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
       "var x i32=1;let r=b.use(bump[i32],inout x,2);return r+x-3;}");
  unsupported("trait V{} impl V for i32{} struct S{"
              "cb func(inout Vec[i32]);} struct H[T]{value T;} "
              "impl[T V] H[T]{func keep[U V](self Self,cb func() S)i32{"
              "return 0;}} func make()S{return S(cb:bump);}"
              "func bump(x inout Vec[i32]){} "
              "func main()i32{let h=H[i32](value:1);"
              "return h.keep[i32](make);}");
}


void trait_method_modes(const fs::path &root) {
  auto move_result = check(root /
      "tests/generics/method-move/trait-modes/lang.toml");
  need(move_result.success(), messages(move_result));
  need(hir::verify(*move_result.value).success(),
       "trait method move fixture HIR rejected");
  const auto &move = *move_result.value;

  auto inout_result = check(root /
      "tests/generics/method-inout/trait-modes/lang.toml");
  need(inout_result.success(), messages(inout_result));
  need(hir::verify(*inout_result.value).success(),
       "trait method inout fixture HIR rejected");
  const auto &inout = *inout_result.value;

  auto name = [](const hir::HirModule &m, SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](const hir::HirModule &m, TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return name(m, t->declaration);
  };
  auto trait_named = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (name(m, t.symbol) == n)
        return &t;
    return nullptr;
  };
  auto template_named = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (name(m, t.function.symbol) == n)
        return &t;
    return nullptr;
  };
  auto function_for = [&](const hir::HirModule &m, SymbolId id)
      -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto impl_for = [&](const hir::HirModule &m, SymbolId trait,
                      const std::string &type) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait && nominal_name(m, im.self_type) == type)
        return &im;
    return nullptr;
  };
  auto impl_method = [&](const hir::HirModule &m, const hir::ImplDecl &im,
                         const std::string &method) -> SymbolId {
    for (auto s : im.methods)
      if (name(m, s) == method)
        return s;
    throw std::runtime_error("trait impl method missing: " + method);
  };
  auto instances_of = [&](const hir::HirModule &m,
                          const hir::FunctionTemplate &t) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == t.function.symbol)
        result.push_back(&i);
    return result;
  };
  auto origin_of = [&](const hir::HirModule &m, SymbolId s) {
    for (const auto &i : m.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  auto constraint_in = [&](const hir::HirModule &m, SymbolId function)
      -> std::pair<HirNodeId, const hir::ConstraintCall *> {
    std::pair<HirNodeId, const hir::ConstraintCall *> result;
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      const auto &n = m.nodes[i];
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(!result.second, "multiple ConstraintCall nodes");
        result = {HirNodeId{i}, c};
      }
    }
    need(result.second, "missing ConstraintCall node");
    return result;
  };

  const auto *take = trait_named(move, "Take");
  const auto *forward_move = template_named(move, "forward");
  const auto *item_impl = take ? impl_for(move, take->symbol, "Item") : nullptr;
  const auto *box_impl = take ? impl_for(move, take->symbol, "Box") : nullptr;
  need(take && forward_move && item_impl && box_impl,
       "trait move declarations missing");
  const auto item_take = impl_method(move, *item_impl, "take");
  const auto box_take = impl_method(move, *box_impl, "take");
  auto [move_cc_id, move_cc] =
      constraint_in(move, forward_move->function.symbol);
  const hir::MethodPrototype *take_prototype = nullptr;
  for (const auto &p : take->methods)
    if (name(move, p.symbol) == "take")
      take_prototype = &p;
  need(take_prototype && move_cc->trait.declaration == take->symbol &&
           move_cc->method == take_prototype->symbol &&
           move_cc->method != item_take,
       "constraint call must target the trait prototype");
  need(name(move, move_cc->method) == "take" &&
           move_cc->args.size() == 4 &&
           move_cc->args[1].label == std::optional<std::string>("second") &&
           move_cc->args[2].label == std::optional<std::string>("value") &&
           move_cc->args[3].label == std::optional<std::string>("first") &&
           move_cc->args[1].parameter_index == 3 &&
           move_cc->args[2].parameter_index == 1 &&
           move_cc->args[3].parameter_index == 2 &&
           move_cc->args[0].mode == hir::ParamMode::ordinary &&
           move_cc->args[1].mode == hir::ParamMode::ordinary &&
           move_cc->args[2].mode == hir::ParamMode::move &&
           move_cc->args[3].mode == hir::ParamMode::ordinary,
       "trait move ConstraintCall order/labels/modes changed");
  const auto &template_move =
      std::get<hir::Move>(move.nodes[move_cc->args[2].expression.value].data);
  need(template_move.binding ==
           forward_move->function.parameters[1].symbol,
       "trait move ConstraintCall lost its template Move binding");

  auto move_instances = instances_of(move, *forward_move);
  need(move_instances.size() == 1 &&
           nominal_name(move, move_instances[0]->type_args[0]) == "Box",
       "trait generic impl forward instance missing");
  const auto *move_instance_function =
      function_for(move, move_instances[0]->symbol);
  need(move_instance_function, "trait move instance function missing");
  size_t move_concrete_calls = 0;
  for (const auto &n : move.nodes) {
    if (move.scopes[n.scope.value].function !=
        move_instance_function->symbol)
      continue;
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol ||
        origin_of(move, *c->direct_symbol) != box_take)
      continue;
    need(c->type_args.empty() && c->args.size() == 4 &&
             c->args[2].mode == hir::ParamMode::move,
         "concrete trait impl move call lost its mode");
    auto mv = std::get_if<hir::Move>(
        &move.nodes[c->args[2].expression.value].data);
    need(mv && mv->binding ==
                    move_instance_function->parameters[1].symbol,
         "concrete trait impl Move was not renamed to the instance");
    ++move_concrete_calls;
  }
  need(move_concrete_calls == 1,
       "trait impl ConstraintCall did not become one concrete Call");
  size_t direct_item_calls = 0;
  for (const auto &n : move.nodes) {
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol ||
        origin_of(move, *c->direct_symbol) != item_take)
      continue;
    need(c->args.size() == 4 &&
             c->args[2].mode == hir::ParamMode::move &&
             std::get<hir::Move>(
                 move.nodes[c->args[2].expression.value].data)
                 .binding.has_value(),
         "concrete receiver trait move call lost its Move");
    ++direct_item_calls;
  }
  need(direct_item_calls == 1,
       "concrete receiver trait impl method call missing");
  (void)move_cc_id;

  invalid(move, "trait move Move binding erased", [&](auto &x) {
    bool done = false;
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          move_instance_function->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data);
            mv && mv->binding) {
          mv->binding.reset();
          done = true;
          break;
        }
    need(done, "trait move concrete Move missing");
  });
  invalid(move, "trait move instance origin", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == move_instances[0]->symbol) {
        i.origin = box_take;
        return;
      }
    throw std::runtime_error("trait move instance missing");
  });
  invalid(move, "trait move instance type arguments", [&](auto &x) {
    hir::Type i64;
    i64.kind = hir::TypeKind::SignedInt;
    i64.bits = 64;
    for (auto &i : x.instances)
      if (i.symbol == move_instances[0]->symbol) {
        i.type_args[0] = x.types.intern(i64);
        return;
      }
    throw std::runtime_error("trait move instance missing");
  });

  const auto *adjust = trait_named(inout, "Adjust");
  const auto *forward_inout = template_named(inout, "forward");
  const auto *holder_impl =
      adjust ? impl_for(inout, adjust->symbol, "Holder") : nullptr;
  const auto *box_adjust =
      adjust ? impl_for(inout, adjust->symbol, "Box") : nullptr;
  need(adjust && forward_inout && holder_impl && box_adjust,
       "trait inout declarations missing");
  const auto holder_set = impl_method(inout, *holder_impl, "set");
  const auto box_set = impl_method(inout, *box_adjust, "set");
  auto [inout_cc_id, inout_cc] =
      constraint_in(inout, forward_inout->function.symbol);
  (void)inout_cc_id;
  need(inout_cc->trait.declaration == adjust->symbol &&
           name(inout, inout_cc->method) == "set" &&
           inout_cc->args.size() == 4 &&
           inout_cc->args[1].label == std::optional<std::string>("log") &&
           inout_cc->args[2].label == std::optional<std::string>("value") &&
           inout_cc->args[3].label == std::optional<std::string>("slot") &&
           inout_cc->args[1].parameter_index == 3 &&
           inout_cc->args[2].parameter_index == 2 &&
           inout_cc->args[3].parameter_index == 1 &&
           inout_cc->args[1].mode == hir::ParamMode::inout &&
           inout_cc->args[2].mode == hir::ParamMode::ordinary &&
           inout_cc->args[3].mode == hir::ParamMode::inout &&
           inout.nodes[inout_cc->args[1].expression.value].kind() ==
               hir::NodeKind::place &&
           inout.nodes[inout_cc->args[3].expression.value].kind() ==
               hir::NodeKind::place,
       "trait inout ConstraintCall order/labels/modes/places changed");
  HirNodeId read_slot{};
  for (size_t i = 0; i < inout.nodes.size(); ++i) {
    const auto &n = inout.nodes[i];
    if (inout.scopes[n.scope.value].function !=
        forward_inout->function.symbol)
      continue;
    if (auto r = std::get_if<hir::Read>(&n.data);
        r && r->symbol == forward_inout->function.parameters[1].symbol)
      read_slot = HirNodeId{i};
  }
  need(read_slot.value != 0, "trait inout slot Read node missing");
  auto inout_instances = instances_of(inout, *forward_inout);
  need(inout_instances.size() == 1,
       "trait inout forward instance missing");
  const auto *inout_instance_function =
      function_for(inout, inout_instances[0]->symbol);
  need(inout_instance_function, "trait inout instance function missing");
  size_t inout_concrete_calls = 0, holder_direct_calls = 0;
  for (const auto &n : inout.nodes) {
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol)
      continue;
    auto target = origin_of(inout, *c->direct_symbol);
    if (inout.scopes[n.scope.value].function ==
        inout_instance_function->symbol) {
      if (target != box_set)
        continue;
      need(c->args.size() == 4 && c->args[1].mode == hir::ParamMode::inout &&
               c->args[3].mode == hir::ParamMode::inout &&
               inout.nodes[c->args[1].expression.value].kind() ==
                   hir::NodeKind::place &&
               inout.nodes[c->args[3].expression.value].kind() ==
                   hir::NodeKind::place,
           "concrete generic trait impl inout call lost its Places");
      ++inout_concrete_calls;
    } else if (target == holder_set) {
      ++holder_direct_calls;
    }
  }
  need(inout_concrete_calls == 1 && holder_direct_calls == 2,
       "concrete receiver or generic trait impl inout call missing");

  invalid(inout, "trait inout Place replaced by Read", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          forward_inout->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[3].expression = read_slot;
          n.children[3] = read_slot;
          return;
        }
    throw std::runtime_error("trait inout ConstraintCall missing");
  });
  invalid(inout, "trait inout ordinary alias of borrowed storage",
          [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          forward_inout->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[2].expression = read_slot;
          n.children[2] = read_slot;
          return;
        }
    throw std::runtime_error("trait inout ConstraintCall missing");
  });
  invalid(inout, "trait inout mode erased from Place", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          forward_inout->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[3].mode = hir::ParamMode::ordinary;
          return;
        }
    throw std::runtime_error("trait inout ConstraintCall missing");
  });
  invalid(inout, "trait inout label changed", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          forward_inout->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[1].label = std::string("value");
          return;
        }
    throw std::runtime_error("trait inout ConstraintCall missing");
  });
  invalid(inout, "trait inout parameter index changed", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function ==
          forward_inout->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[3].parameter_index = 2;
          return;
        }
    throw std::runtime_error("trait inout ConstraintCall missing");
  });

  auto two_traits = source(R"(
trait Left{func pick(self Self,slot inout i32)i32;}
trait Right{func pick(self Self,slot inout i32)i32;}
struct S{items Vec[i32];}
impl Left for S{func pick(self Self,slot inout i32)i32{slot=slot+1;return 1;}}
impl Right for S{func pick(self Self,slot inout i32)i32{slot=slot+2;return 2;}}
func forward[T Left](owner T,slot inout i32)i32{return owner.pick(inout slot);}
func main()i32{let s=S(items:[1]);var x i32=0;let r=forward(s,inout x);return r-1;}
)");
  need(two_traits.success(), messages(two_traits));
  need(hir::verify(*two_traits.value).success(),
       "same-signature distinct-trait source HIR rejected");
  const auto &tw = *two_traits.value;
  const auto *left = trait_named(tw, "Left");
  const auto *right = trait_named(tw, "Right");
  const auto *tw_forward = template_named(tw, "forward");
  need(left && right && tw_forward, "same-signature traits missing");
  const auto *left_impl = impl_for(tw, left->symbol, "S");
  const auto *right_impl = impl_for(tw, right->symbol, "S");
  need(left_impl && right_impl, "same-signature impls missing");
  const auto left_pick = impl_method(tw, *left_impl, "pick");
  const auto right_pick = impl_method(tw, *right_impl, "pick");
  auto tw_instances = instances_of(tw, *tw_forward);
  need(tw_instances.size() == 1, "same-signature forward instance missing");
  const auto *tw_function = function_for(tw, tw_instances[0]->symbol);
  need(tw_function, "same-signature forward function missing");
  size_t tw_calls = 0;
  for (const auto &n : tw.nodes) {
    if (tw.scopes[n.scope.value].function != tw_function->symbol)
      continue;
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->direct_symbol && origin_of(tw, *c->direct_symbol) == left_pick)
      ++tw_calls;
  }
  need(tw_calls == 1, "same-signature forward selected the wrong trait impl");
  invalid(tw, "trait target changed to other same-signature trait",
          [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == tw_function->symbol)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->direct_symbol) {
          c->direct_symbol = right_pick;
          return;
        }
    throw std::runtime_error("same-signature concrete Call missing");
  });
  invalid(tw, "trait ConstraintCall changed to other same-signature trait",
          [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == tw_forward->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->trait = hir::TraitRef{right->symbol, {}};
          c->method = right->methods.front().symbol;
          return;
        }
    throw std::runtime_error("same-signature ConstraintCall missing");
  });

  auto duplicate = source(R"(
trait Pair{func swap(self Self,a inout i32,b inout i32);}
struct S{items Vec[i32];}
impl Pair for S{func swap(self Self,a inout i32,b inout i32){let t=a;a=b;b=t;}}
func forward[T Pair](owner T,a inout i32,b inout i32){owner.swap(inout a,inout b);}
func main(){let s=S(items:[1]);var x i32=0;var y i32=0;forward(s,inout x,inout y);}
)");
  need(duplicate.success(), messages(duplicate));
  need(hir::verify(*duplicate.value).success(),
       "two scalar trait inout source HIR rejected");
  const auto &dup = *duplicate.value;
  const auto *dup_forward = template_named(dup, "forward");
  need(dup_forward, "two scalar trait inout forward missing");
  auto [dup_id, dup_cc] = constraint_in(dup, dup_forward->function.symbol);
  (void)dup_id;
  need(dup_cc->args.size() == 3 &&
           dup_cc->args[1].mode == hir::ParamMode::inout &&
           dup_cc->args[2].mode == hir::ParamMode::inout,
       "two scalar trait inout arguments missing");
  invalid(dup, "duplicate trait inout storage", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == dup_forward->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[2].expression = c->args[1].expression;
          n.children[2] = c->args[2].expression;
          return;
        }
    throw std::runtime_error("two scalar trait inout ConstraintCall missing");
  });

  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  auto accept = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "new trait move source HIR rejected: " + text + "\n" +
             messages(r));
    return *r.value;
  };
  unsupported("trait V{func bump(self Self,slot inout Vec[i32]);}"
              "struct S{items Vec[i32];} impl V for S{"
              "func bump(self Self,slot inout Vec[i32]){}}"
              "func main(){let s=S(items:[1]);var v Vec[i32]=[2];"
              "s.bump(inout v);}");
  unsupported("trait V{func take(self move Self)Self;}"
              "struct S{items Vec[i32];} impl V for S{"
              "func take(self move Self)Self{return move self;}}"
              "func main(){var s=S(items:[1]);let r=s.take();}");
  unsupported("trait V{func bump(self inout Self);}"
              "struct S{items Vec[i32];} impl V for S{"
              "func bump(self inout Self){}}"
              "func main(){var s=S(items:[1]);s.bump(inout s);}");
  // Reclassified: non-self move on a generic trait method is now accepted;
  // the original Source bytes are unchanged and runtime coverage is in the
  // trait-generic-move fixture. New inout below remains unavailable.
  accept("trait V{func take[U](self Self,value move U)U;}"
         "struct S{items Vec[i32];} impl V for S{"
         "func take[U](self Self,value move U)U{return move value;}}"
         "func main(){let s=S(items:[1]);var x i32=2;"
         "let r=s.take(move x);}");
  // Reclassified: a type-qualified call now passes every declared parameter
  // from index zero, including a receiverless static move method.
  accept("trait V{func make(value move i32)i32;}"
         "struct S{items Vec[i32];} impl V for S{"
         "func make(value move i32)i32{return value;}}"
         "func main(){var x i32=1;let r=S.make(move x);}");
  bad("trait V{func set(self Self,slot inout i32);}"
      "struct S{items Vec[i32];} impl V for S{"
      "func set(self Self,other inout i32){}}"
      "func main(){let s=S(items:[1]);var x i32=2;s.set(inout x);}",
      "E_TYPE_MISMATCH");
  bad("trait Left{func pick(self Self)i32;}"
      "trait Right{func pick(self Self)i32;}"
      "struct S{items Vec[i32];} "
      "impl Left for S{func pick(self Self)i32{return 1;}} "
      "impl Right for S{func pick(self Self)i32{return 2;}} "
      "func main(){let s=S(items:[1]);return s.pick();}",
      "E_TYPE_MISMATCH");

  // Ordinary trait methods predate the new move/inout source restriction. A
  // first Self parameter named owner is still legal both when the bounded
  // template is called and when it is merely checked but never instantiated.
  auto legacy = source(R"(
trait V{func ordinary(owner Self)i32;}
struct S{x i32;}
impl V for S{func ordinary(owner Self)i32{return owner.x;}}
func invoke[T V](x T)i32{return x.ordinary();}
func unused[T V](x T)i32{return x.ordinary();}
func main()i32{return invoke(S(x:1))-1;}
)");
  need(legacy.success(), messages(legacy));
  need(hir::verify(*legacy.value).success(),
       "ordinary owner-named Self receiver HIR rejected");
  const auto &lg = *legacy.value;
  const auto *lg_trait = trait_named(lg, "V");
  const auto *lg_invoke = template_named(lg, "invoke");
  const auto *lg_unused = template_named(lg, "unused");
  need(lg_trait && lg_invoke && lg_unused &&
           lg_trait->methods.size() == 1 &&
           lg_trait->methods.front().parameters.front().label == "owner",
       "ordinary owner-named prototype was not preserved");
  for (const auto *t : {lg_invoke, lg_unused}) {
    auto [id, cc] = constraint_in(lg, t->function.symbol);
    (void)id;
    need(cc->trait.declaration == lg_trait->symbol &&
             name(lg, cc->method) == "ordinary" && cc->args.size() == 1 &&
             cc->args[0].mode == hir::ParamMode::ordinary,
         "ordinary owner-named ConstraintCall shape changed");
  }
  size_t invoke_instances = 0, unused_instances = 0;
  for (const auto &i : lg.instances) {
    if (i.origin == lg_invoke->function.symbol)
      ++invoke_instances;
    if (i.origin == lg_unused->function.symbol)
      ++unused_instances;
  }
  need(invoke_instances == 1 && unused_instances == 0,
       "ordinary owner-named template instance counts changed");

  // Reclassified: an ordinary owner-labelled receiver is the same receiver,
  // and only the literal-label restriction on the new move mode is removed.
  // The older inout route still requires its literal self label below.
  auto owner_new_mode = source(
      "trait V{func ordinary(owner Self,value move Vec[i32])Vec[i32];}"
      "struct S{x i32;} impl V for S{"
      "func ordinary(owner Self,value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func invoke[T V](x T,value move Vec[i32])Vec[i32]{"
      "return x.ordinary(move value);} "
      "func main()i32{var v Vec[i32]=[1];let r=invoke(S(x:1),move v);"
      "return r[0]-1;}");
  need(owner_new_mode.success(), messages(owner_new_mode));
  invalid(inout, "trait move/inout owner label rejected", [&](auto &x) {
    for (auto &tr : x.traits)
      if (name(x, tr.symbol) == "Adjust")
        for (auto &method : tr.methods)
          if (name(x, method.symbol) == "set") {
            method.parameters.front().label = "owner";
            return;
          }
    throw std::runtime_error("Adjust set prototype missing");
  });
}


void trait_self_inout(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "trait Self inout HIR rejected: " + text + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  auto source_name = [](const hir::HirModule &m, SymbolId s)
      -> const std::string & { return m.resolved->symbols.at(s.value).name; };
  auto nominal_name = [&](const hir::HirModule &m, TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return source_name(m, t->declaration);
  };
  auto is_i32 = [](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_bool = [](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Bool;
  };
  auto trait_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (source_name(m, t.symbol) == n)
        return &t;
    return nullptr;
  };
  auto prototype_for = [&](const hir::HirModule &m, const hir::TraitDecl &tr,
                           const std::string &n)
      -> const hir::MethodPrototype * {
    for (const auto &p : tr.methods)
      if (source_name(m, p.symbol) == n)
        return &p;
    return nullptr;
  };
  auto template_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (source_name(m, t.function.symbol) == n)
        return &t;
    return nullptr;
  };
  auto function_for = [&](const hir::HirModule &m, SymbolId id)
      -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto type_name = [&](const hir::HirModule &m, TypeId id) -> std::string {
    auto nominal = nominal_name(m, id);
    if (!nominal.empty())
      return nominal;
    auto t = m.types.find(id);
    if (!t)
      return {};
    switch (t->kind) {
    case hir::TypeKind::Bool:
      return "bool";
    case hir::TypeKind::String:
      return "string";
    case hir::TypeKind::Unit:
      return "Unit";
    case hir::TypeKind::Float:
      return t->bits == 32 ? "f32" : "f64";
    case hir::TypeKind::SignedInt:
    case hir::TypeKind::UnsignedInt:
      return std::string(t->kind == hir::TypeKind::SignedInt ? "i" : "u") +
             std::to_string(t->bits);
    default:
      return {};
    }
  };
  auto impl_for = [&](const hir::HirModule &m, SymbolId trait,
                      const std::string &type) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait && type_name(m, im.self_type) == type)
        return &im;
    return nullptr;
  };
  auto impl_method = [&](const hir::HirModule &m, const hir::ImplDecl &im,
                         const std::string &method) -> SymbolId {
    for (auto s : im.methods)
      if (source_name(m, s) == method)
        return s;
    throw std::runtime_error("trait impl method missing: " + method);
  };
  auto origin_of = [&](const hir::HirModule &m, SymbolId s) {
    for (const auto &i : m.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  auto constraint_in = [&](const hir::HirModule &m, SymbolId function)
      -> std::pair<size_t, const hir::ConstraintCall *> {
    std::pair<size_t, const hir::ConstraintCall *> result;
    result.first = static_cast<size_t>(-1);
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      const auto &n = m.nodes[i];
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(result.first == static_cast<size_t>(-1),
             "multiple ConstraintCall nodes");
        result = {i, c};
      }
    }
    need(result.second, "missing ConstraintCall node");
    return result;
  };
  auto instance_for = [&](const hir::HirModule &m,
                          const hir::FunctionTemplate &t, TypeId arg)
      -> const hir::FunctionInstance * {
    for (const auto &i : m.instances)
      if (i.origin == t.function.symbol && i.type_args.size() == 1 &&
          i.type_args[0] == arg)
        return &i;
    return nullptr;
  };
  auto has_parameter = [](const hir::HirModule &m, TypeId root) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root);
  };

  auto fixture = check(root /
                       "tests/generics/method-inout/trait-self/lang.toml");
  need(fixture.success(), messages(fixture));
  need(hir::verify(*fixture.value).success(),
       "trait Self inout fixture HIR rejected");
  const auto &fm = *fixture.value;
  const auto *flow = trait_for(fm, "SelfFlow");
  const auto *through_fail = template_for(fm, "relay_fail");
  const auto *through_saved = template_for(fm, "relay_saved");
  need(flow && through_fail && through_saved,
       "trait Self inout fixture declarations missing");
  need(through_fail->params.size() == 1 &&
           through_saved->params.size() == 1 &&
           source_name(fm, through_fail->params[0].symbol) == "T" &&
           source_name(fm, through_saved->params[0].symbol) == "T",
       "trait Self was registered as a method generic parameter");
  const auto *fail_proto = prototype_for(fm, *flow, "fail");
  const auto *saved_proto = prototype_for(fm, *flow, "saved");
  need(fail_proto && saved_proto, "trait Self prototypes missing");
  const auto *fail_sig = fm.types.find(fail_proto->signature);
  need(fail_sig && fail_sig->parameters.size() == 5,
       "trait Self prototype signature shape changed");
  need(fail_sig->parameters[1].mode == hir::ParamMode::inout &&
           fail_sig->parameters[2].mode == hir::ParamMode::ordinary,
       "trait Self slot mode or neighbour changed");
  {
    auto t = fm.types.find(fail_sig->parameters[1].type);
    need(t && t->kind == hir::TypeKind::TypeParameter &&
             t->declaration == flow->self,
         "trait Self slot lost the real trait Self type");
    need(fail_proto->parameters[1].label == "slot" &&
             fail_sig->parameters[0].mode == hir::ParamMode::ordinary,
         "trait Self prototype label/receiver changed");
  }
  need(function_for(fm, fail_proto->symbol) == nullptr,
       "trait prototype gained a fake body");
  auto [fail_cc_id, fail_cc] =
      constraint_in(fm, through_fail->function.symbol);
  (void)fail_cc_id;
  need(fail_cc->trait.declaration == flow->symbol &&
           source_name(fm, fail_cc->method) == "fail" &&
           fail_cc->args.size() == 5 &&
           fail_cc->args[1].mode == hir::ParamMode::inout &&
           fail_cc->args[1].parameter_index == 1 &&
           !fail_cc->args[1].label &&
           fail_cc->args[3].mode == hir::ParamMode::move,
       "trait Self ConstraintCall modes/indices/labels changed");
  {
    const auto &place = std::get<hir::Place>(
        fm.nodes[fail_cc->args[1].expression.value].data);
    need(place.symbol == through_fail->function.parameters[1].symbol,
         "trait Self ConstraintCall lost its template Place binding");
    need(has_parameter(fm, fail_cc->receiver),
         "trait Self ConstraintCall receiver is not the template type");
    const auto *sig = fm.types.find(fail_cc->signature);
    need(sig && sig->parameters[1].mode == hir::ParamMode::inout &&
             sig->parameters[1].type == fail_cc->receiver,
         "trait Self ConstraintCall signature did not substitute Self to T");
    const auto &mv = std::get<hir::Move>(
        fm.nodes[fail_cc->args[3].expression.value].data);
    need(mv.binding == through_fail->function.parameters[3].symbol,
         "trait Self ConstraintCall lost its template Move binding");
  }

  const auto *flow_i32 = impl_for(fm, flow->symbol, "i32");
  const auto *flow_bool = impl_for(fm, flow->symbol, "bool");
  need(flow_i32 && flow_bool, "trait Self scalar impls missing");
  const auto ordered_i32 = impl_method(fm, *flow_i32, "ordered");
  const auto saved_i32 = impl_method(fm, *flow_i32, "saved");
  const auto saved_bool = impl_method(fm, *flow_bool, "saved");
  size_t direct_i32 = 0;
  for (const auto &n : fm.nodes) {
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol)
      continue;
    auto target = origin_of(fm, *c->direct_symbol);
    if (target != ordered_i32 && target != saved_i32)
      continue;
    const auto *sig = fm.types.find(c->signature);
    need(sig && sig->parameters.size() >= 2 &&
             sig->parameters[1].mode == hir::ParamMode::inout &&
             is_i32(fm, sig->parameters[1].type),
         "concrete trait Self call lost its i32 inout signature");
    bool slot = false;
    for (const auto &a : c->args)
      if (a.parameter_index == 1 && a.mode == hir::ParamMode::inout) {
        need(fm.nodes[a.expression.value].kind() == hir::NodeKind::place,
             "concrete trait Self call lost its i32 inout Place");
        slot = true;
      }
    need(slot, "concrete trait Self inout argument missing");
    ++direct_i32;
  }
  need(direct_i32 >= 2,
       "concrete receiver trait Self inout calls missing");

  const auto *saved_instance =
      instance_for(fm, *through_saved, [&] {
        for (size_t i = 1; i <= fm.types.size(); ++i)
          if (is_bool(fm, TypeId{i}))
            return TypeId{i};
        throw std::runtime_error("bool TypeId missing");
      }());
  need(saved_instance, "relay_saved[bool] instance missing");
  const auto *saved_fn = function_for(fm, saved_instance->symbol);
  need(saved_fn, "relay_saved[bool] function missing");
  {
    const auto *sig = fm.types.find(saved_fn->signature);
    need(sig && sig->parameters.size() == 5 &&
             sig->parameters[1].mode == hir::ParamMode::inout &&
             is_bool(fm, sig->parameters[1].type) &&
             !has_parameter(fm, saved_fn->signature),
         "trait Self instance retained T or lost inout bool");
  }
  size_t saved_calls = 0;
  for (const auto &n : fm.nodes) {
    if (fm.scopes[n.scope.value].function != saved_fn->symbol)
      continue;
    need(!std::holds_alternative<hir::ConstraintCall>(n.data),
         "concrete trait Self instance retained ConstraintCall");
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol ||
        origin_of(fm, *c->direct_symbol) != saved_bool)
      continue;
    const auto *sig = fm.types.find(c->signature);
    need(sig && sig->parameters[1].mode == hir::ParamMode::inout &&
             is_bool(fm, sig->parameters[1].type),
         "concrete trait Self bool call lost its inout type");
    const auto &place = std::get<hir::Place>(
        fm.nodes[c->args[1].expression.value].data);
    need(place.symbol == saved_fn->parameters[1].symbol,
         "concrete trait Self Place was not renamed to the instance");
    ++saved_calls;
  }
  need(saved_calls == 1,
       "concrete trait Self bool call missing or duplicated");

  auto mixed = good(R"(
trait Exchange {
    func replace(self Self, slot inout Self, value Self) Self;
    func swap(self Self, first inout Self, second inout Self);
}
impl Exchange for u32 {
    func replace(self Self, slot inout Self, value Self) Self {
        let old = slot; slot = value; return old;
    }
    func swap(self Self, first inout Self, second inout Self) {
        let old = first; first = second; second = old;
    }
}
impl Exchange for f32 {
    func replace(self Self, slot inout Self, value Self) Self {
        let old = slot; slot = value; return old;
    }
    func swap(self Self, first inout Self, second inout Self) {
        let old = first; first = second; second = old;
    }
}
func relay[T Exchange](receiver T, slot inout T, value T) T {
    return receiver.replace(inout slot, value);
}
func outer[T Exchange](receiver T, slot inout T, value T) T {
    return relay(receiver, inout slot, value);
}
trait Legacy { func ordinary(owner Self) i32; }
struct LegacyItem { x i32; }
impl Legacy for LegacyItem {
    func ordinary(owner Self) i32 { return owner.x; }
}
func legacy_invoke[T Legacy](value T) i32 { return value.ordinary(); }
func legacy_unused[T Legacy](value T) i32 { return value.ordinary(); }
func main() i32 {
    let r u32 = 5;
    var x u32 = 5;
    let old = r.replace(inout x, 8);
    let f f32 = 1.5;
    var y f32 = 1.5;
    let forwarded = outer[f32](f, inout y, 2.5);
    var first u32 = 7;
    var second u32 = 7;
    r.swap(second: inout second, first: inout first);
    if old != 5 || x != 8 || forwarded != 1.5 || y != 2.5 ||
       first != 7 || second != 7 {
        return 1;
    }
    let legacy = LegacyItem(x: 1);
    if legacy_invoke(legacy) != 1 { return 2; }
    return 0;
}
)");
  {
    const auto *relay = template_for(mixed, "relay");
    const auto *outer = template_for(mixed, "outer");
    const auto *legacy_invoke = template_for(mixed, "legacy_invoke");
    const auto *legacy_unused = template_for(mixed, "legacy_unused");
    need(relay && outer && legacy_invoke && legacy_unused,
         "explicit/inferred/same-constraint or legacy templates missing");
    size_t relay_f32 = 0, legacy_invoke_instances = 0,
           legacy_unused_instances = 0;
    for (const auto &i : mixed.instances) {
      if (i.origin == relay->function.symbol && i.type_args.size() == 1) {
        auto t = mixed.types.find(i.type_args[0]);
        if (t && t->kind == hir::TypeKind::Float && t->bits == 32)
          ++relay_f32;
      }
      if (i.origin == legacy_invoke->function.symbol)
        ++legacy_invoke_instances;
      if (i.origin == legacy_unused->function.symbol)
        ++legacy_unused_instances;
    }
    need(relay_f32 == 1 && legacy_invoke_instances == 1 &&
             legacy_unused_instances == 0,
         "explicit forwarding or ordinary owner legacy template changed");
    auto [outer_cc_id, outer_cc] =
        constraint_in(mixed, legacy_invoke->function.symbol);
    (void)outer_cc_id;
    need(outer_cc->args.size() == 1 &&
             outer_cc->args[0].mode == hir::ParamMode::ordinary,
         "ordinary owner ConstraintCall shape changed");
  }

  const std::string exchange = R"(
trait Exchange {
    func replace(self Self, slot inout Self, value Self) Self;
    func swap(self Self, first inout Self, second inout Self);
}
impl Exchange for i32 {
    func replace(self Self, slot inout Self, value Self) Self {
        let old = slot; slot = value; return old;
    }
    func swap(self Self, first inout Self, second inout Self) {
        let old = first; first = second; second = old;
    }
}
)";
  bad(exchange + "func main(){var x i32=1;let r i32=0;"
                 "r.replace(inout x,inout x);}", "E_CALL_ARGUMENT");
  bad(exchange + "func main(){var x i32=1;let r i32=0;"
                 "r.replace(inout x,x);}", "E_CALL_ARGUMENT");
  bad(exchange + "func read(slot inout i32)i32{return slot;} "
                 "func main(){var x i32=1;let r i32=0;"
                 "r.replace(inout x,read(inout x));}", "E_CALL_ARGUMENT");
  bad(exchange + "func main(){var x i32=1;let r i32=0;"
                 "r.swap(inout x,inout x);}", "E_CALL_ARGUMENT");
  bad(exchange + "func main(){let x i32=1;let r i32=0;"
                 "r.replace(inout x,2);}", "E_CALL_ARGUMENT");
  bad(exchange + "func f(x i32){let r i32=0;r.replace(inout x,2);} "
                 "func main(){f(1);}", "E_CALL_ARGUMENT");
  unsupported(exchange + "struct S{x i32;} func main(){let r i32=0;"
              "var s S=S(x:1);r.replace(inout s.x,2);}");
  unsupported(exchange + "func main(){let r i32=0;var v Vec[i32]=[1];"
              "r.replace(inout v[0],2);}");
  unsupported(exchange + "func main(){let r i32=0;r.replace(inout 1,2);}");
  bad(exchange + "struct S{x i32;} func main(){let s S=S(x:1);"
                 "var x i32=1;s.replace(inout x,2);}", "E_TYPE_MISMATCH");
  bad(exchange + "struct S{x i32;} "
                 "func forward[T Exchange](receiver T,slot inout T,value T)T{"
                 "return receiver.replace(inout slot,value);} "
                 "func main(){let s S=S(x:1);var x i32=1;"
                 "forward(s,inout x,2);}", "E_TYPE_MISMATCH");

  good("trait V{func bad(self Self,cb func(inout Self));} "
       "struct S{x i32;} impl V for S{"
       "func bad(self Self,cb func(inout Self)){}} "
       "func main()i32{return 0;}");
  unsupported("trait V{func bad(self inout Self);} struct S{x i32;} "
              "impl V for S{func bad(self inout Self){}} "
              "func main()i32{var s S=S(x:1);s.bad(inout s);return 0;}");
  unsupported("trait V{func bad(self Self,slot inout Vec[Self]);} "
              "struct S{x i32;} impl V for S{"
              "func bad(self Self,slot inout Vec[Self]){}} "
              "func main()i32{return 0;}");
  auto trait_self_struct = good("trait V{func set(self Self,slot inout Self);} "
      "struct S{x i32;} impl V for S{"
      "func set(self Self,slot inout Self){}} "
      "func main()i32{return 0;}");
  need(hir::verify(trait_self_struct).success(),
       "trait POD struct Self inout did not publish");
  auto trait_self_enum = good("trait V{func set(self Self,slot inout Self);} "
      "enum E{One;} impl V for E{"
      "func set(self Self,slot inout Self){}} "
      "func main()i32{return 0;}");
  need(hir::verify(trait_self_enum).success(),
       "trait POD enum Self inout did not publish");
  auto trait_self_generic = good("trait V{func set(self Self,slot inout Self);} "
      "struct Box[T]{value T;} impl[T] V for Box[T]{"
      "func set(self Self,slot inout Self){}} "
      "func main()i32{return 0;}");
  need(hir::verify(trait_self_generic).success(),
       "trait generic POD Self inout did not publish");
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "impl V for Vec[i32]{func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "impl V for string{func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "impl V for Unit{func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
  good("trait W{func bump(self Self,slot inout i32);} "
       "struct Box[T]{value T;} impl[T] W for Box[T]{"
       "func bump(self Self,slot inout i32){}} "
       "func main()i32{return 0;}");

  auto hirs = good(R"(
trait Exchange {
    func replace(self Self, slot inout Self, value Self) Self;
}
trait Other {
    func replace(self Self, slot inout Self, value Self) Self;
}
struct S { x i32; }
impl Exchange for i32 {
    func replace(self Self, slot inout Self, value Self) Self {
        let old = slot; slot = value; return old;
    }
}
impl Other for i32 {
    func replace(self Self, slot inout Self, value Self) Self {
        let old = slot; slot = value; return old;
    }
}
func through[T Exchange](receiver T, slot inout T, value T) T {
    let observed = slot;
    return receiver.replace(inout slot, value);
}
func main() i32 {
    let s = S(x:1);
    var x i32 = 1;
    let a = through[i32](1, inout x, 2);
    return a - 1;
}
)");
  const auto *exchange_trait = trait_for(hirs, "Exchange");
  const auto *other_trait = trait_for(hirs, "Other");
  const auto *through = template_for(hirs, "through");
  need(exchange_trait && other_trait && through,
       "corruption fixture declarations missing");
  const auto *exchange_i32 =
      impl_for(hirs, exchange_trait->symbol, "i32");
  const auto *other_i32 = impl_for(hirs, other_trait->symbol, "i32");
  need(exchange_i32 && other_i32, "corruption fixture impls missing");
  const auto exchange_replace = impl_method(hirs, *exchange_i32, "replace");
  const auto other_replace = impl_method(hirs, *other_i32, "replace");
  auto [cc_id, cc] = constraint_in(hirs, through->function.symbol);
  size_t read_slot = static_cast<size_t>(-1);
  for (size_t i = 0; i < hirs.nodes.size(); ++i) {
    const auto &n = hirs.nodes[i];
    if (hirs.scopes[n.scope.value].function != through->function.symbol)
      continue;
    if (auto r = std::get_if<hir::Read>(&n.data);
        r && r->symbol == through->function.parameters[1].symbol)
      read_slot = i;
  }
  need(read_slot != static_cast<size_t>(-1),
       "corruption fixture slot read missing");
  TypeId i32_type{};
  for (size_t i = 1; i <= hirs.types.size(); ++i)
    if (is_i32(hirs, TypeId{i}))
      i32_type = TypeId{i};
  need(i32_type.value, "corruption fixture i32 type missing");
  const auto *through_instance = instance_for(hirs, *through, i32_type);
  need(through_instance, "corruption fixture through[i32] instance missing");
  const auto *through_fn = function_for(hirs, through_instance->symbol);
  need(through_fn, "corruption fixture through function missing");
  SymbolId observed_template{};
  for (const auto &n : hirs.nodes) {
    if (hirs.scopes[n.scope.value].function != through->function.symbol)
      continue;
    if (auto b = std::get_if<hir::Binding>(&n.data);
        b && source_name(hirs, b->symbol) == "observed")
      observed_template = b->symbol;
  }
  need(observed_template.value, "corruption fixture observed local missing");
  SymbolId observed_instance{};
  for (const auto &i : hirs.instances)
    if (i.symbol == through_instance->symbol)
      for (const auto &[from, to] : i.renamed)
        if (from == observed_template)
          observed_instance = to;
  need(observed_instance.value,
       "corruption fixture renamed observed local missing");
  auto concrete_call = [&](hir::HirModule &x) -> std::optional<size_t> {
    for (size_t i = 0; i < x.nodes.size(); ++i) {
      const auto &n = x.nodes[i];
      if (x.scopes[n.scope.value].function != through_fn->symbol)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(x, *c->direct_symbol) == exchange_replace)
        return i;
    }
    return std::nullopt;
  };

  invalid(hirs, "trait Self inout Place replaced by Read", [&](auto &x) {
    auto &n = x.nodes[cc_id];
    auto &c = std::get<hir::ConstraintCall>(n.data);
    c.args[1].expression = HirNodeId{read_slot};
    n.children[1] = HirNodeId{read_slot};
  });
  invalid(hirs, "trait Self inout mode erased", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[1].mode =
        hir::ParamMode::ordinary;
  });
  invalid(hirs, "trait Self receiver0 mode changed", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[0].mode =
        hir::ParamMode::inout;
  });
  invalid(hirs, "trait Self receiver0 index changed", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[0].source_index =
        1;
  });
  invalid(hirs, "trait Self slot source index changed", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[1].source_index =
        3;
  });
  invalid(hirs, "trait Self slot parameter index changed", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data)
        .args[1].parameter_index = 2;
  });
  invalid(hirs, "trait Self slot label changed", [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[1].label =
        std::string("value");
  });
  invalid(hirs, "trait Self receiver changed", [&](auto &x) {
    hir::Type t;
    t.kind = hir::TypeKind::SignedInt;
    t.bits = 64;
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).receiver =
        x.types.intern(t);
  });
  invalid(hirs, "trait Self method moved to another trait", [&](auto &x) {
    auto &c = std::get<hir::ConstraintCall>(x.nodes[cc_id].data);
    c.trait = hir::TraitRef{other_trait->symbol, {}};
    c.method = other_trait->methods.front().symbol;
  });
  invalid(hirs, "trait Self concrete instance type argument changed",
          [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == through_instance->symbol) {
        hir::Type t;
        t.kind = hir::TypeKind::Bool;
        i.type_args[0] = x.types.intern(t);
        return;
      }
    throw std::runtime_error("through instance missing");
  });
  invalid(hirs, "trait Self concrete Place binding changed", [&](auto &x) {
    auto id = concrete_call(x);
    need(id.has_value(), "concrete trait Self call missing");
    auto &n = x.nodes[*id];
    auto &c = std::get<hir::Call>(n.data);
    auto &place = std::get<hir::Place>(x.nodes[c.args[1].expression.value].data);
    place.symbol = observed_instance;
  });
  invalid(hirs, "trait Self concrete target moved to other trait", [&](auto &x) {
    auto id = concrete_call(x);
    need(id.has_value(), "concrete trait Self call missing");
    std::get<hir::Call>(x.nodes[*id].data).direct_symbol = other_replace;
  });
  invalid(hirs, "trait Self concrete inout made non-scalar", [&](auto &x) {
    auto id = concrete_call(x);
    need(id.has_value(), "concrete trait Self call missing");
    TypeId s_type{};
    for (size_t i = 1; i <= x.types.size(); ++i)
      if (nominal_name(x, TypeId{i}) == "S")
        s_type = TypeId{i};
    need(s_type.value, "corruption fixture S type missing");
    auto &n = x.nodes[*id];
    auto &c = std::get<hir::Call>(n.data);
    auto sig = *x.types.find(c.signature);
    sig.parameters[1].type = s_type;
    c.signature = x.types.intern(sig);
    x.nodes[c.args[1].expression.value].type = s_type;
  });
  invalid_recomputed(hirs, "trait Self label corruption recomputed",
                     [&](auto &x) {
    std::get<hir::ConstraintCall>(x.nodes[cc_id].data).args[1].label =
        std::string("value");
  });
  invalid_recomputed(hirs, "trait Self target corruption recomputed",
                     [&](auto &x) {
    auto id = concrete_call(x);
    need(id.has_value(), "concrete trait Self call missing");
    std::get<hir::Call>(x.nodes[*id].data).direct_symbol = other_replace;
  });
  invalid_recomputed(hirs, "trait Self Place corruption recomputed",
                     [&](auto &x) {
    auto id = concrete_call(x);
    need(id.has_value(), "concrete trait Self call missing");
    auto &c = std::get<hir::Call>(x.nodes[*id].data);
    auto &place =
        std::get<hir::Place>(x.nodes[c.args[1].expression.value].data);
    place.symbol = observed_instance;
  });
}

void trait_self_callback(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "trait Self callback HIR rejected: " + text + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  // The old fixed-scalar callback remains legal even when the actual receiver
  // is a struct. The callback itself has no Self in its inout position.
  good(R"(
struct Holder { x i32; }
trait Sink {
    func use(self Self, f func(inout i32, i32) i32, slot inout i32) i32;
}
func replace(slot inout i32, value i32) i32 {
    let old = slot; slot = value; return old;
}
impl Sink for Holder {
    func use(self Self, f func(inout i32, i32) i32,
             slot inout i32) i32 { return f(inout slot, 3); }
}
func main() i32 {
    let h = Holder(x:1);
    var v i32 = 2;
    let old = h.use(replace, inout v);
    return old + v - 5;
}
)");

  // i32/bool are covered by the runtime fixture; this adds both implemented
  // floating widths, callback factories, concrete calls and constrained
  // forwarding without introducing a new type rule.
  good(R"(
trait FloatFlow {
    func apply(self Self, f func(inout Self, Self) Self,
               slot inout Self, value Self) Self;
    func factory(self Self) (func(inout Self, Self) Self);
}
func bump32(slot inout f32, value f32) f32 {
    let old = slot; slot = value; return old;
}
func bump64(slot inout f64, value f64) f64 {
    let old = slot; slot = value; return old;
}
impl FloatFlow for f32 {
    func apply(self Self, f func(inout Self, Self) Self,
               slot inout Self, value Self) Self {
        return f(inout slot, value);
    }
    func factory(self Self) (func(inout Self, Self) Self) { return bump32; }
}
impl FloatFlow for f64 {
    func apply(self Self, f func(inout Self, Self) Self,
               slot inout Self, value Self) Self {
        return f(inout slot, value);
    }
    func factory(self Self) (func(inout Self, Self) Self) { return bump64; }
}
func relay[T FloatFlow](receiver T, f func(inout T, T) T,
                        slot inout T, value T) T {
    return receiver.apply(f, inout slot, value);
}
func main() i32 {
    let o32 f32 = 1.5;
    var s32 f32 = 2.5;
    let g32 = o32.factory();
    let a = g32(inout s32, 4.5);
    let b = o32.apply(bump32, inout s32, 6.5);
    let c = relay(o32, bump32, inout s32, 8.5);
    if a != 2.5 || b != 4.5 || c != 6.5 || s32 != 8.5 { return 1; }
    let o64 f64 = 1.5;
    var s64 f64 = 2.5;
    let g64 = o64.factory();
    let d = g64(inout s64, 4.5);
    let e = o64.apply(bump64, inout s64, 6.5);
    let g = relay(o64, bump64, inout s64, 8.5);
    if d != 2.5 || e != 4.5 || g != 6.5 || s64 != 8.5 { return 2; }
    return 0;
}
)");

  // Concrete Self data now follows the finite-POD aggregate rule. A struct
  // with a trivial field and an enum are legal; an unresolved generic impl
  // stays a template, and a function field is permitted as aggregate member.
  good("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
       "struct S{x i32;} impl V for S{"
       "func bad(self Self,f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");
  good("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
       "enum E{One;} impl V for E{"
       "func bad(self Self,f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");
  unsupported("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
              "impl V for Vec[i32]{"
              "func bad(self Self,f func(inout Self,Self)Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
              "impl V for string{"
              "func bad(self Self,f func(inout Self,Self)Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
              "impl V for Unit{"
              "func bad(self Self,f func(inout Self,Self)Self){}} "
              "func main()i32{return 0;}");
  good("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
       "struct Box[U]{value U;} impl[U] V for Box[U]{"
       "func bad(self Self,f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");
  good("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
       "struct Box[U]{value U;} impl V for Box[func() i32]{"
       "func bad(self Self,f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");

  // Actual first-position inout Self is deferred at the trait declaration
  // and checked with concrete POD data at this i32 application. A callback
  // that accesses the same slot twice is a separate call-site rule.
  good("trait V{func bad(self inout Self,"
       "f func(inout Self,Self)Self);}"
       "impl V for i32{func bad(self inout Self,"
       "f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");
  good("trait V[U]{func bad(self Self,"
       "f func(inout Self,Self)Self);}"
       "impl V[i32] for i32{func bad(self Self,"
       "f func(inout Self,Self)Self){}} "
       "func main()i32{return 0;}");
  // This old source has a method-own parameter on the prototype but not on
  // the impl. It is now a precise source signature error, not a capability
  // limit. The own-consistent counterpart below still rejects the unsupported
  // Self-dependent inout callback even when actually called.
  bad("trait V{func bad[U](self Self,"
      "f func(inout Self,Self)Self);}"
      "impl V for i32{func bad(self Self,"
      "f func(inout Self,Self)Self){}} "
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  // The old source returned a Unit method from an i32 main. That independent
  // type error remains; the corrected call below exercises the newly legal
  // method-own Self callback.
  bad("trait V{func bad[U](self Self,"
      "f func(inout Self,Self)Self);}"
      "impl V for i32{func bad[U](self Self,"
      "f func(inout Self,Self)Self){}} "
      "func bump(slot inout i32,value i32)i32{"
      "let old=slot;slot=value;return old;} "
      "func main()i32{let o i32=1;"
      "let f func(inout i32,i32)i32=bump;"
      "return o.bad[i32](f);}", "E_TYPE_MISMATCH");
  good("trait V{func bad[U](self Self,"
       "f func(inout Self,Self)Self);}"
       "impl V for i32{func bad[U](self Self,"
       "f func(inout Self,Self)Self){}} "
       "func bump(slot inout i32,value i32)i32{"
       "let old=slot;slot=value;return old;} "
       "func main()i32{let o i32=1;"
       "let f func(inout i32,i32)i32=bump;"
       "o.bad[i32](f);return 0;}");
  // A trait-static prototype has no receiver, but its callback's first inout
  // Self is callback data, not an actual self. Type qualification supplies
  // every declared parameter and is now accepted with a real POD Self.
  good("trait V{func bad(f func(inout Self,Self)Self);}"
       "func replace(slot inout i32,value i32)i32{"
       "let old=slot;slot=value;return old;}"
       "impl V for i32{func bad(f func(inout Self,Self)Self){}} "
       "func main()i32{let f func(inout i32,i32)i32=replace;"
       "i32.bad(f);return 0;}");
  bad("trait V{func bad(self Self,f func(inout Self,Self)Self);}"
      "impl V for i32{func bad(self Self,"
      "f func(inout Self,Self)Self){}} "
      "func main()i32{let o i32=1;let m=o.bad;return 0;}",
      "E_TYPE_MISMATCH");
  bad("func main()i32{var x i32=1;"
      "let f func(inout i32,i32)i32="
      "func(slot inout i32,value i32)i32{slot=x;return slot;};"
      "return 0;}", "E_CAPABILITY_UNAVAILABLE");
  bad("trait V{func bad(self Self,f func(inout Self,Self)Self,"
      "slot inout Self)Self;}"
      "impl V for i32{func bad(self Self,f func(inout Self,Self)Self,"
      "slot inout Self)Self{return f(inout slot,slot);}} "
      "func main()i32{return 0;}", "E_CALL_ARGUMENT");

  auto fixture = check(root /
                       "tests/generics/method-inout/trait-self-callback/lang.toml");
  need(fixture.success(), messages(fixture));
  need(hir::verify(*fixture.value).success(),
       "trait Self callback fixture HIR rejected");
  const auto &fm = *fixture.value;
  auto source_name = [](const hir::HirModule &m, SymbolId s)
      -> const std::string & { return m.resolved->symbols.at(s.value).name; };
  auto is_i32 = [](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto is_bool = [](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::Bool;
  };
  auto trait_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (source_name(m, t.symbol) == n)
        return &t;
    return nullptr;
  };
  auto prototype_for = [&](const hir::HirModule &m, const hir::TraitDecl &tr,
                           const std::string &n)
      -> const hir::MethodPrototype * {
    for (const auto &p : tr.methods)
      if (source_name(m, p.symbol) == n)
        return &p;
    return nullptr;
  };
  auto template_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (source_name(m, t.function.symbol) == n)
        return &t;
    return nullptr;
  };
  auto function_for = [&](const hir::HirModule &m, SymbolId id)
      -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto type_name = [&](const hir::HirModule &m, TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t)
      return {};
    switch (t->kind) {
    case hir::TypeKind::Bool:
      return "bool";
    case hir::TypeKind::Float:
      return t->bits == 32 ? "f32" : "f64";
    case hir::TypeKind::SignedInt:
    case hir::TypeKind::UnsignedInt:
      return std::string(t->kind == hir::TypeKind::SignedInt ? "i" : "u") +
             std::to_string(t->bits);
    default:
      return {};
    }
  };
  auto impl_for = [&](const hir::HirModule &m, SymbolId trait,
                      const std::string &type) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == trait && type_name(m, im.self_type) == type)
        return &im;
    return nullptr;
  };
  auto impl_method = [&](const hir::HirModule &m, const hir::ImplDecl &im,
                         const std::string &method) -> SymbolId {
    for (auto s : im.methods)
      if (source_name(m, s) == method)
        return s;
    throw std::runtime_error("trait callback impl method missing: " + method);
  };
  auto origin_of = [&](const hir::HirModule &m, SymbolId s) {
    for (const auto &i : m.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  auto instance_for = [&](const hir::HirModule &m,
                          const hir::FunctionTemplate &t, TypeId arg)
      -> const hir::FunctionInstance * {
    for (const auto &i : m.instances)
      if (i.origin == t.function.symbol && i.type_args.size() == 1 &&
          i.type_args[0] == arg)
        return &i;
    return nullptr;
  };
  auto has_parameter = [](const hir::HirModule &m, TypeId root) {
    std::function<bool(TypeId)> visit = [&](TypeId id) {
      auto t = m.types.find(id);
      if (!t)
        return false;
      if (t->kind == hir::TypeKind::TypeParameter)
        return true;
      for (auto a : t->type_args)
        if (visit(a))
          return true;
      for (const auto &p : t->parameters)
        if (visit(p.type))
          return true;
      if (visit(t->result))
        return true;
      return t->throws_type && visit(*t->throws_type);
    };
    return visit(root);
  };
  auto type_id = [&](const auto &predicate) {
    for (size_t i = 1; i <= fm.types.size(); ++i)
      if (predicate(fm, TypeId{i}))
        return TypeId{i};
    throw std::runtime_error("fixture scalar type missing");
  };
  const TypeId i32_type = type_id(is_i32);
  const TypeId bool_type = type_id(is_bool);

  const auto *flow = trait_for(fm, "CallbackFlow");
  need(flow, "trait callback fixture trait missing");
  auto is_self = [&](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::TypeParameter &&
           t->declaration == flow->self;
  };
  const auto *apply_proto = prototype_for(fm, *flow, "apply");
  const auto *factory_proto = prototype_for(fm, *flow, "factory");
  need(apply_proto && factory_proto, "trait callback prototypes missing");
  need(apply_proto->parameters.front().label == "self",
       "trait callback receiver label changed");
  {
    const auto *sig = fm.types.find(apply_proto->signature);
    need(sig && sig->kind == hir::TypeKind::Function &&
             sig->parameters.size() == 5,
         "trait callback apply signature shape changed");
    need(sig->parameters[0].mode == hir::ParamMode::ordinary &&
             is_self(fm, sig->parameters[0].type),
         "trait callback actual self is not ordinary Self");
    need(sig->parameters[2].mode == hir::ParamMode::inout &&
             is_self(fm, sig->parameters[2].type),
         "trait callback top-level inout Self changed");
    const auto *cb = fm.types.find(sig->parameters[1].type);
    need(cb && cb->kind == hir::TypeKind::Function &&
             cb->parameters.size() == 2 &&
             cb->parameters[0].mode == hir::ParamMode::inout &&
             is_self(fm, cb->parameters[0].type) &&
             is_self(fm, cb->parameters[1].type) &&
             is_self(fm, cb->result),
         "callback parameter did not preserve inout Self at position zero");
  }
  {
    const auto *sig = fm.types.find(factory_proto->signature);
    need(sig && sig->kind == hir::TypeKind::Function &&
             sig->result.value,
         "trait callback factory signature missing");
    const auto *cb = fm.types.find(sig->result);
    need(cb && cb->kind == hir::TypeKind::Function &&
             cb->parameters.size() == 2 &&
             cb->parameters[0].mode == hir::ParamMode::inout &&
             is_self(fm, cb->parameters[0].type) &&
             is_self(fm, cb->parameters[1].type) &&
             is_self(fm, cb->result),
         "returned function type lost inout Self at position zero");
  }

  const auto *i32_impl = impl_for(fm, flow->symbol, "i32");
  const auto *bool_impl = impl_for(fm, flow->symbol, "bool");
  need(i32_impl && bool_impl, "trait callback scalar impls missing");
  const SymbolId apply_i32 = impl_method(fm, *i32_impl, "apply");
  const SymbolId apply_bool = impl_method(fm, *bool_impl, "apply");
  {
    const auto *fn = function_for(fm, apply_i32);
    need(fn, "trait callback i32 apply function missing");
    const auto *sig = fm.types.find(fn->signature);
    need(sig && sig->parameters.size() == 5 &&
             sig->parameters[0].mode == hir::ParamMode::ordinary &&
             is_i32(fm, sig->parameters[0].type),
         "concrete callback method lost ordinary i32 self");
    const auto *cb = fm.types.find(sig->parameters[1].type);
    need(cb && cb->kind == hir::TypeKind::Function &&
             cb->parameters.size() == 2 &&
             cb->parameters[0].mode == hir::ParamMode::inout &&
             is_i32(fm, cb->parameters[0].type) &&
             is_i32(fm, cb->parameters[1].type) &&
             is_i32(fm, cb->result),
         "concrete callback parameter did not substitute Self to i32");
  }

  SymbolId replace_symbol{}, reset_symbol{};
  for (const auto &fn : fm.functions) {
    if (source_name(fm, fn.symbol) == "replace")
      replace_symbol = fn.symbol;
    if (source_name(fm, fn.symbol) == "reset")
      reset_symbol = fn.symbol;
  }
  need(replace_symbol.value && reset_symbol.value,
       "trait callback free function values missing");
  size_t replace_values = 0, reset_values = 0, indirect_calls = 0;
  for (const auto &n : fm.nodes) {
    if (auto v = std::get_if<hir::FunctionValue>(&n.data)) {
      for (const auto &p : flow->methods)
        need(v->symbol != p.symbol, "trait method value escaped as a callback");
      if (v->symbol == replace_symbol) {
        ++replace_values;
        auto t = fm.types.find(n.type);
        need(t && t->kind == hir::TypeKind::Function &&
                 t->parameters.size() == 2 &&
                 t->parameters[0].mode == hir::ParamMode::inout &&
                 is_i32(fm, t->parameters[0].type) &&
                 is_i32(fm, t->parameters[1].type) &&
                 is_i32(fm, t->result),
             "replace function value lost its concrete callback type");
      }
      if (v->symbol == reset_symbol) {
        ++reset_values;
        auto t = fm.types.find(n.type);
        need(t && t->kind == hir::TypeKind::Function &&
                 t->parameters.size() == 2 &&
                 t->parameters[0].mode == hir::ParamMode::inout &&
                 is_bool(fm, t->parameters[0].type) &&
                 is_bool(fm, t->parameters[1].type) &&
                 is_bool(fm, t->result),
             "reset function value lost its concrete callback type");
      }
    }
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->kind == hir::CalleeKind::function_value)
      ++indirect_calls;
  }
  need(replace_values >= 1 && reset_values >= 1 && indirect_calls >= 1,
       "callback free function value or indirect call missing");

  const auto *through = template_for(fm, "through");
  const auto *outer = template_for(fm, "outer");
  const auto *make = template_for(fm, "make");
  need(through && outer && make, "callback forwarding templates missing");
  const auto *through_i32 = instance_for(fm, *through, i32_type);
  const auto *outer_i32 = instance_for(fm, *outer, i32_type);
  const auto *make_bool = instance_for(fm, *make, bool_type);
  need(through_i32 && outer_i32 && make_bool,
       "callback template instances missing");
  const auto *through_fn = function_for(fm, through_i32->symbol);
  const auto *outer_fn = function_for(fm, outer_i32->symbol);
  const auto *make_fn = function_for(fm, make_bool->symbol);
  need(through_fn && outer_fn && make_fn,
       "callback instance functions missing");
  need(!has_parameter(fm, through_fn->signature) &&
           !has_parameter(fm, outer_fn->signature) &&
           !has_parameter(fm, make_fn->signature),
       "callback instance retained template type parameters");
  {
    const auto *sig = fm.types.find(through_fn->signature);
    need(sig && sig->parameters.size() == 5 &&
             sig->parameters[1].type.value &&
             is_i32(fm, sig->parameters[2].type),
         "through[i32] signature did not substitute its callback/slot");
    const auto *cb = fm.types.find(sig->parameters[1].type);
    need(cb && cb->kind == hir::TypeKind::Function &&
             cb->parameters[0].mode == hir::ParamMode::inout &&
             is_i32(fm, cb->parameters[0].type),
         "through[i32] callback type is not concrete");
  }
  {
    const auto *sig = fm.types.find(make_fn->signature);
    const auto *cb = sig ? fm.types.find(sig->result) : nullptr;
    need(cb && cb->kind == hir::TypeKind::Function &&
             cb->parameters[0].mode == hir::ParamMode::inout &&
             is_bool(fm, cb->parameters[0].type) &&
             is_bool(fm, cb->result),
         "factory-returning instance lost its concrete callback result");
  }
  size_t through_apply = 0, outer_forward = 0;
  for (const auto &n : fm.nodes) {
    if (fm.scopes[n.scope.value].function == through_fn->symbol) {
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(fm, *c->direct_symbol) == apply_i32) {
        const auto *sig = fm.types.find(c->signature);
        need(sig && sig->parameters.size() == 5 &&
                 sig->parameters[2].mode == hir::ParamMode::inout &&
                 is_i32(fm, sig->parameters[2].type) &&
                 c->args[2].mode == hir::ParamMode::inout &&
                 fm.nodes[c->args[2].expression.value].kind() ==
                     hir::NodeKind::place,
             "through instance did not call the concrete callback method");
        ++through_apply;
      }
    }
    if (fm.scopes[n.scope.value].function == outer_fn->symbol) {
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(fm, *c->direct_symbol) == through->function.symbol) {
        const auto *sig = fm.types.find(c->signature);
        const auto *cb = sig ? fm.types.find(sig->parameters[1].type) : nullptr;
        need(cb && cb->kind == hir::TypeKind::Function &&
                 cb->parameters[0].mode == hir::ParamMode::inout &&
                 is_i32(fm, cb->parameters[0].type),
             "same-constraint forwarding did not preserve the callback");
        ++outer_forward;
      }
    }
  }
  need(through_apply == 1 && outer_forward == 1,
       "callback concrete call or same-constraint forwarding missing");

  // A separate source gives two same-signature free functions so a corrupted
  // FunctionValue can keep its type while naming the other implementation.
  auto value_hir = good(R"(
trait FactoryFlow {
    func factory(self Self) (func(inout Self, Self) Self);
}
func replace(slot inout i32, value i32) i32 {
    let old = slot; slot = value; return old;
}
func duplicate(slot inout i32, value i32) i32 {
    let old = slot; slot = value + 1; return old;
}
impl FactoryFlow for i32 {
    func factory(self Self) (func(inout Self, Self) Self) { return replace; }
}
func main() i32 { return 0; }
)");
  SymbolId duplicate_symbol{};
  for (const auto &fn : value_hir.functions)
    if (source_name(value_hir, fn.symbol) == "duplicate")
      duplicate_symbol = fn.symbol;
  need(duplicate_symbol.value, "same-signature callback helper missing");

  // Nested callback mode/type, a foreign owner type parameter, an
  // unsubstituted Self, the concrete target/instance key, and the top-level
  // inout Place are checked from real HIR, not only by the producer.
  const hir::HirModule fixture_hir = *fixture.value;
  const TypeId self_type =
      fm.types.find(apply_proto->signature)->parameters[0].type;
  const TypeId through_t =
      fm.types.find(through->function.signature)->parameters[0].type;

  invalid(fixture_hir, "trait callback nested mode changed", [&](auto &x) {
    for (auto &tr : x.traits)
      if (tr.symbol == flow->symbol)
        for (auto &p : tr.methods)
          if (p.symbol == apply_proto->symbol) {
            auto sig = *x.types.find(p.signature);
            auto cb = *x.types.find(sig.parameters[1].type);
            cb.parameters[0].mode = hir::ParamMode::ordinary;
            sig.parameters[1].type = x.types.intern(cb);
            p.signature = x.types.intern(sig);
            return;
          }
    throw std::runtime_error("trait callback prototype missing");
  });
  invalid(fixture_hir, "trait callback wrong owner type", [&](auto &x) {
    for (auto &tr : x.traits)
      if (tr.symbol == flow->symbol)
        for (auto &p : tr.methods)
          if (p.symbol == apply_proto->symbol) {
            auto sig = *x.types.find(p.signature);
            auto cb = *x.types.find(sig.parameters[1].type);
            cb.parameters[0].type = through_t;
            sig.parameters[1].type = x.types.intern(cb);
            p.signature = x.types.intern(sig);
            return;
          }
    throw std::runtime_error("trait callback prototype missing");
  });
  invalid(fixture_hir, "trait callback instance retained Self",
          [&](auto &x) {
    for (auto &f : x.functions)
      if (f.symbol == through_i32->symbol) {
        auto sig = *x.types.find(f.signature);
        auto cb = *x.types.find(sig.parameters[1].type);
        cb.parameters[0].type = self_type;
        sig.parameters[1].type = x.types.intern(cb);
        f.signature = x.types.intern(sig);
        return;
      }
    throw std::runtime_error("through[i32] instance missing");
  });
  auto concrete_apply_call = [&](hir::HirModule &x)
      -> std::optional<size_t> {
    for (size_t i = 0; i < x.nodes.size(); ++i) {
      const auto &n = x.nodes[i];
      if (x.scopes[n.scope.value].function != through_i32->symbol)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(x, *c->direct_symbol) == apply_i32)
        return i;
    }
    return std::nullopt;
  };
  invalid_recomputed(fixture_hir, "trait callback concrete target changed",
                     [&](auto &x) {
    auto id = concrete_apply_call(x);
    need(id.has_value(), "concrete callback call missing");
    std::get<hir::Call>(x.nodes[*id].data).direct_symbol = apply_bool;
  });
  invalid(fixture_hir, "trait callback instance args changed",
          [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == through_i32->symbol) {
        i.type_args[0] = bool_type;
        return;
      }
    throw std::runtime_error("through[i32] instance missing");
  });
  invalid_recomputed(fixture_hir, "trait callback inout Place aliased",
                     [&](auto &x) {
    auto id = concrete_apply_call(x);
    need(id.has_value(), "concrete callback call missing");
    auto &c = std::get<hir::Call>(x.nodes[*id].data);
    auto &place =
        std::get<hir::Place>(x.nodes[c.args[2].expression.value].data);
    place.symbol = through_fn->parameters[3].symbol;
  });
  invalid(value_hir, "trait callback function value source changed",
          [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto v = std::get_if<hir::FunctionValue>(&n.data);
          v && source_name(x, v->symbol) == "replace") {
        v->symbol = duplicate_symbol;
        return;
      }
    throw std::runtime_error("replace function value missing");
  });
}


void trait_static_calls() {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto published = hir::verify(*r.value);
    need(published.success(), "trait-static HIR rejected: " + messages(published));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  auto packages = [&](const std::string &lib, const std::string &app) {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang", "package lib; " + lib);
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; " + app);
    return check(t.path / "lang.toml");
  };

  const std::string maker =
      "trait Maker{func make(seed i32)Self;func zero()Self;}"
      "struct Box[T]{value i32;}"
      "impl[U] Maker for Box[U]{func make(seed i32)Self{return Box[U](value:seed);}"
      "func zero()Self{return Box[U](value:0);}} ";
  const std::string echo =
      "trait Echo{func copy(owner Self,delta i32)Self;}"
      "impl[U] Echo for Box[U]{func copy(owner Self,delta i32)Self{"
      "return Box[U](value:owner.value+delta);}} ";
  const std::string build =
      "func build[T Maker](seed i32)T{return T.make(seed);}"
      "func zero[T Maker]()T{return T.zero();}"
      "func relay[T Maker](seed i32)T{return build[T](seed);}"
      "func copy_value[T Echo](value T,delta i32)T{"
      "return T.copy(delta:delta,owner:value);} ";
  auto generic = good(
      maker + echo + build +
      "func main()i32{let a=Box[i32].make(3);let b=build[Box[i64]](4);"
      "let c=relay[Box[i32]](5);let d=zero[Box[i32]]();"
      "let e=copy_value[Box[i32]](a,4);"
      "return a.value+b.value+c.value+d.value+e.value-23;}");
  need(generic.instances.size() >= 5,
       "trait-static generic impl did not instantiate every concrete key");
  std::set<std::vector<uint64_t>> keys;
  for (const auto &instance : generic.instances) {
    std::vector<uint64_t> key;
    for (auto t : instance.type_args)
      key.push_back(t.value);
    need(!key.empty(), "trait-static instance lost its type arguments");
    keys.insert(key);
  }
  need(keys.size() >= 3, "phantom impl type arguments were not kept distinct");

  auto ordered = good(
      "trait Ordered{func pick(first i32,second i32)i32;}"
      "impl Ordered for i32{func pick(first i32,second i32)i32{"
      "return first*10+second;}} "
      "func order_test[T Ordered](log inout i64)i32{"
      "return T.pick(second:markv(inout log,3),first:markv(inout log,2));}"
      "func markv(log inout i64,digit i64)i32{log=log*10+digit;return i32(digit);}"
      "func main()i32{var log i64=0;let x=order_test[i32](inout log);"
      "if log!=32{return 1;}return x-23;}");
  need(!ordered.instances.empty(), "named reorder template did not instantiate");
  bool saw_static = false;
  for (const auto &n : ordered.nodes)
    if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
      saw_static = true;
      need(n.children.size() == c->args.size(),
           "trait-static ConstraintCall retained a synthetic receiver");
      need(c->args.size() == 2 && !n.children.empty(),
           "trait-static named arguments were lost");
      need(c->args[0].source_index == 0 &&
               c->args[0].parameter_index == 1 &&
               c->args[0].label == std::string("second") &&
               c->args[1].source_index == 1 &&
               c->args[1].parameter_index == 0 &&
               c->args[1].label == std::string("first"),
           "named trait-static arguments were sorted by parameter");
    }
  need(saw_static, "trait-static named ConstraintCall missing");

  auto same_name = good(
      "trait Left{func pick(self Self)i32;func zero()i32;}"
      "trait Right{func pick(self Self)i32;func zero()i32;}"
      "struct Box{} impl Left for Box{func pick(self Self)i32{return 1;}"
      "func zero()i32{return 1;}}"
      "impl Right for Box{func pick(self Self)i32{return 2;}"
      "func zero()i32{return 2;}}"
      "func left[T Left]()i32{return T.zero();}"
      "func both[T Left](x T)i32{let a=T.zero();let b=x.pick();return a+b;}"
      "func main()i32{return left[Box]();}");
  const hir::ConstraintCall *zero_call = nullptr;
  const hir::ConstraintCall *value_call = nullptr;
  for (const auto &n : same_name.nodes)
    if (auto c = std::get_if<hir::ConstraintCall>(&n.data))
      if (c->args.empty())
        zero_call = c;
      else
        value_call = c;
  need(zero_call && value_call, "zero/value trait-static calls missing");
  auto zero_receiver = same_name.types.find(zero_call->receiver);
  need(zero_call->args.empty() && zero_receiver &&
           zero_receiver->kind == hir::TypeKind::TypeParameter,
       "zero-argument trait-static receiver/args changed");
  need(!zero_call->args.size(), "zero-argument static call fabricated a receiver");

  auto concrete = [&](const hir::HirModule &h, const std::string &name) {
    for (const auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
        auto target = *c->direct_symbol;
        for (const auto &i : h.instances)
          if (i.symbol == target) {
            target = i.origin;
            break;
          }
        if (h.resolved->symbols[target.value].name == name)
          return &n;
      }
    return static_cast<const hir::HirNode *>(nullptr);
  };
  need(concrete(same_name, "zero"), "trait-static concrete call missing");
  for (const auto &n : same_name.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
      auto target = *c->direct_symbol;
      for (const auto &i : same_name.instances)
        if (i.symbol == target) {
          target = i.origin;
          break;
        }
      const auto *impl = [&]() -> const hir::ImplDecl * {
        for (const auto &im : same_name.impls)
          if (std::find(im.methods.begin(), im.methods.end(), target) !=
              im.methods.end())
            return &im;
        return nullptr;
      }();
      if (impl && impl->trait)
        need(same_name.resolved->symbols[impl->trait->declaration.value].name == "Left",
             "Left-bound call selected Right.pick");
    }

  auto enum_pick = good(
      "enum E{A(x i32);} trait Pick{func A(seed i32)Self;}"
      "impl Pick for E{func A(seed i32)Self{return E.A(x:seed);}}"
      "func build[T Pick](seed i32)T{return T.A(seed);}"
      "func main()i32{switch build[E](7){case E.A(x):return x;}}");
  bool enum_call = false;
  for (const auto &n : enum_pick.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
      auto target = *c->direct_symbol;
      for (const auto &i : enum_pick.instances)
        if (i.symbol == target) {
          target = i.origin;
          break;
        }
      if (enum_pick.resolved->symbols[target.value].name == "A")
        enum_call = true;
    }
  need(enum_call, "T.bound enum method was not a concrete Call");

  good(maker + echo +
       "func main()i32{let T=Box[i32](value:3);let v=T.copy(delta:4);"
       "return v.value-7;}");

  good(
      "trait Apply{func apply(cb func(Self)Self,value Self)Self;}"
      "impl Apply for i32{func apply(cb func(i32)i32,value i32)i32{"
      "return cb(value);}}"
      "func use[T Apply](value T)T{let cb func(T)T=func(x T)T{return x;};"
      "return T.apply(cb,value);}"
      "func main()i32{return use[i32](7)-7;}");

  auto cross = packages(
      "pub trait Maker{func make(seed i32)Self;}"
      "pub struct Box{pub value i32;}"
      "impl Maker for Box{func make(seed i32)Self{return Box(value:seed);}}"
      "pub func build[T Maker](seed i32)T{return T.make(seed);}",
      "func main()i32{return lib.build[lib.Box](7).value;}");
  need(cross.success(), messages(cross));
  need(hir::verify(*cross.value).success(), "cross-package trait-static HIR rejected");

  auto direct = good(
      "trait Maker{func make(seed i32)Self;}"
      "struct A{value i32;} struct B{value i32;}"
      "impl Maker for A{func make(seed i32)Self{return A(value:seed);}}"
      "impl Maker for B{func make(seed i32)Self{return B(value:seed);}}"
      "func main()i32{return A.make(3).value-3;}");
  invalid(direct, "concrete Type.method target from another impl", [&](auto &h) {
    SymbolId other{};
    for (const auto &im : h.impls)
      if (im.trait) {
        const auto t = h.types.find(im.self_type);
        if (t && t->kind == hir::TypeKind::NominalStruct &&
            h.resolved->symbols[t->declaration.value].name == "B")
          other = im.methods[0];
      }
    need(other.value, "other impl method missing");
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
        auto target = *c->direct_symbol;
        for (const auto &i : h.instances)
          if (i.symbol == target) {
            target = i.origin;
            break;
          }
        if (h.resolved->symbols[target.value].name == "make") {
          c->direct_symbol = other;
          return;
        }
      }
    throw std::runtime_error("concrete Type.method target missing");
  });
  invalid(generic, "trait-static instance type arguments", [&](auto &h) {
    for (auto &i : h.instances)
      if (!i.type_args.empty()) {
        i.type_args[0] = TypeId{1};
        return;
      }
    throw std::runtime_error("trait-static instance missing");
  });
  invalid(generic, "trait-static instance origin", [&](auto &h) {
    SymbolId other{};
    for (const auto &t : h.templates)
      if (h.resolved->symbols[t.function.symbol.value].name == "zero")
        other = t.function.symbol;
    need(other.value, "zero template missing");
    for (auto &i : h.instances) {
      const auto &origin = h.resolved->symbols[i.origin.value].name;
      if (origin == "build") {
        i.origin = other;
        return;
      }
    }
    throw std::runtime_error("build instance missing");
  });

  bad("trait Maker{func make(seed i32)Self;} func f[T](x T){T.make(1);}",
      "E_TYPE_MISMATCH");
  bad("trait A{func get(self Self)i32;} trait B{} func f[T B](x T){T.get();}",
      "E_TYPE_MISMATCH");
  bad("trait Maker{func make(seed i32)Self;} struct Box{value i32;}"
      "func build[T Maker](seed i32)T{return T.make(seed);}"
      "func main()i32{let b=build[Box](1);return 0;}",
      "E_TYPE_MISMATCH");
  bad("trait Maker{func make(seed i32)Self;} struct Box{value i32;}"
      "func main(){T.make(1);}", "E_NAME_NOT_FOUND");
  bad("trait Maker{func make(seed i32)Self;}"
      "func build[T Maker](seed i32)T{return T.other(seed);}", "E_TYPE_MISMATCH");
  bad(maker + echo + build +
          "func main()i32{return build[Box[i32]](nope:1).value;}",
      "E_CALL_ARGUMENT");
  // `make` has no method-own generic parameter. The explicit suffix is now a
  // precise wrong-arity source error instead of the old blanket capability
  // message that covered all method generics.
  bad(maker +
          "func build[T Maker](seed i32)T{return T.make[i32](seed);}",
      "E_TYPE_MISMATCH");
  bad("trait Left{func pick()i32;} trait Right{func pick()i32;}"
      "struct Box{} impl Left for Box{func pick()i32{return 1;}}"
      "impl Right for Box{func pick()i32{return 2;}}"
      "func main()i32{return Box.pick();}", "E_TYPE_MISMATCH");
  bad("enum E{A(x i32);} impl E{func A(seed i32)E{return E.A(x:seed);}} "
      "func main()i32{let e=E.A();return 0;}", "E_CALL_ARGUMENT");
  bad("enum E{Bad;} trait Checked{func make(seed i32)Self throws E;}"
      "struct Box{value i32;}"
      "impl Checked for Box{func make(seed i32)Self throws E{return Box(value:seed);}}"
      "func build[T Checked](seed i32)T{return T.make(seed);}"
      "func main()i32{return 0;}", "E_EFFECT_UNHANDLED");
  // The Type/T actual-Self route is open; moving or borrowing an ordinary
  // parameter is still the ordinary source argument error.
  bad("trait Take{func take(value move Self)Self;}"
      "func f[T Take](x T)T{return T.take(move x);}",
      "E_CALL_ARGUMENT");
  bad("trait Bump{func bump(slot inout Self);}"
      "func f[T Bump](x T){T.bump(inout x);}",
      "E_CALL_ARGUMENT");
  good("trait Callback{func apply(cb func(inout Self)Self)Self;}"
       "func f[T Callback](cb func(inout T)T)T{return T.apply(cb);}");

  auto damaged = good(
      "trait Maker{func make(seed i32)i32;}"
      "impl Maker for i32{func make(seed i32)i32{return seed;}}"
      "func build[T Maker](seed i32)i32{return T.make(seed);}"
      "func main()i32{return 0;}");
  auto constraint = [&](hir::HirModule &h) -> HirNodeId {
    for (size_t i = 0; i < h.nodes.size(); ++i)
      if (std::holds_alternative<hir::ConstraintCall>(h.nodes[i].data))
        return HirNodeId{i};
    throw std::runtime_error("trait-static corruption ConstraintCall missing");
  };
  auto cid = constraint(damaged);
  invalid(damaged, "trait-static fabricated receiver", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.args.push_back({0, 0, {}, hir::ParamMode::ordinary, cid});
  });
  invalid(damaged, "trait-static call index", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.args[0].parameter_index = 3;
  });
  invalid_recomputed(damaged, "trait-static call label", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.args[0].label = std::string("wrong");
  });
  invalid(damaged, "trait-static call source order", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.args[0].source_index = 4;
  });
  invalid(damaged, "trait-static call mode", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.args[0].mode = hir::ParamMode::inout;
  });
  invalid(damaged, "trait-static receiver type", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    c.receiver = c.signature;
  });
  invalid(damaged, "trait-static signature substitution", [&](auto &h) {
    auto &c = std::get<hir::ConstraintCall>(h.nodes[cid.value].data);
    auto sig = *h.types.find(c.signature);
    sig.result = TypeId{1};
    c.signature = h.types.intern(sig);
  });
  auto damaged_same = good(
      "trait Left{func pick(self Self)i32;func zero()i32;}"
      "trait Right{func pick(self Self)i32;func zero()i32;}"
      "struct Box{} impl Left for Box{func pick(self Self)i32{return 1;}"
      "func zero()i32{return 1;}}"
      "impl Right for Box{func pick(self Self)i32{return 2;}"
      "func zero()i32{return 2;}}"
      "func both[T Left](x T)i32{let a=T.zero();let b=x.pick();"
      "return a+b;} func main()i32{return both[Box](Box());}");
  auto static_id = constraint(damaged_same);
  HirNodeId value_id{};
  for (size_t i = 0; i < damaged_same.nodes.size(); ++i)
    if (auto c = std::get_if<hir::ConstraintCall>(
            &damaged_same.nodes[i].data);
        c && !c->args.empty())
      value_id = HirNodeId{i};
  need(value_id.value, "value ConstraintCall missing for source-form damage");
  invalid(damaged_same, "constraint source form changed", [&](auto &h) {
    h.nodes[static_id.value].source.node =
        h.nodes[value_id.value].source.node;
  });
  invalid(damaged_same, "value ConstraintCall receiver deleted", [&](auto &h) {
    auto &n = h.nodes[value_id.value];
    auto &c = std::get<hir::ConstraintCall>(n.data);
    c.args.erase(c.args.begin());
    n.children.erase(n.children.begin());
  });
  invalid(damaged_same, "constraint trait substitution", [&](auto &h) {
    const auto *right = [&]() -> const hir::TraitDecl * {
      for (const auto &t : h.traits)
        if (h.resolved->symbols[t.symbol.value].name == "Right")
          return &t;
      return nullptr;
    }();
    need(right != nullptr, "Right trait missing");
    auto &c = std::get<hir::ConstraintCall>(h.nodes[static_id.value].data);
    c.trait = hir::TraitRef{right->symbol, {}};
    c.method = right->methods[0].symbol;
  });
}
void trait_method_generics(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto verified = hir::verify_types(*r.value);
    need(verified.success(), "trait method generics HIR rejected: " + text +
                                 "\n" + messages(verified));
    auto published = hir::verify(*r.value);
    need(published.success(), "trait method generics did not publish: " + text +
                                  "\n" + messages(published));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string values =
      "trait V{func get(owner Self)i32;}"
      "struct One{n i32;} struct Two{n i32;}"
      "impl V for One{func get(owner Self)i32{return 1;}}"
      "impl V for Two{func get(owner Self)i32{return 2;}}";

  // Multiple own parameters, positional alpha renaming, a real bound on one
  // own parameter, both inferred and explicit calls, and an uncalled template.
  good(values +
       "trait Mix{func mix[A,B V](owner Self,first A,second B)i32;}"
       "struct Box[T]{value T;}"
       "impl[T] Mix for Box[T]{"
       "func mix[U,W V](owner Self,first U,second W)i32{"
       "return second.get()+1;}}"
       "func never[T Mix,A,B V](x T,a A,b B)i32{return T.mix[A,B](x,a,b);}"
       "func main()i32{let b=Box[i32](value:0);"
       "let a=b.mix(1,One(n:0));"
       "let c i32=Box[i32].mix[i32,Two](b,1,Two(n:0));"
       "let d=b.mix[bool,One](true,One(n:0));"
       "return a+c+d-7;}");

  // A generic, noncapturing lambda carries the enclosing template's real
  // instance substitution into the method-own argument.
  good(values +
       "trait Pick{func pick[U V](owner Self)i32;}"
       "struct Box[T]{value T;}"
       "impl[T] Pick for Box[T]{func pick[U V](owner Self)i32{return 1;}}"
       "func factory[T Pick]()(func(T) i32){"
       "return func(x T)i32{return T.pick[One](x);};}"
       "func main()i32{let b=Box[i32](value:0);"
       "return factory[Box[i32]]()(b)-1;}");

  // A fixed-scalar callback remains ordinary inside a generic trait method;
  // only a callback inout that depends on the method-own or Self parameter is
  // a new capability limit.
  good(values +
       "trait CB{func run[U V](owner Self,cb func(inout i32,i32)i32,tag U)i32;}"
       "struct H{}"
       "impl CB for H{func run[W V](owner Self,"
       "cb func(inout i32,i32)i32,tag W)i32{"
       "var slot i32=1;return cb(inout slot,2);}}"
       "func bump(slot inout i32,value i32)i32{"
       "let old=slot;slot=value;return old;}"
       "func main()i32{let h=H();"
       "let f func(inout i32,i32)i32=bump;"
       "return h.run[One](f,One(n:0))-1;}");

  // A zero-argument generic trait method (besides the receiver) keeps its
  // phantom own parameter through value, Type, and bounded T entries.
  good(values +
       "trait Pick{func pick[U V](owner Self)i32;}"
       "struct S{}"
       "impl Pick for S{func pick[U V](owner Self)i32{return 1;}}"
       "func via[T Pick,U V](x T)i32{return T.pick[U](x);}"
       "func main()i32{let s=S();"
       "return s.pick[(One)]()+S.pick[Two](s)+via[S,One](s)-3;}");

  // Ordinary owned struct/enum parameters and returns use method-own types.
  good("enum E{V(value i32);} struct S{value i32;}"
       "trait Id{func id[U](owner Self,value U)U;}"
       "struct H{}"
       "impl Id for H{func id[U](owner Self,value U)U{return value;}}"
       "func main()i32{let h=H();let e=h.id[E](E.V(value:7));"
       "let s=h.id[S](S(value:8));"
       "switch e{case E.V(x):return x+s.value-15;}}");

  // A type parameter may itself be used as the method-own value type.
  good(values +
       "trait Wrap{func keep[U,W V](owner Self,value U,tag W)U;}"
       "struct Box[T]{value T;}"
       "impl[T] Wrap for Box[T]{"
       "func keep[U,X V](owner Self,value U,tag X)U{return value;}}"
       "func via[X Wrap,A,B V](x X,value A,tag B)A{"
       "return X.keep[A,B](x,value,tag);}"
       "func main()i32{let b=Box[i32](value:0);"
       "return via[Box[i32],i32,One](b,7,One(n:0))-7;}");

  // A real cross-package public trait and generic impl method.
  {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang",
          "package lib; "
          "pub trait Echo{func echo[U](owner Self,value U)U;}"
          "pub struct Box[T]{pub value i32;}"
          "impl[T] Echo for Box[T]{"
          "func echo[U](owner Self,value U)U{return value;}}");
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; "
          "func via[T lib.Echo,U](x T,value U)U{return T.echo[U](x,value);}"
          "func main()i32{let b=lib.Box[i32](value:0);"
          "let a=via[lib.Box[i32],i32](b,7);"
          "let c=lib.Box[i32].echo[i32](b,8);"
          "return a+c-15;}");
    auto r = check(t.path / "lang.toml");
    need(r.success(), messages(r));
    need(hir::verify_types(*r.value).success(),
         "cross-package generic trait method HIR rejected");
    need(hir::verify(*r.value).success(),
         "cross-package generic trait method did not publish");
  }

  // Explicit/inferred/context conflicts and missing phantom parameters are
  // source errors, not internal HIR failures.
  bad(values +
          "trait Pick{func pick[U V](owner Self)i32;}"
          "struct S{}"
          "impl Pick for S{func pick[U V](owner Self)i32{return 1;}}"
          "func main()i32{let s=S();return s.pick();}",
      "E_TYPE_MISMATCH");
  bad(values +
          "trait Pick{func pick[U V](owner Self)i32;}"
          "struct S{}"
          "impl Pick for S{func pick[U V](owner Self)i32{return 1;}}"
          "func main()i32{let s=S();return s.pick[One,Two]();}",
      "E_TYPE_MISMATCH");
  bad(values +
          "trait Echo{func echo[U](owner Self,value U)U;}"
          "struct S{}"
          "impl Echo for S{func echo[U](owner Self,value U)U{return value;}}"
          "func main()i32{let s=S();let n i32=7;"
          "return s.echo[One](n);}",
      "E_TYPE_MISMATCH");

  // Declaration shape is checked even when no call instantiates the method.
  bad(values +
          "trait Pair{func pair[A,B V](owner Self,first A,second B)i32;}"
          "struct S{}"
          "impl Pair for S{func pair[A](owner Self,first A,second A)i32{"
          "return 1;}} func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  bad(values +
          "trait Pair{func pair[A,B V](owner Self,first A,second B)i32;}"
          "struct S{}"
          "impl Pair for S{func pair[A,W](owner Self,first A,second W)i32{"
          "return 1;}} func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  bad(values +
          "trait Pair{func pair[A](owner Self,first A)i32;}"
          "struct S{}"
          "impl Pair for S{func pair[A](owner Self,second A)i32{return 1;}}"
          "func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  bad("enum E{A;} " + values +
          "trait Pair{func pair[A](owner Self,first A)i32 throws E;}"
          "struct S{}"
          "impl Pair for S{func pair[A](owner Self,first A)i32{return 1;}}"
          "func main()i32{return 0;}",
      "E_TYPE_MISMATCH");

  // New generic trait methods open top-level non-self move and inout. The
  // exact former unsupported Sources are retained and reclassified; their
  // concrete calls and executions are covered by the trait-generic fixtures.
  good(values +
       "trait Move{func take[U](owner Self,value move U)U;}"
       "struct S{}"
       "impl Move for S{func take[U](owner Self,value move U)U{"
       "return move value;}} func main()i32{return 0;}");
  good(values +
       "trait Slot{func set[U](owner Self,slot inout U);}"
       "struct S{}"
       "impl Slot for S{func set[U](owner Self,slot inout U){}}"
       "func main()i32{return 0;}");
  good(values +
       "trait Callback{func run[U](owner Self,cb func(inout U));}"
       "struct S{}"
       "impl Callback for S{func run[U](owner Self,cb func(inout U)){}}"
       "func main()i32{return 0;}");
  good(values +
       "trait Move{func take[U](self move Self,value U)U;}"
       "struct S{}"
       "impl Move for S{func take[U](self move Self,value U)U{"
       "return value;}} func main()i32{return 0;}");

  // Published HIR: the prototype's own parameter is a real type parameter,
  // while its runtime parameter list stays the declared owner Self. Returned
  // callbacks and nominal fields or payloads follow the same deferred
  // template rule, including declarations that are never instantiated.
  good("trait M{func make[U](owner Self) (func(inout U,U)U);}"
       "func main()i32{return 0;}");
  good("trait M{func make[U](owner Self) (func(inout Self,Self)Self);}"
       "func main()i32{return 0;}");
  good("struct Holder[Q]{cb func(inout Q,Q)Q;}"
       "trait M{func use[U](owner Self,value Holder[U])i32;}"
       "func main()i32{return 0;}");
  good("enum Envelope[Q]{Hidden(cb func(inout Q,Q)Q);}"
       "trait M{func use[U](owner Self,value Envelope[U])i32;}"
       "func main()i32{return 0;}");
  good("struct Holder[Q]{cb func(inout i32,i32)i32;}"
       "struct Loop[Q]{cb func(Loop[Q]);}"
       "trait M{func fixed[U](owner Self,value Holder[U])"
       "(func(inout i32,i32)i32);"
       "func ordinary[U](owner Self,value Loop[U]) (func(U)U);}"
       "func main()i32{return 0;}");

  auto phantom_result =
      check(root /
            "tests/generics/method-move/trait-generic-phantom/lang.toml");
  need(phantom_result.success(), messages(phantom_result));
  need(hir::verify_types(*phantom_result.value).success(),
       "phantom trait method fixture HIR rejected");
  need(hir::verify(*phantom_result.value).success(),
       "phantom trait method fixture did not publish");
  const auto &pm = *phantom_result.value;
  auto name = [&](SymbolId s) -> const std::string & {
    return pm.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = pm.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return name(t->declaration);
  };
  auto type_argument_name = [&](TypeId id) -> std::string {
    auto t = pm.types.find(id);
    if (!t)
      return {};
    if (t->kind == hir::TypeKind::TypeParameter)
      return name(t->declaration);
    auto nominal = nominal_name(id);
    if (!nominal.empty())
      return nominal;
    if (t->kind == hir::TypeKind::Bool)
      return "bool";
    if (t->kind == hir::TypeKind::SignedInt)
      return "i" + std::to_string(t->bits);
    return {};
  };
  const hir::TraitDecl *stamp = nullptr;
  for (const auto &t : pm.traits)
    if (name(t.symbol) == "Stamp")
      stamp = &t;
  need(stamp && stamp->methods.size() == 1, "Stamp trait missing");
  const auto &stamp_prototype = stamp->methods.front();
  need(stamp_prototype.parameters.size() == 1 &&
           stamp_prototype.parameters.front().label == "owner",
       "prototype runtime parameters were confused with own type parameters");
  const auto &prototype_sig = *pm.types.find(stamp_prototype.signature);
  need(prototype_sig.parameters.size() == 1,
       "prototype signature lost its owner Self parameter");
  const modules::Symbol *own_u = nullptr;
  for (const auto &s : pm.resolved->symbols)
    if (s.name == "U" &&
        s.kind == modules::SymbolKind::type_parameter && s.definition &&
        s.declaration &&
        s.declaration->snapshot == stamp_prototype.source.span.snapshot &&
        s.declaration->begin_byte >=
            stamp_prototype.source.span.begin_byte &&
        s.declaration->end_byte <= stamp_prototype.source.span.end_byte)
      own_u = &s;
  need(own_u, "prototype own type parameter has no real source symbol");
  std::function<bool(TypeId)> contains_declaration = [&](TypeId id) {
    auto t = pm.types.find(id);
    if (!t)
      return false;
    if (t->kind == hir::TypeKind::TypeParameter &&
        t->declaration == own_u->id)
      return true;
    for (auto a : t->type_args)
      if (contains_declaration(a))
        return true;
    for (const auto &p : t->parameters)
      if (contains_declaration(p.type))
        return true;
    if (contains_declaration(t->result))
      return true;
    return t->throws_type && contains_declaration(*t->throws_type);
  };
  need(!contains_declaration(stamp_prototype.signature),
       "phantom own parameter was mixed into the runtime signature");

  const hir::ImplDecl *stamp_impl = nullptr;
  for (const auto &im : pm.impls)
    if (im.trait && im.trait->declaration == stamp->symbol)
      stamp_impl = &im;
  need(stamp_impl, "Stamp impl missing");
  SymbolId stamp_impl_method{0};
  for (auto m : stamp_impl->methods)
    if (name(m) == "stamp")
      stamp_impl_method = m;
  need(stamp_impl_method.value != 0, "Stamp impl method missing");
  const hir::FunctionTemplate *stamp_template = nullptr;
  for (const auto &t : pm.templates)
    if (t.function.symbol == stamp_impl_method)
      stamp_template = &t;
  need(stamp_template && stamp_template->params.size() == 2,
       "generic impl prefix plus method-own template parameters missing");
  std::optional<hir::TraitRef> stamp_bound;
  for (const auto &tr : pm.traits)
    if (name(tr.symbol) == "Tag")
      stamp_bound = hir::TraitRef{tr.symbol, {}};
  need(stamp_template->params[0].symbol == stamp_impl->params[0].symbol &&
           stamp_template->params[1].trait == stamp_bound,
       "method own bound or impl prefix was lost");

  std::map<std::string, const hir::FunctionInstance *> stamp_instances;
  for (const auto &i : pm.instances)
    if (i.origin == stamp_impl_method) {
      need(i.type_args.size() == 2, "Stamp instance lost complete type args");
      auto element = type_argument_name(i.type_args[0]);
      auto tag = type_argument_name(i.type_args[1]);
      need((element == "i32" || element == "bool") &&
               (tag == "One" || tag == "Two"),
           "Stamp instance key category changed: " + element + ":" + tag);
      auto key = element + ":" + tag;
      need(stamp_instances.emplace(key, &i).second,
           "Stamp instance combination was not unique");
    }
  need(stamp_instances.size() == 4 && stamp_instances.count("i32:One") &&
           stamp_instances.count("i32:Two") &&
           stamp_instances.count("bool:One") &&
           stamp_instances.count("bool:Two"),
       "Stamp phantom instance combinations missing");

  size_t via_instances = 0;
  for (const auto &i : pm.instances)
    if (name(i.origin) == "via")
      ++via_instances;
  need(via_instances == 4, "outer Stamp forwarding instances changed");

  // Same-signature phantom instances must not be interchangeable. Swapping
  // either a concrete call target or an instance's second type argument is
  // rejected by the independent verifier.
  const hir::FunctionInstance *one_instance = stamp_instances.at("i32:One");
  const hir::FunctionInstance *two_instance = stamp_instances.at("i32:Two");
  const auto *one_function = [&]() -> const hir::HirFunction * {
    for (const auto &f : pm.functions)
      if (f.symbol == one_instance->symbol)
        return &f;
    return nullptr;
  }();
  need(one_function, "One Stamp instance function missing");
  SymbolId main_symbol{0};
  for (const auto &s : pm.resolved->symbols)
    if (s.name == "main" && s.kind == modules::SymbolKind::function)
      main_symbol = s.id;
  need(main_symbol.value != 0, "main symbol missing");
  invalid(pm, "same-signature phantom direct instance target", [&](auto &x) {
    bool done = false;
    for (auto &n : x.nodes) {
      if (x.scopes[n.scope.value].function != main_symbol)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol == one_instance->symbol) {
        c->direct_symbol = two_instance->symbol;
        done = true;
        break;
      }
    }
    need(done, "explicit phantom One call target missing");
  });
  invalid_recomputed(pm, "same-signature phantom instance arguments",
                     [&](auto &x) {
                       for (auto &i : x.instances)
                         if (i.symbol == one_instance->symbol) {
                           for (auto &j : x.instances)
                             if (j.symbol == two_instance->symbol)
                               std::swap(i.type_args[1], j.type_args[1]);
                           return;
                         }
                       throw std::runtime_error("phantom instance missing");
                     });

  // The ConstraintCall inside the outer via instance is completed to the
  // actual impl method instance. Changing that concrete target to the other
  // same-signature phantom instance must fail the full-key check.
  auto box_element_name = [&](TypeId id) -> std::string {
    auto t = pm.types.find(id);
    if (!t || t->kind != hir::TypeKind::NominalStruct ||
        t->type_args.size() != 1)
      return {};
    return type_argument_name(t->type_args[0]);
  };
  const hir::FunctionInstance *via_one = nullptr;
  for (const auto &i : pm.instances)
    if (name(i.origin) == "via" && i.type_args.size() == 2 &&
        box_element_name(i.type_args[0]) == "i32" &&
        type_argument_name(i.type_args[1]) == "One")
      via_one = &i;
  need(via_one, "via[Box[i32],One] instance missing");
  const auto *via_function = [&]() -> const hir::HirFunction * {
    for (const auto &f : pm.functions)
      if (f.symbol == via_one->symbol)
        return &f;
    return nullptr;
  }();
  need(via_function, "via One function missing");
  invalid_recomputed(pm, "constraint phantom completion target",
                     [&](auto &x) {
                       bool done = false;
                       for (auto &n : x.nodes)
                         if (x.scopes[n.scope.value].function ==
                             via_function->symbol)
                           if (auto c = std::get_if<hir::Call>(&n.data);
                               c && c->direct_symbol == one_instance->symbol) {
                             c->direct_symbol = two_instance->symbol;
                             done = true;
                             break;
                           }
                       need(done, "completed phantom ConstraintCall missing");
                     });

  // The published prototype itself and the impl method remain structurally
  // checked even without a call.
  invalid(pm, "trait generic prototype signature", [&](auto &x) {
    std::optional<TypeId> other;
    for (size_t i = 1; i <= x.types.size(); ++i) {
      auto t = x.types.find(TypeId{i});
      if (t && t->kind == hir::TypeKind::Function &&
          t->parameters.empty() && i != stamp_prototype.signature.value) {
        other = TypeId{i};
        break;
      }
    }
    need(other.has_value(), "different function signature missing");
    for (auto &tr : x.traits)
      if (tr.methods.front().symbol == stamp_prototype.symbol) {
        tr.methods.front().signature = *other;
        return;
      }
    throw std::runtime_error("Stamp prototype missing");
  });
  invalid(pm, "trait generic impl method signature", [&](auto &x) {
    for (auto &t : x.templates)
      if (t.function.symbol == stamp_impl_method) {
        t.function.signature = stamp_prototype.signature;
        return;
      }
    throw std::runtime_error("Stamp impl template missing");
  });
  invalid(pm, "trait method own bound identity", [&](auto &x) {
    for (auto &t : x.templates)
      if (t.function.symbol == stamp_impl_method) {
        t.params[1].trait.reset();
        return;
      }
    throw std::runtime_error("Stamp impl template missing");
  });
  invalid(pm, "trait method own source order", [&](auto &x) {
    for (auto &t : x.templates)
      if (t.function.symbol == stamp_impl_method) {
        std::swap(t.params[0], t.params[1]);
        return;
      }
    throw std::runtime_error("Stamp impl template missing");
  });
}


void trait_generic_move(const fs::path &root) {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "trait generic move HIR rejected: " + text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "trait generic move did not publish: " + text + "\n" +
             messages(r));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source rejection became E_IR_INVALID");
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };

  const std::string source_text = R"(
trait Tag { func code(owner Self) i32; }
struct One { n i32; }
struct Two { n i32; }
impl Tag for One { func code(owner Self) i32 { return 1; } }
impl Tag for Two { func code(owner Self) i32 { return 2; } }
trait Take {
    func take[U, V Tag](owner Self, value move U, tag V) U;
    func multi[A, B, C](owner Self, first move A, second move B, third C) A;
    func static_take[U](value move U) U;
    func echo[U](self move U) U;
    func phantom[U, V](owner Self) i32;
}
struct Box[I] { value I; }
impl[I] Take for Box[I] {
    func take[A, W Tag](owner Self, value move A, tag W) A {
        let local Vec[i32] = [1];
        defer {
            let read = local[0];
        }
        return move value;
    }
    func multi[A, B, C](owner Self, first move A, second move B, third C) A {
        return move first;
    }
    func static_take[U](value move U) U { return move value; }
    func echo[U](self move U) U { return move self; }
    func phantom[U, V](owner Self) i32 { return 1; }
}
func via[T Take, U, V Tag](owner T, value move U, tag V) U {
    return T.take[U, V](owner: owner, value: move value, tag: tag);
}
func via_static[T Take, U](value move U) U {
    return T.static_take[U](move value);
}
func never[T Take, U](x T, value move U) U {
    return T.take[U, One](x, move value, One(n: 0));
}
func factory[T Take]() (func(T) Vec[i32]) {
    return func(x T) Vec[i32] {
        let local Vec[i32] = [9];
        return T.take[Vec[i32], One](x, move local, One(n: 0));
    };
}
func main() i32 {
    let b = Box[i32](value: 0);
    var v1 Vec[i32] = [1];
    let a = b.take[Vec[i32], One](move v1, One(n: 0));
    var v2 Vec[i32] = [2];
    let c = Box[i32].take[Vec[i32], One](b, move v2, One(n: 0));
    let d = via[Box[i32], Vec[i32], One](b, [3], One(n: 0));
    var v3 Vec[i32] = [4];
    let e = b.multi[Vec[i32], Vec[i32], One](move v3, [5], One(n: 0));
    var v4 Vec[i32] = [6];
    let f = Box[i32].static_take[Vec[i32]](move v4);
    let g = via_static[Box[i32], Vec[i32]]([7]);
    let h = b.phantom[One, Two]();
    let i = Box[i32].phantom[One, Two](b);
    let b2 = Box[bool](value: false);
    let h2 = b.phantom[Two, One]();
    let i2 = Box[i32].phantom[Two, One](b);
    let h3 = b2.phantom[One, Two]();
    let i3 = Box[bool].phantom[Two, One](b2);
    let j = factory[Box[i32]]()(b);
    let eb = Box[i32](value: 0);
    let er = Box[i32].echo[Box[i32]](move eb);
    return a[0] + c[0] + d[0] + e[0] + f[0] + g[0] + h + i + h2 + i2 +
           h3 + i3 + j[0] + er.value - 15;
}
)";
  auto m = good(source_text);

  auto name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto nominal_name = [&](TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return name(t->declaration);
  };
  std::function<std::string(TypeId)> type_argument_name = [&](TypeId id)
      -> std::string {
    auto t = m.types.find(id);
    if (!t)
      return {};
    if (t->kind == hir::TypeKind::TypeParameter)
      return name(t->declaration);
    auto nominal = nominal_name(id);
    if (!nominal.empty())
      return nominal;
    if (t->kind == hir::TypeKind::Bool)
      return "bool";
    if (t->kind == hir::TypeKind::SignedInt)
      return "i" + std::to_string(t->bits);
    if (t->kind == hir::TypeKind::Vec)
      return "Vec[" +
             (t->type_args.empty() ? std::string{}
                                   : type_argument_name(t->type_args[0])) +
             "]";
    return {};
  };
  auto trait_named = [&](const std::string &n)
      -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (name(t.symbol) == n)
        return &t;
    return nullptr;
  };
  auto template_named = [&](const std::string &n)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (name(t.function.symbol) == n)
        return &t;
    return nullptr;
  };
  auto function_for = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : m.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  auto impl_for = [&](const hir::TraitDecl &tr,
                      const std::string &type) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == tr.symbol && nominal_name(im.self_type) == type)
        return &im;
    return nullptr;
  };
  auto impl_method = [&](const hir::ImplDecl &im,
                         const std::string &method) -> SymbolId {
    for (auto s : im.methods)
      if (name(s) == method)
        return s;
    throw std::runtime_error("trait generic move impl method missing");
  };
  auto instances_of = [&](SymbolId origin) {
    std::vector<const hir::FunctionInstance *> result;
    for (const auto &i : m.instances)
      if (i.origin == origin)
        result.push_back(&i);
    return result;
  };
  auto origin_of = [&](SymbolId s) {
    for (const auto &i : m.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  auto constraint_in = [&](SymbolId function)
      -> std::pair<HirNodeId, const hir::ConstraintCall *> {
    std::pair<HirNodeId, const hir::ConstraintCall *> result;
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      const auto &n = m.nodes[i];
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(!result.second, "multiple trait generic move ConstraintCalls");
        result = {HirNodeId{i}, c};
      }
    }
    need(result.second, "missing trait generic move ConstraintCall");
    return result;
  };
  auto main_symbol = [&] {
    for (const auto &s : m.resolved->symbols)
      if (s.name == "main" && s.kind == modules::SymbolKind::function)
        return s.id;
    throw std::runtime_error("main symbol missing");
  }();
  auto calls_in = [&](SymbolId function) {
    std::vector<const hir::Call *> result;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == function)
        if (auto c = std::get_if<hir::Call>(&n.data))
          result.push_back(c);
    return result;
  };

  const auto *take = trait_named("Take");
  const auto *box_impl = take ? impl_for(*take, "Box") : nullptr;
  const auto *via = template_named("via");
  const auto *via_static = template_named("via_static");
  const auto *never = template_named("never");
  const auto *factory = template_named("factory");
  need(take && box_impl && via && via_static && never && factory,
       "trait generic move declarations missing");
  const auto take_method = impl_method(*box_impl, "take");
  const auto multi_method = impl_method(*box_impl, "multi");
  const auto static_method = impl_method(*box_impl, "static_take");
  const auto echo_method = impl_method(*box_impl, "echo");
  const auto phantom_method = impl_method(*box_impl, "phantom");

  // The template source keeps a real ConstraintCall to the trait prototype.
  // The T-qualified entry passes the ordinary owner explicitly at index 0;
  // there is no synthetic receiver argument or receiver child.
  auto [cc_id, cc] = constraint_in(via->function.symbol);
  (void)cc_id;
  const hir::MethodPrototype *take_prototype = nullptr;
  for (const auto &p : take->methods)
    if (name(p.symbol) == "take")
      take_prototype = &p;
  need(take_prototype && cc->trait.declaration == take->symbol &&
           cc->method == take_prototype->symbol &&
           cc->method != take_method,
       "generic move ConstraintCall target changed");
  need(cc->args.size() == 3 && cc->args[0].parameter_index == 0 &&
           cc->args[0].label == std::optional<std::string>("owner") &&
           cc->args[0].mode == hir::ParamMode::ordinary &&
           cc->args[1].parameter_index == 1 &&
           cc->args[1].label == std::optional<std::string>("value") &&
           cc->args[1].mode == hir::ParamMode::move &&
           cc->args[2].parameter_index == 2 &&
           cc->args[2].label == std::optional<std::string>("tag") &&
           cc->args[2].mode == hir::ParamMode::ordinary,
       "T-qualified generic move argument order/labels/modes changed");
  auto template_move = std::get_if<hir::Move>(
      &m.nodes[cc->args[1].expression.value].data);
  need(template_move &&
           template_move->binding == via->function.parameters[1].symbol,
       "template generic move lost its Move binding");

  auto [cc_static_id, cc_static] = constraint_in(via_static->function.symbol);
  (void)cc_static_id;
  need(cc_static->args.size() == 1 &&
           cc_static->args[0].parameter_index == 0 &&
           !cc_static->args[0].label &&
           cc_static->args[0].mode == hir::ParamMode::move,
       "T-qualified static generic move shape changed");

  // The instance completes the prototype call to the real impl method,
  // renames the parameter/local/Move binding, and keeps the full instance
  // key [impl arg, method arg...].
  auto via_instances = instances_of(via->function.symbol);
  need(via_instances.size() == 1,
       "via should have one Box[i32] instance for this source");
  for (const auto *inst : via_instances) {
    need(inst->type_args.size() == 3,
         "via instance lost its complete type arguments");
    const auto *f = function_for(inst->symbol);
    need(f, "via instance function missing");
    bool saw_move = false;
    for (const auto &n : m.nodes) {
      if (m.scopes[n.scope.value].function != f->symbol)
        continue;
      if (auto mv = std::get_if<hir::Move>(&n.data); mv) {
        need(mv->binding &&
                 (mv->binding == f->parameters[1].symbol ||
                  mv->binding == f->parameters[2].symbol),
             "via instance Move was not renamed to its parameter");
        saw_move = true;
      }
      if (auto c = std::get_if<hir::Call>(&n.data); c) {
        need(c->kind == hir::CalleeKind::direct && c->direct_symbol &&
                 c->type_args.empty(),
             "completed generic move call kept template metadata");
        need(origin_of(*c->direct_symbol) == take_method ||
                 origin_of(*c->direct_symbol) == multi_method ||
                 origin_of(*c->direct_symbol) == static_method,
             "completed generic move call selected the wrong impl method");
        need(c->args.size() == 3 || c->args.size() == 1,
             "completed generic move call arity changed");
        need(c->args[1].mode == hir::ParamMode::move ||
                 (c->args.size() == 1 &&
                  c->args[0].mode == hir::ParamMode::move),
             "completed generic move call lost its move mode");
      }
    }
    need(saw_move, "via instance has no Move node");
  }

  auto take_instances = instances_of(take_method);
  need(take_instances.size() >= 1, "impl generic move instances missing");
  auto binding_symbols = [&](SymbolId function) {
    std::set<uint64_t> result;
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == function)
        if (auto b = std::get_if<hir::Binding>(&n.data))
          result.insert(b->symbol.value);
    return result;
  };
  std::set<uint64_t> all_instance_bindings, all_instance_parameters;
  for (const auto *inst : take_instances) {
    need(inst->type_args.size() == 3,
         "take instance key is not [impl arg, own args...]");
    const auto *f = function_for(inst->symbol);
    need(f, "take instance function missing");
    auto instance_bindings = binding_symbols(inst->symbol);
    need(!instance_bindings.empty(), "take instance has no local binding");
    for (auto id : instance_bindings)
      need(all_instance_bindings.insert(id).second,
           "take instance local binding was not renamed uniquely");
    for (const auto &p : f->parameters)
      need(all_instance_parameters.insert(p.symbol.value).second,
           "take instance parameter was not renamed uniquely");
    need(!inst->renamed.empty(), "take instance rename map is empty");
  }
  for (const auto *inst : instances_of(multi_method))
    need(inst->type_args.size() == 4,
         "multi instance key is not [impl arg, own args...]");
  for (const auto *inst : instances_of(static_method))
    need(inst->type_args.size() == 2,
         "static_take instance key is not [impl arg, own arg]");

  // Value, concrete Type and bounded T all complete one real take target;
  // the value receiver is argument zero and the explicit Type/T forms start
  // their written arguments at index zero.
  size_t main_take_calls = 0, main_static_calls = 0;
  for (const auto *c : calls_in(main_symbol)) {
    if (!c->direct_symbol)
      continue;
    auto origin = origin_of(*c->direct_symbol);
    if (origin == take_method) {
      ++main_take_calls;
      need(c->args.size() == 3 &&
               c->args[0].parameter_index == 0 &&
               c->args[0].mode == hir::ParamMode::ordinary,
           "value/Type generic move take shape changed");
    }
    if (origin == static_method) {
      ++main_static_calls;
      need(c->args.size() == 1 && c->args[0].parameter_index == 0 &&
               c->args[0].mode == hir::ParamMode::move,
           "static Type generic move starts at index zero");
    }
    if (origin == echo_method) {
      need(c->args.size() == 1 && c->args[0].parameter_index == 0 &&
               c->args[0].mode == hir::ParamMode::move,
           "static own-named-self does not start at index zero");
      const auto *inst = [&]() -> const hir::FunctionInstance * {
        for (const auto &i : m.instances)
          if (i.symbol == *c->direct_symbol)
            return &i;
        return nullptr;
      }();
      need(inst && inst->type_args.size() == 2 &&
               type_argument_name(inst->type_args[1]) == "Box",
           "echo own argument did not concretize to the impl Self");
    }
  }
  need(main_take_calls == 2,
       "value and concrete Type take calls were not both present");
  need(main_static_calls == 1,
       "concrete Type static take call missing");

  // The static own parameter named self and concretized to Self keeps its
  // instance Move bound to the concrete parameter, never to a receiver.
  for (const auto *inst : instances_of(echo_method)) {
    const auto *f = function_for(inst->symbol);
    need(f, "echo instance missing");
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == f->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv)
          need(mv->binding == f->parameters[0].symbol,
               "echo static move was mistaken for a receiver move");
  }

  // Four phantom method-own combinations keep four separate full keys even
  // though their runtime signatures are identical.
  std::map<std::string, const hir::FunctionInstance *> phantom_instances;
  for (const auto *inst : instances_of(phantom_method)) {
    need(inst->type_args.size() == 3,
         "phantom instance key is not [impl arg, own args...]");
    auto key = type_argument_name(inst->type_args[0]) + ":" +
               type_argument_name(inst->type_args[1]) + ":" +
               type_argument_name(inst->type_args[2]);
    need(phantom_instances.emplace(key, inst).second,
         "phantom generic move key was not unique");
  }
  need(phantom_instances.size() == 4 &&
           phantom_instances.count("i32:One:Two") &&
           phantom_instances.count("i32:Two:One") &&
           phantom_instances.count("bool:One:Two") &&
           phantom_instances.count("bool:Two:One"),
       "phantom generic move 2x2 full keys missing");

  // Uncalled templates are still checked, including position alpha-renaming
  // and a real single bound; a moved ordinary parameter inside the template
  // keeps its renamed symbol in each instance.
  for (const auto *inst : instances_of(factory->function.symbol))
    for (const auto &n : m.nodes)
      if (m.scopes[n.scope.value].function == inst->symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv)
          need(mv->binding.has_value(),
               "generic lambda move lost its binding");

  size_t hir_bad_graphs = 0;
  auto reject_hir = [&](hir::HirModule copy, const std::string &label,
                        const auto &change) {
    ++hir_bad_graphs;
    invalid(std::move(copy), label, change);
  };
  auto reject_hir_recomputed = [&](hir::HirModule copy,
                                   const std::string &label,
                                   const auto &change) {
    ++hir_bad_graphs;
    invalid_recomputed(std::move(copy), label, change);
  };

  // Same-signature methods from different traits are not interchangeable.
  // The target and the raw source trait identity are both checked.
  const std::string two_traits = R"(
trait Left { func take[U](owner Self, value move U) U; }
trait Right { func take[U](owner Self, value move U) U; }
struct S { items Vec[i32]; }
impl Left for S { func take[U](owner Self, value move U) U { return move value; } }
impl Right for S { func take[U](owner Self, value move U) U { return move value; } }
func via[T Left, U](x T, value move U) U { return T.take[U](x, move value); }
func main() i32 { var v Vec[i32] = [1]; return via[S, Vec[i32]](S(items: [1]), move v)[0] - 1; }
)";
  auto tw = good(two_traits);
  auto tw_name = [&](SymbolId s) -> const std::string & {
    return tw.resolved->symbols.at(s.value).name;
  };
  auto tw_trait = [&](const std::string &n) -> const hir::TraitDecl * {
    for (const auto &t : tw.traits)
      if (tw_name(t.symbol) == n)
        return &t;
    return nullptr;
  };
  const auto *left = tw_trait("Left");
  const auto *right = tw_trait("Right");
  need(left && right, "same-signature move traits missing");
  const hir::ImplDecl *left_impl = nullptr, *right_impl = nullptr;
  for (const auto &im : tw.impls) {
    if (im.trait && im.trait->declaration == left->symbol)
      left_impl = &im;
    if (im.trait && im.trait->declaration == right->symbol)
      right_impl = &im;
  }
  need(left_impl && right_impl, "same-signature move impls missing");
  SymbolId left_take = left_impl->methods.front();
  SymbolId right_take = right_impl->methods.front();
  const hir::FunctionTemplate *tw_via = nullptr;
  for (const auto &t : tw.templates)
    if (tw_name(t.function.symbol) == "via")
      tw_via = &t;
  need(tw_via, "same-signature move via template missing");
  auto tw_origin = [&](SymbolId s) {
    for (const auto &i : tw.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  const auto *tw_call = [&]() -> const hir::Call * {
    for (const auto &n : tw.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && tw_origin(*c->direct_symbol) == left_take)
        return c;
    return nullptr;
  }();
  need(tw_call, "same-signature move selected the wrong trait");
  SymbolId tw_via_instance{0};
  for (const auto &i : tw.instances)
    if (i.origin == tw_via->function.symbol)
      tw_via_instance = i.symbol;
  need(tw_via_instance.value != 0, "same-signature move via instance missing");
  reject_hir(tw, "generic move concrete target changed to other trait",
          [&](auto &x) {
            for (auto &n : x.nodes)
              if (x.scopes[n.scope.value].function == tw_via_instance)
                if (auto c = std::get_if<hir::Call>(&n.data);
                    c && c->direct_symbol) {
                  c->direct_symbol = right_take;
                  return;
                }
            throw std::runtime_error("generic move concrete Call missing");
          });
  reject_hir(tw, "generic move ConstraintCall changed to other trait",
          [&](auto &x) {
            for (auto &n : x.nodes)
              if (x.scopes[n.scope.value].function ==
                  tw_via->function.symbol)
                if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
                  c->trait = hir::TraitRef{right->symbol, {}};
                  c->method = right->methods.front().symbol;
                  return;
                }
            throw std::runtime_error("generic move ConstraintCall missing");
          });

  // Corruptions of the real source arguments, labels, modes, receiver and
  // Move binding are rejected by the independent HIR verifier.
  reject_hir(m, "generic move ConstraintCall argument label", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == via->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[1].label = std::string("tag");
          return;
        }
    throw std::runtime_error("generic move ConstraintCall missing");
  });
  reject_hir(m, "generic move ConstraintCall parameter index", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == via->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[1].parameter_index = 2;
          return;
        }
    throw std::runtime_error("generic move ConstraintCall missing");
  });
  reject_hir(m, "generic move ConstraintCall move mode", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == via->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[1].mode = hir::ParamMode::ordinary;
          return;
        }
    throw std::runtime_error("generic move ConstraintCall missing");
  });
  reject_hir(m, "generic move ConstraintCall receiver argument", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == via->function.symbol)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          c->args[0].expression = c->args[1].expression;
          n.children[0] = c->args[1].expression;
          return;
        }
    throw std::runtime_error("generic move ConstraintCall missing");
  });
  reject_hir(m, "generic move Move binding erased", [&](auto &x) {
    for (auto &n : x.nodes)
      if (x.scopes[n.scope.value].function == via->function.symbol)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          mv->binding.reset();
          return;
        }
    throw std::runtime_error("template generic move Move missing");
  });
  reject_hir(m, "generic move Move binding points at another parameter",
          [&](auto &x) {
            for (auto &n : x.nodes)
              if (x.scopes[n.scope.value].function ==
                  via->function.symbol)
                if (auto mv = std::get_if<hir::Move>(&n.data);
                    mv && mv->binding) {
                  mv->binding = via->function.parameters[0].symbol;
                  return;
                }
            throw std::runtime_error("template generic move Move missing");
          });

  // Phantom own/impl prefix and complete-key separation.
  const auto *phantom_one = phantom_instances.at("i32:One:Two");
  const auto *phantom_two = phantom_instances.at("i32:Two:One");
  reject_hir_recomputed(m, "phantom generic move instance arguments",
                     [&](auto &x) {
                       for (auto &i : x.instances)
                         if (i.symbol == phantom_one->symbol)
                           for (auto &j : x.instances)
                             if (j.symbol == phantom_two->symbol)
                               std::swap(i.type_args[1], j.type_args[1]);
                     });
  reject_hir_recomputed(m, "phantom generic move impl prefix", [&](auto &x) {
    for (auto &i : x.instances)
      if (i.symbol == phantom_one->symbol) {
        i.type_args[0] = i.type_args[1];
        return;
      }
    throw std::runtime_error("phantom instance missing");
  });
  reject_hir(m, "same-signature phantom direct target", [&](auto &x) {
    for (auto &n : x.nodes) {
      if (x.scopes[n.scope.value].function != main_symbol)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(*c->direct_symbol) == phantom_method) {
        c->direct_symbol = phantom_two->symbol;
        return;
      }
    }
    throw std::runtime_error("phantom direct call missing");
  });

  // Top-level method-parameter inout, dependent callbacks and actual-self
  // move declarations are now legal; concrete applications still check data.
  good("trait V{func set[U](owner Self, slot inout U);}"
       "struct S{} impl V for S{func set[U](owner Self, slot inout U){}}"
       "func main()i32{return 0;}");
  good("trait V{func take[U](self move Self, value U)U;}"
       "struct S{} impl V for S{func take[U](self move Self, value U)U{"
       "return value;}} func main()i32{return 0;}");
  good("trait V{func take[U](owner move Self, value U)U;}"
       "struct S{} impl V for S{func take[U](owner move Self, value U)U{"
       "return value;}} func main()i32{return 0;}");
  good("trait V{func run[U](owner Self, cb func(inout U));}"
       "struct S{} impl V for S{func run[U](owner Self, cb func(inout U)){}}"
       "func main()i32{return 0;}");
  good("trait V{func run[U](owner Self, cb func(inout i32,i32)i32, tag U)i32;}"
       "struct S{} impl V for S{func run[U](owner Self,"
       "cb func(inout i32,i32)i32, tag U)i32{"
       "var slot i32=1;return cb(inout slot,2);}}"
       "func main()i32{return 0;}");
  // A later `value move Self` is a non-self parameter even though its type is
  // the impl Self. Only the raw first parameter participates in actual-self
  // classification.
  good("trait Keep{func take[U](owner Self,value move Self,tag U)Self;}"
       "struct S{items Vec[i32];}"
       "impl Keep for S{func take[U](owner Self,value move Self,tag U)Self{"
       "return move value;}}"
       "func main()i32{let a=S(items:[1]);let b=S(items:[2]);"
       "let alias=b;let c=a.take[bool](move b,true);c.items[0]=3;"
       "return alias.items[0]-3;}");

  // Explicit/inferred conflicts, an own bound mismatch, a positional alpha
  // mismatch, and a missing phantom argument are source errors rather than
  // internal HIR failures.
  const std::string values =
      "trait Tag{func code(owner Self)i32;}"
      "struct One{} struct Two{}"
      "impl Tag for One{func code(owner Self)i32{return 1;}}"
      "impl Tag for Two{func code(owner Self)i32{return 2;}}";
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)i32;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self,"
               "value move U)i32{return 1;}}"
               "func main()i32{var v Vec[i32]=[1];"
               "let s=S();return s.pick(move v);}",
      "E_TYPE_MISMATCH");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)i32;}"
               "struct S{} impl Pick for S{func pick[U,W](owner Self,"
               "value move U)i32{return 1;}}"
               "func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  bad(values + "trait Pair{func pair[A,B Tag](owner Self,first move A,"
               "second B)i32;}"
               "struct S{} impl Pair for S{func pair[A,B Tag](owner Self,"
               "second B,first move A)i32{return 1;}}"
               "func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self)i32;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self)i32{"
               "return 1;}}"
               "func main()i32{let s=S();return s.pick[One]();}",
      "E_TYPE_MISMATCH");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)U;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self,"
               "value move U)U{return move value;}}"
               "func main()i32{let s=S();var v Vec[i32]=[1];"
               "let r=s.pick[Vec[i32],One](v);return r[0];}",
      "E_CALL_ARGUMENT");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)U;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self,"
               "value move U)U{return move value;}}"
               "func main()i32{let s=S();var v Vec[i32]=[1];"
               "let r=s.pick[Vec[i32],One](move v);return v[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)U;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self,"
               "value move U)U{return move value;}}"
               "func main()i32{let s=S();var v Vec[i32]=[1];"
               "let r=s.pick[Vec[i32],One](move v);"
               "let q=s.pick[Vec[i32],One](move v);return r[0];}",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad(values + "trait Pick{func pick[U,V Tag](owner Self,value move U)U;}"
               "struct S{} impl Pick for S{func pick[U,V Tag](owner Self,"
               "value move U)U{return move value;}}"
               "func main()i32{let s=S();"
               "let r=S.pick[Vec[i32],One](s,move s,One(n:0));"
               "return 0;}",
      "E_CALL_ARGUMENT");
  bad(values + "trait Pick{func pick[U](owner Self,value move U)U;}"
               "struct S{} impl Pick for S{func pick[U](owner Self,"
               "value move U)U{return move value;}}"
               "func main()i32{let s=S();"
               "let r=s.pick[S](move s);return 0;}",
      "E_CALL_ARGUMENT");
  good(values +
       "trait Pick{func pick[U](owner Self,slot inout U);}"
       "struct S{} impl Pick for S{func pick[U](owner Self,"
       "slot inout U){}}"
       "func main()i32{let s=S();var x i32=1;"
       "S.pick[i32](s,inout x);return 0;}");

  // A real cross-package public trait and generic impl, with value, Type and
  // bounded T move calls, and a noncapturing generic lambda.
  {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang",
          "package lib; "
          "pub trait Take{func take[U](owner Self,value move U)U;}"
          "pub struct Box[T]{pub value i32;}"
          "impl[T] Take for Box[T]{"
          "func take[U](owner Self,value move U)U{return move value;}}");
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; "
          "func via[T lib.Take,U](x T,value move U)U{"
          "return T.take[U](x,move value);}"
          "func factory[T lib.Take]()(func(T)Vec[i32]){"
          "return func(x T)Vec[i32]{let local Vec[i32]=[3];"
          "return T.take[Vec[i32]](x,move local);};}"
          "func main()i32{let b=lib.Box[i32](value:0);"
          "var a Vec[i32]=[1];var c Vec[i32]=[2];"
          "let x=b.take[Vec[i32]](move a);"
          "let y=lib.Box[i32].take[Vec[i32]](b,move c);"
          "let z=via[lib.Box[i32],Vec[i32]](b,[4]);"
          "let w=factory[lib.Box[i32]]()(b);"
          "return x[0]+y[0]+z[0]+w[0]-10;}");
    auto r = check(t.path / "lang.toml");
    need(r.success(), messages(r));
    need(hir::verify_types(*r.value).success(),
         "cross-package generic move HIR rejected");
    need(hir::verify(*r.value).success(),
         "cross-package generic move did not publish");
  }
  std::cout << "trait-generic-move HIR bad graphs rejected: "
            << hir_bad_graphs << "\n";
}

void trait_method_scalar_inout(const fs::path &root) {
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source rejection became E_IR_INVALID");
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  auto unsupported_both = [&](const std::string &text) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(text, backend);
      need(!r.success() && r.failure == FailureKind::unsupported &&
               has(r, "E_CAPABILITY_UNAVAILABLE"),
           std::string(backend) + " backend accepted: " + text + "\n" +
               messages(r));
    }
  };
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "inout control HIR rejected: " + text);
    need(hir::verify(*r.value).success(),
         "inout control did not publish: " + text);
    return *r.value;
  };
  auto fixture = [&](const char *relative) {
    auto r = check(root / relative);
    need(r.success(), std::string("fixture rejected: ") + relative + "\n" +
                          messages(r));
    need(hir::verify_types(*r.value).success(),
         std::string("fixture HIR rejected: ") + relative);
    need(hir::verify(*r.value).success(),
         std::string("fixture did not publish: ") + relative);
    return *r.value;
  };

  auto generic = fixture(
      "tests/generics/method-inout/trait-generic-inout/lang.toml");
  auto own = fixture(
      "tests/generics/method-inout/trait-qualified-own/lang.toml");
  auto qualified = fixture(
      "tests/generics/method-inout/trait-qualified-inout/lang.toml");
  (void)fixture(
      "tests/generics/method-inout/trait-ordinary-holder-control/lang.toml");
  (void)fixture(
      "tests/generics/method-inout/trait-ordinary-return-control/lang.toml");

  auto source_name = [](const hir::HirModule &m, SymbolId s)
      -> const std::string & { return m.resolved->symbols.at(s.value).name; };
  auto nominal_name = [&](const hir::HirModule &m, TypeId id) -> std::string {
    auto t = m.types.find(id);
    if (!t || (t->kind != hir::TypeKind::NominalStruct &&
               t->kind != hir::TypeKind::NominalEnum))
      return {};
    return source_name(m, t->declaration);
  };
  auto is_i32 = [](const hir::HirModule &m, TypeId id) {
    auto t = m.types.find(id);
    return t && t->kind == hir::TypeKind::SignedInt && t->bits == 32;
  };
  auto trait_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (source_name(m, t.symbol) == n)
        return &t;
    return nullptr;
  };
  auto template_for = [&](const hir::HirModule &m, const std::string &n)
      -> const hir::FunctionTemplate * {
    for (const auto &t : m.templates)
      if (source_name(m, t.function.symbol) == n)
        return &t;
    return nullptr;
  };
  auto proto_named = [&](const hir::HirModule &m, const hir::TraitDecl &tr,
                         const std::string &n)
      -> const hir::MethodPrototype * {
    for (const auto &p : tr.methods)
      if (source_name(m, p.symbol) == n)
        return &p;
    return nullptr;
  };
  auto impl_for = [&](const hir::HirModule &m, const hir::TraitDecl &tr,
                      const std::string &n) -> const hir::ImplDecl * {
    for (const auto &im : m.impls)
      if (im.trait && im.trait->declaration == tr.symbol && nominal_name(m, im.self_type) == n)
        return &im;
    return nullptr;
  };
  auto impl_method = [&](const hir::HirModule &m, const hir::ImplDecl &im,
                         const std::string &n) -> SymbolId {
    for (auto s : im.methods)
      if (source_name(m, s) == n)
        return s;
    throw std::runtime_error("trait inout impl method missing: " + n);
  };
  auto origin_of = [&](const hir::HirModule &m, SymbolId s) {
    for (const auto &i : m.instances)
      if (i.symbol == s)
        return i.origin;
    return s;
  };
  auto constraint_in = [&](const hir::HirModule &m, SymbolId function)
      -> std::pair<size_t, const hir::ConstraintCall *> {
    std::pair<size_t, const hir::ConstraintCall *> result{
        static_cast<size_t>(-1), nullptr};
    for (size_t i = 0; i < m.nodes.size(); ++i) {
      const auto &n = m.nodes[i];
      if (m.scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        need(!result.second, "multiple trait inout ConstraintCalls");
        result = {i, c};
      }
    }
    need(result.second, "missing trait inout ConstraintCall");
    return result;
  };

  const auto *flow = trait_for(generic, "GenericFlow");
  const auto *replace_proto = flow ? proto_named(generic, *flow, "replace")
                                   : nullptr;
  need(flow && replace_proto, "generic trait inout declarations missing");
  const auto &replace_sig = *generic.types.find(replace_proto->signature);
  need(replace_sig.parameters.size() == 4 &&
           replace_sig.parameters[1].mode == hir::ParamMode::inout,
       "generic trait inout slot mode changed");
  {
    auto t = generic.types.find(replace_sig.parameters[1].type);
    need(t && t->kind == hir::TypeKind::TypeParameter &&
             t->declaration != flow->self &&
             source_name(generic, t->declaration) == "U",
         "generic trait inout slot is not the real method parameter");
  }
  const auto *via = template_for(generic, "via");
  need(via, "generic trait inout via template missing");
  auto [via_cc_id, via_cc] = constraint_in(generic, via->function.symbol);
  (void)via_cc_id;
  need(via_cc->trait.declaration == flow->symbol && via_cc->args.size() == 4 &&
           via_cc->args[0].parameter_index == 0 &&
           via_cc->args[0].mode == hir::ParamMode::ordinary &&
           via_cc->args[1].parameter_index == 1 &&
           via_cc->args[1].mode == hir::ParamMode::inout &&
           via_cc->args[2].parameter_index == 2 &&
           via_cc->args[3].parameter_index == 3 &&
           via_cc->args[3].mode == hir::ParamMode::inout,
       "generic trait inout ConstraintCall order/modes changed");
  {
    const auto &place = std::get<hir::Place>(
        generic.nodes[via_cc->args[1].expression.value].data);
    need(place.symbol == via->function.parameters[1].symbol,
         "generic trait inout ConstraintCall lost its template Place");
    const auto &sig = *generic.types.find(via_cc->signature);
    auto u = generic.types.find(sig.parameters[1].type);
    need(sig.parameters[1].mode == hir::ParamMode::inout && u &&
             u->kind == hir::TypeKind::TypeParameter &&
             source_name(generic, u->declaration) == "U",
         "generic trait inout signature did not substitute the method U");
  }

  const auto *box_impl = impl_for(generic, *flow, "Box");
  need(box_impl, "generic trait inout Box impl missing");
  const auto replace_impl = impl_method(generic, *box_impl, "replace");
  size_t concrete_replace = 0;
  SymbolId main_symbol{};
  for (const auto &s : generic.resolved->symbols)
    if (s.name == "main" && s.kind == modules::SymbolKind::function)
      main_symbol = s.id;
  need(main_symbol.value, "generic trait inout main missing");
  for (const auto &n : generic.nodes) {
    if (generic.scopes[n.scope.value].function != main_symbol)
      continue;
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c || !c->direct_symbol ||
        origin_of(generic, *c->direct_symbol) != replace_impl)
      continue;
    const auto &sig = *generic.types.find(c->signature);
    need(c->args.size() == 4 &&
             sig.parameters[1].mode == hir::ParamMode::inout &&
             is_i32(generic, sig.parameters[1].type) &&
             generic.nodes[c->args[1].expression.value].kind() ==
                 hir::NodeKind::place,
         "concrete generic trait inout call lost scalar Place");
    ++concrete_replace;
  }
  need(concrete_replace >= 2,
       "direct generic trait inout calls were not instantiated");

  // A method with no own type parameters still gets the same explicit Type/T
  // inout entry. Its first ordinary Self is written at index zero.
  const auto *plain_trait = trait_for(qualified, "Plain");
  const auto *bump = plain_trait ? proto_named(qualified, *plain_trait, "bump")
                                 : nullptr;
  need(plain_trait && bump, "non-own trait inout declarations missing");
  const auto *via_plain = template_for(qualified, "via_plain");
  need(via_plain, "non-own trait inout forwarding template missing");
  auto [plain_cc_id, plain_cc] =
      constraint_in(qualified, via_plain->function.symbol);
  (void)plain_cc_id;
  {
    const auto &sig = *qualified.types.find(plain_cc->signature);
    need(plain_cc->args.size() == 4 &&
             plain_cc->args[0].parameter_index == 0 &&
             plain_cc->args[0].mode == hir::ParamMode::ordinary &&
             plain_cc->args[1].parameter_index == 1 &&
             plain_cc->args[1].mode == hir::ParamMode::inout &&
             is_i32(qualified, sig.parameters[1].type),
         "non-own trait inout static call gained a receiver or lost its place");
  }

  // A non-generic trait method with top-level inout now also has an explicit
  // Type/T entry. The first ordinary Self is argument zero, not a hidden
  // receiver, and the ordinary owner spelling is not special.
  const auto *plain = trait_for(generic, "SelfFlow");
  const auto *selfslot = plain ? proto_named(generic, *plain, "selfslot")
                                : nullptr;
  need(plain && selfslot, "trait Self inout declarations missing");
  const auto &self_sig = *generic.types.find(selfslot->signature);
  {
    auto t = generic.types.find(self_sig.parameters[1].type);
    need(self_sig.parameters[1].mode == hir::ParamMode::inout &&
             t && t->kind == hir::TypeKind::TypeParameter &&
             t->declaration == plain->self,
         "trait Self inout slot lost the real Self or became a method type");
  }
  const auto *via_self = template_for(generic, "via_self");
  need(via_self, "trait Self inout forwarding template missing");
  auto [self_cc_id, self_cc] =
      constraint_in(generic, via_self->function.symbol);
  (void)self_cc_id;
  need(self_cc->args[1].mode == hir::ParamMode::inout &&
           generic.nodes[self_cc->args[1].expression.value].kind() ==
               hir::NodeKind::place,
       "trait Self inout forwarding lost its Place");
  {
    const auto &sig = *generic.types.find(self_cc->signature);
    need(sig.parameters[1].mode == hir::ParamMode::inout &&
             sig.parameters[1].type == self_cc->receiver,
         "trait Self inout forwarding did not substitute Self to T");
  }

  // Method-own first position named `self` stays a static parameter through
  // Type and T even when its concrete argument equals the impl type.
  const auto *own_trait = trait_for(own, "Own");
  const auto *shift = own_trait ? proto_named(own, *own_trait, "shift")
                                 : nullptr;
  need(own_trait && shift, "trait own-first inout declarations missing");
  const auto &shift_sig = *own.types.find(shift->signature);
  {
    auto t = own.types.find(shift_sig.parameters[0].type);
    need(shift_sig.parameters[0].mode == hir::ParamMode::inout &&
             t && t->kind == hir::TypeKind::TypeParameter &&
             t->declaration != own_trait->self &&
             source_name(own, t->declaration) == "U",
         "own first inout was mistaken for the trait receiver");
  }
  const auto *own_via = template_for(own, "via");
  need(own_via, "own first inout via template missing");
  auto [own_cc_id, own_cc] = constraint_in(own, own_via->function.symbol);
  (void)own_cc_id;
  need(own_cc->args.size() == 3 && own_cc->args[0].parameter_index == 0 &&
           own_cc->args[0].mode == hir::ParamMode::inout,
       "T-qualified own first inout gained a synthetic receiver");
  {
    size_t children = 0;
    for (const auto &n : own.nodes)
      if (n.source.node.module == own.nodes[own_cc_id].source.node.module &&
          n.source.node.file == own.nodes[own_cc_id].source.node.file &&
          n.source.node.node == own.nodes[own_cc_id].source.node.node)
        children = n.children.size();
    need(children == own_cc->args.size(),
         "T-qualified own inout ConstraintCall children differ from args");
  }

  // Same-signature phantom method-own instances keep their complete key.
  const std::string phantom = R"(
trait Left { func slot[U, V](owner Self, slot inout U, value U) U; }
trait Right { func slot[U, V](owner Self, slot inout U, value U) U; }
struct One { n i32; }
struct Two { n i32; }
impl Left for i32 {
    func slot[U, V](owner Self, slot inout U, value U) U {
        let old = slot; slot = value; return old;
    }
}
impl Right for i32 {
    func slot[U, V](owner Self, slot inout U, value U) U {
        let old = slot; slot = value; return old;
    }
}
func via[T Left, U, V](receiver T, slot inout U, value U) U {
    return T.slot[U, V](receiver, inout slot, value);
}
func main() i32 {
    var a i32 = 1;
    let x = via[i32, i32, One](0, inout a, 2);
    var b i32 = 3;
    let y = via[i32, i32, Two](0, inout b, 4);
    return x + y - 4;
}
)";
  auto ph = source(phantom);
  need(ph.success(), messages(ph));
  need(hir::verify_types(*ph.value).success(),
       "phantom inout HIR rejected");
  need(hir::verify(*ph.value).success(),
       "phantom inout did not publish");
  auto ph_trait = [&](const std::string &n) -> const hir::TraitDecl * {
    for (const auto &t : ph.value->traits)
      if (source_name(*ph.value, t.symbol) == n)
        return &t;
    return nullptr;
  };
  const auto *left = ph_trait("Left");
  const auto *right = ph_trait("Right");
  need(left && right, "phantom inout traits missing");
  SymbolId left_impl_method{};
  for (const auto &im : ph.value->impls)
    if (im.trait && im.trait->declaration == left->symbol)
      for (auto method : im.methods)
        if (source_name(*ph.value, method) == "slot")
          left_impl_method = method;
  need(left_impl_method.value, "phantom inout impl method missing");
  const auto *ph_via = [&]() -> const hir::FunctionTemplate * {
    for (const auto &t : ph.value->templates)
      if (source_name(*ph.value, t.function.symbol) == "via")
        return &t;
    return nullptr;
  }();
  need(ph_via, "phantom inout via template missing");
  const hir::FunctionInstance *ph_one = nullptr, *ph_two = nullptr;
  for (const auto &i : ph.value->instances) {
    if (i.origin != ph_via->function.symbol || i.type_args.size() != 3)
      continue;
    auto n = nominal_name(*ph.value, i.type_args[2]);
    if (n == "One")
      ph_one = &i;
    if (n == "Two")
      ph_two = &i;
  }
  need(ph_one && ph_two, "phantom inout via instances missing");
  auto ph_target = [&](SymbolId function) -> std::optional<SymbolId> {
    for (const auto &n : ph.value->nodes) {
      if (ph.value->scopes[n.scope.value].function != function)
        continue;
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          origin_of(*ph.value, *c->direct_symbol) == left_impl_method)
        return c->direct_symbol;
    }
    return std::nullopt;
  };
  auto one_target = ph_target(ph_one->symbol);
  auto two_target = ph_target(ph_two->symbol);
  need(one_target && two_target, "phantom inout concrete targets missing");
  auto [ph_cc_id, ph_cc] = constraint_in(*ph.value, ph_via->function.symbol);
  (void)ph_cc_id;

  invalid(generic, "trait generic inout ConstraintCall mode erased",
          [&](auto &x) {
            auto &c = std::get<hir::ConstraintCall>(x.nodes[via_cc_id].data);
            c.args[1].mode = hir::ParamMode::ordinary;
          });
  invalid(generic, "trait generic inout ConstraintCall Place became read",
          [&](auto &x) {
            auto &c = std::get<hir::ConstraintCall>(x.nodes[via_cc_id].data);
            auto &node = x.nodes[c.args[1].expression.value];
            auto place = std::get<hir::Place>(node.data);
            node.data = hir::Read{place.symbol};
          });
  invalid_recomputed(generic, "trait generic inout label recomputed",
                     [&](auto &x) {
                       auto &c =
                           std::get<hir::ConstraintCall>(x.nodes[via_cc_id].data);
                       c.args[1].label = std::string("value");
                     });
  invalid_recomputed(generic, "trait generic inout source index recomputed",
                     [&](auto &x) {
                       auto &c =
                           std::get<hir::ConstraintCall>(x.nodes[via_cc_id].data);
                       c.args[1].source_index = 2;
                     });
  invalid(generic, "trait generic inout receiver changed", [&](auto &x) {
    auto &c = std::get<hir::ConstraintCall>(x.nodes[via_cc_id].data);
    hir::Type t;
    t.kind = hir::TypeKind::Bool;
    c.receiver = x.types.intern(t);
  });
  invalid(*ph.value, "phantom inout target swapped",
          [&](auto &x) {
            for (auto &n : x.nodes)
              if (x.scopes[n.scope.value].function == ph_one->symbol)
                if (auto c = std::get_if<hir::Call>(&n.data);
                    c && c->direct_symbol == *one_target) {
                  c->direct_symbol = *two_target;
                  return;
                }
            throw std::runtime_error("phantom inout One call missing");
          });
  invalid(*ph.value, "phantom inout trait swapped",
          [&](auto &x) {
            auto &c = std::get<hir::ConstraintCall>(x.nodes[ph_cc_id].data);
            c.trait = hir::TraitRef{right->symbol, {}};
            c.method = right->methods.front().symbol;
          });
  invalid_recomputed(*ph.value, "phantom inout instance arguments",
                     [&](auto &x) {
                       for (auto &i : x.instances)
                         if (i.symbol == ph_one->symbol)
                           for (auto &j : x.instances)
                             if (j.symbol == ph_two->symbol) {
                               std::swap(i.type_args[2], j.type_args[2]);
                               return;
                             }
                       throw std::runtime_error("phantom inout instance missing");
                     });

  // Legal new-entry controls with a fixed callback. They also provide HIR
  // for rejecting mutations that introduce a method-own/Self dependency.
  auto fixed_visit = good(R"(
struct C[S, T] {
    f func(S, inout i32, C[S, T]) Unit;
}
func noop(owner i32, slot inout i32, node C[i32, i32]) Unit {}
trait Visit2 {
    func visit2[U](owner Self, slot inout U, node C[Self, U]) Unit;
}
impl Visit2 for i32 {
    func visit2[V](owner Self, slot inout V, node C[Self, V]) Unit {}
}
func via2[T Visit2, U](receiver T, slot inout U, node C[T, U]) Unit {
    T.visit2[U](receiver, inout slot, node);
}
func main() i32 {
    let node = C[i32, i32](f: noop);
    var slot i32 = 0;
    via2[i32, i32](7, inout slot, node);
    return 0;
}
)");
  const std::string fixed_factory = R"(
trait FixedFactory {
    func make(self Self, slot inout i32) (func(inout i32, i32) i32);
}
func fixed_change(slot inout i32, value i32) i32 {
    let old = slot;
    slot = value;
    return old;
}
impl FixedFactory for i32 {
    func make(self Self, slot inout i32) (func(inout i32, i32) i32) {
        slot = 2;
        return fixed_change;
    }
}
)";
  auto fixed_entry = good(fixed_factory + R"(
func fixed_route[T FixedFactory](owner T, slot inout i32)
        (func(inout i32, i32) i32) {
    return T.make(owner, inout slot);
}
func main() i32 {
    var made i32 = 0;
    let callback = fixed_route[i32](7, inout made);
    var slot i32 = 1;
    if callback(inout slot, 8) != 1 || slot != 8 || made != 2 { return 1; }
    return 0;
}
)");

  invalid(fixed_visit, "function-cycle field made method-dependent",
          [&](auto &x) {
            const hir::NominalDecl *c = nullptr;
            for (const auto &n : x.nominals)
              if (source_name(x, n.symbol) == "C")
                c = &n;
            need(c && c->params.size() == 2,
                 "function-cycle nominal missing");
            hir::Type parameter;
            parameter.kind = hir::TypeKind::TypeParameter;
            parameter.declaration = c->params[1].symbol;
            auto dependency = x.types.intern(parameter);
            hir::Type function;
            function.kind = hir::TypeKind::Function;
            function.parameters.push_back(
                {hir::ParamMode::inout, dependency});
            function.parameters.push_back(
                {hir::ParamMode::ordinary, dependency});
            function.result = dependency;
            auto dependent = x.types.intern(function);
            for (auto &n : x.nominals)
              if (n.symbol == c->symbol)
                n.fields.front().type = dependent;
          });
  invalid_recomputed(fixed_visit,
                     "function-cycle field dependency recomputed",
                     [&](auto &x) {
                       const hir::NominalDecl *c = nullptr;
                       for (const auto &n : x.nominals)
                         if (source_name(x, n.symbol) == "C")
                           c = &n;
                       need(c && c->params.size() == 2,
                            "function-cycle nominal missing");
                       hir::Type parameter;
                       parameter.kind = hir::TypeKind::TypeParameter;
                       parameter.declaration = c->params[1].symbol;
                       auto dependency = x.types.intern(parameter);
                       hir::Type function;
                       function.kind = hir::TypeKind::Function;
                       function.parameters.push_back(
                           {hir::ParamMode::inout, dependency});
                       function.parameters.push_back(
                           {hir::ParamMode::ordinary, dependency});
                       function.result = dependency;
                       auto dependent = x.types.intern(function);
                       for (auto &n : x.nominals)
                         if (n.symbol == c->symbol)
                           n.fields.front().type = dependent;
                     });
  invalid(fixed_entry, "fixed Type inout return made Self-dependent",
          [&](auto &x) {
            hir::TraitDecl *trait = nullptr;
            for (auto &t : x.traits)
              if (source_name(x, t.symbol) == "FixedFactory")
                trait = &t;
            need(trait && !trait->methods.empty(),
                 "fixed Type inout trait missing");
            auto &prototype = trait->methods.front();
            hir::Type parameter;
            parameter.kind = hir::TypeKind::TypeParameter;
            parameter.declaration = trait->self;
            auto self = x.types.intern(parameter);
            hir::Type callback;
            callback.kind = hir::TypeKind::Function;
            callback.parameters.push_back({hir::ParamMode::inout, self});
            callback.parameters.push_back({hir::ParamMode::ordinary, self});
            callback.result = self;
            auto dependent = x.types.intern(callback);
            auto signature = *x.types.find(prototype.signature);
            signature.result = dependent;
            prototype.signature = x.types.intern(signature);
          });
  invalid_recomputed(fixed_entry,
                     "fixed Type inout return dependency recomputed",
                     [&](auto &x) {
                       hir::TraitDecl *trait = nullptr;
                       for (auto &t : x.traits)
                         if (source_name(x, t.symbol) == "FixedFactory")
                           trait = &t;
                       need(trait && !trait->methods.empty(),
                            "fixed Type inout trait missing");
                       auto &prototype = trait->methods.front();
                       hir::Type parameter;
                       parameter.kind = hir::TypeKind::TypeParameter;
                       parameter.declaration = trait->self;
                       auto self = x.types.intern(parameter);
                       hir::Type callback;
                       callback.kind = hir::TypeKind::Function;
                       callback.parameters.push_back(
                           {hir::ParamMode::inout, self});
                       callback.parameters.push_back(
                           {hir::ParamMode::ordinary, self});
                       callback.result = self;
                       auto dependent = x.types.intern(callback);
                       auto signature = *x.types.find(prototype.signature);
                       signature.result = dependent;
                       prototype.signature = x.types.intern(signature);
                     });

  bad("trait Set{func set[U](owner Self,slot inout U,value U)U;} "
      "func f[T](x T,slot inout i32){T.set[i32](x,inout slot,1);} "
      "func main()i32{return 0;}",
      "E_TYPE_MISMATCH");
  // Actual-self move/inout declarations are deferred and legal without a
  // concrete trait application.
  good("trait V{func bad[U](owner move Self,slot inout U);} "
       "func main()i32{return 0;}");
  good("trait V{func bad[U](owner inout Self,slot inout U);} "
       "func main()i32{return 0;}");
  good("trait V{func bad[U](owner Self,cb func(inout U));} "
       "func main()i32{return 0;}");
  good("trait V{func bad[U](owner Self,cb func(inout Self));} "
       "func main()i32{return 0;}");
  unsupported("trait V{func bad[U](owner Self,slot inout U);} "
              "struct S{x i32;} impl V for S{"
              "func bad[U](owner Self,slot inout U){}} "
              "func main()i32{let s=S(x:1);var v Vec[i32]=[1];"
              "s.bad[Vec[i32]](inout v);return 0;}");
  // Ordinary-only Type/T calls keep the pre-existing support for a nominal
  // Holder[Self] callback parameter. The new strict top-level-inout check is
  // not applied to this old entry.
  good("struct Holder[Q]{cb func(inout Q,Q)Q;} "
       "trait V{func bad(self Self,value Holder[Self])Self;} "
       "impl V for i32{func bad(self Self,value Holder[i32])Self{"
       "return self;}} "
       "func replace(slot inout i32,value i32)i32{"
       "let old=slot;slot=value;return old;} "
       "func main()i32{let f func(inout i32,i32)i32=replace;"
       "let h=Holder[i32](cb:f);let r i32=0;"
       "i32.bad(r,h);return 0;}");
  good("struct Holder[Q]{cb func(inout Q,Q)Q;} "
       "trait V{func bad(self Self,value Holder[Self])Self;} "
       "impl V for i32{func bad(self Self,value Holder[i32])Self{"
       "return self;}} "
       "func via[T V](receiver T,value Holder[T])T{"
       "return T.bad(receiver,value);} "
       "func replace(slot inout i32,value i32)i32{"
       "let old=slot;slot=value;return old;} "
       "func main()i32{let f func(inout i32,i32)i32=replace;"
       "let h=Holder[i32](cb:f);via[i32](0,h);return 0;}");

  // F1: a recursive nominal function field reuses the top-level method
  // signature TypeId. The callback now depends on method-own U and real Self,
  // both concrete POD i32 here, so the complete application is legal.
  for (const char *backend : {"native", "python"}) {
    auto r = source(R"(
struct C[S, T] {
    f func(S, inout T, C[S, T]) Unit;
}
func noop(owner i32, slot inout i32, node C[i32, i32]) Unit {}
trait Visit {
    func visit[U](owner Self, slot inout U, node C[Self, U]) Unit;
}
impl Visit for i32 {
    func visit[V](owner Self, slot inout V, node C[Self, V]) Unit {}
}
func main() i32 {
    let node = C[i32, i32](f: noop);
    var slot i32 = 0;
    i32.visit[i32](7, inout slot, node);
    return 0;
}
)", backend);
    need(r.success(), std::string(backend) + ": F1 callback rejected\n" +
                          messages(r));
    need(hir::verify_types(*r.value).success(),
         std::string(backend) + ": F1 callback HIR rejected");
    need(hir::verify(*r.value).success(),
         std::string(backend) + ": F1 callback did not publish");
  }
  // The finite recursive function-field control stays legal.
  good(R"(
struct C[S, T] {
    f func(S, inout T, C[S, T]) Unit;
}
func noop(owner i32, slot inout i32, node C[i32, i32]) Unit {}
func main() i32 {
    let node = C[i32, i32](f: noop);
    var slot i32 = 0;
    node.f(7, inout slot, node);
    return 0;
}
)");

  // F2: a returned Self-dependent callback is ordinary callback data. Both
  // the value-form control and the Type/T top-level-inout entry are legal
  // when the substituted Self is a POD.
  const std::string factory_callback = R"(
trait Factory {
    func make(self Self, slot inout i32) (func(inout Self, Self) Self);
}
func change(slot inout i32, value i32) i32 {
    let old = slot;
    slot = value;
    return old;
}
impl Factory for i32 {
    func make(self Self, slot inout i32) (func(inout Self, Self) Self) {
        slot = 2;
        return change;
    }
}
)";
  for (const char *backend : {"native", "python"}) {
    auto r = source(factory_callback + R"(
func route[T Factory](owner T, slot inout i32) (func(inout T, T) T) {
    return T.make(owner, inout slot);
}
func main() i32 {
    var made i32 = 0;
    let callback = route[i32](7, inout made);
    var slot i32 = 1;
    if callback(inout slot, 8) != 1 || slot != 8 || made != 2 { return 1; }
    return 0;
}
)", backend);
    need(r.success(), std::string(backend) + ": F2 callback rejected\n" +
                          messages(r));
    need(hir::verify_types(*r.value).success(),
         std::string(backend) + ": F2 callback HIR rejected");
    need(hir::verify(*r.value).success(),
         std::string(backend) + ": F2 callback did not publish");
  }
  good(factory_callback + R"(
func route[T Factory](owner T, slot inout i32) (func(inout T, T) T) {
    return owner.make(inout slot);
}
func main() i32 {
    var made i32 = 0;
    let callback = route[i32](7, inout made);
    var slot i32 = 1;
    if callback(inout slot, 8) != 1 || slot != 8 || made != 2 { return 1; }
    return 0;
}
)");
  auto generic_trait_self_slot = good(
      "trait V{func bad[U](owner Self,slot inout Self,tag U)Self;} "
      "struct S{x i32;} impl V for S{"
      "func bad[U](owner Self,slot inout Self,tag U)Self{return slot;}} "
      "func main()i32{return 0;}");
  need(hir::verify(generic_trait_self_slot).success(),
       "generic trait POD Self inout did not publish");
  std::cout << "trait-method-scalar-inout HIR checks passed\n";
}

void inherent_static_self_identity() {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "inherent static identity HIR rejected: " + text + "\n" +
             messages(r));
    need(hir::verify(*r.value).success(),
         "inherent static identity did not publish: " + text + "\n" +
             messages(r));
    return *r.value;
  };
  auto unsupported = [&](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
  };
  // The F1 regressions must use the frozen source bytes verbatim, including
  // the original `package app;` line rather than the usual helper prefix.
  auto exact = [&](const std::string &text) {
    Temp t;
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/main.lang", text);
    auto r = check(t.path / "lang.toml");
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "F1 exact source HIR rejected: " + text + "\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "F1 exact source did not publish: " + text + "\n" + messages(r));
    return *r.value;
  };
  auto call_to = [&](const hir::HirModule &m, const std::string &name)
      -> const hir::Call * {
    for (const auto &n : m.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          m.resolved->symbols[c->direct_symbol->value].name == name)
        return c;
    return nullptr;
  };

  // F1 A and B are the exact frozen Sources. The first parameter of an
  // inherent impl method is an explicit static parameter unless the original
  // declaration is literal `self` of the impl type. No completed signature
  // comparison may turn it into a receiver.
  auto a = exact(R"(package app;
struct Host{items Vec[i32];}
impl Host{func consume(value move Host)i32{return value.items[0];}}
func main()i32{let x=Host(items:[7]);return Host.consume(move x);}
)");
  const auto *consume = call_to(a, "consume");
  need(consume && consume->args.size() == 1 &&
           consume->args[0].source_index == 0 &&
           consume->args[0].parameter_index == 0 &&
           !consume->args[0].label &&
           consume->args[0].mode == hir::ParamMode::move,
       "F1 A inherent static move was mistaken for a receiver");

  auto b = exact(R"(package app;
impl i32{func adjust(slot inout i32){slot=slot+1;}}
func main()i32{var slot i32=6;i32.adjust(inout slot);return slot;}
)");
  const auto *adjust = call_to(b, "adjust");
  need(adjust && adjust->args.size() == 1 &&
           adjust->args[0].source_index == 0 &&
           adjust->args[0].parameter_index == 0 &&
           !adjust->args[0].label &&
           adjust->args[0].mode == hir::ParamMode::inout &&
           b.nodes[adjust->args[0].expression.value].kind() ==
               hir::NodeKind::place,
       "F1 B inherent static inout was mistaken for a receiver");

  // Other non-literal labels and both generic forms stay static. In
  // particular an own type parameter named self that equals the impl type is
  // still an explicit parameter.
  auto other = good(
      "struct H{items Vec[i32];}"
      "impl H{func take(other move H)i32{return other.items[0];}}"
      "func main()i32{let h=H(items:[8]);return H.take(move h);}");
  const auto *other_call = call_to(other, "take");
  need(other_call && other_call->args.size() == 1 &&
           other_call->args[0].parameter_index == 0 &&
           other_call->args[0].mode == hir::ParamMode::move,
       "non-literal inherent static move parameter became a receiver");
  good(
      "struct H{marker i32;}"
      "impl H{func take[T](value move T)T{return move value;}}"
      "func main()i32{var v Vec[i32]=[7];"
      "let r=H.take[Vec[i32]](move v);return r[0]-7;}");
  good(
      "struct H{items Vec[i32];}"
      "impl H{func echo[U](self move U)U{return move self;}}"
      "func main()i32{let h=H(items:[9]);"
      "let r=H.echo[H](move h);return r.items[0]-9;}");
  good(
      "struct H{marker i32;}"
      "impl H{func bump[T](slot inout T,value T){slot=value;}}"
      "func main()i32{var x i32=1;H.bump[i32](inout x,2);return x-2;}");

  // Type-qualified actual Self is explicit parameter zero. Move accepts the
  // existing owning values; inout still rejects an owning aggregate below.
  good(
      "struct H{items Vec[i32];}"
      "impl H{func take(self move Self)Self{return move self;}}"
      "func main()i32{let h=H(items:[1]);let r=H.take(move h);return 0;}");
  unsupported(
      "struct H{items Vec[i32];}"
      "impl H{func bump(self inout Self){}}"
      "func main()i32{var h=H(items:[1]);H.bump(inout h);}");
  good(
      "trait T{func take[U](self move Self,value U)U;}"
      "struct H{} impl T for H{func take[U](self move Self,value U)U{"
      "return value;}} func main()i32{return 0;}");
  good(
      "trait T{func take[U](owner move Self,value U)U;}"
      "struct H{} impl T for H{func take[U](owner move Self,value U)U{"
      "return value;}} func main()i32{return 0;}");
  // Non-generic trait actual-self Type calls also use explicit parameter
  // zero; value-form calls with a move/inout Self remain unsupported.
  good(
      "trait T{func take(self move Self)Self;}"
      "struct H{items Vec[i32];}"
      "impl T for H{func take(self move Self)Self{return move self;}}"
      "func main()i32{let h=H(items:[1]);let r=H.take(move h);return 0;}");
  good(
      "trait T{func take(owner move Self)Self;}"
      "struct H{items Vec[i32];}"
      "impl T for H{func take(owner move Self)Self{return move owner;}}"
      "func main()i32{let h=H(items:[1]);let r=H.take(move h);return 0;}");
}

void trait_static_self_qualification() {
  auto good = [&](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto published = hir::verify(*r.value);
    need(published.success(), "Self-qualified HIR rejected: " + messages(published));
    return *r.value;
  };
  auto bad = [&](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
  };
  auto call_target = [&](const hir::HirModule &h, const std::string &name)
      -> const hir::Call * {
    for (const auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol) {
        auto target = *c->direct_symbol;
        for (const auto &i : h.instances)
          if (i.symbol == target) {
            target = i.origin;
            break;
          }
        if (h.resolved->symbols[target.value].name == name)
          return c;
      }
    return nullptr;
  };

  auto root_f1 = good(
      "struct Box{value i32;}"
      "impl Box{func helper(value i32)Self{return Box(value:value);}"
      "func make(value i32)Self{return Self.helper(value);}}"
      "func main()i32{return Box.make(7).value-7;}");
  need(call_target(root_f1, "helper"),
       "root Self.helper regression call missing");
  for (const auto &n : root_f1.nodes)
    need(!std::holds_alternative<hir::ConstraintCall>(n.data),
         "Self.helper became a ConstraintCall");

  auto generic = good(
      "struct Box[T]{value i32;}"
      "impl[U] Box[U]{func helper(value i32)Self{return Box[U](value:value);}"
      "func make(value i32)Self{return Self.helper(value);}"
      "func paren(value i32)Self{return (Self).helper(value);}}"
      "func main()i32{return Box[i32].make(7).value+"
      "Box[i64].paren(8).value-15;}");
  need(call_target(generic, "helper"),
       "generic impl Self.helper regression call missing");
  std::set<uint64_t> helper_keys;
  for (const auto &i : generic.instances)
    for (auto t : i.type_args)
      helper_keys.insert(t.value);
  need(helper_keys.size() >= 2,
       "generic Self.helper did not retain distinct impl type arguments");

  auto trait_self = good(
      "trait Maker{func make(value i32)Self;}"
      "struct Item{value i32;}"
      "impl Item{func helper(value i32)Self{return Item(value:value);}}"
      "impl Maker for Item{func make(value i32)Self{"
      "return Self.helper(value);}}"
      "func main()i32{return Item.make(9).value-9;}");
  need(call_target(trait_self, "helper"),
       "trait impl Self alias did not reach its inherent helper");

  auto guarded = good(
      "trait Value{func pick(self Self)i32;}"
      "struct Box{value i32;}"
      "impl Value for Box{func pick(self Self)i32{return 1;}}"
      "impl Box{func pick(self Self)i32{return 2;}}"
      "func use[T Value](x T)i32{return T.pick(x);}"
      "func main()i32{return use[Box](Box(value:0))-1;}");
  auto pick = call_target(guarded, "pick");
  need(pick && pick->direct_symbol, "bounded T.pick call missing");
  SymbolId inherent_pick{};
  for (const auto &im : guarded.impls)
    if (!im.trait) {
      const auto t = guarded.types.find(im.self_type);
      if (t && t->kind == hir::TypeKind::NominalStruct &&
          guarded.resolved->symbols[t->declaration.value].name == "Box")
        for (auto method : im.methods)
          if (guarded.resolved->symbols[method.value].name == "pick")
            inherent_pick = method;
    }
  need(inherent_pick.value, "inherent pick guard target missing");
  invalid(guarded, "bounded T.pick replaced by inherent method", [&](auto &h) {
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && c->direct_symbol->value ==
                                       pick->direct_symbol->value) {
        c->direct_symbol = inherent_pick;
        return;
      }
    throw std::runtime_error("bounded T.pick target missing");
  });

  auto self_targets = good(
      "struct Box{value i32;} struct Other{value i32;}"
      "impl Box{func helper(value i32)Self{return Box(value:value);}"
      "func alias(value i32)Self{return Box(value:value);}"
      "func make(value i32)Self{return Self.helper(value);}}"
      "impl Other{func helper(value i32)Self{return Other(value:value);}}"
      "func main()i32{return Box.make(7).value-7;}");
  auto self_helper = call_target(self_targets, "helper");
  need(self_helper && self_helper->direct_symbol,
       "Self alias helper target missing");
  SymbolId other_helper{};
  SymbolId box_alias{};
  for (const auto &im : self_targets.impls) {
    const auto t = self_targets.types.find(im.self_type);
    if (!t || t->kind != hir::TypeKind::NominalStruct)
      continue;
    const auto &name = self_targets.resolved->symbols[t->declaration.value].name;
    for (auto method : im.methods) {
      const auto &mn = self_targets.resolved->symbols[method.value].name;
      if (name == "Other" && mn == "helper")
        other_helper = method;
      if (name == "Box" && mn == "alias")
        box_alias = method;
    }
  }
  need(other_helper.value && box_alias.value,
       "Self alias corruption targets missing");
  invalid_recomputed(self_targets, "Self alias target from another impl",
                     [&](auto &h) {
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          c->direct_symbol->value == self_helper->direct_symbol->value) {
        c->direct_symbol = other_helper;
        return;
      }
    throw std::runtime_error("Self alias helper call missing");
  });
  invalid(self_targets, "Self alias target name replaced", [&](auto &h) {
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          c->direct_symbol->value == self_helper->direct_symbol->value) {
        c->direct_symbol = box_alias;
        return;
      }
    throw std::runtime_error("Self alias helper call missing");
  });

  bad("trait Value{} struct Box{value i32;}"
      "impl Box{func helper(value i32)Self{return Box(value:value);}}"
      "func use[T Value](x T)T{return T.helper(1);}",
      "E_TYPE_MISMATCH");
}
void nested_patterns() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto v = hir::verify_types(*r.value);
    need(v.success(), text + "\n" + messages(v));
    auto published = hir::verify(*r.value);
    need(published.success(), text + "\n" + messages(published));
    return *r.value;
  };
  auto bad = [](const std::string &text, const std::string &code = "E_TYPE_MISMATCH") {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source rejection became E_IR_INVALID: " + text +
                                      "\n" + messages(r));
  };
  auto packages = [](const std::string &lib, const std::string &app) {
    Temp t;
    fs::create_directories(t.path / "src/lib");
    write(t.path / "lang.toml",
          "schema_version=1\n[package]\nname=\"test\"\nsource_root=\"src\"\n"
          "entry=\"app\"\n[build]\nbackend=\"native\"\nword_bits=64\n");
    write(t.path / "src/lib/lib.lang", "package lib; " + lib);
    write(t.path / "src/main.lang",
          "package app; import \"app/lib\" as lib; " + app);
    return check(t.path / "lang.toml");
  };
  auto invalid_nested = [&](hir::HirModule m, const std::string &name,
                            const auto &change, bool recompute = false) {
    change(m);
    if (recompute) {
      auto derived = hir::detail::produce_ownership(m, {});
      need(derived.success(),
           "recomputed ownership failed before verification: " + name + "\n" +
               messages(derived));
    }
    auto r = hir::verify_types(m);
    need(!r.success() && !r.value && r.failure == FailureKind::internal &&
             has(r, "E_IR_INVALID") && !r.diagnostics.empty(),
         "corruption accepted: " + name + "\n" + messages(r));
    std::cout << "nested-hir-rejected: " << name << "\n";
    std::cout << "nested-hir-diagnostic: " << name << ": "
              << r.diagnostics.front().code << ": "
              << r.diagnostics.front().message << "\n";
    auto published = hir::verify(m);
    need(!published.success() && has(published, "E_IR_INVALID"),
         "corrupt module published: " + name);
  };
  const std::string declarations =
      "enum Inner{None;Some(value i32);} enum Bit{A;B;} "
      "enum Outer{Empty;Wrap(item Inner);Pair(first Bit,second Bit);} ";
  const std::string exhaustive =
      "switch v{"
      "case Outer.Empty:return 1;"
      "case Outer.Wrap(Inner.None):return 2;"
      "case Outer.Wrap(Inner.Some(number)):return number+3;"
      "case Outer.Pair(Bit.A,Bit.A):return 4;"
      "case Outer.Pair(Bit.A,Bit.B):return 5;"
      "case Outer.Pair(Bit.B,Bit.A):return 6;"
      "case Outer.Pair(Bit.B,Bit.B):return 7;}";
  auto nested = good(declarations + "func pick(v Outer)i32{" + exhaustive +
                     "} func main()i32{return 0;}");
  need(nested.nodes.size() > 10, "nested switch missing HIR");

  good(declarations +
       "func pick(v Outer)i32{switch v{"
       "case Outer.Empty:return 0;"
       "case Outer.Wrap(Inner.None):return 1;"
       "case Outer.Wrap(_):return 2;"
       "case Outer.Pair(_,_):return 3;}} func main()i32{return 0;}");
  good(declarations +
       "func pick(v Outer)i32{switch v{"
       "case Outer.Empty:return 0;"
       "case Outer.Wrap(inner):return 1;"
       "case Outer.Pair(first,second):return 2;}} func main()i32{return 0;}");
  good("enum Never{} func use(v Never)i32{switch v{}} func main()i32{return 0;}");
  good("enum Never{} func use(v Never)i32{switch v{default:return 0;}} "
       "func main()i32{return 0;}");
  good("enum Inner{None;Some(value i32);} enum Box[T]{Empty;Value(item T);} "
       "enum Outer{Empty;Wrap(item Box[Inner]);} func pick(v Outer)i32{switch v{"
       "case Outer.Empty:return 0;"
       "case Outer.Wrap(Box[Inner].Empty):return 1;"
       "case Outer.Wrap(Box[Inner].Value(Inner.None)):return 2;"
       "case Outer.Wrap(Box[Inner].Value(Inner.Some(number))):return number;}} "
       "func main()i32{return 0;}");
  auto r = packages(
      "pub enum Inner{None;Some(value i32);} "
      "pub enum Outer{Empty;Wrap(item Inner);}",
      "func pick(v lib.Outer)i32{switch v{"
      "case lib.Outer.Empty:return 0;"
      "case lib.Outer.Wrap(lib.Inner.None):return 1;"
      "case lib.Outer.Wrap(lib.Inner.Some(number)):return number;}} "
      "func main()i32{return 0;}");
  need(r.success(), messages(r));

  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Empty:return 0;"
      "case Outer.Wrap(Inner.None):return 1;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Empty:return 0;"
      "case Outer.Wrap(Inner.None):return 1;"
      "case Outer.Wrap(Inner.Some(number)):return number;"
      "case Outer.Wrap(_):return 2;"
      "case Outer.Pair(_,_):return 3;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Empty:return 0;"
      "case Outer.Wrap(Inner.None):return 1;"
      "case Outer.Wrap(Inner.Some(number)):return number;"
      "case Outer.Pair(Bit.A,Bit.A):return 4;"
      "case Outer.Pair(Bit.A,Bit.B):return 5;"
      "case Outer.Pair(Bit.B,Bit.A):return 6;"
      "case Outer.Pair(Bit.B,Bit.B):return 7;"
      "case Outer.Pair(_,_):return 8;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Inner.None:return 0;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Wrap(Inner.Missing):return 0;"
      "case Outer.Empty:return 1;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Wrap():return 0;"
      "case Outer.Empty:return 1;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Wrap(Inner.None,Inner.None):return 0;"
      "case Outer.Empty:return 1;}} func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Wrap(Inner.Some):return 0;"
      "case Outer.Empty:return 1;}} func main()i32{return 0;}");
  bad("enum Inner{None;Some(value i32);} enum Box[T]{Empty;Value(item T);} "
      "enum Outer{Empty;Wrap(item Box[Inner]);} func pick(v Outer)i32{switch v{"
      "case Outer.Empty:return 0;"
      "case Outer.Wrap(Box[i32].Empty):return 1;"
      "case Outer.Wrap(Box[Inner].Value(Inner.None)):return 2;"
      "case Outer.Wrap(Box[Inner].Value(Inner.Some(number))):return number;}} "
      "func main()i32{return 0;}");
  bad(declarations + "func pick(v Outer)i32{switch v{"
      "case Outer.Empty:let inner=1;"
      "case Outer.Wrap(value):return 0;"
      "case Outer.Pair(first,second):return 0;} return value;} "
      "func main()i32{return 0;}", "E_NAME_NOT_FOUND");

  {
    std::string deep;
    for (size_t i = 0; i < 40; ++i)
      deep += "enum D" + std::to_string(i) + "{A(item " +
              (i == 39 ? "i32" : "D" + std::to_string(i + 1)) +
              ");B;} ";
    deep += "func pick(v D0)i32{switch v{"
            "case D0.A(inner):return 0;case D0.B:return 1;}} "
            "func main()i32{return 0;}";
    good(deep);
  }

  // F1: a non-enum prefix column must not discard later enum columns.
  const std::string prefix_decls =
      "enum Bit{A;B;} enum Pair{P(number i32,bit Bit);} ";
  good(prefix_decls +
       "func pick(v Pair)i32{switch v{"
       "case Pair.P(_,Bit.A):return 1;"
       "case Pair.P(_,Bit.B):return 2;}} func main()i32{return 0;}");
  bad(prefix_decls +
      "func pick(v Pair)i32{switch v{"
      "case Pair.P(_,Bit.A):return 1;}} func main()i32{return 0;}");
  bad(prefix_decls +
      "func pick(v Pair)i32{switch v{"
      "case Pair.P(_,Bit.A):return 1;"
      "case Pair.P(_,Bit.B):return 2;"
      "case Pair.P(_,_):return 3;}} func main()i32{return 0;}");
  bad(prefix_decls +
      "func pick(v Pair)i32{switch v{"
      "case Pair.P(_,Bit.A):return 1;"
      "case Pair.P(_,Bit.A):return 2;"
      "case Pair.P(_,Bit.B):return 3;}} func main()i32{return 0;}");
  good("enum Bit{A;B;} enum Pair2{P(bit Bit,number i32);} "
       "func pick(v Pair2)i32{switch v{"
       "case Pair2.P(Bit.A,_):return 1;"
       "case Pair2.P(Bit.B,_):return 2;}} func main()i32{return 0;}");
  const std::string vec_prefix =
      "enum Bit{A;B;} enum Pair{P(items Vec[i32],bit Bit);} ";
  good(vec_prefix +
       "func pick(v Pair)i32{switch v{"
       "case Pair.P(_,Bit.A):return 1;"
       "case Pair.P(_,Bit.B):return 2;}} func main()i32{return 0;}");
  bad(vec_prefix +
      "func pick(v Pair)i32{switch v{"
      "case Pair.P(_,Bit.A):return 1;}} func main()i32{return 0;}");
  bad(vec_prefix +
      "func pick(v Pair)i32{switch v{"
      "case Pair.P(_,Bit.A):return 1;"
      "case Pair.P(_,Bit.B):return 2;"
      "case Pair.P(_,_):return 3;}} func main()i32{return 0;}");
  const std::string parameter_prefix =
      "enum Bit{A;B;} enum Pair[T]{P(value T,bit Bit);} ";
  good(parameter_prefix +
       "func pick[T](v Pair[T])i32{switch v{"
       "case Pair[T].P(_,Bit.A):return 1;"
       "case Pair[T].P(_,Bit.B):return 2;}} "
       "func main()i32{let v=Pair[i32].P(value:7,bit:Bit.A);"
       "return pick[i32](v)-1;}");
  bad(parameter_prefix +
      "func pick[T](v Pair[T])i32{switch v{"
      "case Pair[T].P(_,Bit.A):return 1;}} func main()i32{return 0;}");

  // F2: explicit source type arguments in a template are compared after the
  // actual function/instance substitution, including a noncapturing lambda.
  const std::string generic_pattern =
      "enum Inner[T]{A(value T);B;} enum Outer[T]{One(value Inner[T]);} ";
  good(generic_pattern +
       "func pick[T](v Outer[T],fallback T)T{switch v{"
       "case Outer[T].One(Inner[T].A(x)):return x;"
       "case Outer[T].One(Inner[T].B):return fallback;}} "
       "func main()i32{let v=Outer[i32].One(value:Inner[i32].A(value:7));"
       "let z i32=0;return pick[i32](v,z)-7;}");
  good("enum Pair2[A,B]{P(left A,right B);} "
       "func pick[T,U](v Pair2[T,U])i32{switch v{"
       "case Pair2[T,U].P(left,right):return 0;}} "
       "func main()i32{let v=Pair2[i32,bool].P(left:1,right:true);"
       "return pick[i32,bool](v);}");
  good(generic_pattern +
       "func make[T]() (func(Outer[T]) i32) {"
       "return func(value Outer[T]) i32 {"
       "switch value {"
       "case Outer[T].One(Inner[T].A(_)):return 1;"
       "case Outer[T].One(Inner[T].B):return 0;}};} "
       "func main()i32{let f=make[i32]();"
       "let value=Outer[i32].One(value:Inner[i32].A(value:7));"
       "return f(value)-1;}");
  good("enum E[T]{A(value T);B;} "
       "func pick[T](e E[T])i32{switch e{"
       "case E[T].A(_):return 1;case E[T].B:return 0;}} "
       "func main()i32{let e=E[i32].A(value:7);return pick[i32](e)-1;}");
  bad(generic_pattern +
      "func pick[T](v Outer[T],fallback T)T{switch v{"
      "case Outer[bool].One(Inner[T].A(x)):return x;"
      "case Outer[T].One(Inner[T].B):return fallback;}} "
      "func main()i32{return 0;}");
  auto cross = packages(
      "pub enum Inner[T]{A(value T);B;} "
      "pub enum Outer[T]{One(value Inner[T]);}",
      "func pick[T](v lib.Outer[T],fallback T)T{switch v{"
      "case lib.Outer[T].One(lib.Inner[T].A(x)):return x;"
      "case lib.Outer[T].One(lib.Inner[T].B):return fallback;}} "
      "func main()i32{let n i32=7;let z i32=0;"
      "let v=lib.Outer[i32].One(value:lib.Inner[i32].A(value:n));"
      "return pick[i32](v,z)-7;}");
  need(cross.success(), messages(cross));
  auto cross_hir = hir::verify_types(*cross.value);
  need(cross_hir.success(), messages(cross_hir));
  auto cross_published = hir::verify(*cross.value);
  need(cross_published.success(), messages(cross_published));

  // Independent HIR corruption: type/variant/order/source/scope/exhaustive and
  // generic-instance pattern renaming must not be rescued by recomputation.
  auto shaped = good(
      "enum Inner{A(x i32);B(x i32);} enum Outer{Empty;Wrap(item Inner);} "
      "func pick(v Outer)i32{switch v{case Outer.Empty:return 0;"
      "case Outer.Wrap(Inner.A(x)):return x;"
      "case Outer.Wrap(Inner.B(y)):return y;}} func main()i32{return 0;}");
  auto &shaped_switch = [](hir::HirModule &x) -> hir::Switch & {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data))
        return *s;
    throw std::runtime_error("switch missing");
  }(shaped);
  need(shaped_switch.arms.size() == 3, "shaped switch arm count");
  invalid_nested(shaped, "nested inner variant", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data))
        for (auto &a : s->arms)
          if (a.pattern.variant && !a.pattern.payload.empty()) {
            auto &inner = a.pattern.payload[0];
            if (inner.variant) {
              for (const auto &d : x.nominals)
                for (const auto &v : d.variants)
                  if (v.symbol == *inner.variant)
                    for (const auto &other : d.variants)
                      if (other.symbol != *inner.variant &&
                          other.payload.size() == inner.payload.size()) {
                        inner.variant = other.symbol;
                        return;
                      }
            }
          }
    throw std::runtime_error("nested pattern missing");
  });
  invalid_nested(shaped, "nested pattern type", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data))
        for (auto &a : s->arms)
          if (a.pattern.variant && !a.pattern.payload.empty()) {
            a.pattern.payload[0].type = a.pattern.type;
            return;
          }
    throw std::runtime_error("nested pattern missing");
  });
  invalid_nested(shaped, "nested pattern source span", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data))
        for (auto &a : s->arms)
          if (a.pattern.variant && !a.pattern.payload.empty()) {
            a.pattern.payload[0].source.span.begin_byte =
                a.pattern.source.span.end_byte;
            return;
          }
    throw std::runtime_error("nested pattern missing");
  });
  invalid_nested(shaped, "switch exhaustive flag", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data)) {
        s->exhaustive = false;
        return;
      }
    throw std::runtime_error("switch missing");
  }, true);
  invalid_nested(shaped, "nested binding scope", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data)) {
        s->arms[1].scope = s->arms[0].scope;
        return;
      }
    throw std::runtime_error("switch missing");
  });

  auto ordered = good(
      "enum Inner{A;B;} enum Outer{Pair(first Inner,second Inner);} "
      "func pick(v Outer)i32{switch v{"
      "case Outer.Pair(Inner.A,Inner.B):return 1;"
      "case Outer.Pair(Inner.B,Inner.A):return 2;"
      "case Outer.Pair(Inner.A,Inner.A):return 3;"
      "case Outer.Pair(Inner.B,Inner.B):return 4;}} func main()i32{return 0;}");
  invalid_nested(ordered, "nested payload order", [](auto &x) {
    for (auto &n : x.nodes)
      if (auto s = std::get_if<hir::Switch>(&n.data)) {
        auto &payload = s->arms[0].pattern.payload;
        std::swap(payload[0], payload[1]);
        return;
      }
    throw std::runtime_error("switch missing");
  });

  // Literal patterns have their own independent source/value representation.
  // Mutating one field must not be rescued by another producer-provided field.
  auto literal = good(
      "enum E{A(x i32,s string);B(v Vec[i32]);C;} "
      "func scalar(x i32)i32{switch x{case 1:return 1;case 2:return 2;case _:return 0;}} "
      "func floating(x f32)i32{switch x{case 1.5:return 1;default:return 0;}} "
      "func text(x string)i32{switch x{case \"a\\0b\":return 1;default:return 0;}} "
      "func nested(e E)i32{switch e{case E.A(1,\"a\\0b\"):return 1;"
      "case E.A(_,_):return 2;case E.B(_):return 3;case E.C:return 4;}} "
      "func outer_then_inner(x i32)i32{switch x{case 1:"
      "switch x{case 2:return 2;default:return 1;}default:return 0;}} "
      "func main()i32{return 0;}");
  need(hir::verify_types(literal).success(),
       "valid literal HIR rejected before corruption controls");
  need(hir::verify(literal).success(), "valid literal HIR did not publish");
  {
    auto derived = hir::detail::produce_ownership(literal, {});
    need(derived.success(), "valid literal ownership recomputation failed");
    need(hir::verify_types(literal).success(),
         "valid literal HIR rejected after ownership recomputation");
  }
  auto literal_patterns = [](hir::HirModule &x, auto visit) {
    std::function<void(hir::Pattern &)> walk = [&](hir::Pattern &p) {
      if (p.literal)
        visit(p);
      for (auto &child : p.payload)
        walk(child);
    };
    for (auto &n : x.nodes)
      if (auto sw = std::get_if<hir::Switch>(&n.data))
        for (auto &arm : sw->arms)
          walk(arm.pattern);
  };
  invalid(literal, "literal plus binding", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (std::holds_alternative<hir::IntegerLiteral>(*p.literal))
        p.binding = SymbolId{0};
    });
  });
  invalid(literal, "literal plus variant", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (std::holds_alternative<hir::IntegerLiteral>(*p.literal))
        p.variant = SymbolId{0};
    });
  });
  invalid(literal, "literal payload", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (std::holds_alternative<hir::IntegerLiteral>(*p.literal))
        p.payload.emplace_back();
    });
  });
  invalid(literal, "literal alternative/type", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (std::holds_alternative<hir::IntegerLiteral>(*p.literal))
        p.literal = hir::BoolLiteral{true};
    });
  });
  invalid(literal, "integer literal decimal", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (auto v = std::get_if<hir::IntegerLiteral>(&*p.literal))
        v->decimal = "3";
    });
  });
  invalid(literal, "integer literal bits", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (auto v = std::get_if<hir::IntegerLiteral>(&*p.literal))
        ++v->bits;
    });
  });
  invalid(literal, "float literal bits", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (auto v = std::get_if<hir::FloatLiteral>(&*p.literal))
        ++v->bits;
    });
  });
  invalid(literal, "string literal NUL tail", [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (auto v = std::get_if<hir::StringLiteral>(&*p.literal))
        v->utf8.back() = 'c';
    });
  });
  invalid(literal, "literal source replacement", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto sw = std::get_if<hir::Switch>(&n.data))
        for (auto &arm : sw->arms)
          if (arm.pattern.literal) {
            arm.pattern.source = arm.source;
            return;
          }
    throw std::runtime_error("literal pattern missing");
  });
  invalid(literal, "literal deleted as ordinary case", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto sw = std::get_if<hir::Switch>(&n.data))
        for (auto &arm : sw->arms)
          if (arm.pattern.literal) {
            arm.pattern.literal.reset();
            return;
          }
    throw std::runtime_error("literal pattern missing");
  });
  invalid_recomputed(literal, "integer literal after ownership recompute",
                     [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (auto v = std::get_if<hir::IntegerLiteral>(&*p.literal))
        v->decimal = "9";
    });
  });
  auto outer_inner_source_swap = [](hir::HirModule &x) {
    auto node_source = [&](HirNodeId id) -> const hir::Source & {
      return x.nodes.at(id.value).source;
    };
    for (auto &outer : x.nodes) {
      auto *outer_switch = std::get_if<hir::Switch>(&outer.data);
      if (!outer_switch)
        continue;
      for (auto &arm : outer_switch->arms) {
        if (!arm.pattern.literal)
          continue;
        const auto body = node_source(arm.body);
        for (auto &inner : x.nodes) {
          if (!std::get_if<hir::Switch>(&inner.data))
            continue;
          if (inner.source.node.module != body.node.module ||
              inner.source.node.file != body.node.file ||
              inner.source.span.snapshot != body.span.snapshot ||
              inner.source.span.begin_byte < body.span.begin_byte ||
              inner.source.span.end_byte > body.span.end_byte)
            continue;
          for (auto &inner_arm : std::get<hir::Switch>(inner.data).arms) {
            if (!inner_arm.pattern.literal)
              continue;
            arm.pattern.source = inner_arm.pattern.source;
            arm.pattern.literal = inner_arm.pattern.literal;
            return;
          }
        }
      }
    }
    throw std::runtime_error("outer/inner case pattern missing");
  };
  invalid(literal, "outer literal source from body inner case",
          outer_inner_source_swap);
  invalid_recomputed_published(
      literal, "outer inner-case source after ownership recompute",
      outer_inner_source_swap);
  auto sibling_payload_source_swap = [](hir::HirModule &x) {
    for (auto &n : x.nodes)
      if (auto sw = std::get_if<hir::Switch>(&n.data))
        for (auto &arm : sw->arms)
          if (arm.pattern.variant && arm.pattern.payload.size() >= 2) {
            std::swap(arm.pattern.payload[0].source,
                      arm.pattern.payload[1].source);
            return;
          }
    throw std::runtime_error("sibling payload sources missing");
  };
  invalid(literal, "sibling payload source swap",
          sibling_payload_source_swap);
  invalid_recomputed_published(
      literal, "sibling payload source swap after ownership recompute",
      sibling_payload_source_swap);
  auto narrowed_literal_span = [&](auto &x) {
    literal_patterns(x, [](hir::Pattern &p) {
      if (p.source.span.begin_byte < p.source.span.end_byte)
        ++p.source.span.begin_byte;
    });
  };
  invalid(literal, "literal source span narrowed", narrowed_literal_span);
  invalid_recomputed_published(
      literal, "literal source span narrowed after ownership recompute",
      narrowed_literal_span);
  std::cout << "literal-source-ownership: outer/inner case, sibling payload, "
               "narrowed span rejected directly, published, and after "
               "ownership recomputation\n";

  auto generic = good(
      "enum E[T]{A(x T);} enum W[T]{A(x E[T]);} "
      "func unwrap[T](w W[T])T{switch w{case W.A(E.A(x)):return x;}} "
      "func main()i32{let a=unwrap(W[i32].A(x:E[i32].A(x:1)));"
      "let b=unwrap(W[i64].A(x:E[i64].A(x:2)));return a;}");
  invalid_nested(generic, "nested generic instance pattern rename", [](auto &x) {
    need(x.instances.size() >= 2, "generic instances missing");
    auto &a = x.instances[0];
    const auto &b = x.instances[1];
    for (auto &entry : a.renamed) {
      auto original = x.resolved->symbols[entry.first.value].definition;
      if (original && x.resolved->project->module(original->module)
                          .files[original->file]->node(original->node).kind ==
                          AstKind::BindPattern)
        for (auto [from, to] : b.renamed)
          if (from == entry.first) {
            entry.first = to;
            return;
          }
    }
    throw std::runtime_error("nested pattern rename target missing");
  });
}


void aggregate_inout_validation() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    return *r.value;
  };
  auto bad = [](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code),
         text + "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source rejection became E_IR_INVALID");
  };
  auto unavailable = [](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE"),
         text + "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source E_CAP became E_IR_INVALID");
  };
  auto unsupported_budget = [](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE") &&
             messages(r).find("exceeds checking budget") != std::string::npos,
         "growing inout graph did not stop at its explicit budget: " + text +
             "\n" + messages(r));
    need(!has(r, "E_IR_INVALID"), "source budget became E_IR_INVALID");
  };
  auto named_type = [](const hir::HirModule &m, const std::string &name) {
    for (auto &d : m.nominals)
      if (m.resolved->symbols.at(d.symbol.value).name == name)
        for (uint64_t i = 1; i <= m.types.size(); ++i)
          if (auto t = m.types.find(TypeId{i});
              t && t->kind == d.kind && t->declaration == d.symbol)
            return TypeId{i};
    throw std::runtime_error("named aggregate type missing: " + name);
  };

  const std::string types =
      "struct Empty{} struct Pair{x i32;y i32;}"
      "struct Leaf{empty Empty;unit Unit;callback func(i32)i32;pad u64;}"
      "enum Choice{None;Value(v i32);Group(pair Pair,tag i32);} ";
  auto aggregate = good(types +
      "func noop(){} func inc(x i32)i32{return x+1;}"
      "func swap[T](a inout T,b inout T){let t=a;a=b;b=t;}"
      "func replace(slot inout Leaf,value Leaf)i32{let old=slot.pad;"
      "slot=value;return i32(old);}"
      "func main()i32{var a=Leaf(empty:Empty(),unit:noop(),"
      "callback:inc,pad:1);var b=a;swap(inout a,inout b);"
      "var c=Choice.None;var d=Choice.Value(v:2);"
      "swap(inout c,inout d);return 0;}");
  need(hir::verify_types(aggregate).success(),
       "aggregate inout HIR rejected: " + messages(hir::verify_types(aggregate)));
  need(hir::verify(aggregate).success(), "aggregate inout did not publish");
  auto leaf = named_type(aggregate, "Leaf");
  auto pair = named_type(aggregate, "Pair");
  auto choice = named_type(aggregate, "Choice");
  need(hir::inout_data_type(aggregate, leaf),
       "Unit/empty/function fields were not accepted inside a POD aggregate");
  need(hir::classify_value(aggregate, leaf).pod_inout,
       "aggregate ValueOps.pod_inout was not updated");
  need(hir::classify_value(aggregate, pair).pod_inout &&
           hir::classify_value(aggregate, choice).pod_inout,
       "nested struct/enum POD classification missing");
  bool saw_unit = false, saw_function = false;
  for (uint64_t i = 1; i <= aggregate.types.size(); ++i) {
    auto t = aggregate.types.find(TypeId{i});
    if (t->kind == hir::TypeKind::Unit) {
      saw_unit = true;
      need(!hir::inout_data_type(aggregate, TypeId{i}),
           "top-level Unit opened as inout data");
      need(!hir::classify_value(aggregate, TypeId{i}).pod_inout,
           "Unit leaf ValueOps.pod_inout opened");
    }
    if (t->kind == hir::TypeKind::Function) {
      saw_function = true;
      need(!hir::inout_data_type(aggregate, TypeId{i}),
           "top-level Function opened as inout data");
      need(!hir::classify_value(aggregate, TypeId{i}).pod_inout,
           "Function leaf ValueOps.pod_inout opened");
    }
  }
  need(saw_unit && saw_function, "Unit/Function types missing from probe");

  unavailable("struct Own{items Vec[i32];} func f(slot inout Own){} "
              "func main()i32{return 0;}");
  unavailable("enum Owned{Empty;Value(items Vec[i32]);} "
              "func f(slot inout Owned){} func main()i32{return 0;}");
  unavailable("func f(slot inout Vec[i32]){} func main()i32{return 0;}");
  unavailable("func f(slot inout string){} func main()i32{return 0;}");
  unavailable("func f(slot inout Unit){} func main()i32{return 0;}");
  unavailable("func f(slot inout func(i32)i32){} func main()i32{return 0;}");
  unavailable("func f[T](slot inout T){} func main()i32{"
              "var v Vec[i32]=[1];f(inout v);return 0;}");
  unavailable("struct F{callback func(inout Vec[i32]);} "
              "func f(slot inout F){} func main()i32{return 0;}");

  bad("struct S{x i32;} func f(slot inout S){} func main()i32{"
      "let s=S(x:1);f(inout s);return 0;}",
      "E_CALL_ARGUMENT");
  bad("struct S{x i32;} func f(slot inout S){} func outer(s S){"
      "f(inout s);} func main()i32{return 0;}",
      "E_CALL_ARGUMENT");
  bad("struct S{x i32;} func f(a inout S,b inout S){} func main()i32{"
      "var s=S(x:1);f(inout s,inout s);return 0;}",
      "E_CALL_ARGUMENT");
  bad("struct S{x i32;} func g(slot inout S,other i32){} func main()i32{"
      "var s=S(x:1);g(inout s,s.x);return 0;}",
      "E_CALL_ARGUMENT");
  bad("struct S{x i32;} func inner(slot inout S)i32{return 0;}"
      "func outer(slot inout S,other i32){} func main()i32{"
      "var s=S(x:1);outer(inout s,inner(inout s));return 0;}",
      "E_CALL_ARGUMENT");
  bad("struct S{x i32;} func g(slot inout S,other S){} func main()i32{"
      "var s=S(x:1);g(inout s,move s);return 0;}",
      "E_CALL_ARGUMENT");

  auto mutable_hir = good(
      "struct Inner{x i32;} struct Outer{inner Inner;}"
      "struct Holder{items Vec[i32];}"
      "func replace(slot inout Outer){} func main()i32{"
      "var o=Outer(inner:Inner(x:1));replace(inout o);return 0;}");
  // A forged nominal field turns the transitive inout data into an owning
  // Vec. Recomputing ownership must not be enough to publish it.
  invalid_recomputed(mutable_hir, "aggregate inout owning forged field",
                     [&](auto &x) {
    TypeId vector{0};
    for (uint64_t i = 1; i <= x.types.size(); ++i)
      if (x.types.find(TypeId{i})->kind == hir::TypeKind::Vec)
        vector = TypeId{i};
    need(vector.value, "Vec type missing from forged field source");
    for (auto &d : x.nominals)
      if (x.resolved->symbols.at(d.symbol.value).name == "Inner") {
        need(d.fields.size() == 1, "Inner field changed");
        d.fields[0].type = vector;
        return;
      }
    throw std::runtime_error("Inner declaration missing");
  });

  auto place_hir = good(
      "struct S{x i32;} func replace(slot inout S){} func main()i32{"
      "var s=S(x:1);replace(inout s);return 0;}");
  // The call still carries the right type/mode, but its argument is no longer
  // the required whole Place. Recomputing ownership must not hide this.
  invalid_recomputed(place_hir, "aggregate inout argument replaced by read",
                     [](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data))
        for (auto &a : c->args)
          if (a.mode == hir::ParamMode::inout) {
            auto &argument = x.nodes[a.expression.value];
            auto symbol = std::get<hir::Place>(argument.data).symbol;
            argument.data = hir::Read{symbol};
            return;
          }
    throw std::runtime_error("aggregate inout Place missing");
  });

  // Correction regressions: same nominal with a strictly smaller concrete
  // argument is finite; fixed callback data is not Self-dependent; and a real
  // Self wrapped in a generic aggregate follows the concrete Self data rule.
  auto same_nominal = good(
      "struct Box[T]{value T;} "
      "func replace[T](slot inout T,next T)T{"
      "let old=slot;slot=next;return old;} "
      "func main()i32{var slot=Box[Box[i32]](value:Box[i32](value:3));"
      "let next=Box[Box[i32]](value:Box[i32](value:8));"
      "let old=replace[Box[Box[i32]]](inout slot,next);"
      "if old.value.value!=3||slot.value.value!=8{return 1;}return 0;}");
  need(hir::verify(same_nominal).success(),
       "same-nominal finite aggregate was rejected");

  auto fixed_callback = good(
      "struct Pair{value i32;} struct Owner{token i32;} "
      "trait Edit{func apply(self Self,slot inout Pair,"
      "callback func(inout Pair)Unit)Unit;} "
      "impl Edit for Pair{func apply(self Self,slot inout Pair,"
      "callback func(inout Pair)Unit)Unit{callback(inout slot);}} "
      "impl Edit for Owner{func apply(self Self,slot inout Pair,"
      "callback func(inout Pair)Unit)Unit{callback(inout slot);}} "
      "impl Edit for i32{func apply(self Self,slot inout Pair,"
      "callback func(inout Pair)Unit)Unit{callback(inout slot);}} "
      "func bump(slot inout Pair)Unit{slot.value=slot.value+5;} "
      "func main()i32{let a=Pair(value:100);var p=Pair(value:7);"
      "a.apply(inout p,bump);let b=Owner(token:1);"
      "b.apply(inout p,bump);let c i32=1;c.apply(inout p,bump);return 0;}");
  need(hir::verify(fixed_callback).success(),
       "fixed POD callback with coincident Self was rejected");
  auto pair_type = named_type(fixed_callback, "Pair");
  const hir::TraitDecl *edit = nullptr;
  for (const auto &tr : fixed_callback.traits)
    if (fixed_callback.resolved->symbols.at(tr.symbol.value).name == "Edit")
      edit = &tr;
  need(edit != nullptr, "fixed callback trait missing");
  const hir::MethodPrototype *apply = nullptr;
  for (const auto &p : edit->methods)
    if (fixed_callback.resolved->symbols.at(p.symbol.value).name == "apply")
      apply = &p;
  need(apply != nullptr, "fixed callback prototype missing");
  auto callback_type = [](const hir::HirModule &m, const hir::MethodPrototype &p) {
    const auto &sig = *m.types.find(p.signature);
    for (const auto &param : sig.parameters)
      if (m.types.find(param.type)->kind == hir::TypeKind::Function)
        return param.type;
    throw std::runtime_error("callback parameter missing");
  };
  (void)callback_type(fixed_callback, *apply);
  (void)pair_type;

  const std::string function_cycle =
      "struct Wrap[T]{apply func(Wrap[T])T;value T;}"
      "struct Owner{token i32;} ";
  auto fixed_cycle = good(function_cycle +
      "trait Edit{func apply(self Self,slot inout Wrap[i32],"
      "callback func(inout Wrap[i32])Unit)Unit;}"
      "impl Edit for Owner{func apply(self Self,slot inout Wrap[i32],"
      "callback func(inout Wrap[i32])Unit)Unit{callback(inout slot);}}"
      "func read(value Wrap[i32])i32{return value.value;}"
      "func bump(slot inout Wrap[i32])Unit{slot.value=slot.value+5;}"
      "func main()i32{let owner=Owner(token:100);"
      "var slot=Wrap[i32](apply:read,value:7);owner.apply(inout slot,bump);"
      "if slot.apply(slot)!=12{return 1;}return 0;}");
  need(hir::verify(fixed_cycle).success(),
       "fixed callback function cycle was classified as Self-dependent");
  auto growing_signature = good(
      "struct Wrap[T]{factory func()Wrap[Wrap[T]];}"
      "struct Owner{token i32;}"
      "trait Edit{func apply(self Self,slot inout Wrap[i32],"
      "callback func(inout Wrap[i32])Unit)Unit;}"
      "impl Edit for Owner{func apply(self Self,slot inout Wrap[i32],"
      "callback func(inout Wrap[i32])Unit)Unit{}}"
      "func main()i32{return 0;}");
  need(hir::verify(growing_signature).success(),
       "ordinary growing function signature was rejected as an inout graph");
  auto recursive_self_callback = good(function_cycle +
      "trait Edit{func apply(self Self,"
      "callback func(inout Wrap[i32],inout Self)Unit)Unit;}"
      "impl Edit for Owner{func apply(self Self,"
      "callback func(inout Wrap[i32],inout Self)Unit)Unit{}}"
      "func main()i32{return 0;}");
  need(hir::verify(recursive_self_callback).success(),
       "recursive POD Self callback did not publish");

  auto wrapped_self = good(
      "struct Wrap[T]{value T;} struct Pair{value i32;} "
      "trait Edit{func edit(self Self,slot inout Wrap[Self],"
      "next Wrap[Self])Wrap[Self];} "
      "impl Edit for Pair{func edit(self Self,slot inout Wrap[Self],"
      "next Wrap[Self])Wrap[Self]{let old=slot;slot=next;return old;}} "
      "func main()i32{let o=Pair(value:1);var s=Wrap[Pair](value:Pair(value:3));"
      "let old=o.edit(inout s,Wrap[Pair](value:Pair(value:8)));"
      "if old.value.value!=3||s.value.value!=8{return 1;}return 0;}");
  need(hir::verify(wrapped_self).success(),
       "wrapped Self was not accepted as a concrete POD aggregate");
  unavailable("struct Wrap[T]{value T;} struct Own{items Vec[i32];} "
              "trait Edit{func edit(self Self,slot inout Wrap[Self])Unit;} "
              "impl Edit for Own{func edit(self Self,slot inout Wrap[Self])"
              "Unit{}} func main()i32{return 0;}");
  // Value-form actual-self inout remains an explicit capability limit, even
  // though the same explicit Type call would be a legal POD whole binding.
  unavailable("struct Pair{value i32;} impl Pair{func bad(self inout Pair){}} "
              "func main()i32{var p=Pair(value:1);p.bad(inout p);return 0;}");
  good("struct Pair{value i32;} "
       "trait Apply{func run[U](owner Self,"
       "callback func(inout U))Unit;} "
       "impl Apply for Pair{func run[U](owner Self,"
       "callback func(inout U))Unit{}} "
       "func main()i32{return 0;}");

  auto self_callback = good(
      "struct Pair{value i32;} "
      "trait V{func bad(self Self,callback func(inout Self))Unit;} "
      "impl V for i32{func bad(self Self,callback func(inout Self))Unit{}} "
      "func main()i32{let p=Pair(value:1);return p.value-1;}");
  need(hir::verify(self_callback).success(),
       "numeric Self callback did not publish");
  auto self_pair_type = named_type(self_callback, "Pair");
  const hir::TraitDecl *v = nullptr;
  for (const auto &tr : self_callback.traits)
    if (self_callback.resolved->symbols.at(tr.symbol.value).name == "V")
      v = &tr;
  need(v != nullptr, "Self callback trait missing");
  const hir::MethodPrototype *bad_callback = nullptr;
  for (const auto &p : v->methods)
    if (self_callback.resolved->symbols.at(p.symbol.value).name == "bad")
      bad_callback = &p;
  need(bad_callback != nullptr, "Self callback prototype missing");
  auto i32_type = TypeId{0};
  for (uint64_t i = 1; i <= self_callback.types.size(); ++i) {
    auto t = self_callback.types.find(TypeId{i});
    if (t->kind == hir::TypeKind::SignedInt && t->bits == 32) {
      i32_type = TypeId{i};
      break;
    }
  }
  need(i32_type.value, "i32 type missing");
  auto self_component = callback_type(self_callback, *bad_callback);
  need(self_component.value && i32_type.value && v->self.value,
       "Self callback component identities missing");
  (void)self_pair_type;
}


void generic_trait_method_generics() {
  auto good = [](const std::string &s) {
    auto r = source(s);
    need(r.success(), s + "\n" + messages(r));
    auto verified = hir::verify_types(*r.value);
    need(verified.success(), "generic trait own method HIR rejected: " + s +
                                 "\n" + messages(verified));
    auto published = hir::verify(*r.value);
    need(published.success(), "generic trait own method did not publish: " + s +
                                  "\n" + messages(published));
    return *r.value;
  };
  auto bad = [](const std::string &s) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::source &&
             (has(r, "E_TYPE_MISMATCH") || has(r, "E_CALL_ARGUMENT")) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         s + "\n" + messages(r));
  };
  auto unsupported = [](const std::string &s) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE") &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         s + "\n" + messages(r));
  };
  auto unsupported_both = [&](const std::string &s) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(s, backend);
      need(!r.success() && r.failure == FailureKind::unsupported &&
               has(r, "E_CAPABILITY_UNAVAILABLE") &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
           std::string(backend) + ": " + s + "\n" + messages(r));
    }
  };

  // A generic trait's ordinary method can carry several own parameters with
  // one existing bound. The bound may name the ordered trait argument or the
  // real Self, and the impl may alpha-rename both positions. Value, concrete
  // Type and bounded T entries share the complete application.
  const std::string alpha_bounds = R"(
trait Tag[A] { func code(owner Self) i32; }
struct One { n i32; }
struct Two { n i32; }
struct Key {}
impl Tag[bool] for i32 { func code(owner Self) i32 { return 14; } }
impl Tag[Key] for One { func code(owner Self) i32 { return 21; } }
trait Use[A] {
    func read[U Tag[A]](owner Self, x U) i32;
    func pair[P,Q](owner Self, first P, second Q) Q;
}
impl[I] Use[I] for Key {
    func read[V Tag[I]](owner Self, x V) i32 { return x.code(); }
    func pair[X,Y](owner Self, first X, second Y) Y { return second; }
}
trait SelfUse[A] { func own[U Tag[Self]](owner Self, x U) i32; }
impl SelfUse[bool] for Key {
    func own[W Tag[Key]](owner Self, x W) i32 { return x.code(); }
}
func via[T Use[bool],P,Q](owner T, first P, second Q) i32 {
    let a = T.read[i32](owner, 14);
    let b = owner.read(i32(14));
    let c i32 = T.pair[P,i32](owner, first, 7);
    return a + b + c - 35;
}
func main() i32 {
    let k = Key();
    if via[Key,i32,i32](k, 1, 7) != 0 { return 1; }
    if k.own[One](One(n: 0)) != 21 { return 3; }
    if k.own(One(n: 0)) != 21 { return 4; }
    return 0;
}
)";
  auto alpha = good(alpha_bounds);

  invalid_recomputed(alpha, "generic trait own Source suffix signature",
                     [&](auto &h) {
                       TypeId bool_id{};
                       for (uint64_t id = 1; id <= h.types.size(); ++id) {
                         auto t = h.types.find(TypeId{id});
                         if (t && t->kind == hir::TypeKind::Bool) {
                           bool_id = TypeId{id};
                           break;
                         }
                       }
                       need(bool_id.value, "bool type missing");
                       for (auto &n : h.nodes)
                         if (auto c =
                                 std::get_if<hir::ConstraintCall>(&n.data)) {
                           auto sig = *h.types.find(c->signature);
                           sig.result = bool_id;
                           c->signature = h.types.intern(sig);
                           return;
                         }
                       throw std::runtime_error("alpha ConstraintCall missing");
                     });

  // Synthetic declarations and bad alpha/bound relationships remain source
  // errors even when the method is never called.
  bad("trait Tag[A]{func code(owner Self)i32;}"
      "trait Use[A]{func read[U Tag[A]](owner Self,x U)i32;}"
      "struct Key{} impl[I] Use[I] for Key{"
      "func read[V Tag[bool]](owner Self,x V)i32{return 1;}}"
      "func main()i32{return 0;}");
  bad("trait Tag[A]{func code(owner Self)i32;}"
      "trait Use[A]{func own[U Tag[Self]](owner Self,x U)i32;}"
      "struct Key{} struct Other{} "
      "impl Tag[Key] for Key{func code(owner Self)i32{return 1;}}"
      "impl Use[bool] for Key{func own[V Tag[Other]](owner Self,x V)i32{"
      "return 1;}} func main()i32{return 0;}");
  bad("trait Convert[A]{func get[U](owner Self,x U)U;}"
      "struct Key{} impl Convert[i32] for Key{"
      "func get[U](owner Self,x U)U{return x;}}"
      "impl Convert[bool] for Key{func get[U](owner Self,x U)U{return x;}}"
      "func main()i32{let k=Key();return k.get(1);}");

  // Top-level non-self modes combine with own parameters. Dependent inout
  // callbacks in parameters, returns and throws remain legal templates until
  // a complete application substitutes concrete data.
  good("trait New[A]{func take[U](owner Self,value move U)U;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func update[U](owner Self,slot inout U)Unit;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func run[U](owner Self,cb func(inout U));}"
       "func main()i32{return 0;}");

  // The complete graph is rechecked after nominal fields exist. These hidden
  // callbacks still depend only on deferred Self/trait/method-own identities,
  // so they are legal declarations; concrete non-POD applications are checked
  // in the dedicated rejection cases below.
  const std::string holder =
      "struct Holder[T]{cb func(inout T);}";
  const std::string main_zero = "func main()i32{return 0;}";
  for (const char *backend : {"native", "python"}) {
    for (const std::string &program : {
             holder +
                 "trait Work[A]{func run[U](owner Self,holder Holder[U]);}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run[U](owner Self)Holder[U];}" +
                 main_zero,
             std::string("enum Holder[T]{Item(cb func(inout T));}") +
                 "trait Work[A]{func run[U](owner Self,holder Holder[U]);}" +
                 main_zero,
             std::string("enum Err[T]{Fault(cb func(inout T));}") +
                 "trait Work[A]{func run[U](owner Self) throws Err[U];}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run[U](owner Self)(func()Holder[U]);}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run[U](owner Self,holder Holder[A]);}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run[U](owner Self,holder Holder[Self]);}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run(owner Self,holder Holder[Self]);}" +
                 main_zero,
             holder +
                 "trait Work[A]{func run(owner Self,holder Holder[A]);}" +
                 main_zero}) {
      auto r = source(program, backend);
      need(r.success(),
           std::string(backend) + ": dependent hidden callback rejected\n" +
               messages(r));
      need(hir::verify(*r.value).success(),
           std::string(backend) + ": dependent hidden callback HIR rejected");
      need(hir::verify_types(*r.value).success(),
           std::string(backend) + ": dependent hidden callback types rejected");
    }
  }

  // Fixed callback data stays legal for generic and non-generic traits, and
  // the old ordinary non-generic path remains accepted.
  for (const char *backend : {"native", "python"}) {
    auto fixed = source(
        holder +
            "trait Work[A]{func run[U](owner Self,holder Holder[i32]);}" +
            "trait Plain{func run(owner Self,holder Holder[i32]);}" +
            main_zero,
        backend);
    need(fixed.success(),
         std::string(backend) + ": fixed hidden callback was rejected\n" +
             messages(fixed));
    need(hir::verify(*fixed.value).success(),
         std::string(backend) + ": fixed hidden callback HIR rejected");
    need(hir::verify_types(*fixed.value).success(),
         std::string(backend) + ": fixed hidden callback types rejected");
  }


  // Four complete [P,V] phantom instances share one Flag.stamp origin. A
  // repeated combination is deduplicated, while equal method signatures do
  // not merge different P or V.
  const std::string phantom = R"(
trait Code { func code() i32; }
struct One {} struct Two {} struct Flag {}
impl Code for One { func code() i32 { return 14; } }
impl Code for Two { func code() i32 { return 25; } }
trait Mark[A] { func stamp[U Code](owner Self) i32; }
impl[P] Mark[P] for Flag {
    func stamp[V Code](owner Self) i32 { return V.code(); }
}
func drive[A,U Code,T Mark[A]](owner T) i32 { return T.stamp[U](owner); }
func main() i32 {
    let f = Flag();
    if drive[i32,One,Flag](f) != 14 || drive[i32,Two,Flag](f) != 25 ||
       drive[bool,One,Flag](f) != 14 || drive[bool,Two,Flag](f) != 25 ||
       drive[i32,One,Flag](f) != 14 { return 1; }
    return 0;
}
)";
  auto m = good(phantom);
  auto name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  auto type_named = [&](const std::string &n) -> TypeId {
    for (uint64_t id = 1; id <= m.types.size(); ++id) {
      auto t = m.types.find(TypeId{id});
      if (!t)
        continue;
      if ((t->kind == hir::TypeKind::NominalStruct ||
           t->kind == hir::TypeKind::NominalEnum) &&
          name(t->declaration) == n)
        return TypeId{id};
      if (n == "bool" && t->kind == hir::TypeKind::Bool)
        return TypeId{id};
      if (n == "i32" && t->kind == hir::TypeKind::SignedInt &&
          t->bits == 32)
        return TypeId{id};
    }
    throw std::runtime_error("test type missing: " + n);
  };
  const hir::TraitDecl *mark_trait = nullptr;
  for (const auto &t : m.traits)
    if (name(t.symbol) == "Mark")
      mark_trait = &t;
  need(mark_trait && mark_trait->params.size() == 1 &&
           mark_trait->methods.size() == 1,
       "generic trait own method declaration missing");
  const hir::ImplDecl *mark_impl = nullptr;
  for (const auto &im : m.impls)
    if (im.trait && mark_trait && im.trait->declaration == mark_trait->symbol)
      mark_impl = &im;
  need(mark_impl && mark_impl->params.size() == 1 &&
           mark_impl->methods.size() == 1,
       "generic trait own method impl missing");
  const auto stamp = mark_impl->methods.front();
  std::vector<const hir::FunctionInstance *> stamp_instances;
  for (const auto &i : m.instances)
    if (i.origin == stamp)
      stamp_instances.push_back(&i);
  need(stamp_instances.size() == 4,
       "phantom own method instances were not four complete keys");
  std::set<std::pair<uint64_t, uint64_t>> keys;
  const auto i32 = type_named("i32");
  const auto bool_type = type_named("bool");
  const auto one = type_named("One");
  const auto two = type_named("Two");
  for (const auto *i : stamp_instances) {
    need(i->type_args.size() == 2, "phantom own instance key arity");
    keys.insert({i->type_args[0].value, i->type_args[1].value});
  }
  need(keys == std::set<std::pair<uint64_t, uint64_t>>{
                   {i32.value, one.value}, {i32.value, two.value},
                   {bool_type.value, one.value}, {bool_type.value, two.value}},
       "phantom own method complete instance keys were merged or lost");
  size_t drive_instances = 0;
  for (const auto &i : m.instances)
    if (name(i.origin) == "drive")
      ++drive_instances;
  need(drive_instances == 4,
       "duplicate phantom drive combination was not deduplicated");
  size_t mark_constraints = 0;
  for (const auto &n : m.nodes)
    if (auto c = std::get_if<hir::ConstraintCall>(&n.data);
        c && c->trait.declaration == mark_trait->symbol) {
      need(c->trait.args.size() == 1 && c->args.size() == 1,
           "phantom constraint call lost its complete trait application");
      ++mark_constraints;
    }
  need(mark_constraints == 1,
       "phantom constraint call count changed");

  invalid(m, "generic trait own bound trait application swapped",
          [&](auto &h) {
            for (auto &n : h.nodes)
              if (auto c = std::get_if<hir::ConstraintCall>(&n.data))
                if (c->trait.declaration == mark_trait->symbol) {
                  c->trait.args[0] = bool_type;
                  return;
                }
            throw std::runtime_error("phantom ConstraintCall missing");
          });
  invalid_recomputed(m, "generic trait own instance key changed",
                     [&](auto &h) {
                       for (auto &i : h.instances)
                         if (i.origin == stamp && i.type_args.size() == 2 &&
                             i.type_args[0] == i32) {
                           i.type_args[1] = two;
                           return;
                         }
                       throw std::runtime_error("phantom instance missing");
                     });
  invalid_recomputed(m, "generic trait own bound-violating instance key",
                     [&](auto &h) {
                       for (auto &i : h.instances)
                         if (i.origin == stamp && i.type_args.size() == 2) {
                           i.type_args[1] = i32;
                           return;
                         }
                       throw std::runtime_error("phantom instance missing");
                     });
  invalid_recomputed(m, "generic trait own clone rename",
                     [&](auto &h) {
                       auto first = std::find_if(
                           h.instances.begin(), h.instances.end(),
                           [&](const auto &i) { return i.origin == stamp; });
                       auto second = std::find_if(
                           first + 1, h.instances.end(),
                           [&](const auto &i) { return i.origin == stamp; });
                       need(first != h.instances.end() &&
                                second != h.instances.end() &&
                                !first->renamed.empty() &&
                                !second->renamed.empty(),
                            "phantom renamed maps missing");
                       first->renamed.front().second =
                           second->renamed.front().second;
                     });
  invalid_recomputed(m, "generic trait own call target same-signature swap",
                     [&](auto &h) {
                       const hir::FunctionInstance *owner = nullptr;
                       for (const auto &i : h.instances)
                         if (name(i.origin) == "drive" &&
                             i.type_args.size() == 3 &&
                             i.type_args[1] == one)
                           owner = &i;
                       need(owner, "drive instance missing");
                       SymbolId replacement{};
                       for (const auto &i : h.instances)
                         if (i.origin == stamp && i.type_args.size() == 2 &&
                             i.type_args[1] == two)
                           replacement = i.symbol;
                       need(replacement.value, "replacement stamp instance missing");
                       for (auto &n : h.nodes)
                         if (h.scopes[n.scope.value].function == owner->symbol)
                           if (auto c = std::get_if<hir::Call>(&n.data);
                               c && c->direct_symbol) {
                             c->direct_symbol = replacement;
                             return;
                           }
                       throw std::runtime_error("concrete stamp call missing");
                     });
}

void generic_trait_applications() {
  auto good = [](const std::string &s) {
    auto r = source(s);
    need(r.success(), s + "\n" + messages(r));
    auto verified = hir::verify(*r.value);
    need(verified.success(), messages(verified));
    return *r.value;
  };
  auto bad = [](const std::string &s, const std::string &code) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::source && has(r, code) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         s + "\n" + messages(r));
  };
  auto unsupported = [](const std::string &s) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE") &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         s + "\n" + messages(r));
  };
  auto scalar = [](const hir::HirModule &m, hir::TypeKind kind,
                   unsigned bits) -> TypeId {
    for (uint64_t id = 1; id <= m.types.size(); ++id) {
      auto t = m.types.find(TypeId{id});
      if (t && t->kind == kind && t->bits == bits)
        return TypeId{id};
    }
    throw std::runtime_error("scalar type missing from HIR context");
  };
  auto same_trait = [](const hir::ImplDecl &im, SymbolId trait,
                       std::vector<TypeId> args) {
    return im.trait && im.trait->declaration == trait &&
           im.trait->args == args;
  };

  // Same Self, two complete applications whose trait parameters do not occur
  // in the method signature. Value/T/forwarded calls must each select by the
  // full bound identity, and concrete instances must be ordinary Calls.
  const std::string dual =
      "trait Convert[A,B]{func code(owner Self)i32;}"
      "struct Key{value i32;}"
      "impl Convert[i32,bool] for Key{func code(owner Self)i32{"
      "return owner.value+11;}}"
      "impl Convert[bool,i32] for Key{func code(owner Self)i32{"
      "return owner.value+22;}}"
      "func first[T Convert[i32,bool]](owner T)i32{return owner.code();}"
      "func second[T Convert[bool,i32]](owner T)i32{return T.code(owner);}"
      "func main()i32{let key=Key(value:3);"
      "if first(key)!=14||second(key)!=25{return 1;}return 0;}";
  auto dual_module = good(dual);
  const hir::TraitDecl *convert = nullptr;
  for (const auto &t : dual_module.traits)
    if (dual_module.resolved->symbols[t.symbol.value].name == "Convert")
      convert = &t;
  need(convert && convert->params.size() == 2,
       "generic trait parameters were not published");
  for (const auto &p : convert->params)
    need(!p.trait, "generic trait parameter gained a bound");
  auto i32 = scalar(dual_module, hir::TypeKind::SignedInt, 32);
  auto bool_type = scalar(dual_module, hir::TypeKind::Bool, 0);
  const hir::ImplDecl *first_impl = nullptr;
  const hir::ImplDecl *second_impl = nullptr;
  for (const auto &im : dual_module.impls)
    if (same_trait(im, convert->symbol, {i32, bool_type}))
      first_impl = &im;
    else if (same_trait(im, convert->symbol, {bool_type, i32}))
      second_impl = &im;
  need(first_impl && second_impl,
       "same-Self complete trait applications were merged or lost");
  const hir::FunctionTemplate *first_template = nullptr;
  const hir::FunctionTemplate *second_template = nullptr;
  for (const auto &t : dual_module.templates) {
    auto name = dual_module.resolved->symbols[t.function.symbol.value].name;
    if (name == "first")
      first_template = &t;
    if (name == "second")
      second_template = &t;
  }
  need(first_template && second_template &&
           first_template->params.size() == 1 &&
           first_template->params[0].trait &&
           first_template->params[0].trait->declaration == convert->symbol &&
           first_template->params[0].trait->args ==
               std::vector<TypeId>{i32, bool_type} &&
           second_template->params[0].trait->args ==
               std::vector<TypeId>{bool_type, i32},
       "template bounds did not retain ordered trait arguments");
  size_t constraints = 0;
  for (const auto &n : dual_module.nodes)
    if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
      need(c->trait.declaration == convert->symbol &&
               c->trait.args.size() == 2,
           "ConstraintCall lost its complete trait application");
      ++constraints;
    }
  need(constraints == 2,
       "template bodies did not retain both phantom applications");
  auto instance_of = [&](const hir::FunctionTemplate &t)
      -> const hir::FunctionInstance * {
    for (const auto &i : dual_module.instances)
      if (i.origin == t.function.symbol)
        return &i;
    return nullptr;
  };
  auto first_instance = instance_of(*first_template);
  auto second_instance = instance_of(*second_template);
  need(first_instance && second_instance,
       "phantom generic instances were not created");
  auto concrete_code = [&](SymbolId owner) -> std::vector<SymbolId> {
    std::vector<SymbolId> result;
    for (const auto &n : dual_module.nodes)
      if (dual_module.scopes[n.scope.value].function == owner)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->direct_symbol &&
            dual_module.resolved->symbols[c->direct_symbol->value].name ==
                "code") {
          need(c->type_args.empty(),
               "concrete phantom call retained type arguments");
          result.push_back(*c->direct_symbol);
        }
    return result;
  };
  auto first_calls = concrete_code(first_instance->symbol);
  auto second_calls = concrete_code(second_instance->symbol);
  need(first_calls.size() == 1 && second_calls.size() == 1 &&
           first_calls[0] != second_calls[0],
       "same-signature phantom implementations shared a method target");
  auto origin_is = [&](SymbolId instance, const hir::ImplDecl &impl) {
    for (auto method : impl.methods)
      if (method == instance)
        return true;
    for (const auto &i : dual_module.instances)
      if (i.symbol == instance)
        for (auto method : impl.methods)
          if (i.origin == method)
            return true;
    return false;
  };
  need(origin_is(first_calls[0], *first_impl) &&
           origin_is(second_calls[0], *second_impl),
       "phantom value/T call selected the other complete application");

  // Joint overlap: Box[bool] has two distinct applications; the same pair
  // with Match[bool,Y] overlaps on Self and arguments together and must be
  // rejected at declaration time.
  good("trait Match[A,B]{func code(owner Self)i32;}"
       "struct Box[T]{value T;}"
       "impl[X] Match[X,bool] for Box[X]{func code(owner Self)i32{return 31;}}"
       "impl[Y] Match[i32,Y] for Box[bool]{func code(owner Self)i32{return 42;}}"
       "func left[T Match[bool,bool]](owner T)i32{return owner.code();}"
       "func right[T Match[i32,bool]](owner T)i32{return T.code(owner);}"
       "func main()i32{let x=Box[bool](value:true);"
       "if left(x)!=31||right(x)!=42{return 1;}return 0;}");
  bad("trait Match[A,B]{func code(owner Self)i32;}"
      "struct Box[T]{value T;}"
      "impl[X] Match[X,bool] for Box[X]{func code(owner Self)i32{return 31;}}"
      "impl[Y] Match[bool,Y] for Box[bool]{func code(owner Self)i32{return 42;}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Convert[A,B]{func code(owner Self)i32;}"
      "struct Key{}"
      "impl Convert[i32] for Key{func code(owner Self)i32{return 11;}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Convert[A,B]{func code(owner Self)i32;}"
      "struct Key{}"
      "impl Convert[i32,bool] for Key{func code(owner Self)i32{return 11;}}"
      "func need[T Convert[bool,i32]](owner T)i32{return T.code(owner);}"
      "func main()i32{return need(Key());}", "E_TYPE_MISMATCH");
  bad("trait Convert[A,B]{func code(owner Self)i32;}"
      "struct Key{}"
      "impl Convert[i32,bool] for Key{func code(owner Self)i32{return 11;}}"
      "impl Convert[bool,i32] for Key{func code(owner Self)i32{return 22;}}"
      "func main()i32{let key=Key();let result i32=key.code();return result;}",
      "E_TYPE_MISMATCH");
  bad("trait Convert[A,B]{func get(owner Self)A;}"
      "struct Key{}"
      "impl Convert[i32,bool] for Key{func get(owner Self)i32{return 11;}}"
      "impl Convert[bool,i32] for Key{func get(owner Self)bool{return true;}}"
      "func main()i32{let key=Key();let result i32=key.get();return result-11;}",
      "E_TYPE_MISMATCH");
  // The old declaration-only rejections now accept; execution is covered by
  // the modes-* projects without changing these original Source strings.
  good("trait New[A]{func identity[U](owner Self,value U)U;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func take[U](owner Self,value move U)U;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func update[U](owner Self,slot inout U)Unit;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func take(owner Self,value move A)A;}"
       "func main()i32{return 0;}");
  good("trait New[A]{func update(owner Self,slot inout i32)Unit;}"
       "func main()i32{return 0;}");

  // An impl parameter that occurs only in the trait application must enter
  // the method instance key even though the checked signature never shows it.
  const std::string phantom =
      "trait Tag[A]{func mark(owner Self)i32;}"
      "struct Mark{value i32;}"
      "impl[U] Tag[U] for Mark{func mark(owner Self)i32{return owner.value+6;}}"
      "func use[T Tag[i32]](owner T)i32{return T.mark(owner);}"
      "func main()i32{let mark=Mark(value:1);"
      "if use(mark)!=7{return 1;}return 0;}";
  auto phantom_module = good(phantom);
  auto phantom_i32 = scalar(phantom_module, hir::TypeKind::SignedInt, 32);
  auto phantom_bool = scalar(phantom_module, hir::TypeKind::Bool, 0);
  const hir::ImplDecl *tag_impl = nullptr;
  const hir::TraitDecl *tag_trait = nullptr;
  for (const auto &t : phantom_module.traits)
    if (phantom_module.resolved->symbols[t.symbol.value].name == "Tag")
      tag_trait = &t;
  for (const auto &im : phantom_module.impls)
    if (im.trait && tag_trait && im.trait->declaration == tag_trait->symbol)
      tag_impl = &im;
  need(tag_trait && tag_trait->params.size() == 1 && tag_impl &&
           tag_impl->params.size() == 1 && tag_impl->trait->args.size() == 1 &&
           tag_impl->trait->args[0] ==
               [&] {
                 for (uint64_t id = 1; id <= phantom_module.types.size(); ++id) {
                   auto t = phantom_module.types.find(TypeId{id});
                   if (t->kind == hir::TypeKind::TypeParameter &&
                       t->declaration == tag_impl->params[0].symbol)
                     return TypeId{id};
                 }
                 return TypeId{0};
               }(),
       "phantom impl parameter was not the only trait argument");
  const hir::FunctionInstance *mark_instance = nullptr;
  for (const auto &i : phantom_module.instances)
    if (tag_impl && i.origin == tag_impl->methods[0])
      mark_instance = &i;
  need(mark_instance && mark_instance->type_args.size() == 1 &&
           mark_instance->type_args[0] == phantom_i32,
       "trait-application-only impl parameter was absent from the instance key");
  auto mark_function = [&](SymbolId id) -> const hir::HirFunction * {
    for (const auto &f : phantom_module.functions)
      if (f.symbol == id)
        return &f;
    return nullptr;
  };
  need(mark_function(mark_instance->symbol) != nullptr,
       "phantom method instance function missing");
  bool saw_phantom_call = false;
  for (const auto &n : phantom_module.nodes)
    if (auto c = std::get_if<hir::Call>(&n.data);
        c && c->direct_symbol == mark_instance->symbol) {
      need(c->type_args.empty(), "concrete phantom call retained arguments");
      saw_phantom_call = true;
    }
  need(saw_phantom_call, "concrete phantom call target missing");

  // Corrupt each identity component independently. Recomputing ownership must
  // not launder a bad trait application or a forged instance key.
  invalid_recomputed(phantom_module, "swap impl trait argument", [&](auto &x) {
    for (auto &im : x.impls)
      if (im.trait && tag_trait && im.trait->declaration == tag_trait->symbol)
        im.trait->args[0] = phantom_bool;
  });
  invalid_recomputed(phantom_module, "drop trait application argument",
                     [&](auto &x) {
    for (auto &im : x.impls)
      if (im.trait && tag_trait &&
          im.trait->declaration == tag_trait->symbol)
        im.trait->args.clear();
  });
  invalid_recomputed(phantom_module, "swap template bound argument",
                     [&](auto &x) {
    for (auto &t : x.templates)
      for (auto &p : t.params)
        if (p.trait && tag_trait && p.trait->declaration == tag_trait->symbol)
          p.trait->args[0] = phantom_bool;
  });
  invalid_recomputed(phantom_module, "swap constraint call argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        c->trait.args[0] = phantom_bool;
        changed = true;
      }
    need(changed, "missing ConstraintCall corruption target");
  });
  invalid_recomputed(phantom_module, "forge phantom instance key",
                     [&](auto &x) {
    bool changed = false;
    for (auto &i : x.instances)
      if (tag_impl && i.origin == tag_impl->methods[0]) {
        i.type_args[0] = phantom_bool;
        changed = true;
      }
    need(changed, "missing phantom instance corruption target");
  });
  invalid_recomputed(dual_module, "swap complete impl application",
                     [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (im.trait && convert && im.trait->declaration == convert->symbol &&
          im.trait->args == std::vector<TypeId>{i32, bool_type}) {
        im.trait->args = {bool_type, i32};
        changed = true;
      }
    need(changed, "missing dual application corruption target");
  });
  invalid_recomputed(dual_module, "swap template bound application",
                     [&](auto &x) {
    bool changed = false;
    for (auto &t : x.templates)
      for (auto &p : t.params)
        if (p.trait && convert && p.trait->declaration == convert->symbol &&
            p.trait->args == std::vector<TypeId>{i32, bool_type}) {
          p.trait->args = {bool_type, i32};
          changed = true;
        }
    need(changed, "missing dual bound corruption target");
  });
  invalid_recomputed(dual_module, "drop constraint call argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
        c->trait.args.pop_back();
        changed = true;
      }
    need(changed, "missing constraint argument corruption target");
  });

  // Same-shaped phantom applications from two different declarations must not
  // be interchangeable. Swapping either the impl header or the constraint
  // call to the other real trait is rejected by the independent Source check.
  const std::string twin =
      "trait Left[A]{func pick(owner Self)i32;}"
      "trait Right[A]{func pick(owner Self)i32;}"
      "struct Key{}"
      "impl Left[i32] for Key{func pick(owner Self)i32{return 1;}}"
      "impl Right[i32] for Key{func pick(owner Self)i32{return 2;}}"
      "func use[T Left[i32]](owner T)i32{return T.pick(owner);}"
      "func main()i32{let k=Key();if use(k)!=1{return 1;}return 0;}";
  auto twin_module = good(twin);
  SymbolId left_trait{0}, right_trait{0};
  for (const auto &t : twin_module.traits) {
    auto name = twin_module.resolved->symbols[t.symbol.value].name;
    if (name == "Left")
      left_trait = t.symbol;
    if (name == "Right")
      right_trait = t.symbol;
  }
  need(left_trait.value && right_trait.value, "twin traits missing");
  invalid_recomputed(twin_module, "swap constraint trait declaration",
                     [&](auto &x) {
    bool changed = false;
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data);
          c && c->trait.declaration == left_trait) {
        c->trait.declaration = right_trait;
        changed = true;
      }
    need(changed, "missing twin constraint corruption target");
  });
  invalid_recomputed(twin_module, "swap impl trait declaration",
                     [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (im.trait && im.trait->declaration == left_trait) {
        im.trait->declaration = right_trait;
        changed = true;
      }
    need(changed, "missing twin impl corruption target");
  });
  invalid_recomputed(twin_module, "drop trait parameter list", [&](auto &x) {
    bool changed = false;
    for (auto &tr : x.traits)
      if (tr.symbol == left_trait || tr.symbol == right_trait) {
        tr.params.clear();
        changed = true;
      }
    need(changed, "missing trait parameter corruption target");
  });

  // F1: a bound may name a nominal declared later in the same package. The
  // real declaration/kind/arity pre-pass must make it visible without
  // inventing a foreign type, and the bound must still select the real impl.
  const std::string forward =
      "trait Mark[T]{func get(owner Self)i32;}"
      "struct Holder[U Mark[Later]]{value U;}"
      "struct Later{}"
      "impl Mark[Later] for i32{func get(owner Self)i32{return owner;}}"
      "func read[T Mark[Later]](value T)i32{return value.get();}"
      "func main()i32{let h=Holder[i32](value:7);"
      "return read(h.value)-7;}";
  auto forward_module = good(forward);
  auto forward_i32 = scalar(forward_module, hir::TypeKind::SignedInt, 32);
  SymbolId mark_symbol{0}, later_symbol{0}, forward_read{0};
  const hir::NominalDecl *holder = nullptr;
  const hir::ImplDecl *mark_impl = nullptr;
  TypeId later_type{0};
  for (const auto &t : forward_module.traits)
    if (forward_module.resolved->symbols[t.symbol.value].name == "Mark")
      mark_symbol = t.symbol;
  for (uint64_t id = 1; id <= forward_module.types.size(); ++id) {
    auto t = forward_module.types.find(TypeId{id});
    if (t->kind == hir::TypeKind::NominalStruct &&
        forward_module.resolved->symbols[t->declaration.value].name ==
            "Later" &&
        t->type_args.empty()) {
      later_symbol = t->declaration;
      later_type = TypeId{id};
    }
  }
  for (const auto &d : forward_module.nominals)
    if (forward_module.resolved->symbols[d.symbol.value].name == "Holder")
      holder = &d;
  for (const auto &im : forward_module.impls)
    if (im.trait && im.trait->declaration == mark_symbol &&
        im.trait->args == std::vector<TypeId>{later_type})
      mark_impl = &im;
  for (const auto &t : forward_module.templates)
    if (forward_module.resolved->symbols[t.function.symbol.value].name ==
        "read")
      forward_read = t.function.symbol;
  need(mark_symbol.value && later_symbol.value && later_type.value && holder &&
           holder->params.size() == 1 && holder->params[0].trait &&
           holder->params[0].trait->declaration == mark_symbol &&
           holder->params[0].trait->args ==
               std::vector<TypeId>{later_type} &&
           mark_impl && forward_read.value,
       "forward nominal bound was not registered with its real identity");
  bool forward_call = false;
  for (const auto &i : forward_module.instances)
    if (i.origin == forward_read) {
      for (const auto &n : forward_module.nodes)
        if (forward_module.scopes[n.scope.value].function == i.symbol)
          if (auto c = std::get_if<hir::Call>(&n.data);
              c && c->direct_symbol &&
              forward_module.resolved->symbols[c->direct_symbol->value].name ==
                  "get") {
            need(c->direct_symbol == mark_impl->methods[0],
                 "forward bound selected the wrong impl method");
            forward_call = true;
          }
    }
  need(forward_call, "forward bound runtime call target missing");
  invalid_recomputed(forward_module, "forward nominal bound argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &d : x.nominals)
      if (holder && d.symbol == holder->symbol &&
          d.params[0].trait) {
        d.params[0].trait->args[0] = forward_i32;
        changed = true;
      }
    need(changed, "missing forward nominal corruption target");
  });
  invalid_recomputed(forward_module, "forward impl application argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (im.trait && im.trait->declaration == mark_symbol &&
          im.trait->args == std::vector<TypeId>{later_type}) {
        im.trait->args[0] = forward_i32;
        changed = true;
      }
    need(changed, "missing forward impl corruption target");
  });

  // F2/F3: prototype own bounds may name the real trait Self, and impl own
  // bounds are compared only after the complete positional alpha map.
  const std::string prototype_self =
      "trait Link[A]{func code(owner Self)i32;}"
      "trait Consumer{func use[U Link[Self]](owner Self,value U)i32;}"
      "func main()i32{return 0;}";
  good(prototype_self);
  const std::string alpha_body =
      "trait Link[A]{func code(owner Self)i32;}"
      "struct Host{}"
      "impl Link[bool] for i32{func code(owner Self)i32{return owner;}}"
      "trait Consumer{func use[A,U Link[A]](owner Self,value U)i32;}"
      "impl Consumer for Host{func use[B,V Link[B]](owner Self,value V)i32{"
      "return value.code();}}"
      "func main()i32{let host=Host();return host.use[bool,i32](9)-9;}";
  auto alpha_module = good(alpha_body);
  auto alpha_i32 = scalar(alpha_module, hir::TypeKind::SignedInt, 32);
  auto alpha_bool = scalar(alpha_module, hir::TypeKind::Bool, 0);
  SymbolId link_symbol{0};
  for (const auto &t : alpha_module.traits)
    if (alpha_module.resolved->symbols[t.symbol.value].name == "Link")
      link_symbol = t.symbol;
  SymbolId alpha_use{0}, alpha_code{0};
  for (const auto &im : alpha_module.impls)
    if (im.trait) {
      auto self = alpha_module.types.find(im.self_type);
      if (self &&
          alpha_module.resolved->symbols[self->declaration.value].name ==
              "Host")
        alpha_use = im.methods[0];
      if (im.trait->declaration == link_symbol &&
          im.trait->args == std::vector<TypeId>{alpha_bool})
        alpha_code = im.methods[0];
    }
  const hir::FunctionTemplate *alpha_template = nullptr;
  for (const auto &t : alpha_module.templates)
    if (t.function.symbol == alpha_use)
      alpha_template = &t;
  need(link_symbol.value && alpha_use.value && alpha_template &&
           alpha_template->params.size() == 2 &&
           alpha_template->params[1].trait &&
           alpha_template->params[1].trait->declaration == link_symbol &&
           alpha_template->params[1].trait->args.size() == 1,
       "alpha own bound was not retained on the impl method template");
  auto own_parameter = [&](const hir::HirModule &m, SymbolId declaration) {
    for (uint64_t id = 1; id <= m.types.size(); ++id) {
      auto t = m.types.find(TypeId{id});
      if (t->kind == hir::TypeKind::TypeParameter &&
          t->declaration == declaration)
        return TypeId{id};
    }
    return TypeId{0};
  };
  auto first_own = own_parameter(
      alpha_module, alpha_template->params[0].symbol);
  need(first_own.value &&
           alpha_template->params[1].trait->args[0] == first_own,
       "alpha bound did not use the impl's own parameter identity");
  const hir::FunctionInstance *alpha_instance = nullptr;
  for (const auto &i : alpha_module.instances)
    if (i.origin == alpha_use &&
        i.type_args == std::vector<TypeId>{alpha_bool, alpha_i32})
      alpha_instance = &i;
  need(alpha_instance, "alpha runtime instance/key missing");
  bool alpha_called = false;
  for (const auto &n : alpha_module.nodes)
    if (alpha_module.scopes[n.scope.value].function == alpha_instance->symbol)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol &&
          alpha_module.resolved->symbols[c->direct_symbol->value].name ==
              "code") {
        need(c->type_args.empty(),
             "alpha concrete call retained type arguments");
        need(c->direct_symbol == alpha_code,
             "alpha concrete call selected the wrong Link application");
        alpha_called = true;
      }
  need(alpha_called, "alpha runtime call target missing");
  invalid_recomputed(alpha_module, "alpha impl own bound argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &t : x.templates)
      if (t.function.symbol == alpha_use && t.params.size() == 2 &&
          t.params[1].trait) {
        t.params[1].trait->args[0] = alpha_i32;
        changed = true;
      }
    need(changed, "missing alpha bound corruption target");
  });
  invalid_recomputed(alpha_module, "alpha complete instance key",
                     [&](auto &x) {
    bool changed = false;
    for (auto &i : x.instances)
      if (alpha_instance && i.symbol == alpha_instance->symbol) {
        i.type_args[0] = alpha_i32;
        changed = true;
      }
    need(changed, "missing alpha instance corruption target");
  });

  const std::string self_bound =
      "trait Link[A]{func code(owner Self)i32;}"
      "struct Key{}"
      "impl Link[Key] for i32{func code(owner Self)i32{return owner;}}"
      "trait Consumer{func use[U Link[Self]](owner Self,value U)i32;}"
      "impl Consumer for Key{func use[V Link[Self]](owner Self,value V)i32{"
      "return value.code();}}"
      "func main()i32{let key=Key();return key.use[i32](7)-7;}";
  auto self_module = good(self_bound);
  auto self_i32 = scalar(self_module, hir::TypeKind::SignedInt, 32);
  SymbolId self_link{0}, self_use{0}, self_code{0};
  TypeId key_type{0};
  for (const auto &t : self_module.traits)
    if (self_module.resolved->symbols[t.symbol.value].name == "Link")
      self_link = t.symbol;
  for (uint64_t id = 1; id <= self_module.types.size(); ++id) {
    auto t = self_module.types.find(TypeId{id});
    if (t->kind == hir::TypeKind::NominalStruct &&
        self_module.resolved->symbols[t->declaration.value].name == "Key")
      key_type = TypeId{id};
  }
  for (const auto &im : self_module.impls)
    if (im.trait) {
      auto self = self_module.types.find(im.self_type);
      if (self &&
          self_module.resolved->symbols[self->declaration.value].name ==
              "Key")
        self_use = im.methods[0];
      if (im.trait->declaration == self_link &&
          im.trait->args == std::vector<TypeId>{key_type})
        self_code = im.methods[0];
    }
  const hir::FunctionTemplate *self_template = nullptr;
  for (const auto &t : self_module.templates)
    if (t.function.symbol == self_use)
      self_template = &t;
  need(self_link.value && self_use.value && key_type.value && self_template &&
           self_template->params.size() == 1 &&
           self_template->params[0].trait &&
           self_template->params[0].trait->declaration == self_link &&
           self_template->params[0].trait->args ==
               std::vector<TypeId>{key_type},
       "impl own Self bound was not alpha/substituted to the real Self");
  const hir::FunctionInstance *self_instance = nullptr;
  for (const auto &i : self_module.instances)
    if (i.origin == self_use &&
        i.type_args == std::vector<TypeId>{self_i32})
      self_instance = &i;
  need(self_instance, "combined Self-bound instance/key missing");
  bool self_called = false;
  for (const auto &n : self_module.nodes)
    if (self_module.scopes[n.scope.value].function == self_instance->symbol)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol) {
        need(c->direct_symbol == self_code,
             "combined Self-bound call selected the wrong impl method");
        self_called = true;
      }
  need(self_called, "combined Self-bound runtime call target missing");
  invalid_recomputed(self_module, "combined Self bound argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &t : x.templates)
      if (t.function.symbol == self_use && !t.params.empty() &&
          t.params[0].trait) {
        t.params[0].trait->args[0] = self_i32;
        changed = true;
      }
    need(changed, "missing combined bound corruption target");
  });

  const std::string mixed_bound =
      "trait Link[A]{func code(owner Self)i32;}"
      "trait Pair[A,B]{func pair(owner Self)i32;}"
      "struct Key{}"
      "impl Link[bool] for i32{func code(owner Self)i32{return owner;}}"
      "impl Pair[Key,bool] for i32{func pair(owner Self)i32{return owner;}}"
      "trait Consumer{func use[A,U Link[A],V Pair[Self,A]]"
      "(owner Self,key A,u U,v V)i32;}"
      "impl Consumer for Key{func use[B,X Link[B],Y Pair[Self,B]]"
      "(owner Self,key B,u X,v Y)i32{return u.code()+v.pair();}}"
      "func main()i32{let key=Key();"
      "return key.use[bool,i32,i32](true,9,8)-17;}";
  auto mixed_module = good(mixed_bound);
  auto mixed_i32 = scalar(mixed_module, hir::TypeKind::SignedInt, 32);
  auto mixed_bool = scalar(mixed_module, hir::TypeKind::Bool, 0);
  SymbolId mixed_use{0};
  TypeId mixed_key{0};
  for (const auto &im : mixed_module.impls)
    if (im.trait) {
      auto self = mixed_module.types.find(im.self_type);
      if (self &&
          mixed_module.resolved->symbols[self->declaration.value].name ==
              "Key")
        mixed_use = im.methods[0];
    }
  for (uint64_t id = 1; id <= mixed_module.types.size(); ++id) {
    auto t = mixed_module.types.find(TypeId{id});
    if (t->kind == hir::TypeKind::NominalStruct &&
        mixed_module.resolved->symbols[t->declaration.value].name == "Key")
      mixed_key = TypeId{id};
  }
  const hir::FunctionTemplate *mixed_template = nullptr;
  for (const auto &t : mixed_module.templates)
    if (t.function.symbol == mixed_use)
      mixed_template = &t;
  need(mixed_use.value && mixed_key.value && mixed_template &&
           mixed_template->params.size() == 3 &&
           mixed_template->params[1].trait &&
           mixed_template->params[2].trait,
       "combined Self/sibling own-bound template is missing");
  {
    auto first_own =
        own_parameter(mixed_module, mixed_template->params[0].symbol);
    need(first_own.value &&
             mixed_template->params[1].trait->args ==
                 std::vector<TypeId>{first_own} &&
             mixed_template->params[2].trait->args ==
                 std::vector<TypeId>{mixed_key, first_own},
         "combined Self/sibling bounds were not fully substituted");
  }
  const hir::FunctionInstance *mixed_instance = nullptr;
  for (const auto &i : mixed_module.instances)
    if (i.origin == mixed_use &&
        i.type_args == std::vector<TypeId>{mixed_bool, mixed_i32, mixed_i32})
      mixed_instance = &i;
  need(mixed_instance, "combined Self/sibling instance/key missing");
  invalid_recomputed(mixed_module, "combined Self/sibling bound argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &t : x.templates)
      if (t.function.symbol == mixed_use && t.params.size() == 3 &&
          t.params[2].trait) {
        t.params[2].trait->args[0] = mixed_i32;
        changed = true;
      }
    need(changed, "missing mixed bound corruption target");
  });

  // Real mismatches stay source errors: different trait declaration,
  // different Self application, and wrong arity through the forward path.
  bad("trait Link[A]{func code(owner Self)i32;}"
      "trait Other[A]{func code(owner Self)i32;}"
      "trait Consumer{func use[A,U Link[A]](owner Self,key A,value U)i32;}"
      "impl Consumer for i32{func use[B,V Other[B]](owner Self,key B,"
      "value V)i32{return value.code();}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Link[A]{func code(owner Self)i32;}"
      "struct Host{} struct Key{}"
      "trait Consumer{func use[U Link[Self]](owner Self,value U)i32;}"
      "impl Consumer for Key{func use[V Link[Host]](owner Self,value V)i32{"
      "return value.code();}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Mark[T]{}"
      "struct Holder[U Mark[Later[i32]]]{value U;}"
      "struct Later{}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  unsupported("trait Tag[T]{} struct Data{items Vec[i32];}"
              "struct Holder[U Tag[func(inout Data)Unit]]{value U;}"
              "func main()i32{return 0;}");
  good("trait Tag[T]{} struct Data{items i32;}"
       "struct Holder[U Tag[func(inout Data)Unit]]{value U;}"
       "func main()i32{return 0;}");
}

void generic_trait_method_modes(const fs::path &root) {
  auto fixture = [&](const std::string &name) {
    auto r = check(root / "tests/generics/generic-traits" / name / "lang.toml");
    need(r.success(), name + "\n" + messages(r));
    need(hir::verify_types(*r.value).success() && hir::verify(*r.value).success(),
         "generic trait mode fixture HIR rejected: " + name);
    auto rederived = *r.value;
    auto ownership = hir::detail::produce_ownership(rederived, {});
    need(ownership.success() && hir::verify_types(rederived).success() &&
             hir::verify(rederived).success(),
         "legal recomputed generic trait mode control rejected: " + name +
             "\n" + messages(ownership));
    return *r.value;
  };
  for (const char *name : {"modes-before-move-three-entries",
                           "modes-before-pod-inout-writeback",
                           "modes-before-static-own-named-self",
                           "modes-lifetime"})
    (void)fixture(name);
  auto m = fixture("modes-combinations");

  auto reject = [](const std::string &text, const std::string &code,
                   FailureKind failure = FailureKind::source) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(text, backend);
      need(!r.success() && r.failure == failure && has(r, code) &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
           std::string(backend) + ": " + text + "\n" + messages(r));
    }
  };
  const std::string take =
      "trait Take[A]{func take[U](owner Self,value move U)U;}"
      "struct Host{items Vec[i32];}"
      "impl Take[bool] for Host{func take[V](owner Self,value move V)V{"
      "return move value;}}";
  reject(take + "func main()i32{let h=Host(items:[1]);let v Vec[i32]=[2];"
                "let r=h.take[Vec[i32]](v);return 0;}",
         "E_CALL_ARGUMENT");
  reject(take + "func main()i32{let h=Host(items:[1]);let v Vec[i32]=[2];"
                "let r=h.take[Vec[i32]](move v);return v[0];}",
         "E_OWNERSHIP_USE_AFTER_MOVE");
  reject(take + "func main()i32{let h=Host(items:[1]);let v Vec[i32]=[2];"
                "let r=h.take[Vec[i32]](move v);"
                "let q=Host.take[Vec[i32]](h,move v);return 0;}",
         "E_OWNERSHIP_USE_AFTER_MOVE");
  reject(take + "func main()i32{let h=Host(items:[1]);"
                "let r=h.take[Host](move h);return 0;}",
         "E_CALL_ARGUMENT");

  const std::string edit =
      "trait Edit[A]{func edit[U](owner Self,slot inout U,value U)U;}"
      "struct Pair{value i32;}"
      "impl Edit[bool] for Pair{func edit[V](owner Self,slot inout V,value V)V{"
      "let old=slot;slot=value;return old;}}";
  reject(edit + "func main()i32{var h=Pair(value:1);"
                "let r=h.edit[Pair](inout h,Pair(value:2));return 0;}",
         "E_CALL_ARGUMENT");
  reject(edit + "func main()i32{let h=Pair(value:0);var x i32=1;"
                "let r=Pair.edit[i32](h,inout x,x);return 0;}",
         "E_CALL_ARGUMENT");
  reject(edit + "func read(x inout i32)i32{return x;}"
                "func main()i32{let h=Pair(value:0);var x i32=1;"
                "let r=h.edit[i32](inout x,read(inout x));return 0;}",
         "E_CALL_ARGUMENT");
  reject(edit + "func main()i32{let h=Pair(value:0);let x i32=1;"
                "let r=h.edit[i32](inout x,2);return 0;}",
         "E_CALL_ARGUMENT");
  reject(edit + "func main()i32{let h=Pair(value:0);var x=Pair(value:1);"
                "let r=h.edit[i32](inout x.value,2);return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);
  reject("trait Edit[A]{func edit[U](owner Self,a inout U,b inout U);}"
         "struct Host{} impl Edit[bool] for Host{"
         "func edit[V](owner Self,a inout V,b inout V){}}"
         "func main()i32{let h=Host();var x i32=1;"
         "h.edit[i32](inout x,inout x);return 0;}", "E_CALL_ARGUMENT");
  reject(edit + "func main()i32{let h=Pair(value:0);var x Vec[i32]=[1];"
                "let r=h.edit[Vec[i32]](inout x,[2]);return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);
  reject(edit + "struct Owned{items Vec[i32];}"
                "func main()i32{let h=Pair(value:0);var x=Owned(items:[1]);"
                "let r=h.edit[Owned](inout x,Owned(items:[2]));return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);

  // The raw prototype determines actual Self, regardless of its label.
  // Actual-self move/inout is deferred at the declaration and checked for
  // concrete data when the trait is applied.
  for (const char *mode : {"move", "inout"})
    for (const char *backend : {"native", "python"}) {
      auto context = std::string("trait Closed[A]{func run[U](owner ") + mode +
                     " Self,value U)U;}func main()i32{return 0;}";
      auto r = source(context, backend);
      need(r.success(), std::string(backend) + ": " + context + "\n" +
                            messages(r));
      need(hir::verify_types(*r.value).success() &&
               hir::verify(*r.value).success(),
           std::string(backend) + ": actual self declaration HIR rejected");
    }
  // A legal top-level mode does not make a callback dependency illegal. The
  // same graphs are accepted as templates and rejected when a complete trait
  // application substitutes owning data.
  for (const char *argument : {"U", "A", "Self"}) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(std::string("struct Holder[T]{cb func(inout T);}") +
                          "trait Closed[A]{func run[U](owner Self,"
                          "slot inout i32,holder Holder[" + argument +
                          "]);}func main()i32{return 0;}",
                      backend);
      need(r.success(),
           std::string(backend) + ": dependent mode callback rejected\n" +
               messages(r));
      need(hir::verify_types(*r.value).success(),
           std::string(backend) + ": dependent mode callback HIR rejected");
      need(hir::verify(*r.value).success(),
           std::string(backend) + ": dependent mode callback did not publish");
    }
  }
  for (const char *backend : {"native", "python"}) {
    auto r = source(
        "struct Holder[T]{cb func(inout T);}"
        "trait Closed[A]{func run[U](owner Self,value move U)"
        "(func()Holder[A]);}func main()i32{return 0;}",
        backend);
    need(r.success(), std::string(backend) +
                          ": dependent return callback rejected\n" +
                          messages(r));
    need(hir::verify_types(*r.value).success(),
         std::string(backend) + ": dependent return callback HIR rejected");
    auto e = source("enum E[T]{Bad(cb func(inout T));}"
                    "trait Closed[A]{func run[U](owner Self,value move U) "
                    "throws E[A];}func main()i32{return 0;}",
                    backend);
    need(e.success(), std::string(backend) +
                          ": dependent throws callback rejected\n" +
                          messages(e));
    need(hir::verify_types(*e.value).success(),
         std::string(backend) + ": dependent throws callback HIR rejected");
  }
  const std::string owning = "struct Own{items Vec[i32];}";
  reject("struct Holder[T]{cb func(inout T);}" + owning +
             "trait Closed[A]{func run[U](owner Self,holder Holder[A]);}"
             "impl Closed[Own] for i32{"
             "func run[U](owner Self,holder Holder[Own]){}}"
             "func main()i32{return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);
  reject("struct Holder[T]{cb func(inout T);}" + owning +
             "trait Closed[A]{func run[U](owner Self,holder Holder[Self]);}"
             "impl Closed[bool] for Own{"
             "func run[U](owner Self,holder Holder[Own]){}}"
             "func main()i32{return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);
  reject("struct Holder[T]{cb func(inout T);}" + owning +
             "trait Closed[A]{func run[U](owner Self,value move U)"
             "(func()Holder[A]);}"
             "impl Closed[Own] for i32{"
             "func run[U](owner Self,value move U)(func()Holder[Own]){"
             "return func()Holder[Own]{return Holder[Own](cb:noop);};}}"
             "func noop(slot inout Own){}"
             "func main()i32{return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);
  reject("enum E[T]{Bad(cb func(inout T));}" + owning +
             "trait Closed[A]{func run[U](owner Self,value move U) "
             "throws E[A];}"
             "impl Closed[Own] for i32{"
             "func run[U](owner Self,value move U) throws E[Own]{}}"
             "func main()i32{return 0;}",
         "E_CAPABILITY_UNAVAILABLE", FailureKind::unsupported);

  auto name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  SymbolId forward{}, edit_template{}, relay{};
  for (const auto &t : m.templates) {
    if (name(t.function.symbol) == "forward")
      forward = t.function.symbol;
    if (name(t.function.symbol) == "edit" && t.params.size() == 1)
      edit_template = t.function.symbol;
    if (name(t.function.symbol) == "relay")
      relay = t.function.symbol;
  }
  need(forward.value && edit_template.value && relay.value,
       "generic trait mode templates missing");
  SymbolId forward_instance{};
  for (const auto &i : m.instances)
    if (i.origin == forward)
      forward_instance = i.symbol;
  need(forward_instance.value, "generic trait mode forward instance missing");
  for (const auto &n : m.nodes)
    if (m.scopes[n.scope.value].function == forward_instance)
      need(!std::holds_alternative<hir::ConstraintCall>(n.data),
           "concrete generic trait mode call retained ConstraintCall");

  size_t corruptions = 0;
  auto damaged = [&](const std::string &label, const auto &change) {
    // Each mutation locates a concrete target and asserts a changed value.
    // Both entry points must reject after ownership was rebuilt successfully.
    invalid(m, label, change);
    invalid_recomputed(m, label + " recomputed", change);
    ++corruptions;
  };
  damaged("generic trait mode Move binding erased", [&](auto &h) {
    for (auto &n : h.nodes)
      if (h.scopes[n.scope.value].function == forward_instance)
        if (auto mv = std::get_if<hir::Move>(&n.data); mv && mv->binding) {
          mv->binding.reset();
          return;
        }
    throw std::runtime_error("concrete forward Move mutation target missing");
  });
  damaged("generic trait mode Place replaced by Read", [&](auto &h) {
    for (auto &n : h.nodes)
      if (h.scopes[n.scope.value].function == edit_template)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          need(c->args.size() > 1 && c->args[1].mode == hir::ParamMode::inout,
               "edit ConstraintCall has no inout argument");
          auto &node = h.nodes[c->args[1].expression.value];
          auto place = std::get<hir::Place>(node.data);
          node.data = hir::Read{place.symbol};
          return;
        }
    throw std::runtime_error("edit Place mutation target missing");
  });
  damaged("generic trait mode clone parameter renamed", [&](auto &h) {
    for (auto &i : h.instances)
      if (i.symbol == forward_instance) {
        need(i.renamed.size() > 1 &&
                 i.renamed[0].second != i.renamed[1].second,
             "forward clone mutation would be empty or unchanged");
        i.renamed[1].second = i.renamed[0].second;
        return;
      }
    throw std::runtime_error("forward clone mutation target missing");
  });
  damaged("generic trait mode complete TraitRef changed", [&](auto &h) {
    TypeId boolean{};
    for (uint64_t id = 1; id <= h.types.size(); ++id)
      if (h.types.find(TypeId{id})->kind == hir::TypeKind::Bool)
        boolean = TypeId{id};
    need(boolean.value, "bool type missing for trait mutation");
    for (auto &n : h.nodes)
      if (h.scopes[n.scope.value].function == forward)
        if (auto c = std::get_if<hir::ConstraintCall>(&n.data)) {
          need(c->trait.args.size() == 1 && c->trait.args[0] != boolean,
               "complete trait mutation would be unchanged");
          c->trait.args[0] = boolean;
          return;
        }
    throw std::runtime_error("forward TraitRef mutation target missing");
  });
  damaged("generic trait mode full impl-own key changed", [&](auto &h) {
    for (auto &i : h.instances)
      if (i.origin == relay && i.type_args.size() == 3) {
        need(i.type_args[0] != i.type_args[2],
             "full key mutation would be unchanged");
        i.type_args[2] = i.type_args[0];
        return;
      }
    throw std::runtime_error("relay full key mutation target missing");
  });
  std::cout << "generic-trait-method-modes: " << corruptions
            << " changed HIR graphs rejected directly and after ownership\n";
}

void trait_parameter_bounds(const fs::path &root) {
  const auto cases = root / "tests/generics/generic-traits";
  auto good = [](const std::string &s) {
    auto r = source(s);
    need(r.success(), s + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "trait parameter bound HIR rejected\n" + messages(r));
    auto published = hir::verify(*r.value);
    need(published.success(), messages(published));
    auto recomputed = *r.value;
    need(hir::detail::produce_ownership(recomputed, {}).success() &&
             hir::verify_types(recomputed).success(),
         "recomputed trait parameter bound control rejected");
    return *r.value;
  };
  auto bad = [](const std::string &s, const std::string &code) {
    auto r = source(s);
    need(!r.success() && r.failure == FailureKind::source && has(r, code) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         s + "\n" + messages(r));
  };
  auto unsupported = [](const std::string &s) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(s, backend);
      need(!r.success() && r.failure == FailureKind::unsupported &&
               has(r, "E_CAPABILITY_UNAVAILABLE") &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR") &&
               std::any_of(r.diagnostics.begin(), r.diagnostics.end(),
                           [](const auto &d) {
                             return d.code == "E_CAPABILITY_UNAVAILABLE" &&
                                    d.primary &&
                                    d.primary->begin_byte < d.primary->end_byte;
                           }),
           std::string(backend) + ": " + s + "\n" + messages(r));
    }
  };
  // Data fields are declared after the trait bound. Recheck their completed
  // types before publishing HIR, including callbacks hidden in Holder.
  unsupported("trait Tag[A]{}"
              "trait Use[U Tag[func(inout Data)Unit]]{}"
              "struct Data{items Vec[i32];}func main()i32{return 0;}");
  unsupported("trait Tag[A]{}trait Use[U Tag[Holder[Data]]]{}"
              "struct Holder[T]{cb func(inout T)Unit;}"
              "struct Data{items Vec[i32];}func main()i32{return 0;}");
  good("trait Tag[A]{}trait Use[U Tag[func(inout Data)Unit]]{}"
       "struct Data{value i32;}func main()i32{return 0;}");
  good("trait Tag[A]{}trait Use[U Tag[Holder[Data]]]{}"
       "struct Holder[T]{cb func(inout T)Unit;}"
       "struct Data{value i32;}func main()i32{return 0;}");
  // Holder[A] is a legal template; its inout data must be checked again when
  // the trait application substitutes concrete Data for A.
  unsupported("trait Tag[T]{}struct Holder[T]{cb func(inout T)Unit;}"
              "struct Data{items Vec[i32];}trait Use[A,U Tag[Holder[A]]]{}"
              "struct Proof{}struct Host{}"
              "impl Tag[Holder[Data]] for Proof{}"
              "impl Use[Data,Proof] for Host{}func main()i32{return 0;}");
  good("trait Tag[T]{}struct Holder[T]{cb func(inout T)Unit;}"
       "struct Data{value i32;}trait Use[A,U Tag[Holder[A]]]{}"
       "struct Proof{}struct Host{}impl Tag[Holder[Data]] for Proof{}"
       "impl Use[Data,Proof] for Host{}func main()i32{return 0;}");
  auto scalar = [](const hir::HirModule &m, hir::TypeKind kind,
                   unsigned bits) -> TypeId {
    for (uint64_t id = 1; id <= m.types.size(); ++id) {
      auto t = m.types.find(TypeId{id});
      if (t && t->kind == kind && t->bits == bits)
        return TypeId{id};
    }
    throw std::runtime_error("scalar type missing from bound module");
  };

  // A real trait declaration keeps both ordered parameters and their actual
  // bounds. The second bound names the first sibling parameter; the method
  // own bound names the real trait Self. The concrete impl and relay exercise
  // alpha renaming, full application selection and an uncalled declaration.
  auto first = check(cases / "trait-parameter-bound-runtime/lang.toml");
  need(first.success(), messages(first));
  const hir::TraitDecl *use = nullptr, *mark = nullptr, *tag = nullptr,
                        *code = nullptr;
  for (const auto &tr : first.value->traits) {
    auto name = first.value->resolved->symbols[tr.symbol.value].name;
    if (name == "Use")
      use = &tr;
    if (name == "Mark")
      mark = &tr;
    if (name == "Tag")
      tag = &tr;
    if (name == "Code")
      code = &tr;
  }
  need(use && mark && tag && code && use->params.size() == 2 &&
           use->params[0].trait &&
           use->params[0].trait->declaration == code->symbol &&
           use->params[1].trait &&
           use->params[1].trait->declaration == mark->symbol &&
           use->params[1].trait->args.size() == 1 &&
           first.value->types.find(use->params[1].trait->args[0])->kind ==
               hir::TypeKind::TypeParameter &&
           first.value->types.find(use->params[1].trait->args[0])->declaration ==
               use->params[0].symbol,
       "trait parameter bounds lost their real sibling identity");
  const hir::FunctionTemplate *run_template = nullptr;
  const hir::ImplDecl *use_impl = nullptr;
  for (const auto &im : first.value->impls)
    if (im.trait && im.trait->declaration == use->symbol) {
      use_impl = &im;
      for (auto method : im.methods)
        for (const auto &t : first.value->templates)
          if (t.function.symbol == method)
            run_template = &t;
    }
  need(use_impl && run_template && run_template->params.size() == 1 &&
           run_template->params[0].trait &&
           run_template->params[0].trait->declaration == tag->symbol &&
           run_template->params[0].trait->args.size() == 1,
       "impl method own bound lost the real trait Self application");
  bool relay_constraint = false;
  for (const auto &n : first.value->nodes)
    if (auto c = std::get_if<hir::ConstraintCall>(&n.data))
      if (c->trait.declaration == use->symbol && c->trait.args.size() == 2) {
        need(c->trait.args[0] == scalar(*first.value, hir::TypeKind::SignedInt, 32) &&
                 c->trait.args[1] ==
                     scalar(*first.value, hir::TypeKind::SignedInt, 32),
             "ConstraintCall lost its complete trait parameter application");
        relay_constraint = true;
      }
  need(relay_constraint,
       "relay did not retain a complete bounded ConstraintCall");
  auto owned_first = *first.value;
  need(hir::detail::produce_ownership(owned_first, {}).success() &&
           hir::verify_types(owned_first).success() &&
           hir::verify(owned_first).success(),
       "legal trait parameter bound control rejected after ownership");

  const auto i32 = scalar(*first.value, hir::TypeKind::SignedInt, 32);
  const auto bool_type = scalar(*first.value, hir::TypeKind::Bool, 0);
  auto both = [&](const hir::HirModule &m, const std::string &name,
                  const auto &change) {
    invalid(m, "direct " + name, change);
    invalid_recomputed(m, "recomputed " + name, change);
  };
  // This Self belongs to the trait parameter bound, before any method-own
  // parameter is declared. Another trait's identically named Self is distinct.
  auto self = check(cases / "trait-parameter-self-modes-runtime/lang.toml");
  need(self.success(), messages(self));
  const hir::TraitDecl *edit = nullptr, *rel = nullptr;
  for (const auto &tr : self.value->traits) {
    const auto &name = self.value->resolved->symbols.at(tr.symbol.value).name;
    if (name == "Edit")
      edit = &tr;
    if (name == "Rel")
      rel = &tr;
  }
  need(edit && rel && edit->self != rel->self && edit->params.size() == 1 &&
           edit->params[0].trait &&
           edit->params[0].trait->declaration == rel->symbol &&
           edit->params[0].trait->args.size() == 1,
       "trait parameter Self bound declaration missing");
  const auto self_type =
      self.value->types.find(edit->params[0].trait->args[0]);
  need(self_type && self_type->kind == hir::TypeKind::TypeParameter &&
           self_type->declaration == edit->self,
       "trait parameter bound did not retain its own trait's real Self");
  auto owned_self = *self.value;
  need(hir::verify_types(owned_self).success() &&
           hir::verify(owned_self).success() &&
           hir::detail::produce_ownership(owned_self, {}).success() &&
           hir::verify_types(owned_self).success() &&
           hir::verify(owned_self).success(),
       "legal trait parameter Self bound rejected before/after ownership");
  both(*self.value, "trait parameter bound uses another trait's Self",
       [&](auto &x) {
         hir::Type other;
         other.kind = hir::TypeKind::TypeParameter;
         other.declaration = rel->self;
         const auto other_self = x.types.intern(other);
         for (auto &tr : x.traits)
           if (tr.symbol == edit->symbol) {
             need(tr.params[0].trait->args[0] != other_self,
                  "trait Self provenance mutation would be unchanged");
             tr.params[0].trait->args[0] = other_self;
             return;
           }
         throw std::runtime_error("trait Self provenance mutation target missing");
       });
  both(*self.value, "trait application violates parameter's Self bound",
       [&](auto &x) {
         for (auto &im : x.impls)
           if (im.trait && im.trait->declaration == edit->symbol) {
             const auto key = im.trait->args.at(0);
             const auto key_type = x.types.find(key);
             need(key_type && key_type->kind == hir::TypeKind::NominalStruct,
                  "Self-bound impl Key type missing");
             for (uint64_t id = 1; id <= x.types.size(); ++id) {
               const auto candidate = x.types.find(TypeId{id});
               if (candidate && candidate->kind == hir::TypeKind::NominalStruct &&
                   candidate->declaration == key_type->declaration &&
                   candidate->type_args == std::vector<TypeId>{key}) {
                 need(TypeId{id} != key,
                      "Self-bound application mutation would be unchanged");
                 im.trait->args[0] = TypeId{id};
                 return;
               }
             }
             throw std::runtime_error("Key[Key[Owner]] mutation type missing");
           }
         throw std::runtime_error("Self-bound impl mutation target missing");
       });
  auto wrong_self =
      check(cases / "reject-trait-parameter-wrong-self/lang.toml");
  need(!wrong_self.success() && wrong_self.failure == FailureKind::source &&
           has(wrong_self, "E_TYPE_MISMATCH") &&
           !has(wrong_self, "E_IR_INVALID") &&
           !has(wrong_self, "E_INTERNAL_ERROR"),
       "wrong trait parameter Self application was not a source error\n" +
           messages(wrong_self));
  both(*first.value, "delete real trait parameter bound",
                     [&](auto &x) {
    bool changed = false;
    for (auto &tr : x.traits)
      if (use && tr.symbol == use->symbol) {
        tr.params[0].trait.reset();
        changed = true;
      }
    need(changed, "missing trait parameter bound deletion target");
  });
  both(*first.value, "swap real trait parameter bound",
                     [&](auto &x) {
    bool changed = false;
    for (auto &tr : x.traits)
      if (use && tr.symbol == use->symbol) {
        tr.params[0].trait = hir::TraitRef{mark->symbol, {i32}};
        changed = true;
      }
    need(changed, "missing trait parameter bound swap target");
  });
  both(*first.value, "swap sibling bound argument",
                     [&](auto &x) {
    bool changed = false;
    for (auto &tr : x.traits)
      if (use && tr.symbol == use->symbol && tr.params[1].trait) {
        tr.params[1].trait->args[0] = i32;
        changed = true;
      }
    need(changed, "missing sibling bound corruption target");
  });
  both(*first.value, "swap impl trait application",
                     [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (use_impl && im.trait && im.trait->declaration == use->symbol &&
          im.trait->args.size() == 2) {
        im.trait->args[1] = bool_type;
        changed = true;
      }
    need(changed, "missing bounded impl application target");
  });
  both(*first.value, "swap concrete method instance key",
                     [&](auto &x) {
    bool changed = false;
    for (auto &i : x.instances)
      if (run_template && i.origin == run_template->function.symbol &&
          !i.type_args.empty()) {
        i.type_args[0] = bool_type;
        changed = true;
      }
    need(changed, "missing bounded method instance key target");
  });
  both(*first.value, "swap ConstraintCall trait application",
                     [&](auto &x) {
    bool changed = false;
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data);
          c && use && c->trait.declaration == use->symbol &&
          c->trait.args.size() == 2) {
        c->trait.args[0] = bool_type;
        changed = true;
      }
    need(changed, "missing bounded ConstraintCall application target");
  });

  // Generic impl parameters satisfy a trait parameter's bound, and two
  // phantom applications with the same Self stay distinct.
  auto second =
      check(cases / "trait-parameter-generic-impl-runtime/lang.toml");
  need(second.success() && hir::verify_types(*second.value).success() &&
           hir::verify(*second.value).success(),
       "generic impl trait parameter bound rejected\n" + messages(second));
  const hir::TraitDecl *pick = nullptr;
  for (const auto &tr : second.value->traits)
    if (second.value->resolved->symbols[tr.symbol.value].name == "Pick")
      pick = &tr;
  std::set<std::vector<uint64_t>> picks;
  for (const auto &im : second.value->impls)
    if (im.trait && pick && im.trait->declaration == pick->symbol) {
      std::vector<uint64_t> args;
      for (auto t : im.trait->args)
        args.push_back(t.value);
      picks.insert(std::move(args));
    }
  need(pick && pick->params.size() == 2 && pick->params[0].trait &&
           picks.size() == 2,
       "bounded phantom applications were merged or lost");

  auto cross =
      check(cases / "trait-parameter-cross-package/lang.toml");
  need(cross.success() && hir::verify_types(*cross.value).success(),
       "cross-package trait parameter bound rejected\n" + messages(cross));
  auto forward = check(cases / "bound-forward-nominal/lang.toml");
  need(forward.success() && hir::verify_types(*forward.value).success(),
       "forward nominal trait parameter bound rejected\n" + messages(forward));
  auto nested = check(cases / "bound-nested-trait-parameter/lang.toml");
  need(nested.success() && hir::verify_types(*nested.value).success(),
       "nested trait-parameter application was not proven from its real "
       "bound\n" + messages(nested));

  good("trait Code{func code(owner Self)i32;}"
       "impl Code for i32{func code(owner Self)i32{return owner;}}"
       "trait Use[A Code]{func use(owner Self,value A)i32;}"
       "struct Key{}"
       "impl Use[i32] for Key{func use(owner Self,value i32)i32{"
       "return value.code();}}"
       "func relay[A Code,T Use[A]](owner T,value A)i32{"
       "return T.use(owner,value);}"
       "func main()i32{let key=Key();return relay[i32,Key](key,7)-7;}");
  good("trait Code{func code(owner Self)i32;}"
       "impl Code for i32{func code(owner Self)i32{return owner;}}"
       "trait Use[A Code]{func use(owner Self)i32;}"
       "struct Holder[U Use[Later]]{value U;}"
       "struct Later{}"
       "impl Code for Later{func code(owner Self)i32{return 1;}}"
       "impl Use[Later] for Later{func use(owner Self)i32{return 1;}}"
       "func main()i32{let h=Holder[Later](value:Later());"
       "return h.value.use()-1;}");
  good("trait Code{func code(owner Self)i32;}"
       "impl Code for i32{func code(owner Self)i32{return owner;}}"
       "trait Use[A Code]{func get(owner Self)i32;}"
       "trait Outer[B Code]{func run[T Use[B]](owner Self,value T)i32;}"
       "func main()i32{return 0;}");

  bad("trait Code{func code(owner Self)i32;}"
      "trait Use[A Code]{func use(owner Self,value A)i32;}"
      "struct Key{}"
      "impl Use[i32] for Key{func use(owner Self,value i32)i32{return value;}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Code{func code(owner Self)i32;}"
      "impl Code for i32{func code(owner Self)i32{return owner;}}"
      "trait Use[A Code]{func use(owner Self)i32;}"
      "struct Key{}"
      "impl Use[bool] for Key{func use(owner Self)i32{return 0;}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Code{func code(owner Self)i32;}"
      "impl Code for i32{func code(owner Self)i32{return owner;}}"
      "trait Use[A Code]{func use(owner Self)i32;}"
      "func relay[A,T Use[A]](owner T)i32{return T.use(owner);}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Code{func code(owner Self)i32;}"
      "trait Inner[A Code]{func inner(owner Self,value A)i32;}"
      "trait Outer[B Inner[i32]]{func outer(owner Self,value B)i32;}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  bad("trait Code{func code(owner Self)i32;}"
      "impl Code for i32{func code(owner Self)i32{return owner;}}"
      "trait Use[A Code,B]{func use(owner Self,value B)i32;}"
      "struct Key{}"
      "impl Use[i32,bool] for Key{func use(owner Self,value bool)i32{return 0;}}"
      "impl Use[bool,i32] for Key{func use(owner Self,value i32)i32{return 1;}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");
  std::cout << "trait-parameter-bounds: real Self/sibling/method bounds, "
               "generic impl, forward, cross-package, phantom keys and "
               "Source/HIR corruptions checked\n";
}

void ordinary_trait_receiver(const fs::path &root) {
  const auto cases = root / "tests/generics/generic-traits";
  auto control = check(cases / "ordinary-receiver-identity/lang.toml");
  need(control.success(), messages(control));
  auto legal = *control.value;
  need(hir::detail::produce_ownership(legal, {}).success() &&
           hir::verify_types(legal).success() && hir::verify(legal).success(),
       "ordinary receiver legal recomputed control rejected");
  for (const char *name : {"ordinary-receiver-reject-concrete",
                           "ordinary-receiver-reject-constrained"}) {
    auto r = check(cases / name / "lang.toml");
    need(!r.success() && r.failure == FailureKind::source &&
             has(r, "E_CALL_ARGUMENT") && !has(r, "E_IR_INVALID"),
         std::string(name) + "\n" + messages(r));
  }
  const std::string own =
      "struct Host{value i32;}"
      "trait Static[A]{func get[U](self U)i32;}"
      "impl Static[Host] for Host{func get[V](self V)i32{return 7;}}";
  for (const char *backend : {"native", "python"})
    for (const auto &body : {
             "func main()i32{let h=Host(value:7);return h.get[Host]();}",
             "func read[T Static[T]](owner T)i32{return owner.get[T]();}"
             "func main()i32{let h=Host(value:7);return read(h);}"}) {
      auto r = source(own + body, backend);
      need(!r.success() && r.failure == FailureKind::source &&
               has(r, "E_CALL_ARGUMENT") && !has(r, "E_IR_INVALID"),
           std::string(backend) + ": ordinary own parameter became receiver\n" +
               messages(r));
    }

  // A function field with the same name is legal. Reusing its value-call
  // Source for a static trait Call must not invent a trait receiver.
  for (const char *raw : {"A", "U", "Self"}) {
    const bool own = std::string(raw) == "U";
    const bool self = std::string(raw) == "Self";
    const std::string callback = self ? "func(Host)i32" : "func()i32";
    auto r = source(
        "trait Static[A]{func get" + std::string(own ? "[U]" : "") +
        "(first " + raw + ")i32;}"
        "struct Host{value i32;get " + callback + ";}"
        "impl Static[Host] for Host{func get" + (own ? "[V]" : "") +
        "(first " + (own ? "V" : "Host") + ")i32{return 7;}}"
        "func main()i32{let h=Host(value:7,get:" +
        (self ? "func(value Host)i32{return value.value;}"
              : "func()i32{return 7;}") +
        ");return Host.get" + (own ? "[Host]" : "") + "(h)+h.get(" +
        (self ? "h" : "") + ")-14;}");
    need(r.success(), std::string(raw) + " function-field control\n" + messages(r));
    auto m = *r.value;
    auto rebuilt = m;
    need(hir::detail::produce_ownership(rebuilt, {}).success() &&
             hir::verify_types(rebuilt).success() && hir::verify(rebuilt).success(),
         "function-field legal recomputed control rejected");
    HirNodeId direct{}, field{}, base{};
    for (size_t i = 0; i < m.nodes.size(); ++i)
      if (auto c = std::get_if<hir::Call>(&m.nodes[i].data)) {
        if (c->direct_symbol &&
            m.resolved->symbols.at(c->direct_symbol->value).name == "get")
          direct = HirNodeId{i};
        if (c->callee &&
            m.nodes[c->callee->value].kind() == hir::NodeKind::field_read) {
          field = HirNodeId{i};
          base = m.nodes[c->callee->value].children.at(0);
        }
      }
    need(direct.value && field.value && base.value,
         "ordinary receiver Source mutation targets missing");
    auto change = [&](auto &h) {
      auto &n = h.nodes[direct.value];
      auto &c = std::get<hir::Call>(n.data);
      need(n.source.node.node != h.nodes[field.value].source.node.node,
           "ordinary receiver Source mutation would be unchanged");
      n.source = h.nodes[field.value].source;
      h.nodes[c.args.at(0).expression.value].source = h.nodes[base.value].source;
    };
    invalid(m, std::string(raw) + " static call gained value Source", change);
    invalid_recomputed(m, std::string(raw) + " receiver Source recomputed", change);
    change(m);
    need(hir::detail::produce_ownership(m, {}).success(),
         "ordinary receiver Source ownership recomputation failed");
    auto rejected = hir::verify_types(m);
    need(messages(rejected).find(self ? "receiver differs from source arguments"
                                     : "first parameter must be the real trait Self") !=
             std::string::npos,
         "ordinary receiver Source rejected for the wrong reason\n" +
             messages(rejected));
  }
  std::cout << "ordinary-trait-receiver: raw A/U source errors; 3 source-form "
               "corruptions rejected after successful ownership recomputation\n";
}

void indexed_function_calls() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "indexed function call HIR rejected\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "published indexed function call HIR rejected");
    auto recomputed = *r.value;
    need(hir::detail::produce_ownership(recomputed, {}).success() &&
             hir::verify_types(recomputed).success(),
         "recomputed indexed function call ownership rejected");
    return *r.value;
  };
  auto bad = [](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         text + "\n" + messages(r));
  };
  auto m = good(R"SRC(
func inc(x i32)i32{return x+1;}
func add2(x i32)i32{return x+2;}
func plus(x i32,y i32)i32{return x+y;}
struct Holder{fs Vec[func(i32)i32];cb func(i32)i32;}
func makeHolder() Holder {return Holder(fs:[inc],cb:add2);}
func makeVec() Vec[func(i32)i32]{return [inc];}
func id[T](x T)T{return x;}
trait Pick{func pick[U](self Self,x i32)i32;}
struct Key{value i32;}
impl Pick for Key{func pick[V](self Self,x i32)i32{return self.value+x;}}
impl Key{func own[U](self Self,x i32)i32{return self.value-x;}}
func main()i32{
 let fs Vec[func(i32)i32]=[inc];
 if fs[0](2)!=3{return 1;}
 if (fs[0])(3)!=4{return 2;}
 let h=makeHolder();
 if h.fs[0](4)!=5{return 3;}
 if h.cb(4)!=6{return 4;}
 if makeHolder().fs[0](5)!=6{return 5;}
 if makeVec()[0](6)!=7{return 6;}
 let grid Vec[Vec[func(i32)i32]]=[[inc],[add2]];
 if grid[1][0](7)!=9{return 7;}
 let f=fs[0];
 if f(8)!=9{return 8;}
 let pair func(i32,i32)i32=plus;
 if pair(2,3)!=5{return 9;}
 if id[i32](9)!=9{return 10;}
 let key=Key(value:10);
 if key.pick[i32](5)!=15{return 11;}
 if Key.pick[i32](key,3)!=13{return 12;}
 if key.own[i32](2)!=8{return 13;}
 if Key.own[i32](key,2)!=8{return 14;}
 return 0;
}
)SRC");
  size_t indirect = 0, indexed = 0, field_index = 0, nested_index = 0;
  size_t result_field_index = 0, result_vec_index = 0, field_callee = 0;
  size_t read_callee = 0, two_arg = SIZE_MAX;
  std::set<std::string> explicit_free;
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const auto &n = m.nodes[i];
    auto c = std::get_if<hir::Call>(&n.data);
    if (!c)
      continue;
    if (c->kind == hir::CalleeKind::function_value) {
      ++indirect;
      need(c->callee && !c->direct_symbol && c->type_args.empty(),
           "function value call metadata");
      const auto &callee = m.nodes[c->callee->value];
      need(callee.type == c->signature &&
               n.children.size() == c->args.size() + 1 &&
               n.children[0] == *c->callee,
           "function value callee/arg order/type");
      for (size_t j = 0; j < c->args.size(); ++j)
        need(n.children[j + 1] == c->args[j].expression &&
                 c->args[j].source_index == j,
             "function value argument source order");
      if (c->args.size() == 2)
        two_arg = i;
      if (callee.kind() == hir::NodeKind::read)
        ++read_callee;
      if (callee.kind() == hir::NodeKind::field_read)
        ++field_callee;
      if (callee.kind() == hir::NodeKind::index_read) {
        ++indexed;
        need(callee.children.size() == 2, "indexed callee arity");
        const auto &base = m.nodes[callee.children[0].value];
        if (base.kind() == hir::NodeKind::field_read) {
          ++field_index;
          const auto &root = m.nodes[base.children.at(0).value];
          if (root.kind() == hir::NodeKind::call)
            ++result_field_index;
        }
        if (base.kind() == hir::NodeKind::index_read)
          ++nested_index;
        if (base.kind() == hir::NodeKind::call)
          ++result_vec_index;
      }
    } else if (c->direct_symbol) {
      const auto &name = m.resolved->symbols.at(c->direct_symbol->value).name;
      if (name == "id" || name == "pick" || name == "own") {
        bool concrete_instance = false;
        for (const auto &instance : m.instances)
          if (instance.symbol == *c->direct_symbol &&
              !instance.type_args.empty())
            concrete_instance = true;
        need(!c->callee && (!c->type_args.empty() || concrete_instance),
             "explicit generic function/method lost its complete arguments");
        explicit_free.insert(name);
      }
    }
  }
  need(indirect >= 9 && indexed >= 6 && field_index >= 2 &&
           nested_index >= 1 && result_field_index >= 1 &&
           result_vec_index >= 1 && field_callee >= 1 && read_callee >= 1 &&
           two_arg != SIZE_MAX,
       "indexed function call HIR coverage missing");
  need(explicit_free == std::set<std::string>{"id", "own", "pick"},
       "real explicit generic function/method suffix lost");

  auto first_indexed = [](hir::HirModule &h, size_t &at) {
    for (size_t i = 0; i < h.nodes.size(); ++i)
      if (auto c = std::get_if<hir::Call>(&h.nodes[i].data);
          c && c->kind == hir::CalleeKind::function_value && c->callee &&
          h.nodes[c->callee->value].kind() == hir::NodeKind::index_read) {
        at = i;
        return;
      }
    throw std::runtime_error("indexed function value call missing");
  };
  size_t at = 0;
  (void)first_indexed(m, at);
  invalid(m, "indexed function value signature", [&](auto &h) {
    size_t i = 0;
    first_indexed(h, i);
    std::get<hir::Call>(h.nodes[i].data).signature = TypeId{1};
  });
  invalid(m, "indexed function value callee type", [&](auto &h) {
    size_t i = 0;
    first_indexed(h, i);
    auto &c = std::get<hir::Call>(h.nodes[i].data);
    h.nodes[c.callee->value].type = TypeId{1};
  });
  auto callee_source = [&](auto &h) {
    size_t i = 0;
    first_indexed(h, i);
    auto &n = h.nodes[i];
    auto &c = std::get<hir::Call>(n.data);
    need(!c.args.empty(), "indexed function value call lost argument");
    n.children[0] = *c.callee;
    h.nodes[c.callee->value].source =
        h.nodes[c.args[0].expression.value].source;
  };
  invalid(m, "indexed function value callee source", callee_source);
  invalid_recomputed(m, "recomputed indexed callee source", callee_source);
  invalid(m, "indexed function value callee kind", [&](auto &h) {
    size_t i = 0;
    first_indexed(h, i);
    auto &c = std::get<hir::Call>(h.nodes[i].data);
    c.kind = hir::CalleeKind::direct;
  });
  invalid(m, "indexed function value argument source", [&](auto &h) {
    auto &n = h.nodes[two_arg];
    auto &c = std::get<hir::Call>(n.data);
    need(c.args.size() == 2, "two-argument function value call missing");
    std::swap(c.args[0].expression, c.args[1].expression);
    std::swap(n.children[1], n.children[2]);
  });

  // A scalar-only function has no owning-call Source check to reject a
  // forged non-Call node. The function-value check must reject it directly.
  auto scalar = good("func sub(x i32,y i32)i32{return x-y;}"
                     "func main()i32{let f func(i32,i32)i32=sub;"
                     "return f(7,2)-5;}");
  for (bool swap_arguments : {false, true}) {
    auto noncall_source = [&](auto &h) {
      for (auto &n : h.nodes)
        if (auto c = std::get_if<hir::Call>(&n.data);
            c && c->kind == hir::CalleeKind::function_value) {
          need(c->args.size() == 2, "scalar function-value arguments missing");
          n.source = h.nodes[c->args[0].expression.value].source;
          const auto &file = *h.resolved->project->module(n.source.node.module)
                                  .files.at(n.source.node.file);
          need(file.node(n.source.node.node).kind != AstKind::Call,
               "function-value Source mutation still names a Call");
          if (swap_arguments) {
            std::swap(c->args[0].expression, c->args[1].expression);
            std::swap(n.children[1], n.children[2]);
          }
          return;
        }
      throw std::runtime_error("scalar function-value call missing");
    };
    const std::string label = swap_arguments
                                 ? "non-Call Source hides swapped scalar args"
                                 : "function-value non-Call Source";
    invalid(scalar, label, noncall_source);
    invalid_recomputed(scalar, label + " recomputed", noncall_source);
  }

  bad("func inc(x i32)i32{return x+1;}"
      "func main()i32{let fs Vec[func(i32)i32]=[inc];"
      "return fs[true](2);}", "E_TYPE_MISMATCH");
  bad("func inc(x i32)i32{return x+1;}struct H{fs Vec[func(i32)i32];}"
      "func main()i32{let h=H(fs:[inc]);return h.fs[true](2);}",
      "E_TYPE_MISMATCH");
  bad("func main()i32{let xs Vec[i32]=[1];return xs[0](2);}",
      "E_CALL_ARGUMENT");
  bad("struct H{fs Vec[i32];}func main()i32{let h=H(fs:[1]);"
      "return h.fs[0](2);}", "E_CALL_ARGUMENT");
  bad("func id[T](x T)T{return x;}func main()i32{return id[0](1);}",
      "E_NAME_KIND");
  bad("func id[T](x T)T{return x;}func main()i32{return id[i32,i64](1);}",
      "E_TYPE_MISMATCH");
  bad("trait T{}struct N{}func need[U T](x U)i32{return 1;}"
      "func main()i32{return need[N](N());}", "E_TYPE_MISMATCH");
  bad("struct K{v i32;}impl K{func own[U](self Self,x i32)i32{"
      "return self.v+x;}}func main()i32{let k=K(v:1);"
      "return k.own[0](2);}", "E_NAME_KIND");
  bad("struct K{v i32;}impl K{func own[U](self Self,x i32)i32{"
      "return self.v+x;}}func main()i32{let k=K(v:1);"
      "return k.own[i32,i64](2);}", "E_TYPE_MISMATCH");
  std::cout << "indexed-function-calls: direct/parenthesized/field/nested/"
               "multi-step HIR and generic controls passed\n";
}


void builtin_impl_targets() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "built-in impl HIR rejected\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "published built-in impl HIR rejected");
    auto recomputed = *r.value;
    need(hir::detail::produce_ownership(recomputed, {}).success() &&
             hir::verify_types(recomputed).success(),
         "recomputed built-in impl ownership rejected");
    return *r.value;
  };
  auto bad = [](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         text + "\n" + messages(r));
  };
  auto m = good(R"SRC(
trait First { func first(owner Self) i32; }
trait Convert[A,B] { func convert(owner Self) i32; }
trait Pick[T] { func pick(owner Self,index i32) T; }
trait Text { func text(owner Self) string; }
trait Code { func code(owner Self) i32; }
impl First for Vec[i32] { func first(owner Self) i32 {return owner[0];} }
impl Convert[i32,bool] for Vec[i32] { func convert(owner Self) i32 {return owner[0]+10;} }
impl Convert[bool,i32] for Vec[i32] { func convert(owner Self) i32 {return owner[0]+20;} }
impl[T] Pick[T] for Vec[T] { func pick(owner Self,index i32) T {return owner[index];} }
impl[T] Vec[T] {
  func read(self Self,index i32) T {return self[index];}
  func echo[U](self Self,value U) U {return value;}
}
impl Text for string { func text(owner Self) string {return owner;} }
impl string { func echo(self Self) string {return self;} }
impl Code for Unit { func code(owner Self) i32 {return 3;} }
impl Unit { func value(self Self) i32 {return 4;} }
func make() {}
func use[T Convert[i32,bool]](owner T) i32 {return T.convert(owner)+owner.convert();}
func main() i32 {
  let values Vec[i32]=[1];
  let text="x";
  let unit=make();
  let a=values.first()+values.pick(0)+values.read(0)+use[Vec[i32]](values);
  let b=text.text();
  let d=text.echo();
  let c=Unit.code(unit)+unit.value();
  return 0;
}
)SRC");
  auto symbol_name = [&](SymbolId s) -> const std::string & {
    return m.resolved->symbols.at(s.value).name;
  };
  const hir::ImplDecl *vec_inherent = nullptr;
  const hir::ImplDecl *unit_inherent = nullptr;
  const hir::ImplDecl *string_inherent = nullptr;
  const hir::ImplDecl *convert_impl = nullptr;
  for (const auto &im : m.impls) {
    auto t = m.types.find(im.self_type);
    need(t, "built-in impl self type missing");
    if (!im.trait && t->kind == hir::TypeKind::Vec && !im.params.empty())
      vec_inherent = &im;
    if (!im.trait && t->kind == hir::TypeKind::Unit)
      unit_inherent = &im;
    if (!im.trait && t->kind == hir::TypeKind::String)
      string_inherent = &im;
    if (im.trait && symbol_name(im.trait->declaration) == "Convert" &&
        im.trait->args.size() == 2 && !convert_impl)
      convert_impl = &im;
  }
  need(vec_inherent && unit_inherent && string_inherent && convert_impl,
       "built-in Vec/string/Unit impl declarations missing");
  need(!vec_inherent->params.empty() && !convert_impl->trait->args.empty(),
       "built-in impl parameters missing");
  const auto vec_parameter = vec_inherent->params.front().symbol;

  TypeId i64 = TypeId{0};
  for (uint64_t id = 1; id <= m.types.size(); ++id)
    if (auto t = m.types.find(TypeId{id});
        t && t->kind == hir::TypeKind::SignedInt && t->bits == 64)
      i64 = TypeId{id};
  need(i64.value, "i64 type missing");
  hir::Type vec_i64_type;
  vec_i64_type.kind = hir::TypeKind::Vec;
  vec_i64_type.type_args = {i64};
  const auto vec_i64 = m.types.intern(std::move(vec_i64_type));

  invalid_recomputed(m, "Vec impl self element type swapped", [&](auto &x) {
    for (auto &im : x.impls)
      if (!im.trait && !im.params.empty() &&
          x.types.find(im.self_type)->kind == hir::TypeKind::Vec) {
        im.self_type = vec_i64;
        return;
      }
    throw std::runtime_error("generic Vec inherent impl missing");
  });
  invalid_recomputed(m, "Vec impl complete trait arguments swapped",
                     [&](auto &x) {
    for (auto &im : x.impls)
      if (im.trait && im.trait->args.size() == 2 &&
          x.resolved->symbols[im.trait->declaration.value].name == "Convert") {
        std::swap(im.trait->args[0], im.trait->args[1]);
        return;
      }
    throw std::runtime_error("complete-argument Vec impl missing");
  });
  invalid_recomputed(m, "built-in impl real Self identity changed", [&](auto &x) {
    for (auto &im : x.impls)
      if (!im.trait &&
          x.types.find(im.self_type)->kind == hir::TypeKind::Unit) {
        im.self_parameter = vec_parameter;
        return;
      }
    throw std::runtime_error("Unit inherent impl missing");
  });

  bad("trait Convert[A]{func convert(owner Self)i32;}"
      "impl Convert[i32] for Vec[i32]{func convert(owner Self)i32{return 0;}}"
      "impl Convert[bool] for Vec[i32]{func convert(owner Self)i32{return 1;}}"
      "func main()i32{let v Vec[i32]=[1];return v.convert();}",
      "E_TYPE_MISMATCH");
  bad("trait Pick[T]{func pick(owner Self,index i32)T;}"
      "impl[T] Pick[T] for Vec[T]{func pick(owner Self,index i32)T{return owner[index];}}"
      "impl Pick[i32] for Vec[i32]{func pick(owner Self,index i32)i32{return owner[index];}}"
      "func main()i32{return 0;}", "E_TYPE_MISMATCH");

  auto same = good(R"SRC(
trait Left { func pick(owner Self,index i32) i32; }
trait Right { func pick(owner Self,index i32) i32; }
impl Left for Vec[i32] { func pick(owner Self,index i32) i32 {return owner[index]+1;} }
impl Right for Vec[i32] { func pick(owner Self,index i32) i32 {return owner[index]+2;} }
func forward[T Left](owner T,index i32) i32 {return T.pick(owner,index);}
func main() i32 {let values Vec[i32]=[1];return forward[Vec[i32]](values,0)-2;}
)SRC");
  auto same_name = [&](SymbolId s) -> const std::string & {
    return same.resolved->symbols.at(s.value).name;
  };
  const hir::TraitDecl *left = nullptr, *right = nullptr;
  for (const auto &tr : same.traits) {
    if (same_name(tr.symbol) == "Left") left = &tr;
    if (same_name(tr.symbol) == "Right") right = &tr;
  }
  need(left && right, "same-signature built-in traits missing");
  auto impl_method_for = [&](const hir::TraitDecl &trait) -> SymbolId {
    for (const auto &im : same.impls)
      if (im.trait && im.trait->declaration == trait.symbol)
        for (auto method : im.methods)
          if (same_name(method) == "pick")
            return method;
    throw std::runtime_error("same-signature built-in impl method missing");
  };
  const auto left_pick = impl_method_for(*left);
  const auto right_pick = impl_method_for(*right);
  invalid_recomputed(same, "same-signature built-in method source changed", [&](auto &x) {
    for (auto &n : x.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data);
          c && c->direct_symbol && same_name(*c->direct_symbol) == "pick") {
        c->direct_symbol = right_pick;
        return;
      }
    throw std::runtime_error("same-signature built-in concrete call missing");
  });
  (void)left_pick;
  std::cout << "builtin-impl-targets: fixed/generic Vec, string, Unit, full "
               "trait arguments, ambiguity/overlap, and bad-source HIR\n";
}

void dependent_callback_completion(const fs::path &root) {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    need(hir::verify_types(*r.value).success(),
         "dependent callback HIR rejected\n" + messages(r));
    need(hir::verify(*r.value).success(),
         "dependent callback did not publish\n" + messages(r));
    return *r.value;
  };
  auto unsupported = [](const std::string &text) {
    for (const char *backend : {"native", "python"}) {
      auto r = source(text, backend);
      need(!r.success() && r.failure == FailureKind::unsupported &&
               has(r, "E_CAPABILITY_UNAVAILABLE") &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
           std::string(backend) + ": concrete dependent data accepted\n" +
               text + "\n" + messages(r));
    }
  };
  auto nominal = [](const hir::HirModule &m,
                    const std::string &name) -> TypeId {
    for (uint64_t i = 1; i <= m.types.size(); ++i) {
      auto t = m.types.find(TypeId{i});
      if (t && (t->kind == hir::TypeKind::NominalStruct ||
                t->kind == hir::TypeKind::NominalEnum) &&
          m.resolved->symbols[t->declaration.value].name == name)
        return TypeId{i};
    }
    throw std::runtime_error("dependent callback nominal missing: " + name);
  };
  auto trait = [](const hir::HirModule &m,
                  const std::string &name) -> const hir::TraitDecl * {
    for (const auto &t : m.traits)
      if (m.resolved->symbols[t.symbol.value].name == name)
        return &t;
    return nullptr;
  };

  // Complete substitution of real Self, trait A, generic impl P and
  // method-own U, including struct-held callbacks, a recursive Self callback,
  // a returned Self callback, sibling generic forwarding and a concrete call.
  const std::string full = R"FULL(
struct Pair { value i32; flag bool; }
struct Own { items Vec[i32]; }
struct Holder[T] { cb func(inout T,T)T; }
struct Box[P] { marker i32; value P; }
func keep_own(value Own) Unit {}
func replace(slot inout Pair, value Pair) Pair {
    let old = slot; slot = value; return old;
}
func replace_box(slot inout Box[Pair], value Box[Pair]) Box[Pair] {
    let old = slot; slot = value; return old;
}
trait Flow[A] {
    func run[U](owner Self, callback func(inout A,A)A,
                self_callback func(inout Self,Self)Self,
                slot inout A, self_slot inout Self,
                held Holder[A], self_held Holder[Self], tag U) Unit;
}
impl[P] Flow[P] for Box[P] {
    func run[U](owner Self, callback func(inout P,P)P,
                self_callback func(inout Self,Self)Self,
                slot inout P, self_slot inout Self,
                held Holder[P], self_held Holder[Self], tag U) Unit {}
}
func forward[T Flow[Pair],U](owner T, callback func(inout Pair,Pair)Pair,
        self_callback func(inout T,T)T, slot inout Pair, self_slot inout T,
        held Holder[Pair], self_held Holder[T], tag U) Unit {
    T.run[U](owner, callback, self_callback, inout slot, inout self_slot,
             held, self_held, tag);
}
trait SelfFactory {
    func make[U](owner Self, tag U) (func(inout Self,Self)Self);
}
impl SelfFactory for Pair {
    func make[U](owner Self, tag U) (func(inout Pair,Pair)Pair) {
        return replace;
    }
}
func main() i32 {
    let owner = Box[Pair](marker:0, value:Pair(value:0, flag:false));
    var slot = Pair(value:1, flag:false);
    var self_slot = Box[Pair](marker:1, value:Pair(value:2, flag:true));
    let held = Holder[Pair](cb:replace);
    let self_held = Holder[Box[Pair]](cb:replace_box);
    owner.run[i32](replace, replace_box, inout slot, inout self_slot,
                   held, self_held, 1);
    forward[Box[Pair],i32](owner, replace, replace_box, inout slot,
                           inout self_slot, held, self_held, 2);
    let powner = Pair(value:9, flag:true);
    let factory = powner.make[i32](5);
    var fslot = Pair(value:3, flag:false);
    let old = factory(inout fslot, Pair(value:4, flag:true));
    if old.value != 3 || old.flag || fslot.value != 4 || !fslot.flag { return 1; }
    return 0;
}
)FULL";
  auto complete = good(full);
  const auto owned = nominal(complete, "Own");
  const auto *flow = trait(complete, "Flow");
  const auto *factory = trait(complete, "SelfFactory");
  need(flow && factory, "dependent callback traits missing");
  for (auto &im : complete.impls) {
    need(!im.trait || im.trait->declaration != flow->symbol ||
             im.trait->args.size() == 1,
         "generic dependent impl application arity changed");
  }

  // A concrete trait application that substitutes owning data must reject
  // even when the declaration is never called.
  unsupported("struct Own{items Vec[i32];}"
              "struct Box[P]{marker i32;}"
              "trait F[A]{func bad[U](owner Self,cb func(inout A,A)A)Unit;}"
              "impl[P] F[P] for Box[P]{"
              "func bad[U](owner Self,cb func(inout P,P)P)Unit{}}"
              "func uncalled[T F[Own]](x T)Unit{}"
              "func main()i32{return 0;}");
  // Real Self replacement with owning data is also a source error.
  unsupported("struct Own{items Vec[i32];}"
              "trait S{func bad[U](owner Self,"
              "cb func(inout Self,Self)Self)Unit;}"
              "impl S for Own{func bad[U](owner Self,"
              "cb func(inout Own,Own)Own)Unit{}}"
              "func main()i32{return 0;}");

  // Corrupt the complete trait application and the real Self identity. Both
  // direct verification and a successful ownership recomputation reject.
  SymbolId flow_symbol = flow->symbol;
  auto swap_application = [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (im.trait && im.trait->declaration == flow_symbol &&
          im.trait->args.size() == 1) {
        im.trait->args[0] = owned;
        changed = true;
      }
    need(changed, "dependent callback application mutation target missing");
  };
  invalid(complete, "dependent callback trait application owning", swap_application);
  invalid_recomputed(complete,
                     "dependent callback trait application owning recomputed",
                     swap_application);
  SymbolId factory_symbol = factory->symbol;
  auto replace_self = [&](auto &x) {
    bool changed = false;
    for (auto &im : x.impls)
      if (im.trait && im.trait->declaration == factory_symbol) {
        im.self_type = owned;
        changed = true;
      }
    need(changed, "dependent callback Self mutation target missing");
  };
  invalid(complete, "dependent callback real Self owning", replace_self);
  invalid_recomputed(complete,
                     "dependent callback real Self owning recomputed",
                     replace_self);

  // A same-signature function-value target swap keeps the type but changes
  // the real declaration identity.
  auto values = good(R"(
struct Pair { value i32; }
func replace(slot inout Pair, value Pair) Pair {
    let old = slot; slot = value; return old;
}
func duplicate(slot inout Pair, value Pair) Pair {
    let old = slot; slot = Pair(value: value.value + 1); return old;
}
func main() i32 {
    var slot = Pair(value:1);
    let callback func(inout Pair,Pair)Pair = replace;
    let old = callback(inout slot, Pair(value:2));
    if old.value != 1 || slot.value != 2 { return 1; }
    return 0;
}
)");
  SymbolId replace_symbol{}, duplicate_symbol{};
  for (const auto &f : values.functions) {
    auto name = values.resolved->symbols[f.symbol.value].name;
    if (name == "replace")
      replace_symbol = f.symbol;
    if (name == "duplicate")
      duplicate_symbol = f.symbol;
  }
  need(replace_symbol.value && duplicate_symbol.value,
       "same-signature callback helpers missing");
  auto swap_value = [&](auto &x) {
    bool changed = false;
    for (auto &n : x.nodes)
      if (auto v = std::get_if<hir::FunctionValue>(&n.data);
          v && v->symbol == replace_symbol) {
        v->symbol = duplicate_symbol;
        changed = true;
      }
    need(changed, "dependent callback FunctionValue mutation target missing");
  };
  invalid(values, "dependent callback function value target", swap_value);
  invalid_recomputed(values,
                     "dependent callback function value target recomputed",
                     swap_value);

  // Erase an inner callback inout mode consistently: the function's callback
  // parameter type, the indirect call signature, the argument mode and its
  // Place->Read all change together. The concrete source declaration still
  // proves the original mode, so both entry points reject.
  auto erased = good(R"(
struct Pair { value i32; }
func replace(slot inout Pair, value Pair) Pair {
    let old = slot; slot = value; return old;
}
func apply(callback func(inout Pair,Pair)Pair, slot inout Pair,
           value Pair) Pair {
    return callback(inout slot, value);
}
func main() i32 {
    var slot = Pair(value:1);
    let old = apply(replace, inout slot, Pair(value:2));
    if old.value != 1 || slot.value != 2 { return 1; }
    return 0;
}
)");
  SymbolId apply_symbol{};
  for (const auto &f : erased.functions)
    if (erased.resolved->symbols[f.symbol.value].name == "apply")
      apply_symbol = f.symbol;
  need(apply_symbol.value, "dependent callback erase function missing");
  auto erase_mode = [&](auto &x) {
    bool changed = false;
    for (auto &f : x.functions)
      if (f.symbol == apply_symbol) {
        auto sig = *x.types.find(f.signature);
        need(sig.parameters.size() == 3, "erase apply signature shape");
        auto cb = *x.types.find(sig.parameters[0].type);
        need(cb.kind == hir::TypeKind::Function &&
                 cb.parameters.size() == 2 &&
                 cb.parameters[0].mode == hir::ParamMode::inout,
             "erase callback signature shape");
        cb.parameters[0].mode = hir::ParamMode::ordinary;
        auto callback_type = x.types.intern(cb);
        sig.parameters[0].type = callback_type;
        f.signature = x.types.intern(sig);
        for (auto &n : x.nodes) {
          if (x.scopes[n.scope.value].function != apply_symbol)
            continue;
          auto c = std::get_if<hir::Call>(&n.data);
          if (!c || c->kind != hir::CalleeKind::function_value)
            continue;
          need(c->callee.has_value() && c->args.size() == 2 &&
                   c->args[0].mode == hir::ParamMode::inout,
               "erase indirect callback call shape");
          x.nodes[c->callee->value].type = callback_type;
          c->signature = callback_type;
          c->args[0].mode = hir::ParamMode::ordinary;
          auto &argument = x.nodes[c->args[0].expression.value];
          auto place = std::get<hir::Place>(argument.data);
          argument.data = hir::Read{place.symbol};
          changed = true;
        }
      }
    need(changed, "dependent callback inout erase target missing");
  };
  invalid(erased, "dependent callback inout mode erased", erase_mode);
  invalid_recomputed(erased,
                     "dependent callback inout mode erased recomputed",
                     erase_mode);

  // A returned recursive nominal must expand the next complete application.
  // Wrap[i32] -> Wrap[Vec[i32]] reaches a concrete illegal callback instead
  // of stopping at the shared Wrap declaration or exhausting a budget.
  const std::string recursive_return = R"WRAP(
struct Wrap[T] {
    direct func(inout T);
    next func() Wrap[Vec[T]];
}
trait F[A] { func see[U](owner Self, held Wrap[A]); }
struct Host {}
impl F[i32] for Host { func see[V](owner Self, held Wrap[i32]) {} }
func main() i32 { return 0; }
)WRAP";
  const std::string mutual_next_first = R"WRAP(
struct Wrap[T] { next func() Relay[Vec[T]]; direct func(inout T); }
struct Relay[U] { next func() Wrap[U]; }
trait F[A] { func see[U](owner Self, held Wrap[A]); }
struct Host {}
impl F[i32] for Host { func see[V](owner Self, held Wrap[i32]) {} }
func main() i32 { return 0; }
)WRAP";
  const std::string mutual_direct_first = R"WRAP(
struct Wrap[T] { direct func(inout T); next func() Relay[Vec[T]]; }
struct Relay[U] { next func() Wrap[U]; }
trait F[A] { func see[U](owner Self, held Wrap[A]); }
struct Host {}
impl F[i32] for Host { func see[V](owner Self, held Wrap[i32]) {} }
func main() i32 { return 0; }
)WRAP";
  for (const auto &program : {recursive_return, mutual_next_first,
                              mutual_direct_first})
    for (const char *backend : {"native", "python"}) {
      auto r = source(program, backend);
      auto text = messages(r);
      need(!r.success() && r.failure == FailureKind::unsupported &&
               has(r, "E_CAPABILITY_UNAVAILABLE") &&
               !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR") &&
               text.find("budget") == std::string::npos &&
               text.find("exceeds") == std::string::npos,
           std::string(backend) +
               ": recursive return callback was not a precise source error\n" +
               text);
    }
  auto same_application = good(R"WRAP(
struct Wrap[T] {
    direct func(inout T);
    next func() Wrap[T];
}
trait F[A] { func see[U](owner Self, held Wrap[A]); }
struct Host {}
impl F[i32] for Host { func see[V](owner Self, held Wrap[i32]) {} }
func main() i32 { return 0; }
)WRAP");
  need(hir::verify_types(same_application).success() &&
           hir::verify(same_application).success(),
       "same-application recursive control did not publish");
  auto ordinary_mutual = good(R"WRAP(
struct Wrap[T] { next func() Relay[Vec[T]]; }
struct Relay[U] { next func() Wrap[U]; }
trait F[A] { func see[U](owner Self, held Wrap[A]); }
struct Host {}
impl F[i32] for Host { func see[V](owner Self, held Wrap[i32]) {} }
func main() i32 { return 0; }
)WRAP");
  need(hir::verify_types(ordinary_mutual).success() &&
           hir::verify(ordinary_mutual).success(),
       "ordinary mutual growing-return control did not publish");

  // Cross-package forwarding and a non-capturing lambda are checked from the
  // real project source; the runtime backends cover the same shape in the
  // fixture projects below this C++ test's scope.
  auto cross = check(root /
      "tests/generics/generic-traits/callback-cross-package-lambda/lang.toml");
  need(cross.success(), "dependent cross-package lambda rejected\n" +
                            messages(cross));
  need(hir::verify_types(*cross.value).success(),
       "dependent cross-package lambda HIR rejected");
  need(hir::verify(*cross.value).success(),
       "dependent cross-package lambda did not publish");
  std::cout << "dependent-callback-completion: complete Self/A/P/U graphs, "
               "concrete owning rejects, identity and mode corruptions\n";
}


void explicit_self_modes() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto types = hir::verify_types(*r.value);
    need(types.success(), "actual-self HIR types rejected\n" + messages(types));
    auto published = hir::verify(*r.value);
    need(published.success(), "actual-self HIR did not publish\n" +
                                  messages(published));
    return *r.value;
  };
  auto bad = [](const std::string &text, const std::string &code) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::source && has(r, code) &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         text + "\n" + messages(r));
  };
  auto unsupported = [](const std::string &text) {
    auto r = source(text);
    need(!r.success() && r.failure == FailureKind::unsupported &&
             has(r, "E_CAPABILITY_UNAVAILABLE") &&
             !has(r, "E_IR_INVALID") && !has(r, "E_INTERNAL_ERROR"),
         text + "\n" + messages(r));
  };

  const std::string base = R"(
struct Box { items Vec[i32]; marker i32; }
struct Pair { value i32; }
trait Take { func take(owner move Self) Self; }
trait MoveGeneric[A] { func move_own[U](owner move Self, tag U,
                                        value move A) Self; }
trait Step { func bump(owner inout Self, delta i32) i32; }
trait StepGeneric[A] { func change[U](owner inout Self, delta A, tag U) i32; }
impl Take for Box { func take(owner move Self) Self { return move owner; } }
impl MoveGeneric[i32] for Box {
    func move_own[U](owner move Self, tag U, value move i32) Self {
        return move owner;
    }
}
impl Step for Pair {
    func bump(owner inout Self, delta i32) i32 {
        let old = owner.value;
        owner = Pair(value: old + delta);
        return old;
    }
}
impl StepGeneric[i32] for Pair {
    func change[U](owner inout Self, delta i32, tag U) i32 {
        let old = owner.value;
        owner = Pair(value: old + delta);
        return old;
    }
}
func move_relay[T Take](x move T) T { return T.take(move x); }
func step_relay[T Step](x inout T, delta i32) i32 {
    return T.bump(inout x, delta);
}
func generic_relay[A, T StepGeneric[A], U](x inout T, delta A, tag U) i32 {
    return T.change[U](inout x, delta, tag);
}
func main() i32 {
    let first = Box(items: [1], marker: 2);
    let moved = Box.take(move first);
    let second = Box(items: [3], marker: 4);
    let generic = Box.move_own[bool](move second, true, 5);
    let third = Box(items: [6], marker: 7);
    let relayed = move_relay(move third);
    var p = Pair(value: 8);
    let oldp = Pair.bump(inout p, 2);
    var q = Pair(value: 10);
    let oldq = generic_relay[i32, Pair, bool](inout q, 4, true);
    if moved.items[0] != 1 || generic.items[0] != 3 ||
       relayed.items[0] != 6 || oldp != 8 || p.value != 10 ||
       oldq != 10 || q.value != 14 {
        return 1;
    }
    return 0;
}
)";
  auto m = good(base);
  auto direct = [](hir::HirModule &h, const std::string &name) -> hir::Call & {
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::Call>(&n.data); c && c->direct_symbol)
        if (h.resolved->symbols[c->direct_symbol->value].name == name)
          return *c;
    throw std::runtime_error("actual-self call missing: " + name);
  };
  auto instance = [](hir::HirModule &h,
                     const std::string &name) -> hir::FunctionInstance & {
    for (auto &i : h.instances)
      if (h.resolved->symbols[i.origin.value].name == name)
        return i;
    throw std::runtime_error("actual-self instance missing: " + name);
  };
  auto constraint = [](hir::HirModule &h) -> hir::ConstraintCall & {
    for (auto &n : h.nodes)
      if (auto c = std::get_if<hir::ConstraintCall>(&n.data);
          c && !c->trait.args.empty())
        return *c;
    throw std::runtime_error("actual-self ConstraintCall missing");
  };
  auto bool_type = [](hir::HirModule &h) -> TypeId {
    for (uint64_t id = 1; id <= h.types.size(); ++id)
      if (auto t = h.types.find(TypeId{id});
          t && t->kind == hir::TypeKind::Bool)
        return TypeId{id};
    throw std::runtime_error("bool type missing");
  };
  auto pair_type = [](hir::HirModule &h) -> TypeId {
    for (auto &d : h.nominals)
      if (h.resolved->symbols[d.symbol.value].name == "Pair")
        for (uint64_t id = 1; id <= h.types.size(); ++id)
          if (auto t = h.types.find(TypeId{id});
              t && t->kind == hir::TypeKind::NominalStruct &&
              t->declaration == d.symbol)
            return TypeId{id};
    throw std::runtime_error("Pair type missing");
  };

  auto reject_both = [&](const std::string &name, const auto &change) {
    invalid(m, name, change);
    invalid_recomputed(m, name + " (recomputed ownership)", change);
  };
  reject_both("actual-self prototype type", [&](auto &h) {
    for (auto &t : h.traits)
      if (h.resolved->symbols[t.symbol.value].name == "Take") {
        auto sig = *h.types.find(t.methods.front().signature);
        sig.parameters.front().type = pair_type(h);
        t.methods.front().signature = h.types.intern(sig);
        return;
      }
    throw std::runtime_error("Take prototype missing");
  });
  reject_both("actual-self prototype mode", [&](auto &h) {
    for (auto &t : h.traits)
      if (h.resolved->symbols[t.symbol.value].name == "Take") {
        auto sig = *h.types.find(t.methods.front().signature);
        sig.parameters.front().mode = hir::ParamMode::ordinary;
        t.methods.front().signature = h.types.intern(sig);
        return;
      }
    throw std::runtime_error("Take prototype missing");
  });
  reject_both("actual-self call mode",
              [&](auto &h) {
                direct(h, "take").args.front().mode =
                    hir::ParamMode::ordinary;
              });
  reject_both("actual-self source position", [&](auto &h) {
    auto &call = direct(h, "bump");
    need(call.args.size() == 2, "actual-self bump arity");
    h.nodes[call.args[0].expression.value].source =
        h.nodes[call.args[1].expression.value].source;
  });
  reject_both("actual-self full trait arguments", [&](auto &h) {
    constraint(h).trait.args.front() = bool_type(h);
  });
  reject_both("actual-self method instance key", [&](auto &h) {
    auto &i = instance(h, "move_own");
    need(!i.type_args.empty(), "method instance key arity");
    i.type_args.back() = pair_type(h);
  });

  bad("struct Box { items Vec[i32]; }"
      "impl Box { func take(self move Self) Self { return move self; } }"
      "func main() i32 { let b = Box(items: [1]);"
      "let r = Box.take(b); return 0; }",
      "E_CALL_ARGUMENT");
  bad("struct Box { items Vec[i32]; }"
      "impl Box { func take(self move Self) Self { return move self; } }"
      "func main() i32 { let b = Box(items: [1]);"
      "let r = Box.take(move b); return b.items[0]; }",
      "E_OWNERSHIP_USE_AFTER_MOVE");
  bad("struct Pair { value i32; }"
      "impl Pair { func bump(self inout Self) { self.value += 1; } }"
      "func main() i32 { let p = Pair(value: 1);"
      "Pair.bump(inout p); return p.value - 2; }",
      "E_CALL_ARGUMENT");
  bad("struct Pair { value i32; }"
      "impl Pair { func bump(self inout Self, other i32) {"
      "self.value += other; } }"
      "func main() i32 { var p = Pair(value: 1);"
      "Pair.bump(inout p, p.value); return 0; }",
      "E_CALL_ARGUMENT");
  unsupported("struct Owned { items Vec[i32]; }"
              "impl Owned { func bump(self inout Self) { } }"
              "func main() i32 { var o = Owned(items: [1]);"
              "Owned.bump(inout o); return 0; }");
  unsupported("struct Owned { items Vec[i32]; }"
              "trait Step { func change(owner inout Self); }"
              "impl Step for Owned { func change(owner inout Self) { } }"
              "func main() i32 { return 0; }");
  unsupported("struct Box { items Vec[i32]; }"
              "impl Box { func take(self move Self) Self { return move self; } }"
              "func main() i32 { let b = Box(items: [1]);"
              "let r = b.take(); return 0; }");
  unsupported("struct Pair { value i32; }"
              "impl Pair { func bump(self inout Self) { self.value += 1; } }"
              "func main() i32 { var p = Pair(value: 1);"
              "p.bump(); return 0; }");
  std::cout << "explicit-self-modes: Type/T actual Self move/inout, full "
               "trait/own arguments, controls and corrupt HIR passed\n";
}

void unbound_method_values() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto types = hir::verify_types(*r.value);
    need(types.success(), "unbound method value HIR types rejected\n" +
                              messages(types));
    auto published = hir::verify(*r.value);
    need(published.success(), "unbound method value HIR did not publish\n" +
                                  messages(published));
    return *r.value;
  };
  const std::string base = R"(
enum Err { Bad; }
struct Box[T] { value T; }
struct Host { value i32; }
trait Pick[A, B] {
    func tag(owner Self) i32;
    func echo[U](owner Self, value U) U;
}
impl Pick[i32, bool] for Host {
    func tag(owner Self) i32 { return 11; }
    func echo[V](owner Self, value V) V { return value; }
}
impl Pick[bool, i32] for Host {
    func tag(owner Self) i32 { return 22; }
    func echo[V](owner Self, value V) V { return value; }
}
impl Host {
    func get(self Self) i32 { return self.value; }
    func make(value i32) Self { return Host(value: value); }
    func fail(self Self) i32 throws Err { throw Err.Bad; }
}
impl[T] Box[T] {
    func echo[U](self Self, item U) U { return item; }
    func phantom[U](self Self) T { return self.value; }
}
func first[T Pick[i32, bool]](value T) i32 {
    let tag func(T) i32 = T.tag;
    let echo func(T, i32) i32 = T.echo;
    return tag(value) + echo(value, 7);
}
func second[T Pick[bool, i32]](value T) i32 {
    let tag func(T) i32 = T.tag;
    return tag(value);
}
func main() i32 {
    let host = Host(value: 7);
    let get func(Host) i32 = Host.get;
    let make func(i32) Host = Host.make;
    let err func(Host) i32 throws Err = Host.fail;
    let box = Box[i32](value: 9);
    let echo_bool func(Box[i32], bool) bool = Box[i32].echo[bool];
    let echo_i64 func(Box[i32], i64) i64 = Box[i32].echo[i64];
    let phantom_i64 func(Box[i32]) i32 = Box[i32].phantom[i64];
    let phantom_bool func(Box[i32]) i32 = Box[i32].phantom[bool];
    let made = make(5);
    if get(host) != 7 || made.value != 5 { return 1; }
    if !echo_bool(box, true) || echo_i64(box, 8) != 8 { return 2; }
    if phantom_i64(box) != 9 || phantom_bool(box) != 9 { return 3; }
    if first[Host](host) != 18 || second[Host](host) != 22 { return 4; }
    try { let ignored = err(host); return 5; }
    catch error Err { }
    return 0;
}
)";
  auto m = good(base);
  auto symbol_named = [&](hir::HirModule &h, std::string_view name) {
    std::vector<SymbolId> result;
    for (const auto &s : h.resolved->symbols)
      if (s.name == name)
        result.push_back(s.id);
    return result;
  };
  auto node_owner = [&](hir::HirModule &h, const hir::HirNode &n) {
    return h.scopes[n.scope.value].function;
  };
  auto owner_is = [&](hir::HirModule &h, SymbolId owner,
                      std::string_view name) {
    for (const auto &i : h.instances)
      if (i.symbol == owner)
        return h.resolved->symbols[i.origin.value].name == name;
    return h.resolved->symbols[owner.value].name == name;
  };
  auto find_value = [&](hir::HirModule &h, std::string_view owner,
                        std::string_view target) -> hir::FunctionValue & {
    for (auto &n : h.nodes) {
      auto value = std::get_if<hir::FunctionValue>(&n.data);
      if (!value)
        continue;
      auto function = node_owner(h, n);
      bool in_template = std::any_of(
          h.templates.begin(), h.templates.end(), [&](const auto &t) {
            return t.function.symbol == function;
          });
      if (!in_template && owner_is(h, function, owner) &&
          h.resolved->symbols[value->symbol.value].name == target)
        return *value;
    }
    throw std::runtime_error("method value missing: " + std::string(owner) +
                             "." + std::string(target));
  };
  auto find_prototype_value = [&](hir::HirModule &h,
                                  std::string_view target)
      -> hir::FunctionValue & {
    for (auto &n : h.nodes)
      if (auto v = std::get_if<hir::FunctionValue>(&n.data);
          v && std::any_of(h.templates.begin(), h.templates.end(),
                            [&](const auto &t) {
                              return t.function.symbol ==
                                     node_owner(h, n);
                            }) &&
          h.resolved->symbols[v->symbol.value].name == target)
        return *v;
    throw std::runtime_error("prototype method value missing: " +
                             std::string(target));
  };
  auto find_node = [&](hir::HirModule &h, const hir::FunctionValue &value)
      -> hir::HirNode & {
    for (auto &n : h.nodes)
      if (auto v = std::get_if<hir::FunctionValue>(&n.data); v == &value)
        return n;
    throw std::runtime_error("method value node missing");
  };
  auto literal_source = [&](hir::HirModule &h, const hir::HirNode &value) {
    const auto owner = node_owner(h, value);
    for (const auto &n : h.nodes)
      if (n.kind() == hir::NodeKind::integer_literal &&
          node_owner(h, n) == owner)
        return n.source;
    throw std::runtime_error("same-function integer Source missing");
  };
  auto reject_module = [&](hir::HirModule &module, const std::string &name,
                           const auto &change) {
    invalid(module, name, change);
    invalid_recomputed_published(module, name + " (recomputed ownership)",
                                 change);
  };
  auto prototype = [&](hir::HirModule &h, std::string_view name) {
    for (const auto &t : h.traits)
      for (const auto &p : t.methods)
        if (h.resolved->symbols[p.symbol.value].name == name)
          return p.symbol;
    throw std::runtime_error("trait prototype missing: " + std::string(name));
  };
  auto bool_type = [](hir::HirModule &h) {
    for (uint64_t id = 1; id <= h.types.size(); ++id)
      if (auto t = h.types.find(TypeId{id});
          t && t->kind == hir::TypeKind::Bool)
        return TypeId{id};
    throw std::runtime_error("bool type missing");
  };
  auto reject_both = [&](const std::string &name, const auto &change) {
    invalid(m, name, change);
    invalid_recomputed_published(m, name + " (recomputed ownership)",
                                 change);
  };

  reject_both("unbound prototype borrowed another method", [&](auto &h) {
    find_prototype_value(h, "tag").symbol = prototype(h, "echo");
  });
  reject_both("unbound concrete target used the other same-name impl",
              [&](auto &h) {
                auto &value = find_value(h, "first", "tag");
                auto current = value.symbol;
                SymbolId replacement{};
                for (const auto &s : h.resolved->symbols)
                  if (s.name == "tag" && s.id != current)
                    for (const auto &im : h.impls)
                      if (std::find(im.methods.begin(), im.methods.end(),
                                    s.id) != im.methods.end())
                        replacement = s.id;
                need(replacement.value, "other tag impl missing");
                value.symbol = replacement;
              });
  reject_both("unbound template instance bound/self", [&](auto &h) {
    for (auto &i : h.instances)
      if (h.resolved->symbols[i.origin.value].name == "first") {
        need(i.type_args.size() == 1, "first instance arity");
        i.type_args[0] = bool_type(h);
        return;
      }
    throw std::runtime_error("first instance missing");
  });
  reject_both("unbound impl qualifier argument in target instance",
              [&](auto &h) {
                for (auto &i : h.instances)
                  if (h.resolved->symbols[i.origin.value].name == "echo" &&
                      i.type_args.size() == 2) {
                    i.type_args[0] = bool_type(h);
                    return;
                  }
                throw std::runtime_error("echo instance missing");
              });
  reject_both("unbound explicit method suffix source identity",
              [&](auto &h) {
                hir::HirNode *first = nullptr;
                for (auto &n : h.nodes) {
                  auto value = std::get_if<hir::FunctionValue>(&n.data);
                  if (!value ||
                      h.resolved->symbols[value->symbol.value].name !=
                          "echo" ||
                      std::any_of(h.templates.begin(), h.templates.end(),
                                  [&](const auto &t) {
                                    return t.function.symbol ==
                                           node_owner(h, n);
                                  }))
                    continue;
                  if (!first) {
                    first = &n;
                    continue;
                  }
                  const auto source = first->source;
                  first->source = n.source;
                  n.source = source;
                  return;
                }
                throw std::runtime_error("two echo method values missing");
              });
  reject_both("unbound phantom instances not merged by signature",
              [&](auto &h) {
                auto &i64 =
                    find_node(h, find_value(h, "main", "phantom"));
                auto *i64_value = std::get_if<hir::FunctionValue>(&i64.data);
                need(i64_value, "phantom i64 value missing");
                SymbolId bool_instance{};
                for (const auto &n : h.nodes)
                  if (auto v = std::get_if<hir::FunctionValue>(&n.data);
                      v && h.resolved->symbols[v->symbol.value].name ==
                               "phantom" &&
                      v->symbol != i64_value->symbol) {
                    bool_instance = v->symbol;
                    break;
                  }
                need(bool_instance.value, "phantom bool instance missing");
                i64_value->symbol = bool_instance;
              });
  reject_both("unbound method value signature mode", [&](auto &h) {
    auto &node = find_node(h, find_value(h, "main", "get"));
    auto t = *h.types.find(node.type);
    t.parameters.front().mode = hir::ParamMode::move;
    node.type = h.types.intern(t);
  });
  reject_both("unbound method value throws signature", [&](auto &h) {
    auto &node = find_node(h, find_value(h, "main", "fail"));
    auto t = *h.types.find(node.type);
    need(t.throws_type.has_value(), "throws value type");
    t.throws_type.reset();
    node.type = h.types.intern(t);
  });
  reject_both("unbound prototype in concrete execution", [&](auto &h) {
    find_value(h, "main", "get").symbol = prototype(h, "tag");
  });
  reject_both("unbound concrete method Source replaced by integer",
              [&](auto &h) {
                auto &node = find_node(h, find_value(h, "main", "get"));
                node.source = literal_source(h, node);
              });
  reject_both("unbound concrete Source integer plus other same signature "
              "method",
              [&](auto &h) {
                auto &node = find_node(h, find_value(h, "main", "get"));
                auto &other = find_node(h, find_value(h, "first", "tag"));
                need(node.type == other.type,
                     "concrete integer bypass signature control");
                auto *value = std::get_if<hir::FunctionValue>(&node.data);
                auto *replacement =
                    std::get_if<hir::FunctionValue>(&other.data);
                need(value && replacement,
                     "concrete integer bypass payload");
                node.source = literal_source(h, node);
                value->symbol = replacement->symbol;
              });
  reject_both("unbound source postfix belongs to another method",
              [&](auto &h) {
                auto &from = find_node(h, find_value(h, "main", "get"));
                auto &to = find_node(h, find_value(h, "main", "make"));
                from.source = to.source;
              });
  const std::string unused = R"(
trait Choose[A, B] { func stamp[U](owner Self) i32; }
trait Alien { func alien() bool; }
func idle[T Choose[i32, bool]](owner T) i32 {
    let f func(T) i32 = T.stamp[bool];
    let marker i32 = 0;
    return f(owner);
}
func main() i32 { return 0; }
)";
  auto u = good(unused);
  reject_module(u, "unbound unused prototype Source replaced by integer",
                [&](auto &h) {
                  auto &node =
                      find_node(h, find_prototype_value(h, "stamp"));
                  node.source = literal_source(h, node);
                });
  reject_module(u, "unbound unused Source integer plus unrelated prototype",
                [&](auto &h) {
                  auto &node =
                      find_node(h, find_prototype_value(h, "stamp"));
                  node.source = literal_source(h, node);
                  auto *value =
                      std::get_if<hir::FunctionValue>(&node.data);
                  need(value, "unused prototype payload");
                  for (const auto &trait : h.traits)
                    if (h.resolved->symbols[trait.symbol.value].name ==
                        "Alien") {
                      value->symbol = trait.methods.front().symbol;
                      value->type_args.clear();
                      return;
                    }
                  throw std::runtime_error("Alien prototype missing");
                });
  std::cout << "unbound-method-values: concrete Type/impl-own, bounded T, "
               "storage identity and corrupt HIR passed\n";
}

void const_method_values() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto types = hir::verify_types(*r.value);
    need(types.success(), "const method value HIR types rejected\n" +
                              messages(types));
    auto published = hir::verify(*r.value);
    need(published.success(), "const method value HIR did not publish\n" +
                                  messages(published));
    return *r.value;
  };
  const std::string base = R"(
struct Host { value i32; }
struct Box[T] { value T; }
impl Host {
    func get(self Self) i32 { return self.value; }
    func other(self Self) i32 { return self.value + 1; }
}
impl[T] Box[T] {
    func echo[U](self Self, item U) U { return item; }
}
func free(x i32) i32 { return x; }
const GET func(Host) i32 = Host.get;
const GET_ALIAS func(Host) i32 = GET;
const ECHO func(Box[i32], bool) bool = Box[i32].echo[bool];
const FREE func(i32) i32 = free;
const FREE_ALIAS func(i32) i32 = FREE;
func main() i32 {
    let host = Host(value: 7);
    let f func(Host) i32 = GET_ALIAS;
    let g func(Box[i32], bool) bool = ECHO;
    let h func(i32) i32 = FREE_ALIAS;
    let box = Box[i32](value: 1);
    if f(host) != 7 || !g(box, true) || h(3) != 3 { return 1; }
    return 0;
}
)";
  auto m = good(base);
  size_t get_value = 0;
  bool found_get = false;
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (auto v = std::get_if<hir::FunctionValue>(&m.nodes[i].data);
        v && m.resolved->symbols[v->symbol.value].name == "get") {
      get_value = i;
      found_get = true;
    }
  need(found_get, "const method value use missing");
  SymbolId other{};
  for (const auto &s : m.resolved->symbols)
    if (s.name == "other")
      other = s.id;
  need(other.value, "const mismatch target missing");
  auto corrupt = [&](auto &h) {
    auto *value = &h.nodes[get_value];
    auto *fv = std::get_if<hir::FunctionValue>(&value->data);
    need(fv, "const method value payload missing");
    fv->symbol = other;
  };
  invalid(m, "const method value target differs from initializer",
          corrupt);
  invalid_recomputed_published(
      m, "const method value target differs from initializer "
         "(recomputed ownership)", corrupt);
  std::cout << "const-method-values: initializer alias Source identity and "
               "target corruption passed\n";
}

void local_const_aliases() {
  auto good = [](const std::string &text) {
    auto r = source(text);
    need(r.success(), text + "\n" + messages(r));
    auto types = hir::verify_types(*r.value);
    need(types.success(), "local const alias HIR types rejected\n" +
                              messages(types));
    auto published = hir::verify(*r.value);
    need(published.success(), "local const alias HIR did not publish\n" +
                                  messages(published));
    return *r.value;
  };
  const std::string base = R"(
struct Host { value i32; }
struct Box[T] { value T; }
impl Host {
    func get(self Self) i32 { return self.value; }
    func other(self Self) i32 { return self.value + 1; }
}
impl[T] Box[T] {
    func phantom[U](self Self) T { return self.value; }
}
func free(x i32) i32 { return x; }
func generic[U](x U) U { return x; }
func relay[U](x U) U {
    const ID func(U) U = generic[U];
    if true { const N func(U) U = ID; return N(x); }
    return x;
}
func unused[U](x U) U {
    const ID func(U) U = generic[U];
    const N func(U) U = ID;
    return N(x);
}
func main() i32 {
    let runtime func(Host) i32 = Host.get;
    const F func(Host) i32 = Host.get;
    const OTHER func(Host) i32 = Host.other;
    const G func(Host) i32 = F;
    const OTHER2 func(Host) i32 = OTHER;
    const PAREN func(Host) i32 = (Host.get);
    const PAREN2 func(Host) i32 = (PAREN);
    const FREE func(i32) i32 = free;
    const FREE2 = FREE;
    const PH func(Box[i32]) i32 = Box[i32].phantom[bool];
    const PHI func(Box[i32]) i32 = Box[i32].phantom[i64];
    const P1 = PH;
    const P2 = PHI;
    if true {
        const H func(Host) i32 = G;
        if true {
            const K func(Host) i32 = H;
            let h = Host(value: 7);
            let box = Box[i32](value: 3);
            if K(h) != 7 || OTHER2(h) != 8 || FREE2(3) != 3 { return 1; }
            if P1(box) != 3 || P2(box) != 3 { return 2; }
            if relay[i32](9) != 9 { return 3; }
            return 0;
        }
    }
    return 4;
}
)";
  auto m = good(base);
  auto binding = [](hir::HirModule &h, std::string_view name)
      -> hir::HirNode & {
    hir::HirNode *found = nullptr;
    for (auto &n : h.nodes)
      if (auto b = std::get_if<hir::Binding>(&n.data);
          b && h.resolved->symbols[b->symbol.value].name == name) {
        need(!found, "duplicate local const alias binding: " +
                          std::string(name));
        found = &n;
      }
    need(found, "local const alias binding missing: " + std::string(name));
    return *found;
  };
  auto symbol = [](hir::HirModule &h, std::string_view name) {
    for (const auto &s : h.resolved->symbols)
      if (s.name == name)
        return s.id;
    throw std::runtime_error("symbol missing: " + std::string(name));
  };
  auto reject_both = [&](const std::string &name, const auto &change) {
    invalid(m, name, change);
    invalid_recomputed_published(m, name + " (recomputed ownership)",
                                 change);
  };

  reject_both("local const alias read used another same-signature const",
              [&](auto &h) {
                auto &g = binding(h, "G");
                auto *read =
                    std::get_if<hir::Read>(&h.nodes[g.children[0].value].data);
                need(read, "local alias initializer is not a Read");
                read->symbol = symbol(h, "OTHER");
              });
  reject_both("local const alias read used a same-signature let",
              [&](auto &h) {
                auto &g = binding(h, "G");
                auto *read =
                    std::get_if<hir::Read>(&h.nodes[g.children[0].value].data);
                need(read, "local alias initializer is not a Read");
                read->symbol = symbol(h, "runtime");
              });
  reject_both("local const alias bindings exchanged their initializers",
              [&](auto &h) {
                auto &g = binding(h, "G");
                auto &other2 = binding(h, "OTHER2");
                std::swap(g.children[0], other2.children[0]);
              });
  reject_both("local const alias read Source replaced by another real name",
              [&](auto &h) {
                auto &g = binding(h, "G");
                auto &other2 = binding(h, "OTHER2");
                h.nodes[g.children[0].value].source =
                    h.nodes[other2.children[0].value].source;
              });
  reject_both("local const alias binding Source replaced by another real binding",
              [&](auto &h) {
                auto &g = binding(h, "G");
                auto &other = binding(h, "OTHER");
                g.source = other.source;
              });
  reject_both("local const alias materialized a direct FunctionValue",
              [&](auto &h) {
                auto &f = binding(h, "F");
                auto &g = binding(h, "G");
                auto *direct = std::get_if<hir::FunctionValue>(
                    &h.nodes[f.children[0].value].data);
                need(direct, "direct const function initializer missing");
                auto &child = h.nodes[g.children[0].value];
                child.data = *direct;
              });
  reject_both("local const alias marked writable", [&](auto &h) {
    auto &g = binding(h, "G");
    std::get<hir::Binding>(g.data).writable = true;
  });
  reject_both("local phantom alias target merged by signature",
              [&](auto &h) {
                std::vector<hir::HirNode *> values;
                for (auto &n : h.nodes)
                  if (auto v = std::get_if<hir::FunctionValue>(&n.data);
                      v && h.resolved->symbols[v->symbol.value].name ==
                               "phantom")
                    values.push_back(&n);
                need(values.size() == 2,
                     "two local phantom const function values missing");
                auto *first = std::get_if<hir::FunctionValue>(
                    &values[0]->data);
                auto *second = std::get_if<hir::FunctionValue>(
                    &values[1]->data);
                need(first && second && first->symbol != second->symbol,
                     "phantom const instances were merged");
                first->symbol = second->symbol;
              });
  reject_both("local const alias instance read kept the template symbol",
              [&](auto &h) {
                for (const auto &instance : h.instances) {
                  if (h.resolved->symbols[instance.origin.value].name !=
                      "relay")
                    continue;
                  for (auto &n : h.nodes) {
                    if (h.scopes[n.scope.value].function != instance.symbol)
                      continue;
                    auto *read = std::get_if<hir::Read>(&n.data);
                    if (!read)
                      continue;
                    for (auto [from, to] : instance.renamed)
                      if (to == read->symbol &&
                          h.resolved->symbols[from.value].name == "ID") {
                        read->symbol = from;
                        return;
                      }
                  }
                }
                throw std::runtime_error(
                    "instance local const alias read missing");
              });
  const std::string call_base = R"(
func free(x i32) i32 { return x; }
func other(x i32) i32 { return x + 1; }
func call_alias_let() i32 {
    const F func(i32) i32 = free;
    const A func(i32) i32 = F;
    let G func(i32) i32 = other;
    if G(3) != 4 { return 9; }
    return A(3) - 3;
}
func call_alias_param(param func(i32) i32) i32 {
    const F func(i32) i32 = free;
    const A func(i32) i32 = F;
    return A(3) - 3;
}
func call_shadow_let() i32 {
    const F func(i32) i32 = free;
    if true {
        let F func(i32) i32 = other;
        if F(3) != 4 { return 9; }
    }
    return F(3) - 3;
}
func call_shadow_param(F func(i32) i32) i32 {
    if true {
        const F func(i32) i32 = free;
        if F(3) != 3 { return 9; }
    }
    return F(3) - 4;
}
func main() i32 {
    if call_alias_let() != 0 { return 1; }
    if call_alias_param(other) != 0 { return 2; }
    if call_shadow_let() != 0 { return 3; }
    if call_shadow_param(other) != 0 { return 4; }
    return 0;
}
)";
  auto call_module = good(call_base);
  auto function_symbol = [](hir::HirModule &h, std::string_view name) {
    std::optional<SymbolId> found;
    for (const auto &s : h.resolved->symbols)
      if (s.name == name && s.kind == modules::SymbolKind::function) {
        need(!found, "duplicate function symbol: " + std::string(name));
        found = s.id;
      }
    need(found.has_value(), "function symbol missing: " + std::string(name));
    return *found;
  };
  auto local_symbol = [&](hir::HirModule &h, SymbolId owner,
                          std::string_view name) {
    std::optional<SymbolId> found;
    for (const auto &n : h.nodes)
      if (auto b = std::get_if<hir::Binding>(&n.data);
          b && h.scopes[n.scope.value].function == owner &&
          h.resolved->symbols[b->symbol.value].name == name) {
        need(!found, "duplicate local symbol: " + std::string(name));
        found = b->symbol;
      }
    need(found.has_value(), "local symbol missing: " + std::string(name));
    return *found;
  };
  auto parameter_symbol = [&](hir::HirModule &h, SymbolId owner,
                              std::string_view label) {
    for (const auto &f : h.functions)
      if (f.symbol == owner)
        for (const auto &p : f.parameters)
          if (p.label == label)
            return p.symbol;
    throw std::runtime_error("parameter symbol missing: " +
                             std::string(label));
  };
  auto call_callee = [&](hir::HirModule &h, std::string_view owner_name)
      -> hir::HirNode & {
    const auto owner = function_symbol(h, owner_name);
    hir::HirNode *found = nullptr;
    for (auto &n : h.nodes) {
      if (h.scopes[n.scope.value].function != owner)
        continue;
      auto *call = std::get_if<hir::Call>(&n.data);
      if (!call || call->kind != hir::CalleeKind::function_value ||
          !call->callee)
        continue;
      auto *read = std::get_if<hir::Read>(&h.nodes[call->callee->value].data);
      if (!read || h.resolved->symbols[read->symbol.value].name != "A")
        continue;
      need(!found, "duplicate A call callee");
      found = &h.nodes[call->callee->value];
    }
    need(found, "indirect call callee missing in " + std::string(owner_name));
    return *found;
  };
  auto read_source = [&](hir::HirModule &h, SymbolId owner,
                         SymbolId symbol) {
    for (const auto &n : h.nodes) {
      if (h.scopes[n.scope.value].function != owner)
        continue;
      if (auto *read = std::get_if<hir::Read>(&n.data);
          read && read->symbol == symbol)
        return n.source;
    }
    throw std::runtime_error("real function variable Read Source missing");
  };
  auto reject_call = [&](const std::string &name, const auto &change) {
    invalid(call_module, name, change);
    invalid_recomputed_published(
        call_module, name + " (recomputed ownership)", change);
  };

  reject_call("const call callee read used a same-signature let",
              [&](auto &h) {
                const auto owner = function_symbol(h, "call_alias_let");
                auto *read = std::get_if<hir::Read>(
                    &call_callee(h, "call_alias_let").data);
                need(read, "const call callee is not a Read");
                read->symbol = local_symbol(h, owner, "G");
              });
  reject_call("const call callee read used a same-signature parameter",
              [&](auto &h) {
                const auto owner = function_symbol(h, "call_alias_param");
                auto *read = std::get_if<hir::Read>(
                    &call_callee(h, "call_alias_param").data);
                need(read, "const call callee is not a Read");
                read->symbol = parameter_symbol(h, owner, "param");
              });
  reject_call("const call callee Source replaced by another real Name",
              [&](auto &h) {
                const auto owner = function_symbol(h, "call_alias_let");
                const auto g = local_symbol(h, owner, "G");
                call_callee(h, "call_alias_let").source =
                    read_source(h, owner, g);
              });
  reject_call("const call callee payload and Source replaced together",
              [&](auto &h) {
                const auto owner = function_symbol(h, "call_alias_let");
                const auto g = local_symbol(h, owner, "G");
                const auto g_source = read_source(h, owner, g);
                auto &callee = call_callee(h, "call_alias_let");
                auto *read = std::get_if<hir::Read>(&callee.data);
                need(read, "const call callee is not a Read");
                read->symbol = g;
                callee.source = g_source;
              });
  std::cout << "local-const-aliases: real Read Source identity, local-free "
               "identities, phantom instances, instance renaming, const call "
               "callee identity and corrupt HIR passed\n";
}

int main(int argc, char **argv) {
  try {
    need(argc == 2 || (argc == 3 && std::string(argv[2]) == "--repairs"),
         "root argument required");
    fs::path root = argv[1];
    if (argc == 3) {
      repairs(root);
      return 0;
    }
    auto path = root / "tests/generics/cases.json";
    auto raw = json_object_from_file(path.c_str());
    need(raw, "read cases.json");
    std::unique_ptr<json_object, decltype(&json_object_put)> owner(
        raw, json_object_put);
    auto cases = json_object_object_get(raw, "cases");
    size_t failures = 0;
    for (size_t i = 0; i < json_object_array_length(cases); ++i) {
      auto c = json_object_array_get_idx(cases, i);
      std::string id = json_object_get_string(json_object_object_get(c, "id"));
      std::string manifest =
          json_object_get_string(json_object_object_get(c, "manifest"));
      auto expected = json_object_object_get(c, "expected_static");
      bool accept = std::string(json_object_get_string(json_object_object_get(
                        expected, "status"))) == "accept";
      auto r = check(root / manifest);
      bool good = r.success() == accept &&
                  (accept || (r.failure == FailureKind::source &&
                              !r.diagnostics.empty()));
      std::cout << id << ": " << (good ? "passed" : "FAILED") << "\n";
      if (!good) {
        std::cout << messages(r);
        ++failures;
      }
      if (id == "expanding-recursion")
        need(!r.success() &&
                 messages(r).find("E_INSTANTIATION_LIMIT") != std::string::npos,
             "expansion did not stop at its dedicated budget diagnostic: " +
                 messages(r));
      if (r.success()) {
        auto verified = hir::verify_types(*r.value);
        need(verified.success(), messages(verified));
        if (id == "cross-package-alias")
          need(r.value->instances.size() == 1,
               "alias failed instance deduplication");
      }
    }
    need(!failures, std::to_string(failures) + " fixture failures");
    std::cout << "27 T07 static source fixtures passed\n";
    extra(root);
    generic_move(root);
    constrained_free_move(root);
    constrained_free_inout(root);
    generic_inout(root);
    method_inout(root);
    method_move(root);
    generic_method_move(root);
    generic_impl_move();
    generic_impl_own_method_move(root);
    constrained_inherent_move(root);
    constrained_inherent_inout(root);
    trait_method_modes(root);
    trait_self_inout(root);
    trait_self_callback(root);
    trait_static_calls();
    trait_method_generics(root);
    trait_generic_move(root);
    trait_method_scalar_inout(root);
    inherent_static_self_identity();
    trait_static_self_qualification();
    nested_patterns();
    aggregate_inout_validation();
    generic_trait_method_generics();
    generic_trait_applications();
    generic_trait_method_modes(root);
    ordinary_trait_receiver(root);
    indexed_function_calls();
    trait_parameter_bounds(root);
    builtin_impl_targets();
    explicit_self_modes();
    unbound_method_values();
    const_method_values();
    local_const_aliases();
    dependent_callback_completion(root);
    repairs(root);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}

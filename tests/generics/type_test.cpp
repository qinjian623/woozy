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
  unsupported("func relay[T](value move T)T{return move value;} "
              "func main(){let r=relay[string](move \"x\");}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("func relay[T](value move T)T{return move value;} "
              "func main(){let r=relay[string](\"x\");}",
              "E_CAPABILITY_UNAVAILABLE");

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
      if (im.trait == trait.symbol && nominal_name(im.self_type) == "Item")
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
        need(c->trait == value_trait->symbol, "read template trait changed");
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
      if (im.trait != trait.symbol)
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
        need(c->trait == value_trait->symbol,
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
  unsupported("trait V{func get(self Self)i32;} struct S{x i32;} "
              "impl V for S{func get(self Self)i32{return self.x;}} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var x S=S(x:1);var y S=S(x:2);"
              "swap(inout x,inout y);}");
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
  unsupported("trait V{func get(self Self)i32;} struct S{x i32;} "
              "impl V for S{func get(self Self)i32{return self.x;}} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var x S=S(x:1);var y S=S(x:2);let f=swap[S];"
              "f(inout x,inout y);}");
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
  unsupported("struct S { x i32; } " + swap +
                  "func main(){var s S=S(x:1);var t S=S(x:2);"
                  "swap(inout s,inout t);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("enum E { A; } " + swap +
                  "func main(){var x E=E.A;var y E=E.A;"
                  "swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
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
  unsupported("impl i32{func bump(self inout i32,x inout i32){x+=1;}} "
              "func main(){var x i32=1;i32.bump(inout x,inout x);}",
              "E_CAPABILITY_UNAVAILABLE");

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
  unsupported_span("struct S[T]{f func() S[S[T]];cb func(inout T);} "
                   "func take(value S[S[i32]])i32{return 0;} "
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
      need(!owned.success() &&
               owned.failure == FailureKind::unsupported &&
               has(owned, "E_CAPABILITY_UNAVAILABLE"),
           "owned aggregate backend refusal is not E_CAP");
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
  unsupported(
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
  unsupported(
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
  unsupported(
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
  unsupported(prefix + "func main()i32{let h=H(items:[1],marker:0);"
                       "var v Vec[i32]=[1];"
                       "let r=H.take(h,move v);return r[0];}");
  unsupported(
      "struct H{marker i32;} impl H{"
      "func take[T](self move Self,value move T)T{return move value;}} "
      "func main()i32{let h=H(marker:0);var v Vec[i32]=[1];"
      "let r=h.take(move v);return r[0];}");
  unsupported(
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
  unsupported(box + "impl[T] Box[T]{"
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
  unsupported(box + "impl[T] Box[T]{"
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
  unsupported(box + "impl[T] Box[T]{"
                    "func bump[U](self Self,x inout U){x=x;}} "
                    "func main()i32{let b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;"
                    "Box[i32].bump[i32](b,inout x);return x-2;}");
  unsupported(box + "impl[T] Box[T]{"
                    "func take[U](self move Self,value move U)U{"
                    "return move value;}} "
                    "func main()i32{var b=Box[i32](value:1,items:[1],marker:0);"
                    "var x i32=1;let r=b.take(move x);return r-1;}");
  unsupported(box + "impl[T] Box[T]{"
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
  unsupported(box + "impl[T] Box[T]{"
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
      if (im.trait == trait.symbol && nominal_name(im.self_type) == type)
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
           symbol_name(*box_impl->params[0].trait) == "Value",
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
           symbol_name(*box_take->params[1].trait) == "Value" &&
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
        need(c->trait == value_trait->symbol,
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
      if (im.trait == trait.symbol && nominal_name(im.self_type) == type)
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
           symbol_name(*box_impl->params[0].trait) == "Value",
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
           symbol_name(*box_replace->params[1].trait) == "Value" &&
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
        need(c->trait == value_trait->symbol,
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
      if (im.trait == trait && nominal_name(m, im.self_type) == type)
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
  need(take_prototype && move_cc->trait == take->symbol &&
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
  need(inout_cc->trait == adjust->symbol &&
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
          c->trait = right->symbol;
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
  unsupported("trait V{func take[U](self Self,value move U)U;}"
              "struct S{items Vec[i32];} impl V for S{"
              "func take[U](self Self,value move U)U{return move value;}}"
              "func main(){let s=S(items:[1]);var x i32=2;"
              "let r=s.take(move x);}");
  unsupported("trait V{func make(value move i32)i32;}"
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
    need(cc->trait == lg_trait->symbol &&
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

  // The narrow new-mode rule still rejects a non-self move/inout prototype
  // whose first Self parameter is named owner instead of self.
  auto owner_new_mode = source(
      "trait V{func ordinary(owner Self,value move Vec[i32])Vec[i32];}"
      "struct S{x i32;} impl V for S{"
      "func ordinary(owner Self,value move Vec[i32])Vec[i32]{"
      "return move value;}} "
      "func invoke[T V](x T,value move Vec[i32])Vec[i32]{"
      "return x.ordinary(move value);} "
      "func main()i32{var v Vec[i32]=[1];let r=invoke(S(x:1),move v);"
      "return r[0]-1;}");
  need(!owner_new_mode.success() &&
           owner_new_mode.failure == FailureKind::unsupported &&
           has(owner_new_mode, "E_CAPABILITY_UNAVAILABLE"),
       "owner-named new move mode was accepted");
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
      if (im.trait == trait && type_name(m, im.self_type) == type)
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
  need(fail_cc->trait == flow->symbol &&
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

  unsupported("trait V{func bad(self Self,cb func(inout Self));} "
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
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "struct S{x i32;} impl V for S{"
              "func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "enum E{One;} impl V for E{"
              "func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
  unsupported("trait V{func set(self Self,slot inout Self);} "
              "struct Box[T]{value T;} impl[T] V for Box[T]{"
              "func set(self Self,slot inout Self){}} "
              "func main()i32{return 0;}");
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
    c.trait = other_trait->symbol;
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
    repairs(root);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}

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
  unsupported(
      "impl i32{func take[T](self Self,value move T)T{return move value;}} "
      "func main() i32{let x i32=1;let v Vec[i32]=[2];let r=x.take(move v);"
      "return r[0];}",
      "E_CAPABILITY_UNAVAILABLE");
  const std::string static_move =
      "impl i32{func take[T](value move T)T{return move value;}} ";
  unsupported(static_move +
                  "func main() i32{let values Vec[i32]=[7];"
                  "let result=i32.take(move values);return result[0];}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported(static_move +
                  "func main() i32{let values Vec[i32]=[7];"
                  "let result=i32.take[Vec[i32]](move values);"
                  "return result[0];}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("struct Box[T]{value T;} impl[T] Box[T]{"
              "func take[U](value move U)U{return move value;}} "
              "func main() i32{let values Vec[i32]=[7];"
              "let result=Box[i32].take(move values);return result[0];}",
              "E_CAPABILITY_UNAVAILABLE");
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
  unsupported("trait V{} impl V for i32{} "
              "func take[T V](value move T)T{return move value;} "
              "func main() i32{var x i32=1;let r=take(move x);return r;}",
              "E_CAPABILITY_UNAVAILABLE");
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
  unsupported("trait V{} impl V for i32{} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var x i32=1;var y i32=2;"
              "swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("trait V{} impl V for i32{} "
              "func swap[T V](a inout T,b inout T){} "
              "func main(){var x i32=1;var y i32=2;"
              "let f=swap[i32];f(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("impl i32{func swap[T](a inout T,b inout T){} } "
              "func main(){var x i32=1;var y i32=2;"
              "i32.swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("struct Box[T]{value T;} impl[T] Box[T]{"
              "func swap[U](a inout U,b inout U){} } "
              "func main(){var x i32=1;var y i32=2;"
              "Box[i32].swap(inout x,inout y);}",
              "E_CAPABILITY_UNAVAILABLE");
  unsupported("impl i32{func bump(self Self,x inout i32){x+=1;}} "
              "func main(){var x i32=1;x.bump(inout x);}",
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
    generic_inout(root);
    repairs(root);
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}

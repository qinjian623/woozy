#include "lang/sema/check.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <map>
#include <stdexcept>
using namespace lang;
namespace fs = std::filesystem;
namespace {
size_t checked = 0;
void require(bool b, const std::string &message) {
  if (!b)
    throw std::runtime_error(message);
}
bool has(const auto &r, std::string_view code) {
  for (const auto &d : r.diagnostics)
    if (d.code == code)
      return true;
  return false;
}
std::string messages(const auto &r) {
  std::string s;
  for (const auto &d : r.diagnostics)
    s += d.code + ": " + d.message + "\n";
  return s;
}
struct Temp {
  fs::path path;
  Temp() {
    char p[] = "/tmp/lang-t04-XXXXXX";
    auto result = mkdtemp(p);
    if (!result)
      throw std::runtime_error("mkdtemp failed");
    path = result;
    fs::create_directories(path / "src");
  }
  ~Temp() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};
void write(const fs::path &p, const std::string &s) {
  std::ofstream out(p);
  out << s;
  require(bool(out), "fixture write failed");
}
void manifest(const fs::path &root, std::string backend, unsigned width) {
  write(root / "lang.toml",
        "schema_version = 1\n[package]\nname = \"test\"\nsource_root = "
        "\"src\"\nentry = \"app\"\n[build]\nbackend = \"" +
            backend + "\"\nword_bits = " + std::to_string(width) + "\n");
}
auto resolve(const fs::path &manifest, SourceManager &sources,
             std::string_view backend = {}) {
  auto p = modules::load_project({manifest, {}, {}}, sources);
  require(p.success(), "load prerequisite failed: " + messages(p));
  if (!backend.empty()) p.value->backend = backend;
  auto r = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*p.value)));
  require(r.success(), "resolve prerequisite failed: " + messages(r));
  return r;
}
auto check(const fs::path &manifest, std::string_view backend = {}) {
  SourceManager sources;
  auto r = resolve(manifest, sources, backend);
  auto result = sema::check_project(r);
  ++checked;
  return result;
}
auto source(const std::string &body, const char *error = nullptr,
            std::string backend = "native", unsigned width = 64) {
  Temp t;
  manifest(t.path, backend, width);
  write(t.path / "src/main.lang", "package app; " + body);
  auto r = [&] {
    try {
      return check(t.path / "lang.toml");
    } catch (const std::exception &e) {
      throw std::runtime_error(body + "\n" + e.what());
    }
  }();
  if (error) {
    require(!r.success() && !r.value && has(r, error),
            "expected " + std::string(error) + ": " + body + "\n" +
                messages(r));
    require(r.failure == (std::string_view(error) == "E_CAPABILITY_UNAVAILABLE"
                              ? FailureKind::unsupported
                              : FailureKind::source),
            "failure category");
    require(r.exit_code() == ExitCode::source_error,
            "source/unsupported exit code");
    require(r.diagnostics[0].primary.has_value(),
            "diagnostic source span missing");
    require(r.diagnostics[0].phase == "type",
            "diagnostic phase not in the shared schema");
  } else
    require(r.success(), "source rejected: " + body + "\n" + messages(r));
  return r;
}
auto typed_source(const std::string &body, const char *error = nullptr,
                  std::string backend = "native", unsigned width = 64) {
  Temp t;
  manifest(t.path, backend, width);
  write(t.path / "src/main.lang", "package app; " + body);
  auto r = [&] {
    try {
      SourceManager sources;
      auto result = sema::check_types(resolve(t.path / "lang.toml", sources));
      ++checked;
      return result;
    } catch (const std::exception &e) {
      throw std::runtime_error(body + "\n" + e.what());
    }
  }();
  if (error) {
    require(!r.success() && !r.value && has(r, error),
            "expected " + std::string(error) + ": " + body + "\n" +
                messages(r));
    require(r.failure == (std::string_view(error) == "E_CAPABILITY_UNAVAILABLE"
                              ? FailureKind::unsupported
                              : FailureKind::source),
            "failure category");
    require(r.exit_code() == ExitCode::source_error,
            "source/unsupported exit code");
    require(r.diagnostics[0].primary.has_value(),
            "diagnostic source span missing");
    require(r.diagnostics[0].phase == "type",
            "diagnostic phase not in the shared schema");
  } else
    require(r.success(), "source rejected: " + body + "\n" + messages(r));
  return r;
}
void independent(const fs::path &published) {
  auto path = published / "tests/sema/cases.json";
  json_object *raw = json_object_from_file(path.c_str());
  require(raw, "read independent cases.json");
  std::unique_ptr<json_object, decltype(&json_object_put)> owner(
      raw, json_object_put);
  json_object *cases = nullptr;
  require(json_object_object_get_ex(raw, "cases", &cases), "cases array");
  auto stage_path = published / "tests/sema/t07-expectations.json";
  std::unique_ptr<json_object, decltype(&json_object_put)> stage(
      json_object_from_file(stage_path.c_str()), json_object_put);
  require(bool(stage), "read T07 stage expectations");
  auto overrides = json_object_object_get(stage.get(), "overrides");
  for (size_t i = 0; i < json_object_array_length(cases); ++i) {
    auto c = json_object_array_get_idx(cases, i);
    auto get = [](json_object *v, const char *key) {
      return json_object_object_get(v, key);
    };
    std::string id = json_object_get_string(get(c, "id"));
    std::string expected =
        json_object_get_string(get(get(c, "expected"), "status"));
    if (auto override = json_object_object_get(overrides, id.c_str()))
      expected = json_object_get_string(get(override, "status"));
    auto p = published / json_object_get_string(get(c, "manifest"));
    auto r = check(p);
    if (id == "throws-unavailable" || id == "vec-unavailable") expected = "accept";
    require(r.success() == (expected == "accept"),
            "independent " + id + "\n" + messages(r));
    if (expected == "unavailable")
      require(has(r, "E_CAPABILITY_UNAVAILABLE") &&
                  r.failure == FailureKind::unsupported,
              id);
    if (expected == "reject")
      require(r.failure == FailureKind::source && !has(r, "E_IR_INVALID"),
              id + " was not a source rejection\n" + messages(r));
    std::cout << "fixture " << id << ": passed\n";
  }
}
void suite(const fs::path &published) {
  independent(published);
  {
    auto path = published / "tests/backend_common/cases.json";
    std::unique_ptr<json_object, decltype(&json_object_put)> cases(
        json_object_from_file(path.c_str()), json_object_put);
    require(bool(cases), "read backend-common cases");
    auto items = json_object_object_get(cases.get(), "cases");
    require(items && json_object_is_type(items, json_type_array), "backend cases array");
    for (size_t i = 0; i < json_object_array_length(items); ++i) {
      auto item = json_object_array_get_idx(items, i);
      auto manifest = published / json_object_get_string(json_object_object_get(item, "manifest"));
      for (const char *backend : {"native", "python"}) {
        auto r = check(manifest, backend);
        require(r.success(), manifest.string() + " " + backend + "\n" + messages(r));
      }
    }
  }
  for (const char *name : {"core", "numeric"})
    for (const char *backend : {"native", "python"}) {
      Temp t;
      manifest(t.path, backend, 64);
      fs::copy_file(published / (std::string("examples/") + name + ".lang"),
                    t.path / "src/main.lang");
      auto r = check(t.path / "lang.toml");
      require(r.success(),
              std::string(name) + " " + backend + "\n" + messages(r));
      require(r.value->version() == "compiler-dev/1" &&
                  r.value->config().word_bits == 64,
              "verified target/version");
      for (const auto &n : r.value->module().nodes) {
        require(r.value->module().types.find(n.type) &&
                    n.kind() != hir::NodeKind::unsupported,
                "untyped/unsupported verified node");
      }
      std::cout << name << " " << backend << "64: passed (static HIR only)\n";
    }
  for (const char *backend : {"native", "python"})
    for (unsigned w : {32u, 64u}) {
      source(
          "func f() { let x isize = -2147483648; let y usize = 4294967295; }",
          nullptr, backend, w);
      source("func f() { let x isize = 2147483648; }",
             w == 32 ? "E_TYPE_MISMATCH" : nullptr, backend, w);
      source("func f() { let x usize = 4294967296; }",
             w == 32 ? "E_TYPE_MISMATCH" : nullptr, backend, w);
    }
  source("func f() { let a u64 = 18446744073709551615; let b i64 = "
         "-9223372036854775808; let c = 0xffff_ffff_ffff_ffff; }",
         "E_TYPE_MISMATCH");
  source("func f() { let a u64 = 18446744073709551615; let b i64 = "
         "-9223372036854775808; let c u8 = 0xff; let d u8 = 0b11111111; let e "
         "u8 = 0o377; }");
  source("func f() { let a u64 = 18446744073709551616; }", "E_TYPE_MISMATCH");
  source("func f() { let a = "
         "999999999999999999999999999999999999999999999999999999999999999999999"
         "999; }",
         "E_TYPE_MISMATCH");
  source("func f() { let a i64 = -9223372036854775809; }", "E_TYPE_MISMATCH");
  source("func f() { let a u8 = -1; }", "E_TYPE_MISMATCH");
  source("func f() { let a u8=-(-1); }", "E_TYPE_MISMATCH");
  source("func f() { let a u8=-(-1+0); }", "E_TYPE_MISMATCH");
  source("func f() { let a i8=-(-128); }");
  source("func f() { let a i8=-(-128+0); }");
  source("func f() { let a u8=1+~2; }");
  source("func f() { let a u8=~2+1; }");
  {
    auto r = source("const A u8=1+~2; const B u8=~2+1; const C i8=-(-128); "
                    "func f() {let a=A; let b=B; let c=C;}");
    size_t observed = 0;
    for (const auto &n : r.value->module().nodes)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        const auto &m = r.value->module();
        auto name = m.resolved->symbols[b->symbol.value].name;
        auto literal = std::get_if<hir::IntegerLiteral>(&m.nodes[n.children[0].value].data);
        require(literal && literal->decimal == (name == "c" ? "-128" : "254"),
                "unary integer context or wrap was lost");
        ++observed;
      }
    require(observed == 3, "missing folded unary constants");
  }
  source("func f() { let a i8 = -128; let b = -a; let c = ~a; let d = +a; }");
  source("func f(a i32,b i64) { let x=a+b; }", "E_TYPE_MISMATCH");
  source("func f(a i32,b u32) { let x=a+b; }", "E_TYPE_MISMATCH");
  source("func f(a i32) i32 { return 1+a; }");
  source("func f(a bool) { let x=a+true; }", "E_TYPE_MISMATCH");
  source("func f() { let x = !1; }", "E_TYPE_MISMATCH");
  source("func f() i32 { return; }", "E_TYPE_MISMATCH");
  source("func f() { return 1; }", "E_TYPE_MISMATCH");
  source("func f(a bool) i32 { if a { return 1; } else if !a { return 2; } "
         "else { return 3; } }");
  source("func f() i32 { if true { return 1; } }");
  source("func f() i32 { if false {} else { return 1; } }");
  source("func f() i32 { if false { return 1; } }", "E_TYPE_MISMATCH");
  source("const G bool=true; func f() i32 { if G {return 1;} }");
  source("func f() i32 { const L bool=true; if L {return 1;} }");
  source("func f() i32 { const L bool=true; for L {} }");
  source("func f() i32 { const L bool=false; if L {return 1;} }", "E_TYPE_MISMATCH");
  source("func f() i32 { const L bool=false; for L {} }", "E_TYPE_MISMATCH");
  source("func f() i32 { for {} }");
  source("func f() i32 { for { continue; break; } }");
  source("func f() i32 { for { break; } }", "E_TYPE_MISMATCH");
  source("func f() { break; }", "E_TYPE_MISMATCH");
  source("func f() { continue; }", "E_TYPE_MISMATCH");
  source("func f() { var i i32=0; for i<4 { i+=1; if i==2 { continue; } for { "
         "break; } } }");
  source("func f() { for ; ; {} }");
  source("func f() { for ; false; {} }");
  source("func g() {} func f() { for g(); false; g() {} }");
  source("func f() { for var i=0; ; i+=1 { break; } }");
  source("func f() { for ; 1; {} }", "E_TYPE_MISMATCH");
  source("func f() { let x i32=1; { let x=x+1; } }");
  source("func f() { const x i32=1; x=2; }", "E_TYPE_MISMATCH");
  source("func f(a i32) { a=2; }", "E_CAPABILITY_UNAVAILABLE");
  source("func f() { var x i32=1; x=true; }", "E_TYPE_MISMATCH");
  source("func f() { var x i32=1; let y u8=2; x<<=y; x>>=y; }");
  source("func f() { let x i32=1; let y u8=2; let z=x<<y; }");
  source("func f() { let x=true<<1; }", "E_TYPE_MISMATCH");
  source("func a(x inout i32) { x+=1; } func f() { var x i32=0; a(inout x); }");
  source(
      "func a(x inout bool) { x=!x; } func f() { var x=false; a(inout x); }");
  source("func a(x inout i32) { x=1; } func b(x inout i32) { a(inout x); }");
  source("func a(x inout i32) {} func f() { var x i32=0; a(x); }",
         "E_CALL_ARGUMENT");
  source("func a(x i32) {} func f() { var x i32=0; a(inout x); }",
         "E_CALL_ARGUMENT");
  source("func a(x inout i32,y inout i32) {} func f() { var x i32=0; a(inout "
         "x,inout x); }",
         "E_CALL_ARGUMENT");
  source(
      "func a(x inout i32,y i32) {} func f() { var x i32=0; a(inout x,x+1); }",
      "E_CALL_ARGUMENT");
  source("func a(x i32,y inout i32) {} func f() { var x i32=0; a(x,inout x); }",
         "E_CALL_ARGUMENT");
  source("func a(x inout i32,y inout i32) {} func f() { var x i32=0; var y "
         "i32=0; a(inout x,inout y); }");
  source("func a(x inout i32) {} func f() { let x i32=0; a(inout x); }",
         "E_CALL_ARGUMENT");
  source("func a(x inout i32) {} func f() { const x i32=0; a(inout x); }",
         "E_CALL_ARGUMENT");
  source("func a(x inout i32) {} func f() { var x i64=0; a(inout x); }",
         "E_TYPE_MISMATCH");
  source("func a(x inout i32) {} func f() { var x i32=0; let op= a; op(inout "
         "x); }");
  source("func a(x inout i32) {} func f(x i32) { a(inout x); }",
         "E_CALL_ARGUMENT");
  source("func a(x i32) i32 { return x; } func choose() (func(i32) i32) { "
         "return a; } func f() i32 { return choose()(2); }");
  source("func a(x i32) {} func f() { let op=a; op(x:1); }", "E_CALL_ARGUMENT");
  {
    auto r = source("func ignore(_ i32,_ i32) {} func f() {ignore(1,2);}");
    auto m = r.value->module();
    require(m.functions[0].parameters.size() == 2 &&
                m.functions[0].parameters[0].label.empty() &&
                m.functions[0].parameters[1].label.empty(),
            "discard parameters became named arguments");
    m.functions[0].parameters[0].label = "_";
    auto invalid = hir::verify(m);
    require(!invalid.success() && has(invalid, "E_IR_INVALID"),
            "verifier accepted a discard label");
  }
  source("func ignore(_ i32,_ i32) {} func f() {let op=ignore; op(1,2);}");
  source("func f() {let op=func(_ i32,_ i32) {}; op(1,2);}");
  source("func ignore(_ i32) {} func f() {ignore(_:1);}", "E_CALL_ARGUMENT");
  {
    auto r = source("func f() { let op=func(x i32) i32 { return x; }; }");
    const auto &m = r.value->module();
    require(m.functions.size() == 2, "lambda body missing or duplicated");
    size_t literals = 0;
    for (const auto &f : m.functions) {
      auto ref = f.source.node;
      if (m.resolved->project->modules[ref.module.value].files[ref.file]
              ->node(ref.node).kind == AstKind::FunctionLiteral) {
        ++literals;
        require(m.resolved->symbols[f.symbol.value].name.empty(),
                "lambda reused a named function identity");
      }
    }
    require(literals == 1, "lambda has no source-backed HIR function");
  }
  source("func f() i32 { return (func(x i32) i32 { return x; })(1); }");
  source("func make() (func(i32) i32) { return func(x i32) i32 { return x+1; }; } "
         "func f() i32 { return make()(1); }");
  source("func apply(op func(i32) i32) i32 { return op(1); } "
         "func f() i32 { return apply(func(x i32) i32 { return x; }); }");
  source("func f() i32 { let outer=func(x i32) i32 { "
         "let inner=func(y i32) i32 { return y; }; return inner(x); }; return outer(1); }");
  source("const op=func(x i32) i32 { return x; }; func f() i32 { return op(1); }");
  source("func f() { let op=func(x inout i32) { x+=1; }; var x i32=1; op(inout x); }");
  source("func f() { let op=func(x i32) i32 { return true; }; }", "E_TYPE_MISMATCH");
  source("func f() { let op=func(x i32) i32 { return x; }; op(x:1); }", "E_CALL_ARGUMENT");
  source("func f(a f32) f32 { return a; }");
  source("func f() { var x f32=1.0; x+=1.0; }");
  source("func f() { let x f32=1.0; let y=x%1.0; }", "E_TYPE_MISMATCH");
  source("func unit() {} func f() { let x=unit(); return x; }");
  source("func a(x i32) i32 { return x; } func b(y i32) i32 { return y+1; } "
         "func f() i32 { var op func(i32) i32=a; op=b; return op(2); }");
  source("func f(a f32) f32 { return a+a; }");
  source("func f() { let x i8=i8(300); }", "E_TYPE_MISMATCH");
  source("func f(x i64) i8 { return i8(x); }");
  source("func f() { let x f32=1.0; let y f64=1.0; }");
  {
    auto r = source("func f() { let a f32=1.000000059604644775390625; let b "
                    "f32=1.000000059604644775390626; let c f32=1e-45; let d "
                    "f32=1e-50; let e f32=1e50; let f f64=9007199254740993.0; "
                    "let g f32=0e999999999999999999999999999999; }");
    const auto &m = r.value->module();
    const std::map<std::string, uint64_t> expected{
        {"a", 0x3f800000}, {"b", 0x3f800001}, {"c", 1},
        {"d", 0},          {"e", 0x7f800000}, {"f", 0x4340000000000000ULL},
        {"g", 0}};
    size_t found = 0;
    for (const auto &n : m.nodes)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        auto name = m.resolved->symbols[b->symbol.value].name;
        auto literal =
            std::get_if<hir::FloatLiteral>(&m.nodes[n.children[0].value].data);
        require(literal && literal->bits == expected.at(name),
                "decimal float literal rounding: " + name);
        ++found;
      }
    require(found == expected.size(),
            "float literal expectations not observed");
  }
  for (unsigned width : {32u, 64u}) {
    for (const auto &literal : {
             "1" + std::string(10001, '0') + ".0e-10001",
             "0." + std::string(10000, '0') + "1e10001",
             "1" + std::string(10000, '0') + "1.0e-10001"}) {
      auto r = source("func f() { let x f" + std::to_string(width) + "=" + literal + "; }");
      size_t found = 0;
      for (const auto &n : r.value->module().nodes)
        if (auto f = std::get_if<hir::FloatLiteral>(&n.data)) {
          ++found;
          require(f->bits == (width == 32 ? 0x3f800000ULL : 0x3ff0000000000000ULL),
                  "decimal exponent/significand cancellation did not round to one");
        }
      require(found == 1, "missing long decimal literal");
    }
  }
  for (const char *backend : {"native", "python"})
    for (const char *type : {"f32", "f64"})
      for (const char *op : {"<", "==", "!=", "<=", ">", ">="})
        for (const char *literal : {"1.0", "-1.0", "(1.0)", "(-1.0)",
                                    "(1.0 + 2.0)"}) {
          auto r = typed_source(
              "func p(x " + std::string(type) + ") bool { return " +
                  literal + " " + op + " x; }",
              nullptr, backend);
          unsigned expected_bits = std::string(type) == "f32" ? 32 : 64;
          size_t floats = 0;
          for (const auto &n : r.value->nodes)
            if (std::get_if<hir::FloatLiteral>(&n.data)) {
              auto t = r.value->types.find(n.type);
              require(t && t->bits == expected_bits,
                      "literal-left comparison did not use its peer width");
              ++floats;
            }
          require(floats == (std::string(literal) == "(1.0 + 2.0)" ? 2 : 1),
                  "literal-left comparison nodes missing");
        }
  for (const char *backend : {"native", "python"})
    for (const char *type : {"f32", "f64"})
      for (const char *op : {"+", "-", "*", "/"}) {
        auto r = typed_source("func p(x " + std::string(type) + ") " +
                                  std::string(type) + " { return 1.0 " + op +
                                  " x; }",
                              nullptr, backend);
        unsigned expected_bits = std::string(type) == "f32" ? 32 : 64;
        size_t floats = 0;
        for (const auto &n : r.value->nodes)
          if (std::get_if<hir::FloatLiteral>(&n.data)) {
            auto t = r.value->types.find(n.type);
            require(t && t->bits == expected_bits,
                    "literal-left arithmetic did not use its peer width");
            ++floats;
          }
        require(floats == 1, "literal-left arithmetic nodes missing");
      }
  typed_source("func p(x f32, y f64) bool { return x < y; }",
               "E_TYPE_MISMATCH");
  typed_source("func p(x f32, y f64) { let z = x + y; }",
               "E_TYPE_MISMATCH");
  source("func p(x f32) bool { return 1.0 < x; }");
  source("func p(x f64) bool { return (-1.0) >= x; }");
  source("func p(x f32) f32 { return (1.0 + 2.0) + x; }");
  for (const auto *body : {
           "func f() { var x f32=1.0; x+=1.0; }",
           "func f(a f32) f32 { return a+a; }",
           "func p(x f32) bool { return 1.0 < x; }",
           "func p(x f64) bool { return (-1.0) >= x; }",
           "func p(x f32) f32 { return (1.0 + 2.0) + x; }"})
    source(body, nullptr, "python");
  source("func f() { let x=-true; }", "E_TYPE_MISMATCH");
  source("func f() { let x=~true; }", "E_TYPE_MISMATCH");
  source("func f() { let x=+true; }", "E_TYPE_MISMATCH");
  source("func f() { let x=u64(18446744073709551615); }");
  source("func f() { let x=i32(true); }", "E_CAPABILITY_UNAVAILABLE");
  source("func f() { let x=f32(1); }", "E_CAPABILITY_UNAVAILABLE");
  source("func f() { let x=[1]; }");
  source("func f() { switch 1 { default: break; } }",
         "E_CAPABILITY_UNAVAILABLE");
  source("func f() { defer {} }");
  source("func f() { let x=1; unsafe {} }", "E_CAPABILITY_UNAVAILABLE");
  // Constant expressions keep their typed wrap/truncation semantics when
  // materialized; conversion checks use those values, not host arithmetic.
  {
    auto r = source(
        "const A i8=127; const B i8=A+1; const C i32=-2147483648; const D=C / "
        "-1; const E=C % -1; const F i32=-7 / 3; const G i32=-7 % 3; func f() "
        "{ let b i8=B; let d i32=D; let e i32=E; let f i32=F; let g i32=G; }");
    const auto &m = r.value->module();
    const std::map<std::string, std::pair<std::string, uint64_t>> expected{
        {"b", {"-128", 128}},
        {"d", {"-2147483648", 2147483648}},
        {"e", {"0", 0}},
        {"f", {"-2", 4294967294}},
        {"g", {"-1", 4294967295}}};
    size_t found = 0;
    for (const auto &n : m.nodes)
      if (auto b = std::get_if<hir::Binding>(&n.data)) {
        const auto &name = m.resolved->symbols[b->symbol.value].name;
        if (auto it = expected.find(name); it != expected.end()) {
          auto literal = std::get_if<hir::IntegerLiteral>(
              &m.nodes[n.children[0].value].data);
          require(literal && literal->decimal == it->second.first &&
                      literal->bits == it->second.second,
                  "constant fold: " + name);
          ++found;
        }
      }
    require(found == expected.size(),
            "constant fold expectations not observed");
  }
  source("const X i32=1;");
  source(
      "func even(x i32) bool { if x==0 { return true; } return odd(x-1); } "
      "func odd(x i32) bool { if x==0 { return false; } return even(x-1); }");
  {
    Temp t;
    manifest(t.path, "python", 64);
    {
      std::ofstream out(t.path / "lang.toml", std::ios::app);
      out << "[dependencies.py]\nkind=\"python\"\nbinding=\"binding."
             "json\"\nhost_package=\"host\"\n";
    }
    write(
        t.path / "binding.json",
        R"({"schema_version":1,"contract":"compiler-dev/1","package_requirements":[{"package":"host"}],"modules":[{"logical_path":"py","host_module":"host"}],"types":[],"values":[],"callables":[{"module":"py","name":"run","call_kind":"function","id":"host.run"}]})");
    write(t.path / "src/main.lang",
          "package app; import \"py\" as p; func f() { p.run(); }");
    auto r = check(t.path / "lang.toml");
    require(!r.value && r.failure == FailureKind::unsupported &&
                has(r, "E_CAPABILITY_UNAVAILABLE"),
            "foreign declaration rejection\n" + messages(r));
    require(r.diagnostics[0].primary && r.diagnostics[0].phase == "type",
            "foreign import diagnostic");
  }
  {
    Temp t;
    manifest(t.path, "native", 64);
    {
      std::ofstream out(t.path / "lang.toml", std::ios::app);
      out << "[dependencies.dep]\nkind=\"source\"\npath=\"vendor/dep\"\n";
    }
    fs::create_directories(t.path / "vendor/dep/src");
    manifest(t.path / "vendor/dep", "native", 64);
    write(t.path / "vendor/dep/src/a.lang",
          "package dep; pub const N i32=3; pub func add(x i32) i32 { return "
          "x+N; }");
    write(t.path / "src/main.lang", "package app; import \"dep\" as d; func "
                                    "f() i32 { return d.add(1)+d.N; }");
    write(t.path / "src/other.lang",
          "package app; func g() i32 { return f(); }");
    auto r = check(t.path / "lang.toml");
    require(r.success(), "source-module scalar call\n" + messages(r));
    require(r.value->module().functions.size() == 3,
            "cross-module definitions missing");
  }
  source("const X i32=300; func f() { let x=i8(X); }", "E_TYPE_MISMATCH");
  source("const X i32=1 / 0; func f() {}", "E_CAPABILITY_UNAVAILABLE");
  source("const X=false && (1/0==0); const Y=true || (1<<64==0); func f() { "
         "let x=X; let y=Y; }");
  source("func f(x i32,y u8) { let a=x/0; let b=x<<y; let c=x<<32; let "
         "d=x<<-1; }");
  {
    auto r = source("func a(first i32,second i32) i32 { return first+second; } "
                    "func x() i32 { return 1; } func y() i32 { return 2; } "
                    "func f() i32 { return a(second:x(),first:y()); }");
    bool found = false;
    for (const auto &n : r.value->module().nodes)
      if (const auto *c = std::get_if<hir::Call>(&n.data);
          c && c->args.size() == 2) {
        require(
            c->args[0].source_index == 0 && c->args[0].parameter_index == 1 &&
                c->args[0].label == "second" && c->args[1].parameter_index == 0,
            "named argument order");
        found = true;
      }
    require(found, "named call HIR missing");
  }
  {
    Temp t;
    manifest(t.path, "native", 64);
    write(t.path / "src/main.lang", "package app; func f() {}");
    SourceManager sm;
    auto r = resolve(t.path / "lang.toml", sm);
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = sema::check_project(r, {}, stop.get_token());
    require(cancelled.state == PhaseState::cancelled && !cancelled.value &&
                cancelled.exit_code() == ExitCode::cancelled,
            "checker cancellation");
    require(sema::check_project(r, hir::Config{hir::Backend::python, 64})
                    .exit_code() == ExitCode::internal_error,
            "target mismatch");
    r.state = PhaseState::failed;
    r.failure = FailureKind::source;
    Diagnostic d;
    d.code = "E_NAME_NOT_FOUND";
    d.phase = "resolve";
    d.severity = "error";
    d.message = "test failure";
    r.diagnostics.push_back(d);
    auto failed = sema::check_project(r);
    require(!failed.value && has(failed, "E_NAME_NOT_FOUND") &&
                failed.failure == FailureKind::source,
            "failed phase with retained value entered checker");
    r.state = PhaseState::recovered;
    require(!sema::check_project(r).value, "recovered phase entered checker");
    r.state = PhaseState::cancelled;
    require(sema::check_project(r).exit_code() == ExitCode::cancelled,
            "resolve cancellation lost");
    require(
        sema::check_project(std::shared_ptr<const modules::ResolvedProject>{})
                .exit_code() == ExitCode::internal_error,
        "null input");
  }
  std::cout << "passed: " << checked
            << " real load/resolve/check/verify cases; no source execution\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string_view(argv[1]) == "--project") {
      auto r = check(argv[2]);
      std::cout << messages(r);
      std::cout << (r.success() ? "passed: verified static HIR" : "failed")
                << '\n';
      return int(r.exit_code());
    }
    require(argc == 2, "usage: check_test PUBLISHED_REPO | --project MANIFEST");
    suite(argv[1]);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

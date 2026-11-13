#include "lang/hir/hir.hpp"
#include "../../src/hir/ownership.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace lang;
using namespace lang::hir;
namespace {
void require(bool b, const char *message) {
  if (!b)
    throw std::runtime_error(message);
}
static_assert(!std::is_default_constructible_v<VerifiedHirModule>);
static_assert(!std::is_constructible_v<VerifiedHirModule,
                                       std::shared_ptr<const HirModule>>);
struct Fixture {
  SourceManager sources;
  std::shared_ptr<modules::Project> project;
  std::shared_ptr<modules::ResolvedProject> resolved;
  HirModule m;
  TypeId unit, boolean, i32, signature, main_signature;
  SymbolId add, main, a, b, x, y, iteration;
  HirNodeId call, value_call, loop, jump, cont, literal, read, binding, root,
      add_return, conversion;
  ScopeId add_scope, main_scope, loop_scope, body_scope;
  Source add_src, main_src;
  SymbolId named(std::string_view name) {
    for (auto &s : resolved->symbols)
      if (s.name == name)
        return s.id;
    throw std::runtime_error("missing fixture symbol");
  }
  Source decl(SymbolId id) {
    auto ref = *resolved->symbols[id.value].definition;
    const auto &ast = project->modules[ref.module.value]
                          .files[ref.file]
                          ->nodes[ref.node.value];
    return {ref, ast.span};
  }
  ScopeId scope(SymbolId function, std::optional<ScopeId> parent, Source src) {
    ScopeId id{m.scopes.size()};
    m.scopes.push_back({function, parent, src});
    return id;
  }
  template <class T>
  HirNodeId node(TypeId type, ScopeId scope_id, Source src, T data,
                 std::vector<HirNodeId> ch = {}) {
    HirNodeId id{m.nodes.size()};
    m.nodes.push_back({type, src, scope_id, std::move(ch), std::move(data)});
    return id;
  }
  HirNodeId lit(int value, ScopeId s) {
    return node(i32, s, main_src,
                IntegerLiteral{std::to_string(value), uint32_t(value)});
  }
  HirNodeId rd(SymbolId sym, ScopeId s, Source src) {
    return node(i32, s, src, Read{sym});
  }
  HirNodeId increment(SymbolId target, ScopeId scope_id, Source owner) {
    const auto &file = *project->modules[owner.node.module.value]
                            .files[owner.node.file];
    for (size_t i = 0; i < file.nodes.size(); ++i) {
      const auto &assignment = file.nodes[i];
      if (assignment.kind != AstKind::Assign || assignment.children.size() != 2 ||
          assignment.span.begin_byte < owner.span.begin_byte ||
          assignment.span.end_byte > owner.span.end_byte)
        continue;
      const auto &lhs = file.node(assignment.children[0]);
      if (lhs.kind != AstKind::Name ||
          file.tokens[lhs.token_begin].spelling !=
              resolved->symbols[target.value].name)
        continue;
      auto src = [&](AstNodeId id) {
        auto ref = owner.node;
        ref.node = id;
        return Source{ref, file.node(id).span};
      };
      auto place = node(i32, scope_id, src(assignment.children[0]), Place{target});
      auto one = node(i32, scope_id, src(assignment.children[1]),
                      IntegerLiteral{"1", 1});
      return node(unit, scope_id, src(AstNodeId{i}), Assign{BinaryOp::add},
                  {place, one});
    }
    throw std::runtime_error("fixture increment source missing");
  }
  Fixture(unsigned inout_count = 0, bool unreachable_break = false,
           bool false_branch_break = false) {
    std::string code = R"(package main;
func add(a i32, b i32) i32 { return a + b; }
func main() i32 {
 var x i32 = 0;
 "中文\n";
 for var i i32 = 0; i < 4; i += 1 { if x > 1 { break; } continue; }
 let xf f32 = 1.25;
 let xd f64 = 1.25;
 let c32 f32 = f32(xf);
 let c64 f64 = f64(xd);
 let w f64 = f64(xf);
 let n f32 = f32(xd);
 let cx f64 = f64(x);
 let ci i32 = i32(1.0);
 let converted i32 = i32(x);
 add(b: 2, a: 1);
 add(1, 2);
 return x;
})";
    if (unreachable_break) {
      auto begin = code.find(" for var");
      auto end = code.find("\n add(b:", begin);
      code.replace(begin, end - begin,
                   false_branch_break
                       ? " for var i i32 = 0; true; i += 1 { if false { break; } continue; }"
                       : " for var i i32 = 0; true; i += 1 { continue; break; }");
      code.erase(code.find("\n add(b:"),
                 code.rfind('}') - code.find("\n add(b:"));
    }
    if (inout_count) {
      code.replace(code.find("a i32"), 5, "a inout i32");
      code.replace(code.find("return a + b;"), 13,
                   "a += 1; return a + b;");
      code.replace(code.find("add(b: 2, a: 1);"), 16,
                   "add(b: 2, a: inout x);");
      code.replace(code.find("add(1, 2);"), 10, "add(inout x, 2);");
    }
    if (inout_count == 2) {
      code.replace(code.find("b i32"), 5, "b inout i32");
      code.insert(code.find(" for var"), " var y i32 = 2;\n");
      code.replace(code.find("b: 2"), 4, "b: inout y");
      code.replace(code.find("add(inout x, 2)"), 15,
                   "add(inout x, inout y)");
    }
    auto src = sources.update(sources.add_file("fixture.wz"), code);
    auto tokens = lex(src);
    require(tokens.success(), "fixture lexer");
    auto ast = parse(*tokens.value);
    require(ast.success(), "fixture parser");
    project = std::make_shared<modules::Project>();
    project->backend = "native";
    modules::Module mod;
    mod.id = ModuleId{0};
    mod.logical_path = "app";
    mod.files.push_back(std::make_shared<const AstFile>(std::move(*ast.value)));
    project->modules.push_back(std::move(mod));
    auto names = modules::resolve_project(project);
    require(names.success(), "fixture resolver");
    resolved =
        std::make_shared<modules::ResolvedProject>(std::move(*names.value));
    m.resolved = resolved;
    m.identity = ModuleId{0};
    Type t;
    t.kind = TypeKind::Unit;
    unit = m.types.intern(t);
    t.kind = TypeKind::Bool;
    boolean = m.types.intern(t);
    t.kind = TypeKind::SignedInt;
    t.bits = 32;
    i32 = m.types.intern(t);
    t = Type{};
    t.kind = TypeKind::Function;
    t.result = i32;
    t.parameters = {{ParamMode::ordinary, i32}, {ParamMode::ordinary, i32}};
    if (inout_count)
      t.parameters[0].mode = ParamMode::inout;
    if (inout_count == 2)
      t.parameters[1].mode = ParamMode::inout;
    signature = m.types.intern(t);
    t.parameters.clear();
    main_signature = m.types.intern(t);
    add = named("add");
    main = named("main");
    a = named("a");
    b = named("b");
    x = named("x");
    if (inout_count == 2)
      y = named("y");
    iteration = named("i");
    add_src = decl(add);
    main_src = decl(main);
    add_scope = scope(add, {}, add_src);
    main_scope = scope(main, {}, main_src);
    auto lhs = rd(a, add_scope, add_src), rhs = rd(b, add_scope, add_src);
    auto sum = node(i32, add_scope, add_src, Binary{BinaryOp::add}, {lhs, rhs});
    add_return = node(unit, add_scope, add_src, Return{add}, {sum});
    auto add_body = node(unit, add_scope, add_src, Block{}, {add_return});
    if (inout_count) {
      auto assignment = increment(a, add_scope, add_src);
      m.nodes[add_body.value].children.insert(
          m.nodes[add_body.value].children.begin(), assignment);
    }
    m.functions.push_back(
        {add, signature, {{a, "a"}, {b, "b"}}, add_body, add_scope, add_src});
    literal = lit(0, main_scope);
    binding = node(unit, main_scope, decl(x), Binding{x, true}, {literal});
    loop_scope = scope(main, main_scope, main_src);
    body_scope = scope(main, loop_scope, main_src);
    auto init = node(unit, loop_scope, decl(iteration),
                     Binding{iteration, true}, {lit(0, loop_scope)});
    auto cond = unreachable_break
                    ? node(boolean, loop_scope, main_src, BoolLiteral{true})
                    : node(boolean, loop_scope, main_src, Binary{BinaryOp::less},
                           {rd(iteration, loop_scope, main_src), lit(4, loop_scope)});
    auto step = increment(iteration, loop_scope, main_src);
    HirNodeId body;
    if (false_branch_break) {
      auto then_scope = scope(main, body_scope, main_src);
      jump = node(unit, then_scope, main_src, Break{HirNodeId{0}});
      auto then_ = node(unit, then_scope, main_src, Block{}, {jump});
      auto false_ = node(boolean, body_scope, main_src, BoolLiteral{false});
      auto if_ = node(unit, body_scope, main_src, If{}, {false_, then_});
      cont = node(unit, body_scope, main_src, Continue{HirNodeId{0}});
      body = node(unit, body_scope, main_src, Block{}, {if_, cont});
    } else if (unreachable_break) {
      cont = node(unit, body_scope, main_src, Continue{HirNodeId{0}});
      jump = node(unit, body_scope, main_src, Break{HirNodeId{0}});
      body = node(unit, body_scope, main_src, Block{}, {cont, jump});
    } else {
      auto if_scope = scope(main, body_scope, main_src);
      jump = node(unit, if_scope, main_src, Break{HirNodeId{0}});
      auto then_ = node(unit, if_scope, main_src, Block{}, {jump});
      auto cmp = node(boolean, body_scope, main_src, Binary{BinaryOp::greater},
                      {rd(x, body_scope, main_src), lit(1, body_scope)});
      auto if_ = node(unit, body_scope, main_src, If{}, {cmp, then_});
      cont = node(unit, body_scope, main_src, Continue{HirNodeId{0}});
      body = node(unit, body_scope, main_src, Block{}, {if_, cont});
    }
    loop = node(unit, loop_scope, main_src, For{}, {init, cond, step, body});
    std::get<Break>(m.nodes[jump.value].data).loop = loop;
    std::get<Continue>(m.nodes[cont.value].data).loop = loop;
    if (unreachable_break) {
      root = node(unit, main_scope, main_src, Block{}, {binding, loop});
      m.functions.push_back(
          {main, main_signature, {}, root, main_scope, main_src});
      return;
    }
    auto arg0 = inout_count == 2
                    ? node(i32, main_scope, main_src, Place{y})
                    : lit(2, main_scope);
    auto arg1 = inout_count
                    ? node(i32, main_scope, main_src, Place{x})
                    : lit(1, main_scope);
    Call direct;
    direct.signature = signature;
    direct.direct_symbol = add;
    direct.args = {{0, 1, "b", inout_count == 2 ? ParamMode::inout
                                               : ParamMode::ordinary, arg0},
                   {1, 0, "a", inout_count ? ParamMode::inout
                                           : ParamMode::ordinary, arg1}};
    call = node(i32, main_scope, main_src, direct, {arg0, arg1});
    auto call_stmt =
        node(unit, main_scope, main_src, ExpressionStatement{}, {call});
    const auto &file = *project->modules[main_src.node.module.value]
                            .files[main_src.node.file];
    auto call_ref = main_src.node;
    for (size_t i = 0; i < file.nodes.size(); ++i)
      if (file.nodes[i].kind == AstKind::Call &&
          file.nodes[i].span.begin_byte >= main_src.span.begin_byte &&
          file.nodes[i].span.end_byte <= main_src.span.end_byte &&
          file.nodes[i].span.begin_byte > file.node(call_ref.node).span.begin_byte)
        call_ref.node = AstNodeId{i};
    const auto &written = file.node(call_ref.node);
    require(written.kind == AstKind::Call && written.children.size() == 3,
            "fixture positional call source missing");
    auto ast_source = [&](AstNodeId id) {
      auto ref = main_src.node;
      ref.node = id;
      return Source{ref, file.node(id).span};
    };
    auto argument_source = [&](size_t index) {
      auto id = file.node(written.children[index + 1]).children.at(0);
      if (file.node(id).kind == AstKind::Unary)
        id = file.node(id).children.at(0); // the inout marker is not the Place
      return ast_source(id);
    };
    auto callee = node(signature, main_scope, ast_source(written.children[0]),
                       FunctionValue{add});
    arg0 = inout_count ? node(i32, main_scope, main_src, Place{x})
                      : lit(1, main_scope);
    arg1 = inout_count == 2 ? node(i32, main_scope, main_src, Place{y})
                           : lit(2, main_scope);
    m.nodes[arg0.value].source = argument_source(0);
    m.nodes[arg1.value].source = argument_source(1);
    Call indirect;
    indirect.kind = CalleeKind::function_value;
    indirect.signature = signature;
    indirect.callee = callee;
    indirect.args = {{0, 0, {}, inout_count ? ParamMode::inout
                                           : ParamMode::ordinary, arg0},
                     {1, 1, {}, inout_count == 2 ? ParamMode::inout
                                                : ParamMode::ordinary, arg1}};
    value_call =
        node(i32, main_scope, ast_source(call_ref.node), indirect,
             {callee, arg0, arg1});
    auto value_stmt =
        node(unit, main_scope, main_src, ExpressionStatement{}, {value_call});
    const auto &conversion_file =
        *project->modules[main_src.node.module.value].files[main_src.node.file];
    Source conversion_src, conversion_operand_src;
    bool found_conversion = false;
    for (size_t i = 0; i < conversion_file.nodes.size() && !found_conversion;
         ++i) {
      const auto &candidate = conversion_file.nodes[i];
      if (candidate.kind != AstKind::Call || candidate.children.size() != 2 ||
          candidate.span.begin_byte < main_src.span.begin_byte ||
          candidate.span.end_byte > main_src.span.end_byte)
        continue;
      const auto &callee = conversion_file.node(candidate.children[0]);
      if (callee.kind != AstKind::Name ||
          conversion_file.tokens[callee.token_begin].spelling != "i32")
        continue;
      auto ref = main_src.node;
      ref.node = AstNodeId{i};
      conversion_src = {ref, candidate.span};
      ref.node = conversion_file.node(candidate.children[1]).children.at(0);
      conversion_operand_src = {ref, conversion_file.node(ref.node).span};
      found_conversion = true;
    }
    require(found_conversion, "fixture conversion source missing");
    read = rd(x, main_scope, conversion_operand_src);
    conversion = node(i32, main_scope, conversion_src, IntegerConvert{},
                      {read});
    auto ret = node(unit, main_scope, main_src, Return{main}, {conversion});
    root = node(unit, main_scope, main_src, Block{},
                {binding, loop, call_stmt, value_stmt, ret});
    if (inout_count == 2) {
      auto y_binding = node(unit, main_scope, decl(y), Binding{y, true},
                             {lit(2, main_scope)});
      auto &children = m.nodes[root.value].children;
      children.insert(children.begin() + 1, y_binding);
    }
    m.functions.push_back(
        {main, main_signature, {}, root, main_scope, main_src});
  }
};
size_t cases = 0;
void invalid(const Fixture &f, const char *name,
             const std::function<void(HirModule &)> &edit) {
  auto m = f.m;
  edit(m);
  auto r = verify(m);
  if (r.success() || r.value || r.diagnostics.empty() ||
      r.diagnostics[0].code != "E_IR_INVALID" ||
      r.diagnostics[0].phase != "lowering" ||
      r.exit_code() != ExitCode::internal_error)
    throw std::runtime_error(std::string("invalid module accepted: ") + name);
  ++cases;
}
void inout_tests() {
  Fixture f(1);
  auto valid = verify(f.m);
  if (!valid.success())
    for (const auto &d : valid.diagnostics)
      std::cerr << d.message << '\n';
  require(valid.success(), "writable scalar inout fixture rejected");
  for (auto backend : {Backend::native, Backend::python})
    for (unsigned width : {32u, 64u}) {
      auto m = f.m;
      auto project = std::make_shared<modules::Project>(*f.project);
      project->backend = backend == Backend::native ? "native" : "python";
      project->word_bits = width;
      auto resolved = std::make_shared<modules::ResolvedProject>(*f.resolved);
      resolved->project = project;
      m.resolved = resolved;
      m.config = {backend, width};
      require(verify(m).success(), "inout rejected under target profile");
    }
  invalid(f, "inout parameter source mode", [&](auto &m) {
    Type t = *m.types.find(f.signature);
    t.parameters[0].mode = ParamMode::ordinary;
    m.functions[0].signature = m.types.intern(t);
  });
  invalid(f, "inout literal", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[1].expression;
    m.nodes[arg.value].data = IntegerLiteral{"1", 1};
  });
  invalid(f, "inout binding readonly", [&](auto &m) {
    std::get<Binding>(m.nodes[f.binding.value].data).writable = false;
  });
  invalid(f, "inout place wrong type", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[1].expression;
    m.nodes[arg.value].type = f.boolean;
  });
  invalid(f, "ordinary argument cannot be place", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[0].expression;
    m.nodes[arg.value].data = Place{f.x};
  });
  invalid(f, "inout argument needs mode", [&](auto &m) {
    std::get<Call>(m.nodes[f.call.value].data).args[1].mode = ParamMode::ordinary;
  });
  invalid(f, "inout place is not a read", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[1].expression;
    m.nodes[arg.value].data = Read{f.x};
  });
  invalid(f, "inout child order", [&](auto &m) {
    std::swap(m.nodes[f.call.value].children[0],
              m.nodes[f.call.value].children[1]);
  });
  invalid(f, "inout outside binding scope", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[1].expression;
    m.nodes[arg.value].data = Place{f.iteration};
  });
  invalid(f, "inout place belongs to other function", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[1].expression;
    m.nodes[arg.value].data = Place{f.a};
  });
  invalid(f, "inout also ordinary read", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[0].expression;
    m.nodes[arg.value].data = Read{f.x};
  });
  invalid(f, "inout also nested read", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[f.call.value].data).args[0].expression;
    HirNodeId read{m.nodes.size()};
    m.nodes.push_back({f.i32, f.main_src, f.main_scope, {}, Read{f.x}});
    m.nodes[arg.value].data = Unary{UnaryOp::negate};
    m.nodes[arg.value].children = {read};
  });
  invalid(f, "inout nontrivial parameter", [&](auto &m) {
    // Owning inout is supported now, so this forgery uses a Task handle: its
    // completion lifetime belongs to the executor and HIR has no copy/drop.
    Type task;
    task.kind = TypeKind::Task;
    task.type_args = {f.i32, f.unit};
    auto handle = m.types.intern(task);
    Type t;
    t.kind = TypeKind::Function;
    t.result = f.i32;
    t.parameters = {{ParamMode::inout, handle}};
    m.types.intern(t);
  });
  {
    auto m = f.m;
    // Build the forwarding call in the independent module copy.
    HirNodeId forwarded{m.nodes.size()};
    m.nodes.push_back({f.i32, f.add_src, f.add_scope, {}, Place{f.a}});
    HirNodeId zero{m.nodes.size()};
    m.nodes.push_back({f.i32, f.add_src, f.add_scope, {}, IntegerLiteral{"0", 0}});
    Call call;
    call.signature = f.signature;
    call.direct_symbol = f.add;
    call.args = {{0, 0, {}, ParamMode::inout, forwarded},
                  {1, 1, {}, ParamMode::ordinary, zero}};
    HirNodeId id{m.nodes.size()};
    m.nodes.push_back({f.i32, f.add_src, f.add_scope, {forwarded, zero}, call});
    HirNodeId statement{m.nodes.size()};
    m.nodes.push_back({f.unit, f.add_src, f.add_scope, {id}, ExpressionStatement{}});
    auto &body = m.nodes[m.functions[0].body.value].children;
    body.insert(body.begin(), statement);
    require(verify(m).success(), "exclusive scalar inout forwarding rejected");
  }
  Fixture pair(2);
  valid = verify(pair.m);
  if (!valid.success())
    for (const auto &d : valid.diagnostics)
      std::cerr << d.message << '\n';
  require(valid.success(), "two distinct writable scalar inout arguments rejected");
  invalid(pair, "duplicate inout whole storage", [&](auto &m) {
    auto arg = std::get<Call>(m.nodes[pair.call.value].data).args[0].expression;
    m.nodes[arg.value].data = Place{pair.x};
  });
}
HirModule source_module(std::string code) {
  SourceManager sources;
  auto source = sources.update(sources.add_file("flow.wz"), std::move(code));
  auto tokens = lex(source);
  require(tokens.success(), "flow fixture lexer");
  auto parsed = parse(*tokens.value);
  require(parsed.success(), "flow fixture parser");
  auto project = std::make_shared<modules::Project>();
  project->backend = "native";
  modules::Module module;
  module.id = ModuleId{0};
  module.logical_path = "app";
  module.files.push_back(std::make_shared<const AstFile>(std::move(*parsed.value)));
  project->modules.push_back(std::move(module));
  auto resolved = modules::resolve_project(project);
  require(resolved.success(), "flow fixture resolver");
  HirModule m;
  m.identity = ModuleId{0};
  m.resolved = std::make_shared<const modules::ResolvedProject>(
      std::move(*resolved.value));
  return m;
}
std::pair<SymbolId, Source> source_function(const HirModule &m,
                                          std::string_view name) {
  for (const auto &s : m.resolved->symbols)
    if (s.kind == modules::SymbolKind::function && s.name == name) {
      const auto &ref = *s.definition;
      return {s.id, {ref, m.resolved->project->modules[ref.module.value]
                             .files[ref.file]->node(ref.node).span}};
    }
  throw std::runtime_error("missing flow function symbol");
}
void simple_flow_tests() {
  {
    auto m = source_module("package main; const x i32 = 1;");
    require(verify(m).success(), "functionless scalar module rejected");
    auto orphan_scope = m;
    orphan_scope.scopes.push_back({SymbolId{0}, {}, {}});
    require(!verify(orphan_scope).success(),
            "orphan scope in functionless module accepted");
    ++cases;
    m.nodes.push_back({TypeId{0}, {}, ScopeId{0}, {}, Noop{}});
    require(!verify(m).success(), "orphan node in functionless module accepted");
    ++cases;
  }
  {
    auto m = source_module("package main; func g() {} "
                           "func f() { for g(); false; g() {} }");
    auto [g, gs] = source_function(m, "g");
    auto [f, fs] = source_function(m, "f");
    Type t;
    t.kind = TypeKind::Unit;
    auto unit = m.types.intern(t);
    t.kind = TypeKind::Bool;
    auto boolean = m.types.intern(t);
    t.kind = TypeKind::Function;
    t.result = unit;
    auto signature = m.types.intern(t);
    m.scopes = {{g, {}, gs}, {f, {}, fs}, {f, ScopeId{1}, fs},
                 {f, ScopeId{2}, fs}};
    auto add = [&](TypeId type, Source source, ScopeId scope, NodeData data,
                   std::vector<HirNodeId> children = {}) {
      HirNodeId id{m.nodes.size()};
      m.nodes.push_back({type, source, scope, std::move(children), std::move(data)});
      return id;
    };
    auto g_body = add(unit, gs, ScopeId{0}, Block{});
    Call call;
    call.signature = signature;
    call.direct_symbol = g;
    auto init_call = add(unit, fs, ScopeId{2}, call);
    auto init = add(unit, fs, ScopeId{2}, ExpressionStatement{}, {init_call});
    auto condition = add(boolean, fs, ScopeId{2}, BoolLiteral{false});
    auto step_call = add(unit, fs, ScopeId{2}, call);
    auto step = add(unit, fs, ScopeId{2}, ExpressionStatement{}, {step_call});
    auto body = add(unit, fs, ScopeId{3}, Block{});
    auto loop = add(unit, fs, ScopeId{2}, For{}, {init, condition, step, body});
    auto root = add(unit, fs, ScopeId{1}, Block{}, {loop});
    m.functions = {{g, signature, {}, g_body, ScopeId{0}, gs},
                    {f, signature, {}, root, ScopeId{1}, fs}};
    require(verify(m).success(), "for expression-statement init rejected");
  }
  for (bool with_else : {false, true}) {
    auto m = source_module(with_else
        ? "package main; func f() i32 { if false { return 1; } else { return 2; } }"
        : "package main; func f() i32 { if true { return 1; } }");
    auto [f, source] = source_function(m, "f");
    Type t;
    t.kind = TypeKind::Unit;
    auto unit = m.types.intern(t);
    t.kind = TypeKind::Bool;
    auto boolean = m.types.intern(t);
    t.kind = TypeKind::SignedInt;
    t.bits = 32;
    auto integer = m.types.intern(t);
    t = Type{};
    t.kind = TypeKind::Function;
    t.result = integer;
    auto signature = m.types.intern(t);
    m.scopes = {{f, {}, source}, {f, ScopeId{0}, source}};
    auto add = [&](TypeId type, ScopeId scope, NodeData data,
                   std::vector<HirNodeId> children = {}) {
      HirNodeId id{m.nodes.size()};
      m.nodes.push_back({type, source, scope, std::move(children), std::move(data)});
      return id;
    };
    auto condition = add(boolean, ScopeId{0}, BoolLiteral{!with_else});
    auto one = add(integer, ScopeId{1}, IntegerLiteral{"1", 1});
    auto ret = add(unit, ScopeId{1}, Return{f}, {one});
    auto then_ = add(unit, ScopeId{1}, Block{}, {ret});
    std::vector<HirNodeId> children{condition, then_};
    if (with_else) {
      m.scopes.push_back({f, ScopeId{0}, source});
      auto two = add(integer, ScopeId{2}, IntegerLiteral{"2", 2});
      auto other_ret = add(unit, ScopeId{2}, Return{f}, {two});
      children.push_back(add(unit, ScopeId{2}, Block{}, {other_ret}));
    }
    auto branch = add(unit, ScopeId{0}, If{}, std::move(children));
    auto root = add(unit, ScopeId{0}, Block{}, {branch});
    m.functions = {{f, signature, {}, root, ScopeId{0}, source}};
    require(verify(m).success(), "known bool branch return rejected");
    if (!with_else) {
      m.nodes[condition.value].data = BoolLiteral{false};
      require(!verify(m).success(), "false if without else incorrectly terminates");
      ++cases;
    }
  }
}
void numeric_conversion_tests() {
  Fixture f(1);
  Type t;
  t.kind = TypeKind::Float;
  t.bits = 64;
  const auto f64 = f.m.types.intern(t);
  t.bits = 32;
  const auto f32 = f.m.types.intern(t);
  // Nodes are appended to the module under test, not to the fixture arena.
  auto add = [&](HirModule &m, TypeId type, NodeData data,
                 std::vector<HirNodeId> children = {}) {
    HirNodeId id{m.nodes.size()};
    m.nodes.push_back({type, f.main_src, f.main_scope, std::move(children),
                       std::move(data)});
    return id;
  };
  auto with_source = [&](HirModule &m, HirNodeId id, Source source) {
    m.nodes[id.value].source = source;
    return id;
  };
  auto literal = [&](HirModule &m, TypeId type, uint64_t bits, Source source) {
    return with_source(m, add(m, type, FloatLiteral{bits}), source);
  };
  auto read_x = [&](HirModule &m, Source source = {}) {
    auto id = add(m, f.i32, Read{f.x});
    if (source.span.snapshot.value)
      m.nodes[id.value].source = source;
    return id;
  };
  // A discarded expression statement constrains no result type, so these nodes
  // exercise the conversion rule itself rather than a consumer's expectation.
  auto add_convert = [&](HirModule &m, TypeId type, HirNodeId source,
                         Source conversion_source) {
    auto convert = add(m, type, IntegerConvert{}, {source});
    m.nodes[convert.value].source = conversion_source;
    auto statement =
        add(m, f.unit, ExpressionStatement{}, {convert});
    m.nodes[statement.value].source = conversion_source;
    auto &children = m.nodes[f.root.value].children;
    children.insert(children.end() - 1, statement);
    return convert;
  };

  const auto convs_src = f.main_src;
  const auto &file =
      *f.project->modules[convs_src.node.module.value].files[convs_src.node.file];
  struct CallSource {
    Source conversion, operand;
  };
  auto ast_source = [&](AstNodeId id) {
    auto ref = convs_src.node;
    ref.node = id;
    return Source{ref, file.node(id).span};
  };
  auto call_source = [&](std::string_view callee_name,
                         std::string_view operand_spelling) {
    for (size_t i = 0; i < file.nodes.size(); ++i) {
      const auto &call = file.nodes[i];
      if (call.kind != AstKind::Call || call.children.size() != 2 ||
          call.span.begin_byte < convs_src.span.begin_byte ||
          call.span.end_byte > convs_src.span.end_byte)
        continue;
      const auto &callee = file.node(call.children[0]);
      if (callee.kind != AstKind::Name ||
          file.tokens[callee.token_begin].spelling != callee_name)
        continue;
      const auto &argument = file.node(call.children[1]);
      if (argument.kind != AstKind::Argument || argument.children.size() != 1)
        continue;
      const auto &operand = file.node(argument.children[0]);
      if (file.tokens[operand.token_begin].spelling != operand_spelling)
        continue;
      return CallSource{ast_source(AstNodeId{i}),
                        ast_source(argument.children[0])};
    }
    throw std::runtime_error("fixture conversion call missing");
  };
  const auto int_to_float_call = call_source("f64", "x");
  const auto float_to_int_call = call_source("i32", "1.0");
  const auto identity32_call = call_source("f32", "xf");
  const auto identity64_call = call_source("f64", "xd");
  const auto widening_call = call_source("f64", "xf");
  const auto narrowing_call = call_source("f32", "xd");

  {
    auto m = f.m;
    auto source = read_x(m, int_to_float_call.operand);
    add_convert(m, f64, source, int_to_float_call.conversion);
    auto r = verify(m);
    if (!r.success())
      for (const auto &d : r.diagnostics)
        std::cerr << "int->float: " << d.message << '\n';
    require(r.success(), "integer to float conversion rejected");
  }
  {
    auto m = f.m;
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          float_to_int_call.operand);
    add_convert(m, f.i32, source, float_to_int_call.conversion);
    require(verify(m).success(), "float to integer conversion rejected");
  }
  for (const auto &[type, source_type, bits, call] :
       std::vector<std::tuple<TypeId, TypeId, uint64_t, CallSource>>{
           {f32, f32, 0x3f800000ULL, identity32_call},
           {f64, f64, 0x3ff0000000000000ULL, identity64_call},
           {f64, f32, 0x3f800000ULL, widening_call},
           {f32, f64, 0x3ff0000000000000ULL, narrowing_call}}) {
    auto m = f.m;
    auto source = literal(m, source_type, bits, call.operand);
    add_convert(m, type, source, call.conversion);
    require(verify(m).success(), "float conversion rejected");
  }
  invalid(f, "identity target changed to f32", [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          identity64_call.operand);
    auto convert = add_convert(m, f64, source, identity64_call.conversion);
    m.nodes[convert.value].type = f32;
  });
  invalid(f, "bool conversion destination", [&](HirModule &m) {
    add_convert(m, f.boolean, read_x(m), identity64_call.conversion);
  });
  invalid(f, "bool conversion source", [&](HirModule &m) {
    auto source = add(m, f.boolean, BoolLiteral{true});
    add_convert(m, f.i32, source, identity64_call.conversion);
  });
  invalid(f, "conversion arity", [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          identity64_call.operand);
    auto convert = add(m, f64, IntegerConvert{}, {source, source});
    m.nodes[convert.value].source = identity64_call.conversion;
  });

  auto source_mutation = [&](const char *name,
                             const std::function<void(HirModule &)> &edit) {
    auto m = f.m;
    edit(m);
    require(!hir::verify_types(m).success(),
            (std::string("direct verify_types accepted ") + name).c_str());
    require(!verify(m).success(),
            (std::string("public verify accepted ") + name).c_str());
    auto ownership = hir::detail::produce_ownership(m, {});
    require(ownership.success(),
            (std::string("successful ownership recompute required: ") +
             name).c_str());
    require(!hir::verify_types(m).success(),
            (std::string("verify_types accepted after recompute: ") +
             name).c_str());
    require(!verify(m).success(),
            (std::string("public verify accepted after recompute: ") +
             name).c_str());
  };
  source_mutation("conversion callee replaced", [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          identity64_call.operand);
    auto convert = add_convert(m, f64, source, identity64_call.conversion);
    m.nodes[convert.value].source = identity32_call.conversion;
  });
  source_mutation("conversion operand replaced by same-type operand",
                  [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          identity64_call.operand);
    auto convert = add_convert(m, f64, source, identity64_call.conversion);
    m.nodes[source.value].source = widening_call.operand;
    (void)convert;
  });
  source_mutation("conversion source is not a call", [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          identity64_call.operand);
    auto convert = add_convert(m, f64, source, identity64_call.conversion);
    m.nodes[convert.value].source = f.main_src;
  });
  source_mutation("conversion operand span shortened", [&](HirModule &m) {
    auto source = literal(m, f32, 0x3f800000ULL, identity32_call.operand);
    add_convert(m, f32, source, identity32_call.conversion);
    --m.nodes[source.value].source.span.end_byte;
  });
  source_mutation("narrowing target differs from source callee",
                  [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          narrowing_call.operand);
    auto convert = add_convert(m, f32, source, identity64_call.conversion);
    (void)convert;
  });
  source_mutation("narrowing operand span shortened", [&](HirModule &m) {
    auto source = literal(m, f64, 0x3ff0000000000000ULL,
                          narrowing_call.operand);
    add_convert(m, f32, source, narrowing_call.conversion);
    --m.nodes[source.value].source.span.end_byte;
  });
}
} // namespace
int main() {
  try {
    numeric_conversion_tests();
    inout_tests();
    simple_flow_tests();
    {
      Fixture infinite(0, true);
      require(verify(infinite.m).success(),
              "unreachable break made infinite loop normally terminate");
      invalid(infinite, "reachable loop break requires return", [&](auto &m) {
        m.nodes[infinite.cont.value].data = Noop{};
      });
    }
    {
      Fixture infinite(0, true, true);
      require(verify(infinite.m).success(),
              "statically false branch break made infinite loop terminate");
      invalid(infinite, "statically true loop break requires return", [&](auto &m) {
        auto body = m.nodes[infinite.loop.value].children[3];
        auto branch = m.nodes[body.value].children[0];
        auto condition = m.nodes[branch.value].children[0];
        m.nodes[condition.value].data = BoolLiteral{true};
      });
    }
    Fixture f;
    auto r = verify(f.m);
    if (!r.success())
      for (auto &d : r.diagnostics)
        std::cerr << d.message << '\n';
    require(r.success(), "valid scalar function/call/loop rejected");
    require(dump(*r.value) == dump(f.m), "frozen dump differs");
    require(dump(*r.value) == dump(*r.value), "dump nondeterminism");
    invalid(f, "missing type",
            [&](auto &m) { m.nodes[f.literal.value].type = TypeId{}; });
    invalid(f, "bad type",
            [&](auto &m) { m.nodes[f.literal.value].type = TypeId{9999}; });
    invalid(f, "recovered owning AST", [&](auto &m) {
      auto project = std::make_shared<modules::Project>(*f.project);
      auto ast = std::make_shared<AstFile>(*project->modules[0].files[0]);
      ast->recovered = true;
      project->modules[0].files[0] = ast;
      auto names = std::make_shared<modules::ResolvedProject>(*f.resolved);
      names->project = project;
      m.resolved = names;
    });
    invalid(f, "writable let binding", [&](auto &m) {
      auto code = f.project->modules[0].files[0]->source->original;
      code.replace(code.find("var x"), 3, "let");
      SourceManager sources;
      auto source = sources.update(sources.add_file("fixture.wz"), code);
      auto tokens = lex(source);
      require(tokens.success(), "let fixture lexer");
      auto parsed = parse(*tokens.value);
      require(parsed.success(), "let fixture parser");
      auto project = std::make_shared<modules::Project>(*f.project);
      project->modules[0].files[0] = std::make_shared<const AstFile>(std::move(*parsed.value));
      auto names = modules::resolve_project(project);
      require(names.success(), "let fixture resolver");
      m.resolved = std::make_shared<const modules::ResolvedProject>(std::move(*names.value));
    });
    invalid(f, "bad symbol", [&](auto &m) {
      std::get<Read>(m.nodes[f.read.value].data).symbol = SymbolId{9999};
    });
    invalid(f, "bad node", [&](auto &m) {
      m.nodes[f.root.value].children[0] = HirNodeId{9999};
    });
    invalid(f, "cycle",
            [&](auto &m) { m.nodes[f.root.value].children.push_back(f.root); });
    invalid(f, "shared executable", [&](auto &m) {
      m.nodes[f.root.value].children.push_back(f.binding);
    });
    invalid(f, "source node", [&](auto &m) {
      m.nodes[f.literal.value].source.node.node = AstNodeId{9999};
    });
    invalid(f, "source span", [&](auto &m) {
      m.nodes[f.literal.value].source.span.end_byte = UINT64_MAX;
    });
    invalid(f, "bad scope",
            [&](auto &m) { m.nodes[f.literal.value].scope = ScopeId{9999}; });
    invalid(f, "scope cycle", [&](auto &m) {
      m.scopes[f.main_scope.value].parent = f.main_scope;
    });
    invalid(f, "wrong function scope",
            [&](auto &m) { m.scopes[f.body_scope.value].function = f.add; });
    invalid(f, "bad call parameter", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args[0].parameter_index = 2;
    });
    invalid(f, "duplicate call parameter", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args[0].parameter_index = 0;
    });
    invalid(f, "call incomplete", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args.pop_back();
    });
    invalid(f, "call source index", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args[0].source_index = 1;
    });
    invalid(f, "call child order", [&](auto &m) {
      std::swap(m.nodes[f.call.value].children[0],
                m.nodes[f.call.value].children[1]);
    });
    invalid(f, "call label", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args[0].label = "a";
    });
    invalid(f, "function value label", [&](auto &m) {
      std::get<Call>(m.nodes[f.value_call.value].data).args[0].label = "a";
    });
    invalid(f, "call mode", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).args[0].mode = ParamMode::move;
    });
    invalid(f, "call result",
            [&](auto &m) { m.nodes[f.call.value].type = f.boolean; });
    invalid(f, "call signature", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).signature = f.main_signature;
    });
    invalid(f, "callee symbol kind", [&](auto &m) {
      std::get<Call>(m.nodes[f.call.value].data).direct_symbol = f.x;
    });
    invalid(f, "bad jump node", [&](auto &m) {
      std::get<Break>(m.nodes[f.jump.value].data).loop = HirNodeId{9999};
    });
    invalid(f, "jump kind", [&](auto &m) {
      std::get<Break>(m.nodes[f.jump.value].data).loop = f.root;
    });
    invalid(f, "return function", [&](auto &m) {
      std::get<Return>(m.nodes[f.add_return.value].data).function = f.main;
    });
    invalid(f, "continue target", [&](auto &m) {
      std::get<Continue>(m.nodes[f.cont.value].data).loop = f.root;
    });
    invalid(f, "Error node", [&](auto &m) {
      m.nodes[f.literal.value].data = Unsupported{"Error"};
    });
    invalid(f, "NeutralBracket node", [&](auto &m) {
      m.nodes[f.literal.value].data = Unsupported{"NeutralBracket"};
    });
    invalid(f, "Error type", [&](auto &m) {
      Type t;
      m.nodes[f.literal.value].type = m.types.intern(t);
    });
    invalid(f, "nominal Error is not Any", [&](auto &m) {
      Type t;
      t.kind = TypeKind::NominalEnum;
      m.nodes[f.literal.value].type = m.types.intern(t);
    });
    invalid(f, "throws", [&](auto &m) {
      Type t = *m.types.find(f.signature);
      t.throws_type = f.i32;
      m.functions[0].signature = m.types.intern(t);
    });
    invalid(f, "inout signature", [&](auto &m) {
      Type t = *m.types.find(f.signature);
      t.parameters[0].mode = ParamMode::inout;
      m.functions[0].signature = m.types.intern(t);
    });
    invalid(f, "return type",
            [&](auto &m) { m.nodes[f.conversion.value].type = f.boolean; });
    invalid(f, "missing return", [&](auto &m) {
      auto ret = m.nodes[f.root.value].children.back();
      m.nodes[ret.value].data = ExpressionStatement{};
    });
    invalid(f, "parameter count",
            [&](auto &m) { m.functions[0].parameters.pop_back(); });
    invalid(f, "parameter identity",
            [&](auto &m) { m.functions[0].parameters[0].symbol = f.x; });
    invalid(f, "immutable place", [&](auto &m) {
      auto init = m.nodes[f.loop.value].children[0];
      std::get<Binding>(m.nodes[init.value].data).writable = false;
    });
    invalid(f, "initialization order", [&](auto &m) {
      auto &ch = m.nodes[f.root.value].children;
      std::swap(ch[0], ch[1]);
    });
    invalid(f, "literal range", [&](auto &m) {
      std::get<IntegerLiteral>(m.nodes[f.literal.value].data) = {"2147483648",
                                                                 2147483648};
    });
    invalid(f, "literal bits", [&](auto &m) {
      std::get<IntegerLiteral>(m.nodes[f.literal.value].data).bits = 1;
    });
    invalid(f, "literal canonical", [&](auto &m) {
      std::get<IntegerLiteral>(m.nodes[f.literal.value].data).decimal = "00";
    });
    invalid(f, "bad backend",
            [&](auto &m) { m.config.backend = static_cast<Backend>(99); });
    invalid(f, "bad word width", [&](auto &m) { m.config.word_bits = 16; });
    invalid(f, "project target mismatch",
            [&](auto &m) { m.config.word_bits = 32; });
    invalid(f, "null owner", [&](auto &m) { m.resolved.reset(); });
    invalid(f, "call argument type", [&](auto &m) {
      auto arg = std::get<Call>(m.nodes[f.call.value].data).args[0].expression;
      m.nodes[arg.value].data = BoolLiteral{true};
      m.nodes[arg.value].type = f.boolean;
    });
    invalid(f, "cross function jump", [&](auto &m) {
      auto &ret = m.nodes[f.add_return.value];
      ret.data = ExpressionStatement{};
      HirNodeId bad{m.nodes.size()};
      m.nodes.push_back({f.unit, f.add_src, f.add_scope, {}, Break{f.loop}});
      auto add_body = m.functions[0].body;
      m.nodes[add_body.value].children.push_back(bad);
    });
    invalid(f, "wrong local type",
            [&](auto &m) { m.nodes[f.read.value].type = f.boolean; });
    invalid(f, "wrong source module", [&](auto &m) {
      m.nodes[f.read.value].source.node.module = ModuleId{9999};
    });
    invalid(f, "wrong source file",
            [&](auto &m) { m.nodes[f.read.value].source.node.file = 9999; });
    invalid(f, "scope source outside function",
            [&](auto &m) { m.scopes[f.body_scope.value].source = f.add_src; });
    invalid(f, "orphan node", [&](auto &m) {
      m.nodes.push_back({f.unit, f.main_src, f.main_scope, {}, Noop{}});
    });
    invalid(f, "orphan scope", [&](auto &m) {
      m.scopes.push_back({f.main, f.main_scope, f.main_src});
    });
    // An AST arena can have a writable alias during construction. Publishing
    // must break that alias too, not just freeze HirModule's own vectors.
    {
      auto m = f.m;
      auto project_copy = std::make_shared<modules::Project>(*f.project);
      auto file = std::make_shared<AstFile>(*project_copy->modules[0].files[0]);
      project_copy->modules[0].files[0] = file;
      auto resolved_copy =
          std::make_shared<modules::ResolvedProject>(*f.resolved);
      resolved_copy->project = project_copy;
      m.resolved = resolved_copy;
      auto frozen = verify(m);
      require(frozen.success(), "mutable AST fixture rejected");
      file->nodes[f.main_src.node.node.value].kind = AstKind::NeutralBracket;
      require(!verify(m).success() && verify(frozen.value->module()).success(),
              "AST alias mutated verified data");
    }
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = verify(f.m, stop.get_token());
    require(cancelled.state == PhaseState::cancelled && !cancelled.value &&
                cancelled.exit_code() == ExitCode::cancelled &&
                int(cancelled.exit_code()) == 130,
            "cancellation");
    // All four backend/word-width identities are independently verified.
    for (auto backend : {Backend::native, Backend::python})
      for (unsigned width : {32u, 64u}) {
        auto m = f.m;
        auto project_copy = std::make_shared<modules::Project>(*f.project);
        project_copy->backend =
            backend == Backend::native ? "native" : "python";
        project_copy->word_bits = width;
        auto resolved_copy =
            std::make_shared<modules::ResolvedProject>(*f.resolved);
        resolved_copy->project = project_copy;
        m.resolved = resolved_copy;
        m.config = {backend, width};
        Type target;
        target.kind = TypeKind::Isize;
        auto isize = m.types.intern(target);
        target.kind = TypeKind::Usize;
        auto usize = m.types.intern(target);
        require(isize != usize &&
                    bit_width(*m.types.find(isize), m.config) == width &&
                    bit_width(*m.types.find(usize), m.config) == width,
                "target width/identity");
        HirNodeId literal_id{m.nodes.size()};
        m.nodes.push_back({isize,
                           f.main_src,
                           f.main_scope,
                           {},
                           IntegerLiteral{"2147483647", 2147483647}});
        HirNodeId statement_id{m.nodes.size()};
        m.nodes.push_back({f.unit,
                           f.main_src,
                           f.main_scope,
                           {literal_id},
                           ExpressionStatement{}});
        auto &children = m.nodes[f.root.value].children;
        children.insert(children.begin(), statement_id);
        auto valid = verify(m);
        require(valid.success() && valid.value->config() == m.config,
                "target profile rejected/lost");
        m.nodes[literal_id.value].data =
            IntegerLiteral{"2147483648", 2147483648};
        require(verify(m).success() == (width == 64),
                "isize range used host width");
        m.nodes[literal_id.value].type = usize;
        m.nodes[literal_id.value].data =
            IntegerLiteral{"4294967296", 4294967296};
        require(verify(m).success() == (width == 64),
                "usize range used host width");
      }
    // Verify additional scalar node shapes without a source-language checker.
    {
      auto m = f.m;
      auto add_node = [&](TypeId type, NodeData data,
                          std::vector<HirNodeId> children = {}) {
        HirNodeId id{m.nodes.size()};
        m.nodes.push_back({type, f.main_src, f.main_scope, std::move(children),
                           std::move(data)});
        return id;
      };
      auto add_stmt = [&](HirNodeId expr) {
        auto id = add_node(f.unit, ExpressionStatement{}, {expr});
        auto &children = m.nodes[f.root.value].children;
        children.insert(children.end() - 1, id);
      };
      auto left = add_node(f.boolean, BoolLiteral{false});
      auto right = add_node(f.boolean, BoolLiteral{true});
      auto logical =
          add_node(f.boolean, ShortCircuit{LogicalOp::or_}, {left, right});
      add_stmt(add_node(f.boolean, Unary{UnaryOp::logical_not}, {logical}));
      auto int_lit = add_node(f.i32, IntegerLiteral{"-2147483648", 2147483648});
      add_stmt(add_node(f.i32, Unary{UnaryOp::negate}, {int_lit}));
      Type t;
      t.kind = TypeKind::Float;
      t.bits = 32;
      auto f32 = m.types.intern(t);
      add_stmt(add_node(f32, FloatLiteral{0x80000000}));
      t.kind = TypeKind::String;
      t.bits = 0;
      auto string = m.types.intern(t);
      // String has owned storage; adding it to this synthetic scalar graph
      // correctly requires an exact source body. Literal sources are tested
      // through the source checker instead of this scalar-only builder.
      const auto string_ops = classify_value(m, string);
      require(string_ops.kind == ValueKind::string && string_ops.needs_drop &&
                  !string_ops.pod_inout,
              "String ownership classification changed");
      add_stmt(add_node(f.unit, UnitLiteral{}));
      const auto shapes = verify(m);
      if (!shapes.success()) {
        std::string message = "additional scalar shapes rejected";
        for (const auto &diagnostic : shapes.diagnostics)
          message += "\n" + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
      }
    }
    auto old_dump = dump(*r.value);
    f.m.nodes.clear();
    f.resolved->symbols.clear();
    f.project->modules.clear();
    require(dump(*r.value) == old_dump && verify(r.value->module()).success(),
            "retained mutable aliases changed verified snapshot");
    std::cout << "HIR passed: valid function/direct+value calls/loop, " << cases
              << " corruptions, cancellation, immutable snapshot, "
                 "deterministic dump\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

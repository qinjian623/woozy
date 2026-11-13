// Test-only C++ oracle for the internal Python emitter helpers.
//
// The runner copies the current production src/backend/python/emitter.cpp,
// inserts one public: label after "class Emitter {" (the only change), and
// includes it here.  The same real HIR records are fed to both emitters; every
// successful line is prefixed OK| so a caught exception can never be compared
// as a generated module.
#include "cpp_emitter_public.cpp"
#include <iostream>
using namespace lang;
using namespace lang::hir;
using namespace lang::backend::python;

HirNode nd(TypeId t, NodeData d,
           std::vector<HirNodeId> children = {}) {
    HirNode n{};
    n.type = t;
    n.data = std::move(d);
    n.children = std::move(children);
    n.scope = ScopeId{0};
    n.source = {{ModuleId{0}, 0, AstNodeId{0}}, {SourceSnapshotId{1}, 10, 20}};
    return n;
}

void show(HirModule module, std::vector<HirNode> nodes, size_t id,
          const std::string &label, bool statement) {
    module.nodes = std::move(nodes);
    Emitter e(module, {});
    Suite suite;
    try {
        if (statement) {
            e.statement(HirNodeId{id}, suite);
        } else {
            e.expression(HirNodeId{id}, suite);
        }
        e.print(suite, 0);
        std::cout << label << "=OK|" << string_literal(e.output.code) << '\n';
    } catch (const std::exception &error) {
        std::cout << label << "=FAIL|internal|" << error.what() << '\n';
    }
}

int main() {
    HirModule m;
    auto project = std::make_shared<modules::Project>();
    project->backend = "python";
    project->word_bits = 64;
    auto resolved = std::make_shared<modules::ResolvedProject>();
    resolved->project = project;
    m.resolved = resolved;
    m.config = {Backend::python, 64};
    auto add = [&](TypeKind kind, unsigned bits = 0,
                   SymbolId decl = SymbolId{0},
                   std::vector<TypeId> args = {}) {
        Type t;
        t.kind = kind;
        t.bits = bits;
        t.declaration = decl;
        t.type_args = std::move(args);
        return m.types.intern(std::move(t));
    };
    auto unit = add(TypeKind::Unit);
    add(TypeKind::Bool);
    auto str_type = add(TypeKind::String);
    add(TypeKind::Isize);
    auto uz = add(TypeKind::Usize);
    auto i8 = add(TypeKind::SignedInt, 8);
    auto u32 = add(TypeKind::UnsignedInt, 32);
    auto f32 = add(TypeKind::Float, 32);
    add(TypeKind::Float, 64);
    auto nominal = add(TypeKind::NominalStruct);
    add(TypeKind::NominalStruct, 0, SymbolId{0}, {i8});
    auto vec = add(TypeKind::Vec, 0, SymbolId{0}, {i8});
    auto slice = add(TypeKind::Slice, 0, SymbolId{0}, {vec});
    auto extra_nominal = add(TypeKind::NominalStruct, 0, SymbolId{5});
    NominalDecl decl{};
    decl.symbol = SymbolId{5};
    decl.kind = TypeKind::NominalStruct;
    FieldDecl field{};
    field.symbol = SymbolId{7};
    field.type = i8;
    field.exported = true;
    decl.fields.push_back(field);
    m.nominals.push_back(decl);
    m.ownership.locals.push_back({SymbolId{88}, ScopeId{0}, u32, 0,
                                  ParamMode::ordinary, false});

    auto lit = nd(i8, IntegerLiteral{"5", 5});
    auto second = nd(i8, IntegerLiteral{"2", 2});
    show(m, {lit}, 0, "control_integer", false);

    auto node = nd(i8, Unary{UnaryOp::negate}, {HirNodeId{0}});
    show(m, {lit, node}, 1, "unary_node0", false);
    node.children = {HirNodeId{1}};
    show(m, {nd(unit, Noop{}), lit, node}, 2, "unary_node1", false);

    node = nd(i8, Binary{BinaryOp::add}, {HirNodeId{0}, HirNodeId{1}});
    show(m, {lit, second, node}, 2, "binary_node0", false);
    node = nd(i8, Binary{BinaryOp::add}, {HirNodeId{1}, HirNodeId{2}});
    show(m, {nd(unit, Noop{}), lit, second, node}, 3, "binary_node12",
         false);

    node = nd(u32, IntegerConvert{}, {HirNodeId{0}});
    show(m, {lit, node}, 1, "convert_node0", false);
    auto float_lit = nd(f32, FloatLiteral{0});
    node = nd(i8, IntegerConvert{}, {HirNodeId{1}});
    show(m, {nd(unit, Noop{}), float_lit, node}, 2, "convert_node1",
         false);

    auto text = nd(str_type, StringLiteral{"é"});
    second = nd(uz, IntegerLiteral{"2", 2});
    node = nd(u32, IndexRead{}, {HirNodeId{0}, HirNodeId{1}});
    show(m, {text, second, node}, 2, "index_node0", false);
    node.children = {HirNodeId{1}, HirNodeId{2}};
    show(m, {nd(unit, Noop{}), text, second, node}, 3, "index_node1",
         false);

    auto read = nd(slice, Read{SymbolId{99}});
    node = nd(slice, VecSlice{}, {HirNodeId{0}});
    show(m, {read, node}, 1, "slice_node0", false);
    node.children = {HirNodeId{1}};
    show(m, {nd(unit, Noop{}), read, node}, 2, "slice_node1", false);

    read = nd(extra_nominal, Read{SymbolId{99}});
    node = nd(i8, FieldRead{SymbolId{7}}, {HirNodeId{0}});
    show(m, {read, node}, 1, "field_node0", false);
    node.children = {HirNodeId{1}};
    show(m, {nd(unit, Noop{}), read, node}, 2, "field_node1", false);

    auto body = nd(unit, Noop{});
    ForIn for_in{};
    for_in.element = SymbolId{88};
    for_in.body_scope = ScopeId{0};
    for_in.body = HirNodeId{1};
    node = nd(unit, for_in, {HirNodeId{0}, HirNodeId{1}});
    show(m, {text, body, node}, 2, "for_in_node0", true);
    for_in.body = HirNodeId{2};
    node = nd(unit, for_in, {HirNodeId{1}, HirNodeId{2}});
    show(m, {nd(unit, Noop{}), text, body, node}, 3, "for_in_node1", true);

    read = nd(slice, Read{SymbolId{99}});
    for_in.body = HirNodeId{1};
    node = nd(unit, for_in, {HirNodeId{0}, HirNodeId{1}});
    show(m, {read, body, node}, 2, "for_in_slice_node0", true);
    for_in.body = HirNodeId{2};
    node = nd(unit, for_in, {HirNodeId{1}, HirNodeId{2}});
    show(m, {nd(unit, Noop{}), read, body, node}, 3, "for_in_slice_node1",
         true);

    auto place = nd(i8, Place{SymbolId{77}});
    auto three = nd(i8, IntegerLiteral{"3", 3});
    node = nd(i8, Assign{BinaryOp::add}, {HirNodeId{0}, HirNodeId{1}});
    show(m, {place, three, node}, 2, "compound_assign_node0", true);
    node = nd(i8, Assign{BinaryOp::add}, {HirNodeId{1}, HirNodeId{2}});
    show(m, {nd(unit, Noop{}), place, three, node}, 3,
         "compound_assign_node1", true);
}

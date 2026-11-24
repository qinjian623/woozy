#include "lang/backend/native.hpp"
#include "lang/ir/native.hpp"
#include "lang/sema/check.hpp"
#include "../../src/hir/ownership.hpp"
#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <stdexcept>

using namespace lang;
namespace fs = std::filesystem;
namespace {
void need(bool ok, const std::string &message) {
  if (!ok)
    throw std::runtime_error(message);
}
std::string diagnostics(const auto &r) {
  std::string out;
  for (const auto &d : r.diagnostics)
    out += d.code + ": " + d.message + "\n";
  return out;
}
hir::VerifiedHirModule checked_module(const fs::path &manifest, SourceManager &sources) {
  auto project = modules::load_project({manifest, {}, {}}, sources);
  need(project.success(), diagnostics(project));
  project.value->backend = "native";
  auto resolved = modules::resolve_project(
      std::make_shared<const modules::Project>(std::move(*project.value)));
  need(resolved.success(), diagnostics(resolved));
  auto checked = sema::check_project(resolved);
  need(checked.success(), diagnostics(checked));
  return *checked.value;
}
bool is_error(const auto &result, std::string code = "E_IR_INVALID") {
  if (result.success())
    return false;
  return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [&](const auto &d) { return d.code == code; });
}
HirNodeId find_kind(const hir::HirModule &m, hir::NodeKind kind,
                    const std::function<bool(const hir::HirNode &)> &accept = {}) {
  for (size_t i = 0; i < m.nodes.size(); ++i)
    if (m.nodes[i].kind() == kind && (!accept || accept(m.nodes[i])))
      return HirNodeId{i};
  throw std::runtime_error("probe node missing: " + std::to_string(int(kind)));
}
HirNodeId assignment_index_place(const hir::HirModule &m, HirNodeId place) {
  const auto &node = m.nodes[place.value];
  if (node.kind() == hir::NodeKind::index_place)
    return place;
  if (node.kind() == hir::NodeKind::field_place && !node.children.empty())
    return assignment_index_place(m, node.children[0]);
  throw std::runtime_error("assignment index place missing");
}
hir::UseKind receiver_use(const hir::HirModule &m, HirNodeId receiver) {
  for (const auto &use : m.ownership.uses)
    if (use.consumer == receiver && use.operand == 0)
      return use.kind;
  throw std::runtime_error("receiver ownership use missing");
}
void collect_assignments(const hir::HirModule &m, HirNodeId id,
                         std::vector<HirNodeId> &out) {
  const auto &node = m.nodes[id.value];
  if (node.kind() == hir::NodeKind::assign)
    out.push_back(id);
  for (auto child : node.children)
    collect_assignments(m, child, out);
}
void require_field_receiver_plan(const hir::HirModule &m,
                                 const std::string &name,
                                 std::vector<hir::UseKind> expected) {
  HirNodeId body;
  bool found_main = false;
  for (const auto &function : m.functions)
    if (m.resolved->symbols[function.symbol.value].name == "main") {
      body = function.body;
      found_main = true;
      break;
    }
  need(found_main, name + ": main body missing");
  std::vector<HirNodeId> assignments;
  collect_assignments(m, body, assignments);
  need(assignments.size() == expected.size(),
       name + ": assignment count differs: got " +
           std::to_string(assignments.size()) + ", expected " +
           std::to_string(expected.size()));
  for (size_t i = 0; i < assignments.size(); ++i) {
    const auto &assign = m.nodes[assignments[i].value];
    auto indexed = assignment_index_place(m, assign.children[0]);
    auto receiver = m.nodes[indexed.value].children[0];
    auto actual = receiver_use(m, receiver);
    need(actual == expected[i], name + ": receiver use differs");
  }
}
SymbolId symbol_named(const hir::HirModule &m, std::string_view name) {
  for (const auto &s : m.resolved->symbols)
    if (s.name == name)
      return s.id;
  throw std::runtime_error("probe symbol missing: " + std::string(name));
}
TypeId type_kind(const hir::HirModule &m, hir::TypeKind kind) {
  for (uint64_t id = 1; id <= m.types.size(); ++id)
    if (m.types.find(TypeId{id})->kind == kind)
      return TypeId{id};
  throw std::runtime_error("probe type missing");
}
void bad_hir(const hir::HirModule &original, const std::string &name,
             const std::function<void(hir::HirModule &)> &edit) {
  auto m = original;
  edit(m);
  auto result = hir::verify(m);
  need(is_error(result), "damaged HIR accepted: " + name + "\n" +
                             diagnostics(result));
  std::cout << "bad_hir " << name << " passed\n";
}
void bad_hir_recomputed(const hir::HirModule &original,
                        const std::string &name,
                        const std::function<void(hir::HirModule &)> &edit) {
  auto m = original;
  need(hir::verify(m).success(), "valid HIR control rejected");
  edit(m);
  need(is_error(hir::verify_types(m)),
       "direct verify_types accepted damaged HIR: " + name);
  need(is_error(hir::verify(m)),
       "direct public verify accepted damaged HIR: " + name);
  auto ownership = hir::detail::produce_ownership(m, {});
  need(ownership.success(), "successful ownership recompute required: " +
                                name + "\n" + diagnostics(ownership));
  need(is_error(hir::verify_types(m)),
       "verify_types accepted damaged HIR after ownership recompute: " + name);
  need(is_error(hir::verify(m)),
       "public verify accepted damaged HIR after ownership recompute: " + name);
  std::cout << "bad_hir_recomputed " << name << " passed\n";
}
void bad_native(const ir::Module &original, const std::string &name,
                const std::function<void(ir::Module &)> &edit) {
  auto m = original;
  edit(m);
  auto result = ir::verify(m);
  need(is_error(result), "damaged NativeIR accepted: " + name + "\n" +
                             diagnostics(result));
  std::cout << "bad_native " << name << " passed\n";
}
ir::Instruction &instruction(ir::Module &m, ir::Op op, size_t skip = 0) {
  for (auto &f : m.functions)
    for (auto &b : f.blocks)
      for (auto &i : b.instructions)
        if (i.op == op && !skip--)
          return i;
  throw std::runtime_error("probe instruction missing: " +
                           std::to_string(int(op)));
}
void move_receiver_cleanup_before(ir::Module &m, ir::Op target_op) {
  for (auto &f : m.functions)
    for (auto &b : f.blocks) {
      for (size_t target = 0; target < b.instructions.size(); ++target) {
        if (b.instructions[target].op != target_op)
          continue;
        for (size_t begin = target + 1; begin < b.instructions.size(); ++begin) {
          if (b.instructions[begin].op != ir::Op::ownership_begin)
            continue;
          size_t end = begin + 1;
          while (end < b.instructions.size() &&
                 b.instructions[end].op != ir::Op::ownership_end)
            ++end;
          need(end < b.instructions.size(), "cleanup end missing");
          bool has_destroy = false;
          for (size_t i = begin; i <= end; ++i)
            has_destroy |= b.instructions[i].op == ir::Op::destroy;
          if (!has_destroy)
            continue;
          std::vector<ir::Instruction> batch(
              b.instructions.begin() + begin,
              b.instructions.begin() + end + 1);
          b.instructions.erase(b.instructions.begin() + begin,
                               b.instructions.begin() + end + 1);
          b.instructions.insert(b.instructions.begin() + target,
                                batch.begin(), batch.end());
          return;
        }
      }
    }
  throw std::runtime_error("assignment receiver cleanup batch missing");
}
void move_receiver_cleanup_before_store(ir::Module &m) {
  move_receiver_cleanup_before(m, ir::Op::store_indirect);
}
} // namespace

int main(int argc, char **argv) {
  try {
    need(argc == 2, "usage: indexed_field_probe ROOT");
    const fs::path root = argv[1];
    SourceManager sources;
    const auto nested_manifest =
        root / "tests/native/ownership/indexed-field-nested/lang.toml";
    const auto mutation_manifest =
        root / "tests/native/ownership/indexed-field-mutations/lang.toml";
    const auto generic_manifest =
        root / "tests/native/ownership/indexed-field-generic/lang.toml";
    const auto assign_manifest =
        root / "tests/native/ownership/indexed-field-assign-source/lang.toml";
    const auto borrow_mixed_manifest =
        root / "tests/native/ownership/indexed-field-borrow-mixed/lang.toml";
    const auto lifetime_manifest =
        root / "tests/native/ownership/indexed-field-lifetime/lang.toml";
    const auto nested_fields_manifest =
        root / "tests/native/ownership/indexed-field-nested-fields/lang.toml";
    const auto nested_fallback_manifest =
        root / "tests/native/ownership/indexed-field-nested-fallback/lang.toml";
    const auto compound_manifest =
        root / "tests/native/ownership/indexed-field-compound/lang.toml";

    auto valid = checked_module(nested_manifest, sources);
    const auto valid_hir = valid.module();
    auto valid_result = hir::verify(valid_hir);
    need(valid_result.success(), diagnostics(valid_result));
    auto field_place = find_kind(
        valid_hir, hir::NodeKind::field_place,
        [](const auto &n) { return std::get<hir::FieldPlace>(n.data).field ==
                                   SymbolId{0} || true; });
    auto index_place = find_kind(valid_hir, hir::NodeKind::index_place);
    auto names = [&](const hir::HirModule &m) {
      std::map<std::string, SymbolId> result;
      for (const auto &s : m.resolved->symbols)
        if (s.kind == modules::SymbolKind::field)
          result.emplace(s.name, s.id);
      return result;
    };
    auto fields = names(valid_hir);
    need(fields.contains("x") && fields.contains("y"), "fixture fields missing");
    bad_hir(valid_hir, "field symbol differs from source member", [&](auto &m) {
      for (auto &n : m.nodes)
        if (n.kind() == hir::NodeKind::field_place &&
            std::get<hir::FieldPlace>(n.data).field == fields.at("x")) {
          std::get<hir::FieldPlace>(n.data).field = fields.at("y");
          return;
        }
      throw std::runtime_error("field place target missing");
    });
    bad_hir(valid_hir, "field source is another member", [&](auto &m) {
      const auto &file =
          *m.resolved->project->module(valid_hir.nodes[field_place.value]
                                           .source.node.module)
               .files.at(valid_hir.nodes[field_place.value].source.node.file);
      std::optional<modules::NodeRef> other;
      for (size_t i = 0; i < file.nodes.size(); ++i)
        if (file.node(AstNodeId{i}).kind == AstKind::Member &&
            file.tokens.at(file.node(AstNodeId{i}).token_end - 1).spelling ==
                "y") {
          auto ref = valid_hir.nodes[field_place.value].source.node;
          ref.node = AstNodeId{i};
          other = ref;
          break;
        }
      need(other.has_value(), "source member target missing");
      m.nodes[field_place.value].source.node = *other;
    });
    bad_hir(valid_hir, "index receiver is another place root", [&](auto &m) {
      auto temp = symbol_named(m, "temp");
      auto read = find_kind(m, hir::NodeKind::read, [&](const auto &n) {
        return std::get<hir::Read>(n.data).symbol == temp;
      });
      auto index = find_kind(m, hir::NodeKind::index_place);
      m.nodes[index.value].children[0] = read;
    });
    bad_hir(valid_hir, "index element type differs from Vec", [&](auto &m) {
      auto index = find_kind(m, hir::NodeKind::index_place);
      m.nodes[index.value].type = type_kind(m, hir::TypeKind::SignedInt);
    });
    bad_hir(valid_hir, "index expression is not integer", [&](auto &m) {
      auto index = find_kind(m, hir::NodeKind::index_place);
      m.nodes[m.nodes[index.value].children[1].value].type =
          type_kind(m, hir::TypeKind::Bool);
    });
    bad_hir(valid_hir, "index source is not a real bracket", [&](auto &m) {
      auto index = find_kind(m, hir::NodeKind::index_place);
      m.nodes[index.value].source.node = m.nodes[field_place.value].source.node;
    });
    bad_hir(valid_hir, "ownership recomputation rejects stale records",
            [&](auto &m) {
              need(!m.ownership.full_expressions.empty(),
                   "fixture has no full-expression record");
              m.ownership.full_expressions.pop_back();
            });

    auto assign_source = checked_module(assign_manifest, sources);
    const auto assign_hir = assign_source.module();
    need(hir::verify(assign_hir).success(), "assign source fixture rejected");
    require_field_receiver_plan(
        assign_hir, "pure scalar RHS",
        {hir::UseKind::borrow, hir::UseKind::borrow});
    auto assign_ir = ir::lower(assign_source);
    need(assign_ir.success(), diagnostics(assign_ir));
    auto assign_llvm = backend::native::emit(
        *assign_ir.value, symbol_named(assign_hir, "main"), {});
    need(assign_llvm.success(), diagnostics(assign_llvm));
    const auto &before = assign_llvm.value->llvm_before_optimization;
    need(before.find("@lang_vec_at_v1") == std::string::npos,
         "a same-block checked trivial field address still calls the accessor");
    need(before.find("@lang_native_panic_v1(i32 4,") != std::string::npos,
         "the field address lost its index Panic check");
    std::vector<HirNodeId> assignments;
    for (size_t i = 0; i < assign_hir.nodes.size(); ++i)
      if (assign_hir.nodes[i].kind() == hir::NodeKind::assign)
        assignments.push_back(HirNodeId{i});
    need(assignments.size() == 2, "two source assignments required");
    bad_hir_recomputed(assign_hir, "complete LHS places exchanged", [&](auto &m) {
      std::swap(m.nodes[assignments[0].value].children[0],
                m.nodes[assignments[1].value].children[0]);
    });
    bad_hir_recomputed(assign_hir, "complete RHS expressions exchanged", [&](auto &m) {
      std::swap(m.nodes[assignments[0].value].children[1],
                m.nodes[assignments[1].value].children[1]);
    });

    auto generic = checked_module(generic_manifest, sources);
    bad_hir(generic.module(), "generic field substitution type mismatch", [&](auto &m) {
      auto place = find_kind(m, hir::NodeKind::field_place);
      m.nodes[place.value].type = type_kind(m, hir::TypeKind::SignedInt);
    });

    auto lowered = ir::lower(valid);
    need(lowered.success(), diagnostics(lowered));
    const auto native = lowered.value->module();
    auto verified = ir::verify(native);
    need(verified.success(), diagnostics(verified));
    bad_native(native, "missing bounds check", [](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks) {
          for (size_t i = 1; i < b.instructions.size(); ++i)
            if (b.instructions[i].op == ir::Op::vec_element_address &&
                b.instructions[i - 1].op == ir::Op::vec_check) {
              b.instructions.erase(b.instructions.begin() + i - 1);
              return;
            }
        }
      throw std::runtime_error("bounds/address pair missing");
    });
    bad_native(native, "address index differs from bounds check", [](auto &m) {
      auto &address = instruction(m, ir::Op::vec_element_address);
      address.inputs[1] = address.inputs[0];
    });
    bad_native(native, "field address SSA cycle", [](auto &m) {
      auto &address = instruction(m, ir::Op::field_address);
      need(address.result.has_value(), "field address result missing");
      address.inputs = {*address.result};
    });
    bad_native(native, "address receiver differs from HIR place", [](auto &m) {
      std::vector<std::pair<ir::Function *, ir::Instruction *>> addresses;
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::vec_element_address)
              addresses.push_back({&f, &i});
      need(addresses.size() >= 2, "two element addresses required");
      auto &second = *addresses[1].second;
      auto receiver = addresses[0].second->inputs[0];
      second.inputs[0] = receiver;
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (size_t i = 1; i < b.instructions.size(); ++i)
            if (&b.instructions[i] == &second &&
                b.instructions[i - 1].op == ir::Op::vec_check)
              b.instructions[i - 1].inputs[0] = receiver;
    });
    bad_native(native, "field address field differs from HIR", [&](auto &m) {
      auto &address = instruction(m, ir::Op::field_address);
      for (const auto &s : valid_hir.resolved->symbols)
        if (s.name == "y") {
          address.field = s.id;
          return;
        }
      throw std::runtime_error("y field symbol missing");
    });
    bad_native(native, "element address used without a field", [](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks) {
          for (size_t i = 0; i + 1 < b.instructions.size(); ++i)
            if (b.instructions[i + 1].op == ir::Op::field_address &&
                b.instructions[i].op == ir::Op::vec_element_address) {
              auto element = b.instructions[i].result;
              auto field = b.instructions[i + 1].result;
              need(element && field, "address values missing");
              for (auto &block : f.blocks)
                for (auto &use : block.instructions)
                  for (auto &input : use.inputs)
                    if (input == *field)
                      input = *element;
              b.instructions.erase(b.instructions.begin() + i + 1);
              return;
            }
        }
      throw std::runtime_error("field address pair missing");
    });
    bad_native(native, "element address escapes through a call-like use",
               [](auto &m) {
                 auto &address = instruction(m, ir::Op::vec_element_address);
                 need(address.result.has_value(), "address result missing");
                 auto &escape = instruction(m, ir::Op::vec_check);
                 escape.inputs.push_back(*address.result);
               });
    bad_native(native, "element address escapes through return", [](auto &m) {
      auto &address = instruction(m, ir::Op::vec_element_address);
      need(address.result.has_value(), "address result missing");
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          if (b.terminator.kind == ir::Exit::return_)
            b.terminator.inputs[0] = *address.result;
    });
    bad_native(native, "owner destroyed before element write", [](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks) {
          size_t address = b.instructions.size();
          size_t destroy = b.instructions.size();
          for (size_t i = 0; i < b.instructions.size(); ++i) {
            if (b.instructions[i].op == ir::Op::vec_element_address)
              address = i;
            if (b.instructions[i].op == ir::Op::destroy)
              destroy = i;
          }
          if (address < destroy && address < b.instructions.size() &&
              destroy < b.instructions.size()) {
            auto item = b.instructions[destroy];
            b.instructions.erase(b.instructions.begin() + destroy);
            b.instructions.insert(b.instructions.begin() + address, item);
            return;
          }
        }
      throw std::runtime_error("owner destroy target missing");
    });

    auto lifetime = checked_module(lifetime_manifest, sources);
    auto lifetime_lowered = ir::lower(lifetime);
    need(lifetime_lowered.success(), diagnostics(lifetime_lowered));
    auto lifetime_native = lifetime_lowered.value->module();
    need(ir::verify(lifetime_native).success(), "lifetime valid control rejected");
    bad_native(lifetime_native,
               "complete receiver cleanup batch before field store",
               move_receiver_cleanup_before_store);

    auto nested_fields = checked_module(nested_fields_manifest, sources);
    require_field_receiver_plan(nested_fields.module(),
                                "nested pure scalar RHS",
                                {hir::UseKind::borrow});
    auto nested_fields_lowered = ir::lower(nested_fields);
    need(nested_fields_lowered.success(), diagnostics(nested_fields_lowered));
    const auto nested_fields_native = nested_fields_lowered.value->module();
    need(ir::verify(nested_fields_native).success(),
         "nested field valid control rejected");

    auto nested_fallback = checked_module(nested_fallback_manifest, sources);
    require_field_receiver_plan(nested_fallback.module(),
                                "nested call and owning index RHS fallback",
                                {hir::UseKind::copy, hir::UseKind::copy});
    auto nested_fallback_lowered = ir::lower(nested_fallback);
    need(nested_fallback_lowered.success(),
         diagnostics(nested_fallback_lowered));
    const auto nested_fallback_native =
        nested_fallback_lowered.value->module();
    need(ir::verify(nested_fallback_native).success(),
         "nested fallback valid control rejected");
    bad_native(nested_fallback_native,
               "field-chain address owner cleanup before store",
               move_receiver_cleanup_before_store);

    auto borrow_mixed = checked_module(borrow_mixed_manifest, sources);
    const auto borrow_mixed_hir = borrow_mixed.module();
    need(hir::verify(borrow_mixed_hir).success(),
         "borrow mixed fixture rejected");
    require_field_receiver_plan(
        borrow_mixed_hir, "mixed pure, call, and scalar index RHS",
        {hir::UseKind::borrow, hir::UseKind::copy, hir::UseKind::borrow});

    auto compound = checked_module(compound_manifest, sources);
    require_field_receiver_plan(compound.module(), "call RHS fallback",
                                {hir::UseKind::copy});
    auto compound_lowered = ir::lower(compound);
    need(compound_lowered.success(), diagnostics(compound_lowered));
    const auto compound_native = compound_lowered.value->module();
    need(ir::verify(compound_native).success(),
         "compound valid control rejected");
    bad_native(compound_native,
               "compound field load sees dead container owner", [](auto &m) {
                 move_receiver_cleanup_before(m, ir::Op::load_indirect);
               });

    auto owned = checked_module(mutation_manifest, sources);
    auto owned_lowered = ir::lower(owned);
    need(owned_lowered.success(), diagnostics(owned_lowered));
    const auto owned_native = owned_lowered.value->module();
    auto owned_verified = ir::verify(owned_native);
    need(owned_verified.success(), diagnostics(owned_verified));
    bad_native(owned_native, "owned field replacement misses drop", [](auto &m) {
      auto &replace = instruction(m, ir::Op::replace_indirect);
      replace.op = ir::Op::store_indirect;
    });
    bad_native(owned_native, "trivial field store gains a drop", [](auto &m) {
      for (auto &f : m.functions)
        for (auto &b : f.blocks)
          for (auto &i : b.instructions)
            if (i.op == ir::Op::store_indirect) {
              i.op = ir::Op::replace_indirect;
              return;
            }
      throw std::runtime_error("trivial indirect store target missing");
    });
    bad_native(owned_native,
               "owned field replace sees dead container owner", [](auto &m) {
                 move_receiver_cleanup_before(m, ir::Op::replace_indirect);
               });

    std::cout << "{\"status\":\"passed\",\"hir_mutations\":10,"
                 "\"native_mutations\":15}\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}

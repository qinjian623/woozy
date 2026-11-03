#include "lang/hir/hir.hpp"
#include <iostream>
#include <stdexcept>
using namespace lang;
using namespace lang::hir;
void require(bool b) {
  if (!b)
    throw std::runtime_error("type test failure");
}
int main() {
  TypeContext c;
  require(!c.find(TypeId{}) && !c.find(TypeId{9999}));
  Type unit;
  unit.kind = TypeKind::Unit;
  auto u = c.intern(unit);
  require(u.value == 1 && c.intern(unit) == u);
  for (auto kind : {TypeKind::SignedInt, TypeKind::UnsignedInt})
    for (unsigned bits : {8u, 16u, 32u, 64u}) {
      Type t;
      t.kind = kind;
      t.bits = bits;
      auto id = c.intern(t);
      require(c.intern(t) == id && bit_width(*c.find(id), Config{}) == bits);
    }
  for (auto kind :
       {TypeKind::Bool, TypeKind::String, TypeKind::Isize, TypeKind::Usize}) {
    Type t;
    t.kind = kind;
    auto id = c.intern(t);
    require(c.intern(t) == id);
  }
  for (unsigned bits : {32u, 64u}) {
    Type t;
    t.kind = TypeKind::Float;
    t.bits = bits;
    require(c.intern(t) == c.intern(t));
  }
  Type integer;
  integer.kind = TypeKind::SignedInt;
  integer.bits = 32;
  auto i = c.intern(integer);
  Type fn;
  fn.kind = TypeKind::Function;
  fn.result = u;
  fn.parameters.push_back({ParamMode::ordinary, i});
  auto f = c.intern(fn);
  require(c.intern(fn) == f);
  fn.parameters[0].mode = ParamMode::inout;
  auto inout = c.intern(fn);
  require(inout != f);
  fn.parameters[0].mode = ParamMode::move;
  auto move = c.intern(fn);
  require(move != f && move != inout);
  fn.throws_type = i;
  auto throwing = c.intern(fn);
  require(throwing != move && c.intern(fn) == throwing);
  Type nominal;
  nominal.kind = TypeKind::NominalEnum;
  nominal.declaration = SymbolId{0};
  auto e0 = c.intern(nominal);
  nominal.declaration = SymbolId{1};
  require(c.intern(nominal) != e0);
  auto before = dump(c);
  require(before == dump(c));
  auto rejects = [&](Type t) {
    bool rejected = false;
    try {
      c.intern(t);
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    require(rejected);
  };
  fn.result = TypeId{};
  rejects(fn);
  integer.bits = 17;
  rejects(integer);
  unit.result = i;
  rejects(unit);
  Type bad;
  bad.kind = static_cast<TypeKind>(99);
  rejects(bad);
  std::cout
      << "type tests passed: canonical scalars/signatures/modes/throws/nominal "
         "identity, invalid type shapes\n";
}

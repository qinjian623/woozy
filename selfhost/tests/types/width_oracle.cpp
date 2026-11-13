// Test-only C++ oracle for the independent bit_width error/effect trace.
//
// This links the already-built Root liblang_hir.a.  Each line must match
// width/src/main.wz exactly.
#include "lang/hir/hir.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

using namespace lang::hir;

namespace {

Type scalar(TypeKind kind, unsigned bits) {
  Type value;
  value.kind = kind;
  value.bits = bits;
  return value;
}

Type leaf(TypeKind kind) {
  Type value;
  value.kind = kind;
  return value;
}

void width(const std::string &label, const Type &value, const Config &config) {
  try {
    const auto width = bit_width(value, config);
    std::cout << label << "=ok:" << width << "\n";
  } catch (const std::invalid_argument &error) {
    std::cout << label << "=error:" << error.what() << "\n";
  }
}

} // namespace

int main() {
  const Config cfg64{};
  const Config cfg32{Backend::native, 32};
  const Config cfg16{Backend::native, 16};
  const Config cfg0{Backend::native, 0};
  const Config cfg128{Backend::native, 128};

  width("i8_64", scalar(TypeKind::SignedInt, 8), cfg64);
  width("i16_64", scalar(TypeKind::SignedInt, 16), cfg64);
  width("i32_64", scalar(TypeKind::SignedInt, 32), cfg64);
  width("i64_64", scalar(TypeKind::SignedInt, 64), cfg64);
  width("u8_64", scalar(TypeKind::UnsignedInt, 8), cfg64);
  width("u64_64", scalar(TypeKind::UnsignedInt, 64), cfg64);
  width("f32_32", scalar(TypeKind::Float, 32), cfg32);
  width("f64_64", scalar(TypeKind::Float, 64), cfg64);
  width("isize_64", leaf(TypeKind::Isize), cfg64);
  width("isize_32", leaf(TypeKind::Isize), cfg32);
  width("usize_64", leaf(TypeKind::Usize), cfg64);
  width("usize_32", leaf(TypeKind::Usize), cfg32);
  width("i32_16", scalar(TypeKind::SignedInt, 32), cfg16);
  width("unit_16", leaf(TypeKind::Unit), cfg16);
  width("f64_0", scalar(TypeKind::Float, 64), cfg0);
  width("i32_128", scalar(TypeKind::SignedInt, 32), cfg128);
  width("unit_64", leaf(TypeKind::Unit), cfg64);
  width("bool_64", leaf(TypeKind::Bool), cfg64);
  width("string_64", leaf(TypeKind::String), cfg64);
  width("error_64", leaf(TypeKind::Error), cfg64);
  width("nominal_64", leaf(TypeKind::NominalStruct), cfg64);
  width("function_64", leaf(TypeKind::Function), cfg64);
  width("vec_64", leaf(TypeKind::Vec), cfg64);
  width("task_64", leaf(TypeKind::Task), cfg64);
  width("cptr_64", leaf(TypeKind::CForeignPointer), cfg64);
  return 0;
}

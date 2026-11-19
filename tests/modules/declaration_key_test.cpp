#include "lang/modules/declaration_key.hpp"
#include <cassert>
int main() {
  using lang::modules::declaration_key;
  assert(declaration_key("source:/a", "function", "", "f") ==
         "wz-decl/1:9:source:/a8:function0:1:f");
  assert(declaration_key("ab", "c", "", "f") !=
         declaration_key("a", "bc", "", "f"));
  assert(declaration_key("a", "function", "", "f") !=
         declaration_key("b", "function", "", "f"));
  assert(declaration_key("a", "method", "X", "f") !=
         declaration_key("a", "method", "Y", "f"));
  assert(declaration_key("a", "function", "", "中").ends_with("3:中"));
}

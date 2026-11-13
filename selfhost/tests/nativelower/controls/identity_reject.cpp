#include "lang/hir/hir.hpp"
#include <iostream>
int main() {
  lang::hir::HirModule h;h.config={lang::hir::Backend::native,64};h.identity=lang::ModuleId{1};
  auto p=std::make_shared<lang::modules::Project>();p->backend="native";p->word_bits=64;
  auto r=std::make_shared<lang::modules::ResolvedProject>();r->project=p;h.resolved=r;
  auto checked=lang::hir::verify(h,{},lang::hir::VerifyMode::static_only);
  for(auto &d:checked.diagnostics) std::cout<<d.code<<": "<<d.message<<"\n";
  return checked.success()?0:3;
}

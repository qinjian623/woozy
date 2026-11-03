#include <llvm/Config/llvm-config.h>
#include <llvm/Support/raw_ostream.h>
int main() {
    llvm::outs() << "LLVM " << LLVM_VERSION_STRING << " integration only; no code generation\n";
    return 0;
}

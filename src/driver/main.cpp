#include "lang/version.hpp"
#include <iostream>
#include <string_view>
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: langc --help | --version\n"
                     "Reserved commands: check, build, lsp, capabilities (unimplemented)\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << "langc " << lang::version() << '\n';
        return 0;
    }
    if (argc >= 2) {
        const std::string_view command(argv[1]);
        if (command == "check" || command == "build" || command == "lsp" ||
            command == "capabilities") {
            std::cerr << "E_CAPABILITY_UNAVAILABLE: " << command
                      << " is not implemented in T00 bootstrap\n";
            return 1;
        }
    }
    std::cerr << "E_DRIVER_ARGUMENT: expected --help, --version, check, build, lsp or capabilities\n";
    return 2;
}

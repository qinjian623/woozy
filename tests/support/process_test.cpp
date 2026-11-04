#include "lang/support/process.hpp"
#include <cerrno>
#include <csignal>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
static void require(bool ok) { if (!ok) throw std::runtime_error("require failed"); }
static void emit(int fd, const std::string& s) {
    size_t offset = 0;
    while (offset < s.size()) {
        ssize_t n = write(fd, s.data() + offset, s.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        require(n > 0); offset += static_cast<size_t>(n);
    }
}
int main(int argc, char** argv) {
    if (argc > 1) {
        std::string mode = argv[1];
        if (mode == "args") { for (int i=2;i<argc;++i) emit(1, std::string(argv[i])+"\n"); emit(2,"stderr\n"); return 7; }
        if (mode == "signal") { raise(SIGTERM); return 99; }
        if (mode == "large") { emit(1,std::string(2000000,'a')); emit(2,std::string(2000000,'b')); return 0; }
        return 98;
    }
    auto r = lang::run_process({argv[0],"args","a b","\"quote\"","$(echo forbidden)",""});
    require(r.exit_code==7 && !r.signalled && r.output=="a b\n\"quote\"\n$(echo forbidden)\n\nstderr\n");
    r=lang::run_process({argv[0],"signal"}); require(r.signalled && r.exit_code==128+SIGTERM);
    r=lang::run_process({argv[0],"large"}); require(r.exit_code==0 && !r.signalled && r.output==std::string(2000000,'a')+std::string(2000000,'b'));
    bool caught=false; try { lang::run_process({}); } catch(const std::invalid_argument&) { caught=true; } require(caught);
    caught=false; try { lang::run_process({std::string("x\0y",3)}); } catch(const std::invalid_argument&) { caught=true; } require(caught);
    caught=false; try { lang::run_process({"/nonexistent/lang-process-test-missing"}); } catch(const std::system_error& e) { caught=e.code().value()==ENOENT; } require(caught);
}

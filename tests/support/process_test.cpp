#include "lang/support/process.hpp"
#include "lang/support/publish.hpp"
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
static void require(bool ok) { if (!ok) throw std::runtime_error("require failed"); }
static std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in));
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static void write_text(const std::filesystem::path& p, std::string_view content) {
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    require(bool(out));
}
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

    namespace fs = std::filesystem;
    std::string tmp_pattern = (fs::temp_directory_path() / "lang-publish-test-XXXXXX").string();
    require(::mkdtemp(tmp_pattern.data()) != nullptr);
    const fs::path root(tmp_pattern);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{root};

    // Fresh multi-file publish removes staging directory cleanly.
    fs::path stage_path;
    {
        lang::StagingDir stage(root, ".test-stage-XXXXXX");
        stage_path = stage.path();
        write_text(stage.path() / "first", "v1-a");
        write_text(stage.path() / "second", "v1-b");
        lang::publish_staged_files(stage, root, {"first", "second"});
    }
    require(!fs::exists(stage_path));
    require(read_text(root / "first") == "v1-a");
    require(read_text(root / "second") == "v1-b");

    // Missing staged second file rolls back overwrite of first file.
    caught = false;
    {
        lang::StagingDir stage(root, ".test-stage-XXXXXX");
        stage_path = stage.path();
        write_text(stage.path() / "first", "v2-a");
        try {
            lang::publish_staged_files(stage, root, {"first", "second"});
        } catch (const fs::filesystem_error&) {
            caught = true;
        }
    }
    require(caught);
    require(!fs::exists(stage_path));
    require(read_text(root / "first") == "v1-a");
    require(read_text(root / "second") == "v1-b");

    // Non-regular destination path rejects without touching existing sibling.
    fs::create_directory(root / "dir_out");
    caught = false;
    {
        lang::StagingDir stage(root, ".test-stage-XXXXXX");
        write_text(stage.path() / "first", "v3-a");
        write_text(stage.path() / "dir_out", "not-a-dir");
        try {
            lang::publish_staged_files(stage, root, {"first", "dir_out"});
        } catch (const fs::filesystem_error&) {
            caught = true;
        }
    }
    require(caught);
    require(read_text(root / "first") == "v1-a");
    require(fs::is_directory(root / "dir_out"));
}

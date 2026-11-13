/* Freestanding driver for the five existing Vec-buffer fs operations.
 *
 * run.py prepares a directory containing symlink "link" -> "target". The
 * driver calls the unmodified LLVM functions emitted by e_fs_abi.wz with real
 * LangVec descriptors. A descriptor pointer is required for operations
 * 50/51/52/75/76; a data/length pair would change their arity and ABI.
 */
#include <stdint.h>

typedef struct {
    uint64_t refs;
    uint64_t length;
    uint64_t initialized;
    uint64_t stride;
    uint64_t iterations;
    void *data;
    void *copy;
    void *drop;
    uint64_t capacity;
    void *weak_control;
} LangVec;

typedef struct {
    const unsigned char *data;
    uint64_t bytes;
    uint64_t length;
    void *owner;
} LangText;

typedef struct { int64_t seconds; int64_t nanoseconds; } LangTimespec;

extern int64_t ex_getcwd(LangVec *value, uint64_t count);
extern int64_t ex_getdents(int32_t fd, LangVec *value, uint64_t count);
extern int64_t ex_readlink(LangText *path, LangVec *value, uint64_t count);
extern int64_t ex_nanosleep(LangTimespec request, LangVec *remaining);
extern int64_t ex_getrandom(LangVec *value, uint64_t count, uint32_t flags);
extern int64_t lang_os_open_v1(const char *data, uint64_t length,
                               int32_t flags, uint32_t mode);
extern int64_t lang_os_close_v1(int32_t fd);

static unsigned char cwd_buffer[256];
static unsigned char dents_buffer[8192];
static unsigned char link_buffer[64];
static unsigned char remaining_buffer[16];
static unsigned char random_buffer[16];
static const unsigned char link_name[] = "link";
static const unsigned char missing_name[] = "missing";
static const unsigned char expected_target[] = "target";

static int same_bytes(const unsigned char *a, const unsigned char *b,
                      uint64_t count) {
    for (uint64_t index = 0; index < count; ++index)
        if (a[index] != b[index]) return 0;
    return 1;
}

int main(void) {
    int64_t result;

    LangVec cwd = {0};
    cwd.length = sizeof cwd_buffer;
    cwd.initialized = sizeof cwd_buffer;
    cwd.stride = 1;
    cwd.data = cwd_buffer;
    result = ex_getcwd(&cwd, sizeof cwd_buffer);
    if (result <= 0) return 10;
    if (cwd_buffer[0] != '/') return 11;
    cwd.stride = 2;
    if (ex_getcwd(&cwd, sizeof cwd_buffer) != -22) return 12;

    const int32_t directory = (int32_t)lang_os_open_v1(
        ".", 1, 0 | 65536 | 524288, 0);
    if (directory < 0) return 20;
    LangVec dents = {0};
    dents.length = sizeof dents_buffer;
    dents.initialized = sizeof dents_buffer;
    dents.stride = 1;
    dents.data = dents_buffer;
    result = ex_getdents(directory, &dents, sizeof dents_buffer);
    if (result <= 0) return 21;
    if (lang_os_close_v1(directory) != 0) return 22;
    if (ex_getdents(-1, &dents, sizeof dents_buffer) != -9) return 23;

    LangVec link = {0};
    link.length = sizeof link_buffer;
    link.initialized = sizeof link_buffer;
    link.stride = 1;
    link.data = link_buffer;
    LangText path = {link_name, 4, 4, 0};
    result = ex_readlink(&path, &link, sizeof link_buffer);
    if (result != 6) return 100 + (result < 0 ? -result : result);
    if (!same_bytes(link_buffer, expected_target, 6)) return 31;
    LangText absent = {missing_name, 7, 7, 0};
    if (ex_readlink(&absent, &link, sizeof link_buffer) != -2) return 32;

    LangVec remaining = {0};
    remaining.length = 1;
    remaining.initialized = 1;
    remaining.stride = sizeof(LangTimespec);
    remaining.data = remaining_buffer;
    const LangTimespec zero = {0, 0};
    if (ex_nanosleep(zero, &remaining) != 0) return 40;
    const LangTimespec invalid = {0, -1};
    if (ex_nanosleep(invalid, &remaining) != -22) return 41;

    LangVec random = {0};
    random.length = sizeof random_buffer;
    random.initialized = sizeof random_buffer;
    random.stride = 1;
    random.data = random_buffer;
    if (ex_getrandom(&random, sizeof random_buffer, 0) !=
        (int64_t)sizeof random_buffer) return 50;
    if (ex_getrandom(&random, sizeof random_buffer, 0xffffffffu) != -22)
        return 51;
    return 0;
}

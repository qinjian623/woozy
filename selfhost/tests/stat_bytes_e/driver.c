/* Freestanding C driver for the E-generated operation-213 function.
 *
 * The module under test is the unmodified LLVM text printed by
 * e_stat_bytes.wz.  It returns Result[FileInfoBytes,i32] for a borrowed
 * LangVec descriptor.  This driver supplies a real descriptor, calls the
 * production lang_sys_stat_v1 through the generated function, and checks the
 * empty/NUL/overlong/missing/ENOTDIR errors plus one real non-UTF-8 file.
 *
 * The working directory is prepared by run.py: a one-byte file named 0xff and
 * a regular file named "notdir" exist, while "missing" does not.
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

typedef struct { unsigned char b[120]; } FileInfoBytes;
typedef struct { int32_t tag; FileInfoBytes ok; int32_t err; } Result;

extern Result ex_stat_bytes(LangVec *path);

static uint32_t u32_at(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t u64_at(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value |= (uint64_t)p[index] << (index * 8);
    return value;
}
static int expect_error(const unsigned char *data, uint64_t length,
                        int32_t code, int failure) {
    LangVec path = {0};
    path.length = length;
    path.data = (void *)data;
    Result result = ex_stat_bytes(&path);
    if (result.tag != 1) return failure;
    if (result.err != code) return failure + 1;
    return 0;
}
static const unsigned char nul_path[] = {'.', 0, 'x'};
static const unsigned char invalid_path[] = {0xff};
static const unsigned char missing_path[] = "missing";
static const unsigned char notdir_path[] = "notdir/child";
static unsigned char long_path[4096];

int main(void) {
    int result;
    result = expect_error(0, 0, 2, 10); /* empty spelling -> ENOENT */
    if (result) return result;
    result = expect_error(nul_path, sizeof nul_path, 22, 20); /* NUL -> EINVAL */
    if (result) return result;
    result = expect_error(long_path, sizeof long_path, 36, 30); /* ENAMETOOLONG */
    if (result) return result;
    result = expect_error(missing_path, sizeof missing_path - 1, 2, 40);
    if (result) return result;
    result = expect_error(notdir_path, sizeof notdir_path - 1, 20, 50);
    if (result) return result;

    LangVec path = {0};
    path.length = sizeof invalid_path;
    path.data = (void *)invalid_path;
    Result success = ex_stat_bytes(&path);
    if (success.tag != 0) return 60;
    const uint32_t mode = u32_at(success.ok.b + 24);
    if ((mode & 0170000u) != 0100000u) return 61; /* S_IFREG */
    if (u64_at(success.ok.b + 16) == 0) return 62; /* st_nlink */
    if (u64_at(success.ok.b + 48) != 1) return 63; /* st_size */
    return 0;
}

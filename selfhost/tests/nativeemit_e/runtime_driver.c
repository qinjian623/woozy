// Freestanding C driver for the E runtime-linked probes.
//
// It links the exact LLVM module printed by e_review_extra.wz, a test-only
// scalar ABI wrapper (runtime_wrapper.ll), and the real compiler runtime
// archive.  It checks Result/Option, negative errno, field, and IPC-wait
// behavior.  Return codes identify the first failed check.
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint64_t bytes;
    uint64_t length;
    void *owner;
} LangText;

typedef struct LangHandle LangHandle;

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

extern int32_t wrap_getenv(const uint8_t *, uint64_t, LangText *);
extern int32_t wrap_getenv_bytes(LangVec *, LangVec **);
extern int32_t wrap_field_bool(uint8_t, int32_t);
extern int32_t wrap_field_i32(uint8_t, int32_t);
extern int32_t wrap_field_nested(uint8_t, int32_t, void *);
extern int32_t wrap_field_pointer(uint8_t, int32_t, void *);

extern _Bool ex_wait_cond(void *, uint64_t, void *, uint64_t, int64_t);
extern _Bool ex_wait_sem(void *, uint64_t, int64_t);
extern _Bool ex_wait_trywait(void *, uint64_t);

extern int64_t lang_process_setenv_v1(const LangText *, const LangText *,
                                      int32_t);
extern int64_t lang_process_unsetenv_v1(const LangText *);
extern int64_t lang_process_fork_v1(void);
extern int64_t lang_process_waitpid_v1(int32_t, int32_t, int32_t *);
extern void lang_os_exit_v1(int32_t);
extern void lang_handle_release_v1(LangHandle *);

extern int64_t lang_ipc_map_anon_v1(LangHandle **, uint64_t, int32_t);
extern int64_t lang_ipc_mutex_init_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_mutex_lock_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_mutex_unlock_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_mutex_destroy_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_cond_init_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_cond_signal_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_cond_destroy_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_sem_init_v1(LangHandle *, uint64_t, uint32_t);
extern int64_t lang_ipc_sem_post_v1(LangHandle *, uint64_t);
extern int64_t lang_ipc_sem_destroy_v1(LangHandle *, uint64_t);

extern LangVec *lang_vec_new_v1(uint64_t, uint64_t, void *, void *);
extern void lang_vec_init_v1(LangVec *, const void *);
extern void lang_vec_release_v1(LangVec *);

#define CHECK(condition, code) \
    do { if (!(condition)) return (code); } while (0)

static uint64_t text_size(const char *value) {
    uint64_t size = 0;
    while (value[size]) size += 1;
    return size;
}

static LangText text_of(const char *value, uint64_t size) {
    LangText text;
    text.data = (const uint8_t *)value;
    text.bytes = size;
    text.length = size;
    text.owner = 0;
    return text;
}

static LangVec *bytes_of(const char *value, uint64_t size) {
    LangVec *vec = lang_vec_new_v1(size, 1, 0, 0);
    for (uint64_t index = 0; index < size; index += 1) {
        lang_vec_init_v1(vec, value + index);
    }
    return vec;
}

static int same_bytes(const uint8_t *left, const uint8_t *right,
                      uint64_t size) {
    for (uint64_t index = 0; index < size; index += 1) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

int main(void) {
    const char *name_data = "WZ_SH08E_E_PROBE";
    const char *value_data = "value-123";
    LangText name = text_of(name_data, text_size(name_data));
    LangText value = text_of(value_data, text_size(value_data));

    // Existing getenv: outer Result Ok + inner Option Some.
    CHECK(lang_process_setenv_v1(&name, &value, 1) == 0, 1);
    LangText got_text;
    got_text.data = 0;
    got_text.bytes = 0;
    got_text.length = 0;
    got_text.owner = 0;
    int32_t status = wrap_getenv(name.data, name.bytes, &got_text);
    CHECK(status == 1, 2);
    CHECK(got_text.bytes == value.bytes, 3);
    CHECK(same_bytes(got_text.data, value.data, value.bytes), 4);
    CHECK(got_text.owner != 0, 5);
    lang_handle_release_v1((LangHandle *)got_text.owner);

    // Missing getenv: outer Result Ok + inner Option None.
    CHECK(lang_process_unsetenv_v1(&name) == 0, 6);
    status = wrap_getenv(name.data, name.bytes, &got_text);
    CHECK(status == 0, 7);

    // Negative errno: empty key returns -EINVAL and must stay Err(22), not None.
    status = wrap_getenv((const uint8_t *)"", 0, &got_text);
    CHECK(status == 22, 8);

    // Existing getenv_bytes.
    CHECK(lang_process_setenv_v1(&name, &value, 1) == 0, 9);
    LangVec *key = bytes_of(name_data, name.bytes);
    LangVec *bytes_value = 0;
    status = wrap_getenv_bytes(key, &bytes_value);
    CHECK(status == 1 && bytes_value != 0, 10);
    CHECK(bytes_value->length == value.bytes, 11);
    CHECK(same_bytes((const uint8_t *)bytes_value->data,
        value.data, value.bytes), 12);
    lang_vec_release_v1(bytes_value);
    lang_vec_release_v1(key);

    // Missing getenv_bytes.
    CHECK(lang_process_unsetenv_v1(&name) == 0, 13);
    key = bytes_of(name_data, name.bytes);
    bytes_value = 0;
    status = wrap_getenv_bytes(key, &bytes_value);
    CHECK(status == 0 && bytes_value == 0, 14);
    lang_vec_release_v1(key);

    // Negative errno for getenv_bytes (invalid key containing '=').
    key = bytes_of("A=B", 3);
    status = wrap_getenv_bytes(key, &bytes_value);
    CHECK(status == 22, 15);
    lang_vec_release_v1(key);

    // CFieldRead bool (C i8 storage converted back to i1) and i32.
    CHECK(wrap_field_bool(1, 42) == 1, 16);
    CHECK(wrap_field_i32(1, 42) == 42, 17);

    // Nested record and pointer fields retain their storage types.
    int marker = 7;
    CHECK(wrap_field_nested(1, 42, &marker) == 43, 18);
    CHECK(wrap_field_pointer(1, 42, &marker) == 1, 19);

    // IPC waits: timeout and trywait are not errors; posted sem waits succeed.
    LangHandle *memory = 0;
    CHECK(lang_ipc_map_anon_v1(&memory, 4096, 3) == 0 && memory != 0, 20);
    CHECK(lang_ipc_mutex_init_v1(memory, 0) == 0, 21);
    CHECK(lang_ipc_cond_init_v1(memory, 16) == 0, 22);
    CHECK(lang_ipc_sem_init_v1(memory, 32, 0) == 0, 23);

    // cond_wait success: a child signals the process-shared condition.
    CHECK(lang_ipc_mutex_lock_v1(memory, 0) == 0, 24);
    int64_t child = lang_process_fork_v1();
    CHECK(child >= 0, 25);
    if (child == 0) {
        CHECK(lang_ipc_mutex_lock_v1(memory, 0) == 0, 38);
        CHECK(lang_ipc_cond_signal_v1(memory, 16) == 0, 39);
        CHECK(lang_ipc_mutex_unlock_v1(memory, 0) == 0, 40);
        lang_os_exit_v1(0);
    }
    CHECK(ex_wait_cond(memory, 16, memory, 0, 1000000000) == 1, 26);
    CHECK(lang_ipc_mutex_unlock_v1(memory, 0) == 0, 27);
    int32_t child_status = -1;
    CHECK(lang_process_waitpid_v1((int32_t)child, 0, &child_status) == child,
        28);
    CHECK(child_status == 0, 41);

    // sem_wait timeout is -110 and must not panic; the generated helper
    // returns false for that not-completed path.
    CHECK(ex_wait_sem(memory, 32, 0) == 0, 29);
    CHECK(lang_ipc_sem_post_v1(memory, 32) == 0, 30);
    CHECK(ex_wait_sem(memory, 32, 0) == 1, 31);
    CHECK(ex_wait_trywait(memory, 32) == 0, 32);
    CHECK(lang_ipc_sem_post_v1(memory, 32) == 0, 33);
    CHECK(ex_wait_trywait(memory, 32) == 1, 34);

    CHECK(lang_ipc_cond_destroy_v1(memory, 16) == 0, 35);
    CHECK(lang_ipc_sem_destroy_v1(memory, 32) == 0, 36);
    CHECK(lang_ipc_mutex_destroy_v1(memory, 0) == 0, 37);
    lang_handle_release_v1(memory);
    return 0;
}

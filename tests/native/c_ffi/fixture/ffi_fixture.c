#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Same memory spelling as the generated adapter's read-only byte buffer. */
typedef struct { const uint8_t *data; uint64_t len; } wf_buf_ro_u8;

static int32_t last_void;

int8_t cf_i8(int8_t v) { return (int8_t)(v + 1); }
int16_t cf_i16(int16_t v) { return (int16_t)(v - 2); }
int32_t cf_i32(int32_t v) { return v * 3; }
int64_t cf_i64(int64_t v) { return -v; }
intptr_t cf_isize(intptr_t v) { return -v; }
size_t cf_usize(size_t v) { return v + 1; }
uint8_t cf_u8(uint8_t v) { return (uint8_t)(v + 1); }
uint16_t cf_u16(uint16_t v) { return (uint16_t)(v + 2); }
uint32_t cf_u32(uint32_t v) { return v * 5u; }
uint64_t cf_u64(uint64_t v) { return v ^ 0x5a5a5a5a5a5a5a5aULL; }
float cf_f32(float v) { return v * 1.5f; }
double cf_f64(double v) { return v - 0.25; }
bool cf_bool(bool v) { return !v; }
int32_t cf_add_i32(int32_t a, int32_t b) { return a + b; }
void cf_void(int32_t v) { last_void = v; }
int32_t cf_last_void(void) { return last_void; }

static const uint8_t binary[] = {0x41, 0x00, 0xff, 0x42};
const uint8_t *cf_bytes_ptr(void) { return binary; }
uint64_t cf_bytes_len(void) { return (uint64_t)sizeof binary; }
uint64_t cf_checksum(wf_buf_ro_u8 view) {
  uint64_t total = 0;
  for (uint64_t i = 0; i < view.len; ++i) total = total * 131 + view.data[i];
  return total;
}

static int32_t global_i32 = 11;
int32_t *cf_global_i32_ptr(void) { return &global_i32; }
int32_t cf_read_i32(const int32_t *p) { return p ? *p : -1; }
void cf_write_i32(int32_t *p, int32_t v) { if (p) *p = v; }

typedef struct {
  int32_t x;
  int32_t y;
  double weight;
} cf_point;

cf_point cf_point_make(int32_t x, int32_t y, double weight) {
  cf_point p = {x, y, weight};
  return p;
}
int32_t cf_point_sum(cf_point p) { return p.x + p.y; }
double cf_point_weight(cf_point p) { return p.weight; }
uint64_t cf_point_size(void) { return (uint64_t)sizeof(cf_point); }
uint64_t cf_point_align(void) { return (uint64_t)_Alignof(cf_point); }
uint64_t cf_point_offset_x(void) { return (uint64_t)offsetof(cf_point, x); }
uint64_t cf_point_offset_y(void) { return (uint64_t)offsetof(cf_point, y); }
uint64_t cf_point_offset_weight(void) { return (uint64_t)offsetof(cf_point, weight); }

typedef struct {
  int32_t value;
  int32_t refs;
} cf_resource;

static int32_t live_resources;
cf_resource *cf_resource_new(int32_t value) {
  cf_resource *r = (cf_resource *)malloc(sizeof *r);
  if (!r) return NULL;
  r->value = value;
  r->refs = 1;
  ++live_resources;
  return r;
}
void cf_resource_retain(cf_resource *r) { if (r) ++r->refs; }
void cf_resource_release(cf_resource *r) {
  if (!r) return;
  if (--r->refs == 0) {
    --live_resources;
    free(r);
  }
}
int32_t cf_resource_value(cf_resource *r) { return r ? r->value : -1; }
int32_t cf_resource_refs(cf_resource *r) { return r ? r->refs : -1; }
int32_t cf_resource_live(void) { return live_resources; }

typedef int32_t (*cf_callback)(int32_t, uint64_t);
int32_t cf_apply(cf_callback callback, uint64_t context, int32_t value) {
  return callback ? callback(value, context) : -999;
}

typedef int64_t (*cf_point_callback)(cf_point, uint64_t);
int64_t cf_apply_point(cf_point_callback callback, uint64_t context) {
  cf_point point = {11, 22, 1.5};
  return callback ? callback(point, context) : -1;
}
typedef cf_point (*cf_point_return_callback)(uint64_t);
int64_t cf_apply_point_return(cf_point_return_callback callback,
                              uint64_t context) {
  cf_point point = callback ? callback(context) : (cf_point){0, 0, 0.0};
  return (int64_t)point.x + 100 * (int64_t)point.y;
}

typedef struct { int32_t value; } cf_tiny;
typedef struct { int64_t a; int64_t b; double c; } cf_large;
typedef cf_tiny (*cf_tiny_callback)(cf_tiny, uint64_t);
cf_tiny cf_apply_tiny(cf_tiny_callback callback, uint64_t context) {
  cf_tiny value = {7};
  return callback ? callback(value, context) : (cf_tiny){-1};
}
typedef cf_large (*cf_large_callback)(cf_large, uint64_t);
cf_large cf_apply_large(cf_large_callback callback, uint64_t context) {
  cf_large value = {1, 2, 3.5};
  return callback ? callback(value, context) : (cf_large){-1, -1, -1.0};
}

typedef bool (*cf_narrow_callback)(int8_t, uint16_t, float, bool, uint64_t);
bool cf_apply_narrow(cf_narrow_callback callback, uint64_t context) {
  return callback ? callback(-7, 65000, 2.5f, true, context) : false;
}
typedef double (*cf_float_callback)(float, double, uint64_t);
double cf_apply_float(cf_float_callback callback, uint64_t context) {
  return callback ? callback(1.5f, 2.25, context) : -1.0;
}
typedef int16_t (*cf_int_return_callback)(int8_t, uint16_t, uint64_t);
int16_t cf_apply_int_return(cf_int_return_callback callback, uint64_t context) {
  return callback ? callback(-5, 100, context) : (int16_t)-1;
}

typedef struct { int32_t tag; wf_buf_ro_u8 data; } cf_buffer_view;
cf_buffer_view cf_buffer_view_make(int32_t tag, const uint8_t *data,
                                   uint64_t len) {
  cf_buffer_view view = {tag, {data, len}};
  return view;
}
uint64_t cf_buffer_view_sum(cf_buffer_view view) {
  return (uint64_t)view.tag + view.data.len +
         (view.data.data ? (uint64_t)view.data.data[0] : 0);
}

/* Hosted libc/thread boundary checks. */
static _Thread_local int32_t tls_slot;
int32_t cf_thread_tls(int32_t value) {
  tls_slot = value * 3 + 1;
  for (int i = 0; i < 3; ++i) {
    if (tls_slot != value * 3 + 1) return 1;
  }
  return 0;
}
const uint8_t *cf_env_name(void) { return (const uint8_t *)"WOOZY_FFI_ENV"; }
int32_t cf_strlen_nonempty(const uint8_t *s) {
  return s && s[0] ? (int32_t)strlen((const char *)s) : -1;
}

static const uint8_t sha_input[] = {0x61, 0x62, 0x63};
static uint8_t sha_output[32];
const uint8_t *cf_sha_input(void) { return sha_input; }
uint64_t cf_sha_input_len(void) { return (uint64_t)sizeof sha_input; }
uint8_t *cf_sha_output(void) { return sha_output; }

typedef struct { int32_t value; int32_t failed; } cf_tls_worker_arg;
static void *cf_tls_worker(void *opaque) {
  cf_tls_worker_arg *arg = (cf_tls_worker_arg *)opaque;
  arg->failed = cf_thread_tls(arg->value);
  return NULL;
}
int32_t cf_thread_tls_stress(void) {
  pthread_t threads[8];
  cf_tls_worker_arg args[8];
  int32_t failed = 0;
  for (int i = 0; i < 8; ++i) {
    args[i].value = i + 1;
    args[i].failed = 1;
    if (pthread_create(&threads[i], NULL, cf_tls_worker, &args[i]) != 0)
      return 1;
  }
  for (int i = 0; i < 8; ++i) {
    if (pthread_join(threads[i], NULL) != 0) failed = 1;
    failed |= args[i].failed;
  }
  return failed;
}

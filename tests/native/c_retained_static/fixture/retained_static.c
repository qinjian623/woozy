/*
 * Small user-owned C library for the retained static-callback test.
 *
 * It deliberately has no knowledge of the Woozy runtime.  A C slot stores the
 * function pointer and an opaque context, invokes it later, and releases its
 * own heap userdata only after the worker thread has been joined and every
 * in-flight callback has returned.
 */
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

typedef int32_t (*wrs_callback)(int32_t, uint64_t);

typedef struct wrs_slot {
  wrs_callback callback;
  uint64_t context;
  int32_t *userdata;
  int32_t calls;
  int64_t sum;
  int32_t in_flight;
  int32_t stop;
  int32_t thread_started;
  int32_t thread_joined;
  int32_t thread_count;
  int32_t thread_calls;
  int64_t thread_sum;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t idle;
} wrs_slot;

static pthread_mutex_t g_registry = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_live_slots;
static int32_t g_live_userdata;
static int32_t g_live_threads;
static int32_t g_released_tag = -1;
static int32_t g_released_calls = -1;
static int32_t g_released_thread_calls = -1;

wrs_slot *wrs_open(int32_t initial, int32_t tag) {
  wrs_slot *slot = (wrs_slot *)calloc(1, sizeof *slot);
  int32_t *userdata = (int32_t *)malloc(sizeof *userdata);
  if (!slot || !userdata) {
    free(slot);
    free(userdata);
    return NULL;
  }
  *userdata = tag;
  slot->userdata = userdata;
  slot->sum = initial;
  if (pthread_mutex_init(&slot->mutex, NULL) != 0 ||
      pthread_cond_init(&slot->idle, NULL) != 0) {
    free(userdata);
    free(slot);
    return NULL;
  }
  pthread_mutex_lock(&g_registry);
  ++g_live_slots;
  ++g_live_userdata;
  pthread_mutex_unlock(&g_registry);
  return slot;
}

int32_t wrs_register(wrs_slot *slot, wrs_callback callback,
                     uint64_t context) {
  if (!slot || !callback)
    return 1;
  pthread_mutex_lock(&slot->mutex);
  if (slot->stop) {
    pthread_mutex_unlock(&slot->mutex);
    return 2;
  }
  slot->callback = callback;
  slot->context = context;
  pthread_mutex_unlock(&slot->mutex);
  return 0;
}

int32_t wrs_invoke(wrs_slot *slot, int32_t value) {
  if (!slot)
    return -999;
  pthread_mutex_lock(&slot->mutex);
  if (!slot->callback) {
    pthread_mutex_unlock(&slot->mutex);
    return -998;
  }
  wrs_callback callback = slot->callback;
  uint64_t context = slot->context;
  ++slot->in_flight;
  pthread_mutex_unlock(&slot->mutex);

  int32_t result = callback(value, context);

  pthread_mutex_lock(&slot->mutex);
  ++slot->calls;
  slot->sum += result;
  --slot->in_flight;
  if (slot->in_flight == 0)
    pthread_cond_broadcast(&slot->idle);
  pthread_mutex_unlock(&slot->mutex);
  return result;
}

static void *wrs_worker(void *opaque) {
  wrs_slot *slot = (wrs_slot *)opaque;
  int32_t count;
  pthread_mutex_lock(&slot->mutex);
  count = slot->thread_count;
  pthread_mutex_unlock(&slot->mutex);

  for (int32_t i = 0; i < count; ++i) {
    int32_t stop;
    pthread_mutex_lock(&slot->mutex);
    stop = slot->stop;
    pthread_mutex_unlock(&slot->mutex);
    if (stop && i != 0)
      break;
    int32_t result = wrs_invoke(slot, i);
    if (result == -998 || result == -999)
      break;
    pthread_mutex_lock(&slot->mutex);
    ++slot->thread_calls;
    slot->thread_sum += result;
    pthread_mutex_unlock(&slot->mutex);
  }
  return NULL;
}

int32_t wrs_start_thread(wrs_slot *slot, int32_t count) {
  if (!slot || count <= 0)
    return 1;
  pthread_mutex_lock(&slot->mutex);
  if (slot->thread_started || slot->stop) {
    pthread_mutex_unlock(&slot->mutex);
    return 2;
  }
  slot->thread_started = 1;
  slot->thread_joined = 0;
  slot->thread_count = count;
  pthread_mutex_unlock(&slot->mutex);
  if (pthread_create(&slot->thread, NULL, wrs_worker, slot) != 0) {
    pthread_mutex_lock(&slot->mutex);
    slot->thread_started = 0;
    pthread_mutex_unlock(&slot->mutex);
    return 3;
  }
  pthread_mutex_lock(&g_registry);
  ++g_live_threads;
  pthread_mutex_unlock(&g_registry);
  return 0;
}

int32_t wrs_join(wrs_slot *slot) {
  if (!slot)
    return 1;
  pthread_mutex_lock(&slot->mutex);
  if (!slot->thread_started || slot->thread_joined) {
    pthread_mutex_unlock(&slot->mutex);
    return 2;
  }
  pthread_t thread = slot->thread;
  pthread_mutex_unlock(&slot->mutex);

  if (pthread_join(thread, NULL) != 0)
    return 3;

  pthread_mutex_lock(&slot->mutex);
  slot->thread_joined = 1;
  pthread_mutex_unlock(&slot->mutex);
  pthread_mutex_lock(&g_registry);
  --g_live_threads;
  pthread_mutex_unlock(&g_registry);
  return 0;
}

void wrs_release(wrs_slot *slot) {
  if (!slot)
    return;

  pthread_mutex_lock(&slot->mutex);
  slot->stop = 1;
  int32_t started = slot->thread_started;
  int32_t joined = slot->thread_joined;
  pthread_t thread = slot->thread;
  pthread_mutex_unlock(&slot->mutex);

  if (started && !joined) {
    pthread_join(thread, NULL);
    pthread_mutex_lock(&slot->mutex);
    slot->thread_joined = 1;
    pthread_mutex_unlock(&slot->mutex);
    pthread_mutex_lock(&g_registry);
    --g_live_threads;
    pthread_mutex_unlock(&g_registry);
  }

  pthread_mutex_lock(&slot->mutex);
  while (slot->in_flight != 0)
    pthread_cond_wait(&slot->idle, &slot->mutex);
  int32_t calls = slot->calls;
  int32_t thread_calls = slot->thread_calls;
  int32_t tag = slot->userdata ? *slot->userdata : -1;
  slot->callback = NULL;
  slot->context = 0;
  pthread_mutex_unlock(&slot->mutex);

  pthread_mutex_lock(&g_registry);
  g_released_tag = tag;
  g_released_calls = calls;
  g_released_thread_calls = thread_calls;
  --g_live_slots;
  --g_live_userdata;
  pthread_mutex_unlock(&g_registry);

  free(slot->userdata);
  slot->userdata = NULL;
  pthread_mutex_destroy(&slot->mutex);
  pthread_cond_destroy(&slot->idle);
  free(slot);
}

int32_t wrs_calls(wrs_slot *slot) { return slot ? slot->calls : -1; }
int64_t wrs_sum(wrs_slot *slot) { return slot ? slot->sum : -1; }
int32_t wrs_thread_calls(wrs_slot *slot) {
  return slot ? slot->thread_calls : -1;
}
int64_t wrs_thread_sum(wrs_slot *slot) { return slot ? slot->thread_sum : -1; }
int32_t wrs_live_slots(void) { return g_live_slots; }
int32_t wrs_live_userdata(void) { return g_live_userdata; }
int32_t wrs_live_threads(void) { return g_live_threads; }
int32_t wrs_released_tag(void) { return g_released_tag; }
int32_t wrs_released_calls(void) { return g_released_calls; }
int32_t wrs_released_thread_calls(void) { return g_released_thread_calls; }

int32_t wrs_apply(wrs_callback callback, uint64_t context, int32_t value) {
  return callback ? callback(value, context) : -997;
}

int32_t wrs_touch(int32_t value) { return value + 1; }

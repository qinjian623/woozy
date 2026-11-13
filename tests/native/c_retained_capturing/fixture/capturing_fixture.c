/*
 * User-owned C library for the retained capturing-callback contract.
 *
 * The library knows nothing about the Woozy runtime.  It stores a callback
 * pointer plus the opaque userdata supplied by the caller in a heap-owned
 * `wcc_subscription` token.  Registration returns that token; unregister
 * removes it, prevents new dispatches and waits for every dispatch that has
 * already reserved the callback.  The userdata pointer itself is owned by the
 * caller and is never freed here.
 *
 * All waiting is event based: pthread mutexes and condition variables, no
 * sleep-based synchronization.  The reserved-worker API lets a test prove
 * that unregister drains a dispatch which has reserved `in_flight` but has not
 * yet entered the trampoline.
 */
#include "capturing_fixture.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef union wcc_callback {
  wcc_cb_last last;
  wcc_cb_first first;
  wcc_cb_middle middle;
  wcc_cb_bool boolean;
  wcc_cb_unit unit;
  wcc_cb_scalar scalar;
} wcc_callback;

struct wcc_subscription {
  wcc_callback callback;
  void *userdata;
  int kind;
  int registered;
  int in_flight;
  int calls;
  int64_t sum;
  int32_t last_i32;
  bool last_bool;
  int64_t last_i64;
  int thread_started;
  int thread_joined;
  int thread_count;
  int thread_calls;
  int64_t thread_sum;
  int reserved_value;
  int reserved_flag;
  int gate_open;
  int unregister_active;
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t idle;
  pthread_cond_t reserved_ready;
  pthread_cond_t gate_opened;
  pthread_cond_t unregister_ready;
};

enum {
  WCC_KIND_LAST = 1,
  WCC_KIND_FIRST = 2,
  WCC_KIND_MIDDLE = 3,
  WCC_KIND_BOOL = 4,
  WCC_KIND_UNIT = 5,
  WCC_KIND_SCALAR = 6
};

static pthread_mutex_t g_state = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_live_subscriptions;
static int32_t g_live_threads;
static int32_t g_unregister_calls;
static int32_t g_callback_calls;
static int32_t g_registration_failures;
static int64_t g_callback_sum;

static void note_result(int64_t result) {
  pthread_mutex_lock(&g_state);
  ++g_callback_calls;
  g_callback_sum += result;
  pthread_mutex_unlock(&g_state);
}

static wcc_subscription *new_token(int kind) {
  wcc_subscription *token = (wcc_subscription *)calloc(1, sizeof *token);
  if (!token)
    return NULL;
  token->kind = kind;
  token->registered = 1;
  if (pthread_mutex_init(&token->mutex, NULL) != 0) {
    free(token);
    return NULL;
  }
  if (pthread_cond_init(&token->idle, NULL) != 0) {
    pthread_mutex_destroy(&token->mutex);
    free(token);
    return NULL;
  }
  if (pthread_cond_init(&token->reserved_ready, NULL) != 0) {
    pthread_cond_destroy(&token->idle);
    pthread_mutex_destroy(&token->mutex);
    free(token);
    return NULL;
  }
  if (pthread_cond_init(&token->gate_opened, NULL) != 0) {
    pthread_cond_destroy(&token->reserved_ready);
    pthread_cond_destroy(&token->idle);
    pthread_mutex_destroy(&token->mutex);
    free(token);
    return NULL;
  }
  if (pthread_cond_init(&token->unregister_ready, NULL) != 0) {
    pthread_cond_destroy(&token->gate_opened);
    pthread_cond_destroy(&token->reserved_ready);
    pthread_cond_destroy(&token->idle);
    pthread_mutex_destroy(&token->mutex);
    free(token);
    return NULL;
  }
  pthread_mutex_lock(&g_state);
  ++g_live_subscriptions;
  pthread_mutex_unlock(&g_state);
  return token;
}

static void destroy_token_storage(wcc_subscription *token) {
  pthread_mutex_destroy(&token->mutex);
  pthread_cond_destroy(&token->idle);
  pthread_cond_destroy(&token->reserved_ready);
  pthread_cond_destroy(&token->gate_opened);
  pthread_cond_destroy(&token->unregister_ready);
  free(token);
}

/* Used only by the two failing registration entry points.  The C API already
 * guarantees that no callback can be running or become runnable. */
static void abandon_token(wcc_subscription *token) {
  if (!token)
    return;
  pthread_mutex_lock(&token->mutex);
  token->registered = 0;
  while (token->in_flight != 0)
    pthread_cond_wait(&token->idle, &token->mutex);
  token->callback.last = NULL;
  token->userdata = NULL;
  pthread_mutex_unlock(&token->mutex);
  pthread_mutex_lock(&g_state);
  --g_live_subscriptions;
  pthread_mutex_unlock(&g_state);
  destroy_token_storage(token);
}

static int32_t invoke_last_mode(wcc_subscription *token, int32_t value,
                                int thread_mode) {
  if (!token)
    return -1000;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return -1001;
  }
  wcc_cb_last callback = token->callback.last;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  int32_t result = callback(value, context);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->sum += result;
  token->last_i32 = result;
  if (thread_mode) {
    ++token->thread_calls;
    token->thread_sum += result;
  }
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result);
  return result;
}

wcc_subscription *wcc_subscribe_last(wcc_cb_last callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_LAST);
  if (token)
    token->callback.last = callback, token->userdata = context;
  return token;
}

/* Synchronously calls the callback once before the registration call returns.
 * The runtime context is ACTIVE before C is entered. */
wcc_subscription *wcc_subscribe_initial(wcc_cb_last callback, void *context,
                                        int32_t value) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_LAST);
  if (!token)
    return NULL;
  token->callback.last = callback;
  token->userdata = context;
  (void)invoke_last_mode(token, value, 0);
  return token;
}

/* Returns null after guaranteeing that the callback was never entered. */
wcc_subscription *wcc_subscribe_fail(wcc_cb_last callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_LAST);
  if (token) {
    token->callback.last = callback;
    token->userdata = context;
    abandon_token(token);
  }
  pthread_mutex_lock(&g_state);
  ++g_registration_failures;
  pthread_mutex_unlock(&g_state);
  return NULL;
}

/* Calls the callback once synchronously, then returns null.  This exercises
 * cleanup of a context which has already entered a callback. */
wcc_subscription *wcc_subscribe_fail_after(wcc_cb_last callback, void *context,
                                           int32_t value) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_LAST);
  if (token) {
    token->callback.last = callback;
    token->userdata = context;
    (void)invoke_last_mode(token, value, 0);
    abandon_token(token);
  }
  pthread_mutex_lock(&g_state);
  ++g_registration_failures;
  pthread_mutex_unlock(&g_state);
  return NULL;
}

wcc_subscription *wcc_subscribe_first(wcc_cb_first callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_FIRST);
  if (token)
    token->callback.first = callback, token->userdata = context;
  return token;
}

wcc_subscription *wcc_subscribe_middle(wcc_cb_middle callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_MIDDLE);
  if (token)
    token->callback.middle = callback, token->userdata = context;
  return token;
}

wcc_subscription *wcc_subscribe_bool(wcc_cb_bool callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_BOOL);
  if (token)
    token->callback.boolean = callback, token->userdata = context;
  return token;
}

wcc_subscription *wcc_subscribe_unit(wcc_cb_unit callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_UNIT);
  if (token)
    token->callback.unit = callback, token->userdata = context;
  return token;
}

wcc_subscription *wcc_subscribe_scalar(wcc_cb_scalar callback, void *context) {
  if (!callback)
    return NULL;
  wcc_subscription *token = new_token(WCC_KIND_SCALAR);
  if (token)
    token->callback.scalar = callback, token->userdata = context;
  return token;
}

/* Explicit close and the four automatic-release symbols all enter the same
 * idempotent close path.  The first transition increments the unregister
 * counter; a second call on an already-freed token is a caller bug and must not
 * happen because the runtime closes exactly once. */
static void close_token(wcc_subscription *token) {
  if (!token)
    return;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return;
  }
  token->registered = 0;
  token->unregister_active = 1;
  pthread_cond_broadcast(&token->unregister_ready);
  int join_needed = token->thread_started && !token->thread_joined;
  pthread_t thread = token->thread;
  pthread_mutex_unlock(&token->mutex);

  if (join_needed && pthread_join(thread, NULL) != 0)
    return;

  pthread_mutex_lock(&token->mutex);
  if (join_needed)
    token->thread_joined = 1;
  while (token->in_flight != 0)
    pthread_cond_wait(&token->idle, &token->mutex);
  token->callback.last = NULL;
  token->userdata = NULL;
  pthread_mutex_unlock(&token->mutex);

  pthread_mutex_lock(&g_state);
  ++g_unregister_calls;
  --g_live_subscriptions;
  if (join_needed)
    --g_live_threads;
  pthread_mutex_unlock(&g_state);
  destroy_token_storage(token);
}

void wcc_unsubscribe(wcc_subscription *token) { close_token(token); }
void wcc_unsubscribe_first(wcc_subscription *token) { close_token(token); }
void wcc_unsubscribe_middle(wcc_subscription *token) { close_token(token); }
void wcc_unsubscribe_bool(wcc_subscription *token) { close_token(token); }
void wcc_unsubscribe_unit(wcc_subscription *token) { close_token(token); }
void wcc_unsubscribe_scalar(wcc_subscription *token) { close_token(token); }

/* Ordinary borrowed-token calls.  The runtime borrows the foreign token for
 * the whole C call; these functions never retain it. */
int32_t wcc_invoke_last(wcc_subscription *token, int32_t value) {
  return invoke_last_mode(token, value, 0);
}

/* Two borrowed tokens in one C call.  Both leases are held while the first and
 * then the second callback are entered; no token mutex spans a callback. */
int32_t wcc_pair_last(wcc_subscription *first, wcc_subscription *second,
                      int32_t value) {
  if (!first || !second)
    return -2000;
  int32_t left = invoke_last_mode(first, value, 0);
  int32_t right = invoke_last_mode(second, value + 1, 0);
  return left * 1000 + right;
}

int32_t wcc_invoke_first(wcc_subscription *token, int32_t value) {
  if (!token)
    return -1000;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return -1001;
  }
  wcc_cb_first callback = token->callback.first;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  int32_t result = callback(context, value);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->sum += result;
  token->last_i32 = result;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result);
  return result;
}

int32_t wcc_invoke_middle(wcc_subscription *token, int32_t left,
                          int32_t right) {
  if (!token)
    return -1000;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return -1001;
  }
  wcc_cb_middle callback = token->callback.middle;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  int32_t result = callback(left, context, right);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->sum += result;
  token->last_i32 = result;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result);
  return result;
}

bool wcc_invoke_bool(wcc_subscription *token, bool flag) {
  if (!token)
    return false;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return false;
  }
  wcc_cb_bool callback = token->callback.boolean;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  bool result = callback(flag, context);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->last_bool = result;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result ? 1 : 0);
  return result;
}

void wcc_invoke_unit(wcc_subscription *token, int32_t value) {
  if (!token)
    return;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return;
  }
  wcc_cb_unit callback = token->callback.unit;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  callback(value, context);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->last_i32 = value;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(0);
}

int64_t wcc_invoke_scalar(wcc_subscription *token, int32_t value) {
  if (!token)
    return -1000;
  pthread_mutex_lock(&token->mutex);
  if (!token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return -1001;
  }
  wcc_cb_scalar callback = token->callback.scalar;
  void *context = token->userdata;
  ++token->in_flight;
  pthread_mutex_unlock(&token->mutex);

  int64_t result = callback(value, context);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->sum += result;
  token->last_i64 = result;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result);
  return result;
}

int32_t wcc_info_last(wcc_subscription *token) {
  if (!token)
    return 0;
  pthread_mutex_lock(&token->mutex);
  int32_t registered = token->registered ? 1 : 0;
  pthread_mutex_unlock(&token->mutex);
  return registered;
}

int32_t wcc_calls(wcc_subscription *token) {
  if (!token)
    return -1;
  pthread_mutex_lock(&token->mutex);
  int32_t calls = token->calls;
  pthread_mutex_unlock(&token->mutex);
  return calls;
}

int32_t wcc_calls_first(wcc_subscription *token) { return wcc_calls(token); }
int32_t wcc_calls_middle(wcc_subscription *token) { return wcc_calls(token); }
int32_t wcc_calls_bool(wcc_subscription *token) { return wcc_calls(token); }
int32_t wcc_calls_unit(wcc_subscription *token) { return wcc_calls(token); }
int32_t wcc_calls_scalar(wcc_subscription *token) { return wcc_calls(token); }

int64_t wcc_sum_last(wcc_subscription *token) {
  if (!token)
    return -1;
  pthread_mutex_lock(&token->mutex);
  int64_t sum = token->sum;
  pthread_mutex_unlock(&token->mutex);
  return sum;
}

int32_t wcc_last_value(wcc_subscription *token) {
  if (!token)
    return -1;
  pthread_mutex_lock(&token->mutex);
  int32_t value = token->last_i32;
  pthread_mutex_unlock(&token->mutex);
  return value;
}

bool wcc_subscription_null(wcc_subscription *token) { return token == NULL; }

/* External C pthread invocation.  Registration is expected to be live for the
 * whole run; unregister joins the worker before freeing the token. */
static void *thread_worker(void *opaque) {
  wcc_subscription *token = (wcc_subscription *)opaque;
  pthread_mutex_lock(&token->mutex);
  int32_t count = token->thread_count;
  pthread_mutex_unlock(&token->mutex);
  for (int32_t i = 0; i < count; ++i) {
    int32_t result = invoke_last_mode(token, i, 1);
    if (result == -1000 || result == -1001)
      break;
  }
  return NULL;
}

int32_t wcc_start_thread(wcc_subscription *token, int32_t count) {
  if (!token || count <= 0)
    return 1;
  pthread_mutex_lock(&token->mutex);
  if (token->thread_started || !token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return 2;
  }
  token->thread_started = 1;
  token->thread_joined = 0;
  token->thread_count = count;
  pthread_mutex_unlock(&token->mutex);

  if (pthread_create(&token->thread, NULL, thread_worker, token) != 0) {
    pthread_mutex_lock(&token->mutex);
    token->thread_started = 0;
    pthread_mutex_unlock(&token->mutex);
    return 3;
  }
  pthread_mutex_lock(&g_state);
  ++g_live_threads;
  pthread_mutex_unlock(&g_state);
  return 0;
}

int32_t wcc_join_thread(wcc_subscription *token) {
  if (!token)
    return 1;
  pthread_mutex_lock(&token->mutex);
  if (!token->thread_started || token->thread_joined) {
    pthread_mutex_unlock(&token->mutex);
    return 2;
  }
  pthread_t thread = token->thread;
  pthread_mutex_unlock(&token->mutex);

  if (pthread_join(thread, NULL) != 0)
    return 3;

  pthread_mutex_lock(&token->mutex);
  token->thread_joined = 1;
  pthread_mutex_unlock(&token->mutex);
  pthread_mutex_lock(&g_state);
  --g_live_threads;
  pthread_mutex_unlock(&g_state);
  return 0;
}

int32_t wcc_thread_calls(wcc_subscription *token) {
  if (!token)
    return -1;
  pthread_mutex_lock(&token->mutex);
  int32_t calls = token->thread_calls;
  pthread_mutex_unlock(&token->mutex);
  return calls;
}

int64_t wcc_thread_sum(wcc_subscription *token) {
  if (!token)
    return -1;
  pthread_mutex_lock(&token->mutex);
  int64_t sum = token->thread_sum;
  pthread_mutex_unlock(&token->mutex);
  return sum;
}

/* Reserved dispatch: reserve in_flight, announce the reservation, wait for a
 * controlled event, then enter the caller's trampoline.  Unregister must wait
 * for this reservation even though the callback has not started yet. */
static void *reserved_worker(void *opaque) {
  wcc_subscription *token = (wcc_subscription *)opaque;
  pthread_mutex_lock(&token->mutex);
  wcc_cb_last callback = token->callback.last;
  void *context = token->userdata;
  int32_t value = token->reserved_value;
  ++token->in_flight;
  token->reserved_flag = 1;
  pthread_cond_broadcast(&token->reserved_ready);
  while (!token->gate_open)
    pthread_cond_wait(&token->gate_opened, &token->mutex);
  pthread_mutex_unlock(&token->mutex);

  int32_t result = callback(value, context);

  pthread_mutex_lock(&token->mutex);
  ++token->calls;
  token->sum += result;
  token->last_i32 = result;
  --token->in_flight;
  if (token->in_flight == 0)
    pthread_cond_broadcast(&token->idle);
  pthread_mutex_unlock(&token->mutex);
  note_result(result);
  return NULL;
}

int32_t wcc_start_reserved(wcc_subscription *token, int32_t value) {
  if (!token)
    return 1;
  pthread_mutex_lock(&token->mutex);
  if (token->thread_started || !token->registered) {
    pthread_mutex_unlock(&token->mutex);
    return 2;
  }
  token->thread_started = 1;
  token->thread_joined = 0;
  token->reserved_value = value;
  pthread_mutex_unlock(&token->mutex);

  if (pthread_create(&token->thread, NULL, reserved_worker, token) != 0) {
    pthread_mutex_lock(&token->mutex);
    token->thread_started = 0;
    pthread_mutex_unlock(&token->mutex);
    return 3;
  }
  pthread_mutex_lock(&g_state);
  ++g_live_threads;
  pthread_mutex_unlock(&g_state);
  return 0;
}

int32_t wcc_wait_reserved(wcc_subscription *token) {
  if (!token)
    return 1;
  pthread_mutex_lock(&token->mutex);
  while (!token->reserved_flag)
    pthread_cond_wait(&token->reserved_ready, &token->mutex);
  pthread_mutex_unlock(&token->mutex);
  return 0;
}

int32_t wcc_release_reserved(wcc_subscription *token) {
  if (!token)
    return 1;
  pthread_mutex_lock(&token->mutex);
  token->gate_open = 1;
  pthread_cond_broadcast(&token->gate_opened);
  pthread_mutex_unlock(&token->mutex);
  return 0;
}

int32_t wcc_wait_unregister_active(wcc_subscription *token) {
  if (!token)
    return 1;
  pthread_mutex_lock(&token->mutex);
  while (!token->unregister_active)
    pthread_cond_wait(&token->unregister_ready, &token->mutex);
  pthread_mutex_unlock(&token->mutex);
  return 0;
}

int32_t wcc_apply_static(wcc_static_cb callback, uint64_t context,
                         int32_t value) {
  if (!callback)
    return -999;
  int32_t result = callback(value, context);
  note_result(result);
  return result;
}

int32_t wcc_live_subscriptions(void) {
  pthread_mutex_lock(&g_state);
  int32_t value = g_live_subscriptions;
  pthread_mutex_unlock(&g_state);
  return value;
}

int32_t wcc_live_threads(void) {
  pthread_mutex_lock(&g_state);
  int32_t value = g_live_threads;
  pthread_mutex_unlock(&g_state);
  return value;
}

int32_t wcc_unregister_calls(void) {
  pthread_mutex_lock(&g_state);
  int32_t value = g_unregister_calls;
  pthread_mutex_unlock(&g_state);
  return value;
}

int32_t wcc_callback_calls(void) {
  pthread_mutex_lock(&g_state);
  int32_t value = g_callback_calls;
  pthread_mutex_unlock(&g_state);
  return value;
}

int32_t wcc_registration_failures(void) {
  pthread_mutex_lock(&g_state);
  int32_t value = g_registration_failures;
  pthread_mutex_unlock(&g_state);
  return value;
}

int64_t wcc_callback_sum(void) {
  pthread_mutex_lock(&g_state);
  int64_t value = g_callback_sum;
  pthread_mutex_unlock(&g_state);
  return value;
}

void wcc_reset_counters(void) {
  pthread_mutex_lock(&g_state);
  g_unregister_calls = 0;
  g_callback_calls = 0;
  g_registration_failures = 0;
  g_callback_sum = 0;
  pthread_mutex_unlock(&g_state);
}

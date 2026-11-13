/*
 * Public surface of the user-owned C fixture for retained capturing callbacks.
 *
 * The callback context is always the opaque `void *` created by the caller's
 * runtime.  The fixture stores it together with the callback pointer in an
 * opaque subscription token and never dereferences or frees it.
 */
#ifndef LANG_TEST_C_RETAINED_CAPTURING_FIXTURE_H
#define LANG_TEST_C_RETAINED_CAPTURING_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wcc_subscription wcc_subscription;

typedef int32_t (*wcc_cb_last)(int32_t value, void *context);
typedef int32_t (*wcc_cb_first)(void *context, int32_t value);
typedef int32_t (*wcc_cb_middle)(int32_t left, void *context, int32_t right);
typedef bool (*wcc_cb_bool)(bool flag, void *context);
typedef void (*wcc_cb_unit)(int32_t value, void *context);
typedef int64_t (*wcc_cb_scalar)(int32_t value, void *context);
typedef int32_t (*wcc_static_cb)(int32_t value, uint64_t context);

/* Registration constructors.  Each returns an owned token or NULL.  The
 * context argument is the runtime's opaque userdata and is not owned here. */
wcc_subscription *wcc_subscribe_last(wcc_cb_last callback, void *context);
wcc_subscription *wcc_subscribe_initial(wcc_cb_last callback, void *context,
                                        int32_t value);
wcc_subscription *wcc_subscribe_fail(wcc_cb_last callback, void *context);
wcc_subscription *wcc_subscribe_fail_after(wcc_cb_last callback, void *context,
                                           int32_t value);
wcc_subscription *wcc_subscribe_first(wcc_cb_first callback, void *context);
wcc_subscription *wcc_subscribe_middle(wcc_cb_middle callback, void *context);
wcc_subscription *wcc_subscribe_bool(wcc_cb_bool callback, void *context);
wcc_subscription *wcc_subscribe_unit(wcc_cb_unit callback, void *context);
wcc_subscription *wcc_subscribe_scalar(wcc_cb_scalar callback, void *context);

/* Close/unregister.  The first call marks the token closed, prevents new
 * dispatches and waits for every reserved or active dispatch.  The runtime
 * calls this exactly once; a second call after the token is freed is invalid. */
void wcc_unsubscribe(wcc_subscription *token);
void wcc_unsubscribe_first(wcc_subscription *token);
void wcc_unsubscribe_middle(wcc_subscription *token);
void wcc_unsubscribe_bool(wcc_subscription *token);
void wcc_unsubscribe_unit(wcc_subscription *token);
void wcc_unsubscribe_scalar(wcc_subscription *token);

/* Ordinary borrowed-token calls.  They never retain the token. */
int32_t wcc_invoke_last(wcc_subscription *token, int32_t value);
int32_t wcc_pair_last(wcc_subscription *first, wcc_subscription *second,
                      int32_t value);
int32_t wcc_invoke_first(wcc_subscription *token, int32_t value);
int32_t wcc_invoke_middle(wcc_subscription *token, int32_t left,
                          int32_t right);
bool wcc_invoke_bool(wcc_subscription *token, bool flag);
void wcc_invoke_unit(wcc_subscription *token, int32_t value);
int64_t wcc_invoke_scalar(wcc_subscription *token, int32_t value);

int32_t wcc_info_last(wcc_subscription *token);
int32_t wcc_calls(wcc_subscription *token);
int32_t wcc_calls_first(wcc_subscription *token);
int32_t wcc_calls_middle(wcc_subscription *token);
int32_t wcc_calls_bool(wcc_subscription *token);
int32_t wcc_calls_unit(wcc_subscription *token);
int32_t wcc_calls_scalar(wcc_subscription *token);
int64_t wcc_sum_last(wcc_subscription *token);
int32_t wcc_last_value(wcc_subscription *token);
bool wcc_subscription_null(wcc_subscription *token);

/* External C pthread invocation.  Unregister joins the worker. */
int32_t wcc_start_thread(wcc_subscription *token, int32_t count);
int32_t wcc_join_thread(wcc_subscription *token);
int32_t wcc_thread_calls(wcc_subscription *token);
int64_t wcc_thread_sum(wcc_subscription *token);

/* Controlled pre-trampoline reservation.  wcc_start_reserved reserves
 * in_flight and announces it; wcc_release_reserved lets the worker enter the
 * callback.  wcc_wait_unregister_active reports that wcc_unsubscribe has
 * marked the token closed and is draining. */
int32_t wcc_start_reserved(wcc_subscription *token, int32_t value);
int32_t wcc_wait_reserved(wcc_subscription *token);
int32_t wcc_release_reserved(wcc_subscription *token);
int32_t wcc_wait_unregister_active(wcc_subscription *token);

/* Existing static callback surface, used only to prove its rejection rules
 * are unchanged. */
int32_t wcc_apply_static(wcc_static_cb callback, uint64_t context,
                         int32_t value);

int32_t wcc_live_subscriptions(void);
int32_t wcc_live_threads(void);
int32_t wcc_unregister_calls(void);
int32_t wcc_callback_calls(void);
int32_t wcc_registration_failures(void);
int64_t wcc_callback_sum(void);
void wcc_reset_counters(void);

#ifdef __cplusplus
}
#endif

#endif

// Standalone fixture lifecycle evidence.  This does not use the Woozy
// frontend, backend or runtime: it checks that the user-owned C library really
// stores callback+userdata in an opaque token, returns null only after
// quiescence, prevents new dispatches on unregister, drains a pre-trampoline
// reservation, and supports first/middle/last context positions.
#include <pthread.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

#include "capturing_fixture.h"

namespace {

int failures = 0;

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    ++failures;
  }
}

struct Context {
  std::atomic<int> calls{0};
  std::atomic<int> entered{0};
  int32_t last{0};
  int32_t tag{0};
};

extern "C" int32_t callback_last(int32_t value, void *opaque) {
  auto *context = static_cast<Context *>(opaque);
  context->last = value;
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return value * 2 + context->tag;
}

extern "C" int32_t callback_first(void *opaque, int32_t value) {
  auto *context = static_cast<Context *>(opaque);
  context->last = value;
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return value * 3 + context->tag;
}

extern "C" int32_t callback_middle(int32_t left, void *opaque,
                                   int32_t right) {
  auto *context = static_cast<Context *>(opaque);
  context->last = left;
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return left * 10 + right + context->tag;
}

extern "C" bool callback_bool(bool flag, void *opaque) {
  auto *context = static_cast<Context *>(opaque);
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return !flag;
}

extern "C" void callback_unit(int32_t value, void *opaque) {
  auto *context = static_cast<Context *>(opaque);
  context->last = value;
  context->calls.fetch_add(1, std::memory_order_relaxed);
}

extern "C" int64_t callback_scalar(int32_t value, void *opaque) {
  auto *context = static_cast<Context *>(opaque);
  context->last = value;
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return static_cast<int64_t>(value) * 10000000000LL + context->tag;
}

extern "C" int32_t callback_reserved(int32_t value, void *opaque) {
  auto *context = static_cast<Context *>(opaque);
  context->entered.fetch_add(1, std::memory_order_seq_cst);
  context->last = value;
  context->calls.fetch_add(1, std::memory_order_relaxed);
  return value * 2 + context->tag;
}

extern "C" int32_t callback_static(int32_t value, uint64_t context) {
  return value * 4 + static_cast<int32_t>(context);
}

struct UnregisterThread {
  wcc_subscription *token = nullptr;
  Context *context = nullptr;
  bool entered_before_return = false;
};

extern "C" void *unregister_from_thread(void *opaque) {
  auto *argument = static_cast<UnregisterThread *>(opaque);
  wcc_unsubscribe(argument->token);
  argument->entered_before_return =
      argument->context->entered.load(std::memory_order_seq_cst) != 0;
  return nullptr;
}

void lifecycle() {
  wcc_reset_counters();
  Context context;
  context.tag = 7;
  wcc_subscription *token = wcc_subscribe_last(callback_last, &context);
  check(token != nullptr, "subscribe_last returns a token");
  check(wcc_invoke_last(token, 7) == 21, "ordinary invoke borrows token");
  check(context.calls.load() == 1, "callback entered once");
  check(wcc_info_last(token) == 1, "info sees live token");
  check(wcc_calls(token) == 1, "per-token call count");
  check(wcc_sum_last(token) == 21, "per-token result sum");
  wcc_unsubscribe(token);
  check(wcc_live_subscriptions() == 0, "unregister frees token");
  check(wcc_unregister_calls() == 1, "one unregister transition");
  check(wcc_callback_calls() == 1, "one callback total");
}

void failure_paths() {
  wcc_reset_counters();
  Context context;
  context.tag = 7;
  check(wcc_subscribe_fail(callback_last, &context) == nullptr,
        "null failure result");
  check(context.calls.load() == 0, "failed registration never calls back");
  check(wcc_callback_calls() == 0, "failure leaves callback count zero");
  check(wcc_live_subscriptions() == 0, "failure leaves no live token");
  check(wcc_registration_failures() == 1, "failure counter");
  check(wcc_unregister_calls() == 0, "failure does not call unregister");

  check(wcc_subscribe_fail_after(callback_last, &context, 5) == nullptr,
        "post-callback failure result");
  check(context.calls.load() == 1, "post-callback failure entered once");
  check(context.last == 5, "post-callback failure saw the value");
  check(wcc_callback_calls() == 1, "post-callback failure counted once");
  check(wcc_live_subscriptions() == 0, "post-callback failure is quiescent");
  check(wcc_registration_failures() == 2, "second failure counter");
}

void synchronous_registration() {
  wcc_reset_counters();
  Context context;
  context.tag = 21;
  wcc_subscription *token = wcc_subscribe_initial(callback_last, &context, 21);
  check(token != nullptr, "synchronous registration token");
  check(context.calls.load() == 1, "callback ran before subscribe returned");
  check(context.last == 21, "synchronous value");
  check(wcc_calls(token) == 1, "synchronous call counted");
  check(wcc_invoke_last(token, 1) == 23, "token works after returning");
  wcc_unsubscribe(token);
  check(wcc_live_subscriptions() == 0, "synchronous token closed");
}

void two_resource_borrow() {
  wcc_reset_counters();
  {
    Context context;
    context.tag = 3;
    auto *token = wcc_subscribe_last(callback_last, &context);
    check(token != nullptr, "pair same token");
    // first: 4*2+3=11, second: 5*2+3=13
    check(wcc_pair_last(token, token, 4) == 11013,
          "same token borrowed twice");
    check(context.calls.load() == 2, "same token callback entered twice");
    wcc_unsubscribe(token);
  }
  check(wcc_live_subscriptions() == 0, "same-token pair closed");
  check(wcc_unregister_calls() == 1, "same-token pair unregister");
  {
    Context first_context;
    first_context.tag = 3;
    Context second_context;
    second_context.tag = 5;
    auto *first = wcc_subscribe_last(callback_last, &first_context);
    auto *second = wcc_subscribe_last(callback_last, &second_context);
    check(first != nullptr && second != nullptr, "pair distinct tokens");
    // first: 4*2+3=11, second: 5*2+5=15
    check(wcc_pair_last(first, second, 4) == 11015,
          "distinct tokens borrowed in argument order");
    check(first_context.calls.load() == 1, "first distinct callback once");
    check(second_context.calls.load() == 1, "second distinct callback once");
    wcc_unsubscribe(first);
    wcc_unsubscribe(second);
  }
  check(wcc_live_subscriptions() == 0, "distinct-token pair closed");
  check(wcc_unregister_calls() == 3, "distinct-token pair unregisters");
}

void context_positions() {
  wcc_reset_counters();
  {
    Context context;
    context.tag = 100;
    auto *token = wcc_subscribe_first(callback_first, &context);
    check(token != nullptr, "subscribe_first token");
    check(wcc_invoke_first(token, 5) == 115, "context-first callback");
    check(wcc_calls_first(token) == 1, "context-first count");
    wcc_unsubscribe(token);
  }
  {
    Context context;
    context.tag = 200;
    auto *token = wcc_subscribe_middle(callback_middle, &context);
    check(token != nullptr, "subscribe_middle token");
    check(wcc_invoke_middle(token, 3, 4) == 234, "context-middle callback");
    check(wcc_calls_middle(token) == 1, "context-middle count");
    wcc_unsubscribe(token);
  }
  {
    Context context;
    auto *token = wcc_subscribe_bool(callback_bool, &context);
    check(token != nullptr, "subscribe_bool token");
    check(wcc_invoke_bool(token, true) == false, "bool callback result");
    check(wcc_calls_bool(token) == 1, "bool callback count");
    wcc_unsubscribe(token);
  }
  {
    Context context;
    auto *token = wcc_subscribe_unit(callback_unit, &context);
    check(token != nullptr, "subscribe_unit token");
    wcc_invoke_unit(token, 9);
    check(context.last == 9, "Unit callback observed value");
    check(wcc_calls_unit(token) == 1, "Unit callback count");
    wcc_unsubscribe(token);
  }
  {
    Context context;
    context.tag = 7;
    auto *token = wcc_subscribe_scalar(callback_scalar, &context);
    check(token != nullptr, "subscribe_scalar token");
    check(wcc_invoke_scalar(token, 5) == 50000000007LL,
          "i64 callback result");
    check(wcc_calls_scalar(token) == 1, "i64 callback count");
    wcc_unsubscribe(token);
  }
  check(wcc_live_subscriptions() == 0, "all position tokens closed");
  check(wcc_unregister_calls() == 5, "five unregister transitions");
  check(wcc_apply_static(callback_static, 3, 7) == 31,
        "ordinary static callback path unchanged");
}

void external_thread() {
  wcc_reset_counters();
  Context context;
  context.tag = 7;
  auto *token = wcc_subscribe_last(callback_last, &context);
  check(token != nullptr, "thread token");
  check(wcc_start_thread(token, 16) == 0, "start external C pthread");
  check(wcc_join_thread(token) == 0, "join external C pthread");
  check(wcc_thread_calls(token) == 16, "external thread call count");
  check(wcc_thread_sum(token) == 352, "external thread sum");
  check(wcc_calls(token) == 16, "external calls counted on token");
  check(wcc_live_threads() == 0, "no live worker");
  wcc_unsubscribe(token);
  check(wcc_live_subscriptions() == 0, "thread token closed");
  check(wcc_callback_calls() == 16, "all external callbacks counted");
}

void pre_trampoline_reservation() {
  wcc_reset_counters();
  Context context;
  context.tag = 5;
  auto *token = wcc_subscribe_last(callback_reserved, &context);
  check(token != nullptr, "reserved token");
  check(wcc_start_reserved(token, 11) == 0, "start reserved dispatch");
  check(wcc_wait_reserved(token) == 0, "reservation event");
  check(context.entered.load() == 0,
        "callback has not entered the trampoline yet");

  UnregisterThread argument;
  argument.token = token;
  argument.context = &context;
  pthread_t thread;
  check(pthread_create(&thread, nullptr, unregister_from_thread, &argument) == 0,
        "create unregister thread");
  check(wcc_wait_unregister_active(token) == 0, "unregister is waiting");
  check(context.entered.load() == 0,
        "unregister waits before the trampoline is entered");
  check(wcc_release_reserved(token) == 0, "release reserved dispatch");
  check(pthread_join(thread, nullptr) == 0, "join unregister thread");
  check(context.entered.load() == 1, "reserved callback entered exactly once");
  check(argument.entered_before_return,
        "unregister returned after the callback completed");
  check(wcc_live_subscriptions() == 0, "reserved token freed");
  check(wcc_live_threads() == 0, "reserved worker gone");
  check(wcc_unregister_calls() == 1, "one unregister transition");
  check(wcc_callback_calls() == 1, "one reserved callback");
}

} // namespace

int main() {
  check(wcc_subscription_null(nullptr), "null token predicate");
  lifecycle();
  failure_paths();
  synchronous_registration();
  two_resource_borrow();
  context_positions();
  external_thread();
  pre_trampoline_reservation();
  check(wcc_live_subscriptions() == 0, "no live tokens at exit");
  check(wcc_live_threads() == 0, "no live threads at exit");
  if (failures == 0)
    std::puts("PASS c_retained_capturing fixture lifecycle");
  return failures == 0 ? 0 : 1;
}

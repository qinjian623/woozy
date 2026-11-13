#ifndef LANG_RUNTIME_HOSTED_C_CALLBACK_HPP
#define LANG_RUNTIME_HOSTED_C_CALLBACK_HPP

// Private hosted-only ABI. Source never observes the context or this token.
struct LangCCallbackFn {
  void *code;
  void *env;
};
struct LangCCallbackToken {
  void *context;
  void *previous;
};
static_assert(sizeof(LangCCallbackFn) == 16 && sizeof(LangCCallbackToken) == 16);

extern "C" {
// Called ACTIVE, before entering the ordinary C adapter SAFE scope. Retains env.
void *lang_c_registration_new_v1(void *code, void *env,
                                void (*unregister)(void *) noexcept) noexcept;
// Called ACTIVE after the adapter returns. Null token consumes ctx and returns
// null; the C API must already have stopped and drained any failed registration.
void *lang_c_registration_finish_v1(void *ctx, void *foreign_token) noexcept;
// Explicit close is idempotent, drains foreign-call leases and callbacks, then
// drops the external env root ACTIVE. Self-wait is a terminal Panic.
void lang_c_registration_close_v1(void *ctx) noexcept;
// Consumes the Source resource owner. Collector release defers this external
// root intact until resume. Inherited foreign registrations are abandoned.
void lang_c_registration_release_v1(void *ctx) noexcept;
// The bridge surrounds enter/leave with the existing GC callback scope. Leave
// follows GC callback_leave; no Wz value is accessed afterwards.
const LangCCallbackFn *lang_c_registration_enter_v1(
    void *ctx, LangCCallbackToken *token) noexcept;
void lang_c_registration_leave_v1(LangCCallbackToken *token) noexcept;
// A C adapter leases the foreign token for its whole external call, even in
// SAFE. Closing rejects new unrelated leases and waits for prior ones. Nested
// leases from an existing callback/lease may finish while close is pending.
void *lang_c_registration_borrow_v1(void *ctx,
                                   LangCCallbackToken *token) noexcept;
void lang_c_registration_end_borrow_v1(LangCCallbackToken *token) noexcept;
}

#endif

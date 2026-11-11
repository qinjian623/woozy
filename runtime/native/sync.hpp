#ifndef LANG_RUNTIME_NATIVE_SYNC_HPP
#define LANG_RUNTIME_NATIVE_SYNC_HPP

#include <cstdint>

#include "handle.hpp"

// Private native ABI. Copies share one atomic-ARC object; allocation failure
// and invalid lock use Panic. wait returns with the mutex held. AtomicI64
// operations are seq_cst; fetch_add wraps and strong CAS returns success.

extern "C" {

LangHandle *lang_mutex_new_v1() noexcept;
void lang_mutex_lock_v1(LangHandle *mutex) noexcept;
void lang_mutex_unlock_v1(LangHandle *mutex) noexcept;

LangHandle *lang_condvar_new_v1() noexcept;
void lang_condvar_wait_v1(LangHandle *condvar, LangHandle *mutex) noexcept;
void lang_condvar_notify_one_v1(LangHandle *condvar) noexcept;
void lang_condvar_notify_all_v1(LangHandle *condvar) noexcept;

LangHandle *lang_atomic_i64_new_v1(int64_t initial) noexcept;
int64_t lang_atomic_i64_load_v1(LangHandle *atomic) noexcept;
void lang_atomic_i64_store_v1(LangHandle *atomic, int64_t value) noexcept;
int64_t lang_atomic_i64_fetch_add_v1(LangHandle *atomic, int64_t delta) noexcept;
bool lang_atomic_i64_compare_exchange_v1(LangHandle *atomic, int64_t expected,
                                         int64_t desired) noexcept;

} // extern "C"

#endif

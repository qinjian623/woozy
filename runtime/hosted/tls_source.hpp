#ifndef LANG_RUNTIME_HOSTED_TLS_SOURCE_HPP
#define LANG_RUNTIME_HOSTED_TLS_SOURCE_HPP

#include <stdint.h>

// Explicitly selected hosted OpenSSL link.  The accepted low-level ABI is in
// tls.hpp; this helper only adds storage/resources and wrappers around it.
#include "tls.hpp"

// Small C-owned storage/resource helpers for the Woozy-facing std/tls module.
//
// The language-facing module cannot pass String/Vec storage as a C pointer and
// has no null pointer literal.  These helpers therefore own three tiny stable
// resources (error storage, count storage, byte storage) and wrap the accepted
// lang-tls/1 C ABI in resources whose retain/release functions fit the schema
// v2 C binding contract.  No TLS or HTTP parsing is implemented here.
//
// Ownership
// ---------
// Every Wts* object returned by *_new / wts_context_* / wts_stream_* starts with
// one reference.  retain adds one and release drops one; NULL retain/release is
// a no-op.  A stream wrapper holds one reference to its low-level LangTlsStream
// (which itself retains the context).  Byte/error/count storage may be copied
// normally by Woozy; the last copy releases the C object exactly once.
//
// WtsBytes object allocation may return NULL; wts_bytes_failed(NULL) is true.
// A data-allocation failure leaves a failed object whose data pointer is NULL.
// Zero-capacity storage also has a NULL data pointer.
// All functions are noexcept; C++ exceptions never cross this ABI.

#ifdef __cplusplus
#define WTS_NOEXCEPT noexcept
extern "C" {
#else
#define WTS_NOEXCEPT
#endif

typedef struct WtsError WtsError;
typedef struct WtsCount WtsCount;
typedef struct WtsBytes WtsBytes;
typedef struct WtsContext WtsContext;
typedef struct WtsStream WtsStream;

// Error storage ------------------------------------------------------------
WtsError *wts_error_new(void) WTS_NOEXCEPT;
void wts_error_retain(WtsError *storage) WTS_NOEXCEPT;
void wts_error_release(WtsError *storage) WTS_NOEXCEPT;
void wts_error_reset(WtsError *storage) WTS_NOEXCEPT;
int32_t wts_error_is_null(const WtsError *storage) WTS_NOEXCEPT;
int32_t wts_error_category(const WtsError *storage) WTS_NOEXCEPT;
int32_t wts_error_system_errno(const WtsError *storage) WTS_NOEXCEPT;
int32_t wts_error_ssl_error(const WtsError *storage) WTS_NOEXCEPT;
int64_t wts_error_verify_result(const WtsError *storage) WTS_NOEXCEPT;
uint64_t wts_error_openssl_error(const WtsError *storage) WTS_NOEXCEPT;
uint64_t wts_error_openssl_error_count(const WtsError *storage) WTS_NOEXCEPT;

// Count output storage -----------------------------------------------------
WtsCount *wts_count_new(void) WTS_NOEXCEPT;
void wts_count_retain(WtsCount *storage) WTS_NOEXCEPT;
void wts_count_release(WtsCount *storage) WTS_NOEXCEPT;
uint64_t wts_count_value(const WtsCount *storage) WTS_NOEXCEPT;
void wts_count_set(WtsCount *storage, uint64_t value) WTS_NOEXCEPT;
int32_t wts_count_is_null(const WtsCount *storage) WTS_NOEXCEPT;

// Byte storage -------------------------------------------------------------
// Capacity is capped at SIZE_MAX; zero capacity is valid.  Resize preserves the
// first min(old_length, new_capacity) bytes and clamps length when shrinking.
// Growing leaves length unchanged; set it explicitly after filling the bytes.
WtsBytes *wts_bytes_new(uint64_t capacity) WTS_NOEXCEPT;
void wts_bytes_retain(WtsBytes *storage) WTS_NOEXCEPT;
void wts_bytes_release(WtsBytes *storage) WTS_NOEXCEPT;
uint64_t wts_bytes_capacity(const WtsBytes *storage) WTS_NOEXCEPT;
uint64_t wts_bytes_length(const WtsBytes *storage) WTS_NOEXCEPT;
void wts_bytes_set_length(WtsBytes *storage, uint64_t length) WTS_NOEXCEPT;
int32_t wts_bytes_failed(const WtsBytes *storage) WTS_NOEXCEPT;
uint8_t *wts_bytes_data_mut(WtsBytes *storage) WTS_NOEXCEPT;
const uint8_t *wts_bytes_data(const WtsBytes *storage) WTS_NOEXCEPT;
int32_t wts_bytes_resize(WtsBytes *storage, uint64_t capacity) WTS_NOEXCEPT;

// Context resources --------------------------------------------------------
WtsContext *wts_context_client_default(WtsError *error) WTS_NOEXCEPT;
WtsContext *wts_context_client(WtsBytes *ca_file, WtsError *error) WTS_NOEXCEPT;
WtsContext *wts_context_server(WtsBytes *certificate_chain, WtsBytes *private_key,
                               WtsError *error) WTS_NOEXCEPT;
void wts_context_retain(WtsContext *context) WTS_NOEXCEPT;
void wts_context_release(WtsContext *context) WTS_NOEXCEPT;
int32_t wts_context_is_null(const WtsContext *context) WTS_NOEXCEPT;

// Stream resources ---------------------------------------------------------
// fd is borrowed exactly as in lang-tls/1: the helper never closes it.
WtsStream *wts_stream_client(WtsContext *context, int32_t fd, int32_t name_kind,
                             WtsBytes *name, WtsError *error) WTS_NOEXCEPT;
WtsStream *wts_stream_server(WtsContext *context, int32_t fd,
                             WtsError *error) WTS_NOEXCEPT;
void wts_stream_retain(WtsStream *stream) WTS_NOEXCEPT;
void wts_stream_release(WtsStream *stream) WTS_NOEXCEPT;
int32_t wts_stream_is_null(const WtsStream *stream) WTS_NOEXCEPT;
int32_t wts_stream_descriptor(const WtsStream *stream) WTS_NOEXCEPT;
uint64_t wts_stream_token(const WtsStream *stream) WTS_NOEXCEPT;

// Progress wrappers.  Status values are the frozen LangTlsStatus values.
int32_t wts_handshake(WtsStream *stream, WtsError *error) WTS_NOEXCEPT;
int32_t wts_read(WtsStream *stream, uint8_t *data, uint64_t capacity,
                 WtsCount *count, WtsError *error) WTS_NOEXCEPT;
int32_t wts_write(WtsStream *stream, const uint8_t *data, uint64_t length,
                  WtsCount *count, WtsError *error) WTS_NOEXCEPT;
int32_t wts_shutdown(WtsStream *stream, WtsError *error) WTS_NOEXCEPT;

// Transport callbacks used by std/https through a borrowed ID. IDs are checked
// and never recycled; expired/unknown IDs return fd -1 or INVALID_ARGUMENT,
// never access an address supplied by the caller. Each I/O call temporarily
// retains the stream. Callers keep a stream owner for the transport lifetime.
int32_t wts_transport_fd(uint64_t token) WTS_NOEXCEPT;
int32_t wts_transport_read(uint64_t token, uint8_t *data, uint64_t capacity,
                           WtsCount *count, WtsError *error) WTS_NOEXCEPT;
int32_t wts_transport_write(uint64_t token, const uint8_t *data,
                            uint64_t length, WtsCount *count,
                            WtsError *error) WTS_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif
#undef WTS_NOEXCEPT

#endif // LANG_RUNTIME_HOSTED_TLS_SOURCE_HPP

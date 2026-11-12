#ifndef LANG_RUNTIME_HOSTED_TLS_HPP
#define LANG_RUNTIME_HOSTED_TLS_HPP

#include <stdint.h>

// Explicitly selected hosted TLS ABI: lang-tls/1.
//
// This is a low-level C ABI over OpenSSL 3.  It is only linked into programs
// whose programmer selected the hosted runtime; the freestanding native
// runtime neither includes this header nor depends on libssl/libcrypto.
//
// Ownership and lifetime
// ----------------------
// LangTlsContext and LangTlsStream are opaque reference-counted objects.  Each
// contains a uint64_t atomic reference count, but callers must not depend on
// the object layout or on LangHandle.  Objects start with one reference.
// retain adds one reference; release drops one and destroys on the last one.
// A stream holds one reference to its context until the last stream reference
// is released.  Releasing a context while a stream still exists is safe.
// Null is accepted by retain/release and is a no-op.
//
// The stream borrows `fd`; it never closes it, including on error or release.
// The caller owns the descriptor and may close it after releasing the stream.
// A stream is not safe for concurrent calls; use one stream from one thread at
// a time.  Retain/release are safe to use concurrently with other retain/
// release calls, but not with an I/O call on the same stream.
//
// Security defaults
// -----------------
// Client contexts always verify the server certificate chain and validity.
// There is no "insecure" creation function.  Default trust uses the platform
// OpenSSL default verify paths; an explicit context uses exactly the supplied
// CA file.  Client streams require a non-empty expected name.  DNS names use
// SSL_set1_host for RFC 6125-style matching and SSL_set_tlsext_host_name for
// SNI.  IPv4/IPv6 names use X509_VERIFY_PARAM_set1_ip and are never sent as
// SNI.  LANG_TLS_NAME_AUTO detects syntactically valid IP literals; otherwise
// it treats the name as DNS.  No IDNA conversion is performed.
//
// Both context kinds require TLS 1.2 or newer.  Cipher suites, groups,
// certificate algorithms, security level, and other policy remain OpenSSL's
// configured defaults.  Server contexts load a PEM certificate chain and PEM
// private key and require the two to match.
//
// I/O, retry, and errors
// ----------------------
// The socket BIO uses send(MSG_NOSIGNAL) and recv, so a broken peer cannot
// raise SIGPIPE through this ABI and the process-wide SIGPIPE disposition is
// not changed.  The BIO propagates EAGAIN/EWOULDBLOCK and EINTR as OpenSSL
// retry flags; neither is reported as EOF.
//
// Callers must handle partial I/O and non-blocking sockets:
//
//   * On OK for write, *out_count bytes were accepted.  If it is less than the
//     requested length, advance the buffer and call again for the remainder.
//     SSL_MODE_ENABLE_PARTIAL_WRITE is enabled.  SSL_MODE_ACCEPT_MOVING_WRITE_
//     BUFFER is also enabled, so the pointer may move between calls, but after
//     WANT_READ/WANT_WRITE the caller must retry with the same requested
//     length and the same still-unwritten bytes.  Do not advance on WANT.
//   * On WANT_READ or WANT_WRITE, poll `fd` for readability/writability and
//     retry the same call.  A retry may be immediate after EINTR, but polling
//     is always valid.  No sleep-based busy loop is performed by this ABI.
//   * On read, OK with *out_count > 0 returns decrypted bytes.  A clean TLS
//     close_notify is LANG_TLS_STATUS_EOF with category NONE; repeated reads
//     after it also report EOF.  A TCP EOF or truncation without close_notify
//     is LANG_TLS_STATUS_ERROR with category UNEXPECTED_EOF, never EOF.
//   * Zero-length read/write is a successful no-op with *out_count == 0.
//   * A single call accepts at most SIZE_MAX bytes (all uint64_t lengths are
//     representable on the supported 64-bit hosted targets).
//
// shutdown is bidirectional.  It sends our close_notify and then waits for the
// peer's close_notify.  OK means both directions completed.  WANT_READ means
// our close_notify was sent and the caller should poll for readability and
// retry shutdown; WANT_WRITE means the close_notify write itself should be
// retried.  A caller that only wants to send close_notify may release the
// stream after a WANT_READ result instead of retrying.
//
// Every failure fills the optional LangTlsError with a stable category plus
// diagnostics.  `verify_result` is X509_V_OK (0) or an X509_V_ERR_* value;
// `openssl_error` is the last OpenSSL error code and `openssl_error_count` is
// the number of queued errors drained for this call; `ssl_error` is the
// SSL_get_error classification; `system_errno` is set only for a real socket
// syscall failure and is never used as the primary error identity.  The error
// queue used is OpenSSL's thread-local queue; this ABI clears it at call entry.
//
// All text is passed as bytes plus an explicit length.  Embedded NUL bytes are
// rejected.  Host names must be non-empty and at most 255 bytes.  File paths
// must be non-empty.  Data buffers are arbitrary binary; only a null pointer
// with a non-zero length is invalid.
//
// Returned pointers are owned by the caller with one reference.  Creation
// functions return null on failure and fill `error` when it is non-null.
// Every function is noexcept; C++ exceptions never cross this C ABI.

#ifdef __cplusplus
#define LANG_TLS_NOEXCEPT noexcept
extern "C" {
#else
#define LANG_TLS_NOEXCEPT
#endif

typedef struct LangTlsContext LangTlsContext;
typedef struct LangTlsStream LangTlsStream;

typedef enum LangTlsStatus {
  LANG_TLS_STATUS_OK = 0,
  LANG_TLS_STATUS_WANT_READ = 1,
  LANG_TLS_STATUS_WANT_WRITE = 2,
  LANG_TLS_STATUS_EOF = 3,
  LANG_TLS_STATUS_ERROR = 4
} LangTlsStatus;

typedef enum LangTlsErrorCategory {
  LANG_TLS_ERROR_NONE = 0,
  LANG_TLS_ERROR_INVALID_ARGUMENT = 1,
  LANG_TLS_ERROR_CONTEXT = 2,
  LANG_TLS_ERROR_CERTIFICATE = 3,
  LANG_TLS_ERROR_HANDSHAKE = 4,
  LANG_TLS_ERROR_PROTOCOL = 5,
  LANG_TLS_ERROR_IO = 6,
  LANG_TLS_ERROR_UNEXPECTED_EOF = 7,
  LANG_TLS_ERROR_SHUTDOWN = 8,
  LANG_TLS_ERROR_INTERNAL = 9
} LangTlsErrorCategory;

typedef enum LangTlsNameKind {
  // Detect a syntactically valid IPv4/IPv6 literal and use IP verification.
  // All other names use DNS verification and SNI.
  LANG_TLS_NAME_AUTO = 0,
  LANG_TLS_NAME_DNS = 1,
  LANG_TLS_NAME_IP = 2
} LangTlsNameKind;

typedef struct LangTlsError {
  int32_t category;          // LangTlsErrorCategory
  int32_t system_errno;      // 0 unless a socket syscall failed
  int32_t ssl_error;         // SSL_get_error result, 0 if not applicable
  int32_t reserved;          // always 0
  int64_t verify_result;     // X509_V_OK or X509_V_ERR_*
  uint64_t openssl_error;    // last OpenSSL error code, 0 if none
  uint64_t openssl_error_count;
} LangTlsError;

#ifdef __cplusplus
static_assert(sizeof(LangTlsError) == 40, "LangTlsError ABI size");
static_assert(__builtin_offsetof(LangTlsError, category) == 0,
              "LangTlsError.category offset");
static_assert(__builtin_offsetof(LangTlsError, verify_result) == 16,
              "LangTlsError.verify_result offset");
static_assert(__builtin_offsetof(LangTlsError, openssl_error) == 24,
              "LangTlsError.openssl_error offset");
#endif

// Client context using OpenSSL's platform default trust store.
LangTlsContext *lang_tls_context_client_default_v1(LangTlsError *error)
    LANG_TLS_NOEXCEPT;

// Client context using exactly the supplied PEM CA file (one or more certs).
LangTlsContext *lang_tls_context_client_v1(const uint8_t *ca_file,
                                           uint64_t ca_file_length,
                                           LangTlsError *error)
    LANG_TLS_NOEXCEPT;

// Server context using a PEM certificate chain and PEM private key.  The key
// must match the leaf certificate.  Client certificates are not requested.
LangTlsContext *lang_tls_context_server_v1(
    const uint8_t *certificate_chain_file, uint64_t certificate_chain_length,
    const uint8_t *private_key_file, uint64_t private_key_length,
    LangTlsError *error) LANG_TLS_NOEXCEPT;

void lang_tls_context_retain_v1(LangTlsContext *context) LANG_TLS_NOEXCEPT;
void lang_tls_context_release_v1(LangTlsContext *context) LANG_TLS_NOEXCEPT;

// Create a client stream over the borrowed fd.  `name` is the expected server
// identity.  It is required and must not contain NUL.  The stream retains the
// context.  No handshake is performed here.
LangTlsStream *lang_tls_stream_client_v1(LangTlsContext *context, int32_t fd,
                                         LangTlsNameKind name_kind,
                                         const uint8_t *name,
                                         uint64_t name_length,
                                         LangTlsError *error)
    LANG_TLS_NOEXCEPT;

// Create a server stream over the borrowed fd.  The context must be a server
// context.  The stream retains the context.  No handshake is performed here.
LangTlsStream *lang_tls_stream_server_v1(LangTlsContext *context, int32_t fd,
                                         LangTlsError *error)
    LANG_TLS_NOEXCEPT;

void lang_tls_stream_retain_v1(LangTlsStream *stream) LANG_TLS_NOEXCEPT;
void lang_tls_stream_release_v1(LangTlsStream *stream) LANG_TLS_NOEXCEPT;

// Drive SSL_connect (client) or SSL_accept (server).  Repeated calls after
// success return OK.  The stream role and context role must have matched at
// creation time.
LangTlsStatus lang_tls_handshake_v1(LangTlsStream *stream,
                                    LangTlsError *error) LANG_TLS_NOEXCEPT;

// Decrypt into `data`.  *out_count is always written.  It is > 0 only for OK.
// EOF is reported only for a received close_notify.
LangTlsStatus lang_tls_read_v1(LangTlsStream *stream, void *data,
                               uint64_t capacity, uint64_t *out_count,
                               LangTlsError *error) LANG_TLS_NOEXCEPT;

// Encrypt from `data`.  *out_count is always written.  A positive partial
// count on OK is normal; see the retry rules above.  `length == 0` is a no-op.
LangTlsStatus lang_tls_write_v1(LangTlsStream *stream, const void *data,
                                uint64_t length, uint64_t *out_count,
                                LangTlsError *error) LANG_TLS_NOEXCEPT;

// Send close_notify and wait for the peer's close_notify.  See above.
LangTlsStatus lang_tls_shutdown_v1(LangTlsStream *stream,
                                   LangTlsError *error) LANG_TLS_NOEXCEPT;

#ifdef __cplusplus
} // extern "C"
#endif
#undef LANG_TLS_NOEXCEPT

#endif // LANG_RUNTIME_HOSTED_TLS_HPP

#ifndef LANG_RUNTIME_NATIVE_DNS_HPP
#define LANG_RUNTIME_NATIVE_DNS_HPP

#include <cstdint>

#include "network.hpp"
#include "text.hpp"

// Private native DNS/hostname ABI: lang-native-dns/1.
//
// This layer sits on top of the accepted network primitive ABI, the native
// runtime Vec API, and the canonical borrowed LangText view from text.hpp.
// It is not a Source-visible raw-pointer API: the source standard library
// exposes a typed lookup that throws os.Error.
//
// The hostname is read from LangText.data[0 .. LangText.bytes), which is
// borrowed for the duration of the call and need not be NUL terminated.
// LangText.length is the Unicode scalar count and is not a byte count; DNS
// hostnames are ASCII/punycode and the resolver uses `bytes` exclusively.
// LangText.owner is borrowed and not retained.

extern "C" {

// Resolve a hostname to a fresh owned Vec[LangNetAddress].
//
// Returns 0 on success, or a negative Linux errno. The output is written only
// on success; on failure *out is untouched and all intermediate vectors are
// released. The returned Vec has stride sizeof(LangNetAddress) (24), full
// initialized length, and copy/drop callbacks that retain/release each
// element's `data` Vec. The caller owns one reference to the result Vec and
// must release it with lang_vec_release_v1.
//
// `port` is copied into every result in host byte order.
// `family` is 0 (both IPv4 and IPv6), 2 (IPv4 only), or 10 (IPv6 only).
// Returns up to 64 distinct addresses. `localhost` and numeric IP literals
// are resolved directly.
//
// Hostname input is ASCII DNS presentation form (punycode must already be
// encoded as ASCII; no implicit IDNA), at most 253 bytes excluding a trailing
// dot, with labels at most 63 bytes and no embedded NUL.
//
// Linux reads /etc/hosts and /etc/resolv.conf, applies
// search/domain/ndots ordering, and queries nameservers over UDP with bounded
// attempts and monotonic-clock deadlines. A truncated UDP response is retried
// over TCP with a length prefix. CNAME chains are followed for at most 8 hops.
// Only answer records belonging to the query/CNAME chain are accepted.
// Darwin queries the system resolver and preserves its ordering and IPv6 scope.
//
// Documented error values include -2 for NXDOMAIN/no data, -22 for malformed
// hostnames or DNS messages, and -110 for a timeout. Darwin system resolver
// failures that only indicate retry are returned as -11. Resolver/network failures
// otherwise return the relevant negative errno (for example -ECONNREFUSED or
// -ENETUNREACH). A missing/unreadable resolver file is reported rather than
// being treated as a successful empty answer. DNS name length, label, response
// (65535 bytes), nameserver (3), search-domain (4), retry, and result (64)
// bounds are finite.
int64_t lang_net_lookup_v1(const LangText *hostname, uint16_t port,
                           int32_t family, LangVec **out) noexcept;

// Private deterministic-query helper. It performs one DNS exchange against
// `server` (an IPv4/IPv6 LangNetAddress with a host-order port, typically port
// 53) without reading /etc/hosts or /etc/resolv.conf and without applying
// search domains. `timeout_ms` is a positive per-attempt deadline. `family` is
// 0/2/10; family 0 performs both A and AAAA queries and merges the results.
// Returned addresses have port 0. This entry point exists for resolver tests
// and for lookup's internal retry loop; it is not a Source public API.
int64_t lang_net_dns_query_v1(const LangText *hostname, int32_t family,
                              const LangNetAddress *server,
                              int32_t timeout_ms, LangVec **out) noexcept;

} // extern "C"

#endif // LANG_RUNTIME_NATIVE_DNS_HPP

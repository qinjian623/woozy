# std/https — HTTP/1.1 over std/tls

`std/https` reuses the existing `std/http` framing, limits, header/trailer
parser and persistent `Reader`. It does not contain a second HTTP parser and
does not change the existing `std/http` API or `Host`/`Header.value` byte rules.

The module is available only to explicitly hosted programs that declare the
`woozy_tls` native dependency described by [`std/tls`](../tls/README.md). It
does not add TLS to the freestanding runtime.

## Public types

```wz
pub struct Connection { /* TLS stream, owned socket, persistent HTTP reader */ }
pub struct Server { /* listening socket and TLS server context */ }

pub enum Error {
    Tls(error tls.Error);
    Net(error os.Error);
    Http(error http.Error);
    Closed;
}
```

`Connection` owns the socket created or accepted by this module. Call `close`
once per connection; copies of a `Connection` share the same TLS resource, so do
not close the same connection through two aliases. `Server` owns its listening
socket; call `close_server` when the listener is no longer needed.

## Client

```wz
func connect(context tls.Context, host string, port u16) Connection throws Error
func send(connection Connection, request http.Request) throws Error
func receive(connection Connection, request_method string) http.Response throws Error
func request(context tls.Context, host string, port u16, request http.Request) http.Response throws Error
func close(connection Connection) throws Error
```

`connect` resolves `host` with `std/net.getaddrinfo`, tries the returned
addresses in order, wraps the connected socket with TLS, and verifies the
server identity using `tls.NameKind.Auto` (DNS + SNI for names, IP verification
without SNI for literals). It closes the socket on handshake failure.

`send` and `receive` use one persistent `std/http` reader, so multiple messages
can be sent and received on the same TLS connection. `request` is a
single-request helper that closes the connection afterwards.

If `request` fails while sending or receiving, the TLS stream is already in an
error state. `request` closes the socket it owns exactly once and rethrows the
original error without calling `SSL_shutdown` or waiting for the peer. On
success, it runs the normal graceful `close`. This keeps the borrowed-fd
behavior of `tls.close` unchanged while ensuring an HTTP or fatal TLS failure
never waits for the peer or closes a reused descriptor.

## Server

```wz
func listen(context tls.Context, address net.Address, backlog i32) Server throws Error
func local_address(server Server) net.Address throws Error
func accept(server Server) Connection throws Error
func receive_request(connection Connection) http.Request throws Error
func respond(connection Connection, response http.Response, request_method string) throws Error
func close_server(server Server) throws Error
```

`listen` creates a TCP listener with `SO_REUSEADDR`, `bind` and `listen`.
`accept` accepts one connection and performs the TLS server handshake; on a
handshake failure it closes the accepted socket. `receive_request` and
`respond` reuse the `std/http` persistent reader and encoder for fixed-length,
chunked, trailer, no-body and pipelined/continuous messages.

## Coverage

The source tests exercise:

* a Woozy HTTPS client against an independent Python `ssl` peer, including
  chunked responses, trailers, a 204 response without a body, and two requests
  on one connection;
* a Woozy HTTPS server against an independent Python `ssl` peer, including a
  fragmented chunked request with a trailer and two requests on one connection;
* Woozy-to-Woozy HTTPS with the same persistent connection;
* binary bodies containing NUL and non-UTF-8 bytes, checked byte-for-byte;
* clean close versus TCP truncation and certificate failures at the TLS layer.

The HTTP limits and errors are the existing `std/http` ones. Use
`http.default_limits()` (or a caller-supplied `http.Limits`) exactly as before;
`std/https` does not silently relax them.

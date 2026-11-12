# CON02 channel timed/cancel waits

Owned paths: `stdlib/cancel/`, `stdlib/channel/`, `tests/native/channel-waits/`.
The precise selection order and wakeup protocol are fixed in
`stdlib/channel/CONTRACT.md` before the implementation.

## API

```wz
pub enum SendWaitResult { Sent; Closed; Timeout; Cancelled; }
pub enum ReceiveWaitResult[T] { Item(value T); Closed; Timeout; Cancelled; }
pub func send_timeout[T](pipe Channel[T], value T, timeout_ns i64) SendWaitResult;
pub func receive_timeout[T](pipe Channel[T], timeout_ns i64) ReceiveWaitResult[T];
pub func send_cancel[T](pipe Channel[T], value T, token cancel.Token) SendWaitResult;
pub func receive_cancel[T](pipe Channel[T], token cancel.Token) ReceiveWaitResult[T];
```

- `timeout_ns`: `-1` infinite, `0` one immediate check, positive nanoseconds,
  below `-1` Panics through the same misuse path as `sync.wait_for`.
- `send_timeout`: `Closed` before space/timeout; otherwise `Sent`, then
  `Timeout`.
- `receive_timeout`: queued data first; then `Closed`; otherwise `Timeout`.
- `send_cancel`: `Cancelled` before `Closed` and before free space; otherwise
  `Sent`/wait.
- `receive_cancel`: queued data wins; with no data `Cancelled` wins over
  `Closed`; otherwise wait.
- Successful sends/receives are never rolled back by a later cancel. Close
  still drains committed values.

## Cancellation internals

`cancel.Token` keeps `AtomicI64 cancelled + Mutex + subscriptions`. A
subscription stores a private id, the channel gate, and the condition to wake.
`subscribe` returns `-1` without registering if already cancelled.
`unsubscribe` removes the id. These two functions plus `subscription_count` are
library-internal (used by channel/tests), not the normal user surface.

`request` sets the atomic flag and copies a **new Vec** of subscription structs
under the token lock, releases that lock, then acquires each saved gate and
notifies its condition. It never acquires a channel gate while holding the
token lock. Registration checks the flag under the same token lock, and request
acquires the saved gate before notifying, so the check/register/wait gap cannot
lose a wake. The waiter unregisters before returning; a request snapshot can at
worst send one harmless extra notification using its copied ARC handles.

## Test matrix

| Case | Coverage |
| --- | --- |
| `timeout-basic` | 0 timeout, positive timeout, close-drain, Closed priority |
| `timeout-blocking` | finite/infinite sender and receiver waits, close wake |
| `cancel-basic` | already cancelled, committed data, data/cancel/Closed order |
| `cancel-wake` | 8 senders, shared-token senders, 8 receivers, close wake, zero leftover subscriptions |
| `deadline-spurious` | repeated real cancel notifications wake a timed sender but do not restart its original deadline |
| `races` | data/send vs request, cancel vs close, timeout vs close, registration/request interleaving, repeated cancel/close |
| `owned-timeout-cancel` | 300 owned payloads through bounded send_timeout/receive_cancel, exact count/checksum |
| `invalid-*-timeout` | `<-1` Panic |

## Run

A CLI built from a source tree whose `stdlib` contains this worktree's
`channel`/`cancel` is required. The stable verification used a temp source copy
of the Root tree (with its `wait_for` registry) and this worktree's stdlib
overlay:

```sh
python3 tests/native/channel-waits/run.py \
  --cli /tmp/con02-build/woozyc --root . --out /tmp/con02/stable-waits \
  --ld-library-path /home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib
```

The driver runs O0/O2, checks static ELF (no `INTERP`/`NEEDED`), checks panic
prefixes, and keeps per-command stdout/stderr plus `summary.json`.

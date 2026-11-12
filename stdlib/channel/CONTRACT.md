# `std/channel` timed/cancel contract (channel-waits/1)

This document fixes the priority and wakeup rules before the implementation.
The existing `send`, `receive`, `try_send`, `try_receive`, `close`, bounded and
unbounded behavior remains exactly the `channel/1` contract.

## Public additions

```wz
pub enum SendWaitResult { Sent; Closed; Timeout; Cancelled; }
pub enum ReceiveWaitResult[T] { Item(value T); Closed; Timeout; Cancelled; }

pub func send_timeout[T](pipe Channel[T], value T, timeout_ns i64) SendWaitResult;
pub func receive_timeout[T](pipe Channel[T], timeout_ns i64) ReceiveWaitResult[T];
pub func send_cancel[T](pipe Channel[T], value T, token cancel.Token) SendWaitResult;
pub func receive_cancel[T](pipe Channel[T], token cancel.Token) ReceiveWaitResult[T];
```

`timeout_ns == -1` is infinite, `0` is one immediate check, positive values are
nanoseconds, and values below `-1` Panic as synchronization misuse (the same
rule as `sync.wait_for`). Clock failure is a runtime Panic, not a typed result.

## Selection order

All checks below happen while holding the channel gate, so a decision and its
state change are one critical section.

### `send_timeout`

1. Closed -> `Closed` (no new value is committed).
2. Space available -> enqueue, return `Sent`.
3. For `timeout_ns == 0` -> `Timeout` without waiting.
4. Otherwise wait on the writable condition until state changes or the absolute
   monotonic deadline expires; recheck from step 1. Deadline -> `Timeout`.

### `receive_timeout`

1. Queued value -> dequeue, return `Item`. Close does not discard queued data.
2. Closed and empty -> `Closed`.
3. For `timeout_ns == 0` -> `Timeout` without waiting.
4. Otherwise wait on the readable condition until state changes or the absolute
   monotonic deadline expires; recheck from step 1. Deadline -> `Timeout`.

### `send_cancel`

1. Token already cancelled -> `Cancelled`.
2. Closed -> `Closed`.
3. Space available -> enqueue, return `Sent`.
4. Otherwise register a cancellation subscription, wait on the writable
   condition, unregister, and recheck from step 1.

Therefore cancellation wins over close and over newly available space for a
sender. A value already committed by an earlier `Sent` is never retracted.

### `receive_cancel`

1. Queued value -> dequeue, return `Item`.
2. No data and token cancelled -> `Cancelled`.
3. No data, not cancelled, closed -> `Closed`.
4. Otherwise register a cancellation subscription, wait on the readable
   condition, unregister, and recheck from step 1.

Queued data intentionally wins over cancellation for a receiver. If data and a
cancel request race, the receive may return the already-committed `Item`; it
never loses or duplicates that value. An already-cancelled registration never
sleeps; if there is no queued data it returns `Cancelled` immediately.

## Cancellation registration and wakeup

`std/cancel.Token` keeps an atomic cancelled flag plus a mutex-protected list of
subscriptions. A subscription has a private id, the channel gate (`Mutex`) and
the condition to wake (`Condvar`). The public `subscribe`/`unsubscribe`
functions are library-internal and are called only by `std/channel`.

- The waiter holds the channel gate while it registers or unregisters. It may
  acquire the token lock in that state.
- `request` takes the token lock, sets the atomic flag, makes a **real copied**
  `Vec` snapshot of the subscription structs, releases the token lock, and only
  then acquires each saved gate and notifies its saved condition.
- The token lock is never held while acquiring a channel gate. This breaks the
  gate/token lock cycle and lets a real cancel wake an infinite wait; there is
  no sleep polling and no short-timeout polling.
- Registration rechecks the flag under the token lock. `request` sets the flag
  and snapshots under the same lock. Therefore either registration sees
  `cancelled` and returns immediately, or the request snapshot contains it.
- `request` acquires the saved gate before notifying, so a waiter that
  registered but has not yet entered `sync.wait` cannot lose the wakeup.
- The waiter unregisters before returning. A request snapshot may still notify
  the saved gate/condition after unregister; the copied ARC handles keep those
  objects alive and the extra notification is harmless.
- Subscriptions hold gate/condition handles, not the token or channel state.
  Neither the token nor the channel points back to the other, so no strong
  ownership cycle is formed.

## Deadline rule

Each finite call computes one absolute `CLOCK_MONOTONIC` deadline. Every wait
uses the remaining duration from that fixed deadline. EINTR, spurious
notifications, close notifications, and data/space notifications never restart
the original timeout. Huge positive values saturate instead of wrapping.

## Preservation rules

- Close remains idempotent and wakes both readable and writable waiters.
- Receivers drain every committed value before `Closed`.
- A successful send commits exactly one value; a successful receive removes
  exactly one value. Cancellation or timeout never rolls either operation back.
- Owned `T` values keep the normal channel/ARC copy and drop rules.
- Existing `channel/1` APIs and their priority (for example, `try_send` checks
  closed before full) do not change.

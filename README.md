# Woozy

[简体中文](README.cn.md)

<img src="assets/woozy-eye-sparks.gif" alt="Woozy mascot" width="256">

Woozy is a statically typed programming language with local type inference, structs, enums, generics, and trait constraints. Source files use the `.wz` extension.

```wz
package main;
import "std/io" as io;
import "std/os" as os;

func main() throws os.Error {
    io.println("Hello, world!");
}
```

## Learn Woozy

Most of the guides below are in Chinese.

- Work through the [language tutorial](docs/TUTORIAL.md) to learn types, functions, collections, pattern matching, error handling, modules, and generics.
- Read the [quick start](docs/QUICKSTART.md) if you already know another programming language and want to see how Woozy differs.
- Consult the [language rules](docs/SEMANTICS.md) for names, evaluation order, numbers, copying, ownership, errors, and cleanup.
- Look up keywords, literals, expressions, and declarations in the [lexical and syntax reference](baseline/LEXICAL_SYNTAX.md).
- Find APIs, examples, errors, and resource cleanup rules in the [standard library guide](docs/STDLIB.md).
- Use the [system interfaces](docs/OS.md) guide for standard output, files, directories, and process exit.
- Read [threads and synchronization](docs/CONCURRENCY.md) for thread arguments, joins, shared data, locks, condition variables, and atomic integers.
- Read [processes and signals](docs/PROCESSES.md) for process creation, redirection, waiting, arguments, environment variables, signals, and fork rules.
- See [shared memory and IPC](docs/IPC.md) for shared mappings, process-shared mutexes, condition variables, and semaphores.
- See [calling C libraries](docs/FFI.md) for typed C bindings, pointers, buffers, resources, and callbacks.
- Browse the [examples](examples/) for functions, numbers, errors, and argument passing, or try the [TCP chat server and client](examples/chat/README.md) and the [async HTTP server](examples/http_server/README.md).

## Editor setup

Neovim and Emacs can highlight `.wz` files and start the Woozy language server automatically. With the self-hosted compiler, the server adds completion, hover, definition lookup, and live project diagnostics. Run this from the checkout with `woozyc` on your `PATH`.

```sh
python3 development/editor/setup.py
```

See the [editor guide](development/docs/EDITOR.md) for prerequisites, single-editor setup, and compiler selection.

## Language basics

Types follow names, as in `count i32`. Local bindings can infer their types. Function parameters require an explicit type. Simple statements end with a semicolon.

A `var` binding can be reassigned and its value fields can be modified. A `let` binding permits neither. Structs and enums copy field by field. Assigning a Vec shares the container. Slicing creates a new Vec, and reference-valued elements can still be shared.

A `string` is an immutable UTF-8 value. `text.len`, indexing, and slicing count Unicode code points, and indexing returns a `u32`. The `+` operator creates a new string. Equality and ordering compare bytes without Unicode normalization. String `+=` applies only to a whole `var` binding. Use `std/text` to convert explicitly between strings and `Vec[u8]`.

Ordinary parameters leave arguments available to the caller, and parameter names are read-only. Write `move` at the call site to transfer ownership, or `inout` to let the callee modify the caller's variable.

Function values can capture local bindings when they are created. Captured bindings are read-only. Shared objects such as Vec and Box can still be modified through their APIs. A plain `func` copies the captured values. A `move func(...) { ... }` transfers the owned values it captures. Saving `value.method` saves a logical copy of the receiver. Rebinding the original variable or replacing its value fields leaves that copy intact, while reference fields such as Vec still share their contents.

Declare propagating errors with `throws E`. A caller can catch the error or declare that it propagates the same error type. A `defer` runs when its lexical block exits, with the most recently registered action running first. Panics such as integer division by zero and out-of-bounds access are outside typed error handling.

Declare generics with `[T]` or `[T Trait]` and implement traits explicitly. The dynamic trait types `dyn Trait` and `dyn Trait[Args]` support dynamic dispatch for object-safe methods, storage in `Vec[dyn Trait]`, and copying, moving, and destruction. Trait applications with different type arguments have distinct identities, such as `Convert[i32]` and `Convert[bool]`. Template specialization and associated types are unavailable.

## Current support

Woozy supports integers and floating-point numbers, strings, function values and capturing closures, structs and enums, Vec and Slice, constrained generics, traits and `dyn Trait`, modules, typed `throws`, and block-scoped `defer`. It can generate native code or Python code. Both backends follow the same language rules.

The [standard library](docs/STDLIB.md) covers I/O, filesystems, text and collections, JSON, math, time, random numbers, networking, processes, and concurrency. The system interfaces target native Linux x86_64 programs. Their names and argument order follow C conventions. Calls throw declared errors on failure, and incorrect lock operations panic. Woozy has no raw syscall API.

`std/http` supports HTTP/1.1. `std/http_server` provides an async HTTP/1.1 server with keep-alive, pipelining, nonblocking backpressure, request/body/idle/write timeouts, `Expect: 100-continue`, and graceful stop/drain/abort. `std/unicode` provides Unicode 18 normalization, case folding, and grapheme clusters. `std/timezone` supports named time zones, daylight saving time, and local-time conversion.

Use `std/thread` to start threads with capture-free functions and explicit arguments and to join them. `std/sync` provides mutexes, condition variables, and atomic integers. Shared mutable data requires synchronization by the program. Atomic ARC protects reference counts only.

Both backends collect pure Woozy cycles of strong references involving Vec, Box, and closure environments. The native backend also supports async/await, timer and file descriptor waits, and collection of pure Woozy reference cycles involving tasks that are unstarted or complete. Creating a Task copies and retains its ordinary owned arguments. The executor runs on one thread, and async is currently available only on the native backend. See [threads and async](docs/CONCURRENCY.md). The host manages cycles within foreign Python or C objects.

`std/rwlock` provides read-write locks, and `std/cancel` provides cooperative cancellation. `std/channel` supports bounded and unbounded queues, timeouts, and cancellable waits. `std/task` provides fixed-size thread pools and futures. Shut down the pool explicitly with `shutdown`.

`std/box` provides the shared mutable cell `Box[T]`. `Weak[T]` is a built-in weak reference type, with `empty`, `downgrade`, and `upgrade` provided by `std/weak`. Targets include Vec, function values, `Box[T]`, and reference-counted shared handles. A weak reference leaves the target's lifetime unchanged. Upgrading returns an `Option[T]`.

Native programs can declare and call typed C functions and pass C records with natural layout, pointers, and buffers. C can call a capture-free, non-throwing function immediately or retain it for later use. Indexing pointers and buffers requires an `unsafe` block. C error codes remain explicit. Libraries that require libc need the hosted runtime. Ordinary native programs run independently.

`std/process` supports spawn, exec, wait, arguments, and environment variables. `std/signal` supports synchronous signal waits, and `std/ipc` provides shared memory and synchronization across processes. After a multithreaded fork, only the calling thread remains in the child. The program is responsible for its own locks. The child should usually exec promptly.

Default arguments are not yet supported.

Compiler build instructions, implementation notes, tests, and contributor guides are in [development/](development/README.md).

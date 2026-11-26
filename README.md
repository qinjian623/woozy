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

## Install

Woozy release packages target Linux x86_64 and Apple Silicon. Both include `woozyc`, the standard library, runtime files, and pinned LLVM tools. On a Mac, native programs target macOS 14.4 or later and compiling them requires the Xcode Command Line Tools or another installed macOS SDK.

This command checks for a new release and asks before installing or upgrading. It requires curl and Python 3.12 or later. [GitHub Releases](https://github.com/qinjian623/woozy/releases)

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 -
```

Check for a new version without installing it.

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 - --check
```

Install or upgrade without a prompt.

```sh
curl -fsSL https://raw.githubusercontent.com/qinjian623/woozy/main/install.py | python3 - --yes
```

The installer prefers stable releases when available; an installed pre-release can upgrade to a newer pre-release or a stable release. It verifies the archive before switching `~/.local/bin/woozyc` and keeps previous versions. Add `~/.local/bin` to your `PATH` if needed, or pass `--prefix DIR` to install into another directory.

If `~/.local/bin/woozyc` points to a source build or a package without release metadata, specify a separate prefix to keep that installation.

### Manual installation

The commands below install `v0.1.0-rc.2` from the [releases page](https://github.com/qinjian623/woozy/releases).

```sh
archive=woozy-linux-x86_64.tar.gz   # On Apple Silicon, use woozy-macos-arm64.tar.gz.
base=https://github.com/qinjian623/woozy/releases/download/v0.1.0-rc.2
curl -fLO "$base/$archive"
curl -fLO "$base/$archive.sha256"
sha256sum -c "$archive.sha256"     # On macOS, use shasum -a 256 -c "$archive.sha256".
tar -xzf "$archive"
mkdir -p "$HOME/.local/bin"
ln -s "$PWD/woozy/woozyc" "$HOME/.local/bin/woozyc"
export PATH="$HOME/.local/bin:$PATH"   # Add this line to your shell profile.
woozyc --version
```

Build and run the Hello World example from a repository checkout. The release archive does not include the examples.

```sh
woozyc build --project examples/hello/lang.toml --backend native --out-dir build/hello
./build/hello/program
```

This prints `Hello, world!`. Running generated Python code requires Python 3.12. The `--package-objects` mode uses Ninja. Woozy schedules `--source-make` package builds itself. TLS requires a separately built addon. See the [build guide](development/docs/BUILD.md#安装自举版工具链) and the [standard library guide](docs/STDLIB.md).

## Editor setup

On Linux and macOS, Emacs, Vim, and Neovim can highlight `.wz` files and start the Woozy language server automatically. The self-hosted compiler supplies completion, hover, definition lookup, and live project diagnostics. With `woozyc` on your `PATH`, run this from the checkout to configure Vim and Emacs.

```sh
python3 development/editor/setup.py --editor vim-emacs
```

Omit `--editor` to configure Neovim and Emacs. See the [editor guide](development/docs/EDITOR.md) for prerequisites, single-editor setup, and compiler selection.

## Language basics

Types follow names, as in `count i32`. Local bindings can infer their types, while function parameters require explicit types. Simple statements end with a semicolon.

A `var` binding can be reassigned and have its value fields modified; a `let` binding allows neither. Structs and enums copy field by field. Assigning a Vec shares the container, while slicing creates a new Vec whose reference-valued elements can still be shared.

A `string` is an immutable UTF-8 value. Indexing, slicing, and `text.len` (from `std/text`) count Unicode code points, and indexing returns a `u32`. The `+` operator creates a new string, while equality and ordering compare bytes without Unicode normalization. String `+=` applies only to a whole `var` binding. Use `std/text` to convert explicitly between strings and `Vec[u8]`.

Ordinary parameters leave arguments available to the caller, and parameter bindings are read-only. Write `move` at the call site to transfer ownership, or `inout` to let the callee modify the caller's variable.

Function values can capture local bindings when created. Captured bindings are read-only, though shared objects such as Vec and Box can still be modified through their APIs. A plain `func` copies captured values, whereas `move func(...) { ... }` moves the owned values it captures. Storing `value.method` keeps a logical copy of the receiver: rebinding the original variable or replacing its value fields leaves that copy intact, while reference fields such as Vec still share their contents.

Declare propagating errors with `throws E`; callers either catch the error or declare the same error type. A `defer` runs when its lexical block exits, with the most recently registered action running first. Panics such as integer division by zero and out-of-bounds access are outside typed error handling.

Declare generics with `[T]` or `[T Trait]` and implement traits explicitly. The dynamic trait types `dyn Trait` and `dyn Trait[Args]` support dynamic dispatch for object-safe methods, storage in `Vec[dyn Trait]`, and copying, moving, and destruction. Trait applications with different type arguments have distinct identities, such as `Convert[i32]` and `Convert[bool]`. Template specialization and associated types are not supported.

## Current support

Woozy supports integers and floating-point numbers, strings, function values and capturing closures, structs and enums, Vec and Slice, constrained generics, traits and `dyn Trait`, modules, typed `throws`, and block-scoped `defer`. It can generate native or Python code, and both backends follow the same language rules.

The [standard library](docs/STDLIB.md) covers I/O, filesystems, text and collections, JSON, math, time, random numbers, networking, processes, and concurrency. The system interfaces target native programs on Linux x86_64 and macOS arm64 using C-style names and argument order; calls throw declared errors on failure, incorrect lock operations panic, and there is no raw syscall API.

`std/http` supports HTTP/1.1. `std/http_server` provides an async HTTP/1.1 server with keep-alive, pipelining, nonblocking backpressure, request/body/idle/write timeouts, `Expect: 100-continue`, and graceful stop/drain/abort. `std/unicode` provides Unicode 18 normalization, case folding, and grapheme clusters. `std/timezone` supports named time zones, daylight saving time, and local-time conversion.

Use `std/thread` to spawn threads with capture-free functions and explicit arguments and to join them; `std/sync` provides mutexes, condition variables, and atomic integers. Programs must synchronize shared mutable data themselves—atomic ARC protects reference counts only.

Both backends collect pure Woozy cycles of strong references involving Vec, Box, and closure environments. The native backend also supports async/await, timer and file descriptor waits, and collection of pure Woozy reference cycles involving unstarted or completed tasks. Creating a Task copies and retains its ordinary owned arguments. The executor is single-threaded, and async is currently available only on the native backend; see [threads and async](docs/CONCURRENCY.md). The host manages cycles within foreign Python or C objects.

`std/rwlock` provides read-write locks, and `std/cancel` provides cooperative cancellation. `std/channel` supports bounded and unbounded queues, timeouts, and cancellable waits. `std/task` provides fixed-size thread pools and futures; shut pools down explicitly with `shutdown`.

`std/box` provides the shared mutable cell `Box[T]`. `Weak[T]` is a built-in weak reference type, with `empty`, `downgrade`, and `upgrade` in `std/weak`. Valid targets include Vec, function values, `Box[T]`, and reference-counted shared handles. A weak reference does not extend its target's lifetime, and upgrading returns an `Option[T]`.

Native programs can declare and call typed C functions and pass C records with natural layout, pointers, and buffers. C can call a capture-free, non-throwing function immediately or retain it for later use. Indexing pointers and buffers requires an `unsafe` block, and C error codes remain explicit. Libraries that require libc need the hosted runtime, while ordinary native programs run independently.

`std/process` supports spawn, exec, wait, arguments, and environment variables. `std/signal` supports synchronous signal waits, and `std/ipc` provides shared memory and cross-process synchronization. After a multithreaded fork, only the calling thread survives in the child; the program is responsible for its own locks, and the child should usually exec promptly.

Default arguments are not yet supported.

Compiler build instructions, implementation notes, tests, and contributor guides are in [development/](development/README.md).

## License

Woozy's original code and documentation are licensed under [Apache License 2.0](LICENSE). Third-party material retains its own licenses. The Woozy name and mascot are outside this license grant.

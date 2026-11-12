# Hosted runtime startup and threads

This directory is the hosted replacement for `runtime/native/start.cpp` and
`runtime/native/thread.cpp`, plus the hosted fork path for
`runtime/native/process.cpp`, when the programmer explicitly asks for a hosted
link that may use the C library, pthreads and selected external libraries
(OpenSSL in the validation fixture).

It is **not** a change to the freestanding runtime.  The native `start.cpp`,
`thread.cpp`, `freestanding.cpp`, `CMakeLists.txt`, compiler, stdlib and shared
headers are untouched.  The only existing file changed is
`runtime/native/process.cpp`, and only inside a compile-time
`LANG_RUNTIME_HOSTED` branch; the freestanding branch stays byte-for-byte
equivalent to HEAD (the test script compares the non-hosted process object).
The hosted archive reuses the existing allocator, Vec, text, process, sync,
system/network and math runtime objects.

## What is replaced and why

The native runtime installs its own FS base, owns a raw clone stack/TCB and
unmaps that stack before exiting the raw thread.  Those choices are correct for
a freestanding program but are not compatible with libc/pthreads:

* libc expects its own thread pointer and TCB for `errno`, TLS, stack canaries,
  malloc and external libraries.
* A pthread's stack and TLS must be returned through libc, not by the thread
  unmapping its own stack.

The hosted implementation therefore keeps the language-visible ABI but uses:

* the normal CRT `_start`/`__libc_start_main` and the generated `main`;
* a `.init_array` constructor (`lang_hosted_startup_v1`) to publish
  `argc`/`argv`/`envp` to the existing `lang_process_init_v1`;
* `pthread_create`, `pthread_join`, `pthread_mutex_t` and
  `pthread_cond_t` for runtime threads;
* libc-managed TLS and stack-protector state.  It never writes FS, calls
  `arch_prctl`, copies a glibc TCB or defines `__stack_chk_guard`.

`LangThreadEntry` and the drop boundary are no-throws ABI.  The hosted runtime
objects are compiled with `-fno-exceptions`; they do not catch C++ exceptions,
do not use RTTI and do not require the C++ exception runtime.  A foreign C++
throw through these ABI callbacks is outside the contract and terminates at the
boundary instead of being converted into a fake language status.

## Exported ABI

Thread and handle symbols match `runtime/native/thread.hpp` and
`runtime/native/handle.hpp`:

```text
lang_thread_spawn_v1
lang_thread_error_v1
lang_thread_wait_v1
lang_thread_result_v1
lang_thread_fork_child_v1
lang_thread_unmap_exit_v1   hosted stub; always Panics (native-only epilogue)
lang_handle_retain_v1
lang_handle_release_v1
lang_handle_panic_v1
```

Startup helpers:

```text
lang_hosted_startup_v1      constructor; force-link this symbol
lang_hosted_init_v1         explicit argc/argv/envp helper
lang_init_tcb               native-compatible name; republishes process vectors
                            from an initial stack pointer, with no FS/TCB work
```

`lang_process_init_v1` itself remains the existing implementation in
`runtime/native/process.cpp`.

## Thread semantics

* `spawn` byte-copies the context, then calls the copy hook once.
* The entry sees the copied context and has the existing
  `func(A) -> i64` shape.
* The drop hook runs before completion is published, including on pthread
  creation failure.
* `error` reports only spawn/copy failures, matching the native ABI's
  creation-time contract.
* `wait` returns `0`, the creation error, `-3` (ESRCH) for a handle inherited
  across `fork`, or `-35` (EDEADLK) for a self-join.  It can be called
  repeatedly and concurrently; a condvar publishes completion.
* The first successful `pthread_join` happens when the last handle reference is
  released, so dropping the last handle without calling `wait` still lets the
  entry finish and releases the pthread state.  If that final release happens
  on the runner itself, it self-detaches and frees the state.
* The entry and context-drop callbacks are no-throws ABI boundaries.  The
  runtime does not translate an out-of-contract C++ throw; source-language
  `throws` still uses its normal result ABI.
* `lang_thread_unmap_exit_v1` is intentionally unsupported in hosted mode and
  Panics instead of corrupting the pthread stack.

## Hosted fork and exec

`runtime/hosted/process.cpp` calls libc `fork(3)`, never
`syscall(SYS_fork/SYS_clone)`.  This lets glibc run its internal and user
`pthread_atfork` handlers, so the surviving child has correct malloc, errno/TLS
and pthread clock state.  Both `lang_process_fork_v1` and
`lang_process_spawn_v1` go through the same `fork_process()` choice; `execve`
still uses the existing runtime path after that fork.

The hosted process object registers its atfork hooks at constructor priority
101, before user `main` can register handlers.  POSIX ordering then gives:

* user prepare handlers first, then runtime prepare acquires launch (unless
  this thread already has a language LaunchGuard), environment and allocator,
  in that order;
* in the child, runtime child resets the allocator ticket state and clears the
  environment/launch locks, then user child handlers run;
* in the parent, allocator and environment are released, followed by launch
  only when this prepare hook acquired it.

`spawn` keeps its LaunchGuard from temporary-FD creation through the exec-error
pipe handshake and descriptor cleanup. Direct libc `fork()` waits for that
same lock, so its child cannot inherit another spawn's temporary pipe writer.
Lock ownership is recorded in libc TLS, only in the `LANG_RUNTIME_HOSTED`
branch. The child clears both the locks and its ownership records, so an outer
language Guard cannot later unlock a newly acquired child lock. It does not
reset locks again after user child handlers.

Fork failure is handled by a per-thread idempotent parent release: glibc's
parent handler and the explicit failure fallback cannot double-release an
allocator lock or release another thread's prepare locks. A thread-runtime
atfork child hook also updates
the current hosted thread's process id for direct libc `fork()` calls.

Limits and POSIX rules:

* Only the calling thread exists in the child.  Other runtime handles are
  invalidated by process id and `wait` returns `-3` before touching their
  mutexes.
* Owned runtime locks (environment, allocator, launch) are recovered; user
  business locks and arbitrary libraries' locks remain the user's
  responsibility.
* Do not call runtime spawn/fork or arbitrary non-async-signal-safe code from
  user atfork prepare callbacks.  The test intentionally exercises environment
  and allocator recovery from a user child callback to verify ordering, but
  that is a host-state regression fixture, not general advice.
* Forking while another thread is blocked inside a runtime wait on the current
  thread is outside the supported POSIX single-survivor model.

## Build recipe

Public native sources compiled with the existing freestanding flags:

```text
runtime/native/alloc.cpp
runtime/native/panic.cpp
runtime/native/os.cpp
runtime/native/os_buffers.cpp
runtime/native/sync.cpp
runtime/native/text.cpp
runtime/native/system.cpp
runtime/native/system_buffers.cpp
runtime/native/ipc.cpp
runtime/native/dns.cpp
runtime/native/network_buffers.cpp
runtime/native/network.cpp
runtime/native/numbers.cpp
runtime/native/process.cpp (with -DLANG_RUNTIME_HOSTED)
runtime/native/math/support.cpp
runtime/native/math/vendor/*.c (with runtime/native/math/sources.cmake flags)
```

Excluded from the hosted archive:

```text
runtime/native/start.cpp
runtime/native/thread.cpp
runtime/native/freestanding.cpp
```

Replacement/extra sources:

```text
runtime/hosted/start.cpp
runtime/hosted/thread.cpp
runtime/hosted/process.cpp
```

`runtime/native/process.cpp` remains the public source but must be compiled with
`-DLANG_RUNTIME_HOSTED` in the hosted archive.  The native/freestanding archive
must compile it without that definition.

A ready-to-consume list of sources and flags is provided in
`runtime/hosted/sources.cmake`.  Recommended flags:

* public/math sources: the existing native runtime flags
  (`-std=c++20 -ffreestanding -fno-builtin -fno-exceptions -fno-rtti
  -fno-sanitize=all -fno-builtin-memcpy -fno-builtin-memset
  -fno-builtin-memmove`), math C flags from `runtime/native/math/sources.cmake`.
* hosted `start.cpp`/`thread.cpp`/`process.cpp`: `-std=c++20 -O2 -fPIC
  -pthread -fstack-protector-strong -fno-rtti -fno-exceptions` (do **not** use
  `-ffreestanding` for these files).
* `runtime/native/process.cpp`: add `-DLANG_RUNTIME_HOSTED` only for the
  hosted archive.
* archive with `llvm-ar rcD` or equivalent deterministic archiver.

The current compiler does **not** have a hosted link mode.  A future Root
integration must link generated `program.o` with a normal CRT/dynamic-C link
(for example through the Clang driver, not the current `-nostdlib -static`
path), include the hosted archive, and force the constructor member into the
link, for example:

```text
clang++ program.o \
  -Wl,-u,lang_hosted_startup_v1 \
  -Wl,--start-group libwoozy_hosted_runtime.a -Wl,--end-group \
  -pthread -lssl -lcrypto -o program
```

Without `-Wl,-u,lang_hosted_startup_v1` (or an equivalent
`--whole-archive` section), the constructor member is not extracted from a
static archive and `argc`/`argv`/`envp` never reach the language runtime. The
startup object strongly references `lang_process_init_v1`, which in turn pulls
the hosted fork object and its atfork registration into the link even when the
program only uses allocator/Vec APIs. Weak addresses of `__libc_stack_end` and
`environ` are checked before reading either variable's value. The
hosted archive itself has no C++ exception symbols; the no-argument CRT fixture
is linked with `-nostdlib++ -static-libgcc` and depends only on libc/libm/loader,
demonstrating that the runtime needs no C++ exception library.  This is the missing Root
compiler link mode, not a runtime workaround.

The Native FFI language side is also not part of this batch.  No source-language
declaration currently imports libc/OpenSSL symbols; the tests below are C++
runtime harnesses linked directly against the hosted archive.

## Validation

`tests/native/hosted/run_hosted_tests.sh` builds the archive and runs:

* `hosted_crt_main_test.cpp`: a generated-program-shaped no-argument `main`
  that does not reference a startup helper, proving the forced constructor
  extraction publishes argc/argv/envp.
* `hosted_alloc_crt_test.cpp`: strongly references only allocator APIs. Weak
  hook-address checks prove that forced startup extracts process/fork without
  a process API call, and a real fork child allocates/frees successfully.
  Its object is checked for weak (not strong) hook references. A second link
  renames both startup-vector data references to absent weak symbols, checking
  the fallback without changing the real glibc/CRT symbols.
* `hosted_start_test.cpp`: normal CRT main, constructor-published
  argc/argv/envp, explicit `lang_init_tcb`/`lang_hosted_init_v1`, language
  getenv, and proof that FS/canary are unchanged.
* `hosted_thread_test.cpp`: main thread and multiple hosted runtime threads
  calling errno/TLS, malloc/free, snprintf, getenv and OpenSSL SHA256;
  handwritten TLS isolation; existing Vec/allocator and raw `lang_os_*` file
  operations; copied-handle concurrent joins; self-join EDEADLK; last-handle
  cleanup without join; handle ARC and Panic paths.
* `hosted_process_test.cpp`: user `pthread_atfork` callbacks, libc fork while a
  background runtime thread is active, child libc malloc/errno and
  `pthread_getcpuclockid`/`clock_gettime`, language environment/allocator/
  thread use in the child, nested language fork, `execve`, `spawn`, and a
  seccomp-forced fork-failure path that verifies lock release. Link-time fork
  wrappers pause the real spawn before fork to prove a concurrent libc fork
  cannot retain its error writer. Another controlled overlap holds direct
  prepare locks across a failed language fork's fallback; an environment/
  allocation observer must remain blocked until the owning hook releases them.
  Finally, 32 direct forks run concurrently with 32 seccomp-denied language
  forks. Wrappers control timing; libc and kernel fork calls remain real.

It also checks that the hosted archive has no `__cxa_*`, `__gxx_personality`,
`_ZSt9terminatev` or `_Unwind_*` symbols, runs the existing
`tests/native/thread_runtime_test.cpp` against the untouched freestanding
archive, records `readelf` dynamic dependencies, and rebuilds/compares the
freestanding archive and a freestanding executable before and after the hosted
tests.  It additionally compiles the current `process.cpp` without
`LANG_RUNTIME_HOSTED` and compares that object with the HEAD version.

Run:

```sh
timeout 900 tests/native/hosted/run_hosted_tests.sh
```

The artifacts and `summary.txt` are written under
`tests/native/hosted/.build/` (ignored by git).

# E operation-213 and old Vec-ABI probes

`run.py` assembles a fresh project from the production `selfhost/src/wzcc`
tree, overlays the E files under test, adds `e_stat_bytes.wz` and
`e_fs_abi.wz`, and builds the probe with stage0 at O0 and O2. Both probes drive
the production `emit_instruction` -> `system_call` path (not the private
`e_extra_system_call` helper).

Every subprocess goes through `selfhost/corpus.py::_run_process_impl`: a new
session, bounded pipe draining, `killpg` on timeout, and a 0.5 second cleanup
window. A timeout, signal, incomplete cleanup, or abandoned pipe makes the
command and the final result non-zero while keeping its stdout/stderr, argv,
label, and actual status. `--timeout` bounds each stage0 build (use 900 for the
full joint) and `--program-timeout` (default 30) bounds the generated program
and probes. `selftest_cleanup.py` extracts the real `run()` with `ast` and
exercises normal, timeout, signal, incomplete-cleanup, and end-to-end failure
results with the same helper; it does not mock the implementation.

- `stat_bytes.ll`: operation 213 expands a `Vec[u8]` descriptor into
  `(data, length)` and calls `lang_sys_stat_v1`. `driver.c` executes it against
  empty/NUL/overlong/missing/ENOTDIR paths and a real one-byte file named with
  byte `0xff`; it checks the 120-byte `FileInfo` payload (regular mode, link
  count, size).
- `fs_abi.ll`: operations 50/51/52/75/76 keep the descriptor-pointer ABI:
  `getcwd(ptr vec, i64)`, `getdents(i32, ptr vec, i64)`,
  `readlink(ptr path, i64, ptr vec, i64)`, `nanosleep(ptr req, ptr vec)`,
  `getrandom(ptr vec, i64, i32)`. `fs_abi_driver.c` executes real
  success/failure cases with descriptor buffers, a directory fd, a symlink,
  a zero/invalid timespec and valid/invalid getrandom flags.

Both modules are verified with `opt -passes=verify`, compiled with `llc`, and
linked with the real native runtime. Generated IR is never edited.

The C driver uses a 120-byte record to stand in for the real
`std/os.FileInfo` layout. Op-213 routing and argument expansion are
independent of the nominal field layout, which the C++ project and stat_bytes
fixtures check directly. The typed `stat_bytes` differential and C++ system
tests run separately. This run covers only the E component. The full Wz public check/build
facade is not implemented, so whole-compiler stage1/stage2 execution is
`not_run`. Processes that call `setsid()` and escape the session remain the
original corpus limitation; the runner reports incomplete cleanup and does not
claim to kill them.

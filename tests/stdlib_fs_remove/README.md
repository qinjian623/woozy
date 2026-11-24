# stdlib.fs_remove fixture

All checks use the real Linux filesystem. The main fixture creates a private
scratch root in the runner's temporary working directory and covers:

- mixed files, FIFOs, nested directories, and child symlinks to files and
  directories (symlinks are removed, their targets are not followed);
- a regular-file target (`ENOTDIR`) and a symlink target (`ELOOP`);
- empty, separator-only, `.` and `..` root spellings (`EINVAL`);
- missing roots (`ENOENT`);
- names that are not valid UTF-8 at both file and directory levels;
- a 2048-level tree whose full path exceeds `PATH_MAX`;
- a 600-entry directory spanning multiple `getdents` buffers;
- a parent spelling containing a symlink followed by `..`, which differs from
  the lexically cleaned path;
- repeated success and error calls compared by `/proc/self/fd` count.

`low_limit/` runs under `RLIMIT_NOFILE=64`. It verifies that a shallow tree
still succeeds, a 256-level tree reports typed `EMFILE`, the partial root stays
present, and the descriptor count returns to its pre-call value after unwinding.

`race/` plus `race_harness.c` use `ptrace` to stop the child at deterministic
`newfstatat` boundaries and mutate the real tree there:

- `child-open`: rename the classified child and install a replacement between
  `fstatat` and `openat`; removal must report `EEXIST`, leave the replacement,
  and leave the renamed original untouched.
- `child-rmdir`: replace the child after its contents are processed but before
  the pre-rmdir name check; removal must report `EEXIST` and keep the
  replacement.
- `root-replace`: replace the root before its final name check; removal must
  report `EEXIST` and keep the replacement.
- `root-away`: rename the root away before its final check; the call succeeds
  because the original name is gone, and the renamed root remains.

The harness deliberately does not test the gap between the final name check and
`unlinkat`: Linux has no unlink-by-descriptor for directories, and the current
`std/os` interface exposes no `renameat2(RENAME_EXCHANGE)` quarantine primitive,
so that last race cannot be closed atomically here. The production source
documents this limitation instead of claiming race-proof removal.

Run the main fixture through the shared standard-library runner:

```sh
python3 -B tests/tooling/stdlib_project_test.py /path/to/woozyc \
  tests/stdlib_fs_remove/lang.toml native-o0
```

Run the low-limit and race suites (compiler must expose the descriptor-relative
`std/os` interface):

```sh
python3 -B tests/stdlib_fs_remove/run_low_limit.py /path/to/woozyc \
  tests/stdlib_fs_remove/low_limit/lang.toml native-o0 --limit 64

python3 -B tests/stdlib_fs_remove/run_race.py /path/to/woozyc \
  tests/stdlib_fs_remove/race/lang.toml native-o0
```

`run_race.py` compiles `race_harness.c` with `cc` by default; pass
`--harness /path/to/harness` to use a prebuilt copy.

Repeat with `native-o2`. The Linux x86_64 CMake tests run all three suites.

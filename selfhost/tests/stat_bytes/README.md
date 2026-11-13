# std/os.stat_bytes differential fixture

`run.py` builds the probe at O0 and O2 with the given stage0, builds a C++
oracle against `liblang_native_runtime.a`, creates a directory containing a
one-byte file whose name is the invalid UTF-8 byte `0xff`, and compares both
programs byte for byte. The cases are:

| case | spelling | expected |
| --- | --- | --- |
| empty | zero-length `Vec[u8]` | `-ENOENT` (2) |
| nul | `".\0x"` | `-EINVAL` (22) |
| long | 4096 `A` bytes | `-ENAMETOOLONG` (36) |
| invalid | real file named `0xff` | success, regular file, size 1 |
| unicode | real UTF-8 file | success, regular file, size 1 |
| missing | absent path | `-ENOENT` (2) |
| notdir | `notdir/child` where `notdir` is a file | `-ENOTDIR` (20) |

Example:

```sh
python3 selfhost/tests/stat_bytes/run.py \
  --woozyc build/woozy-stdlib-native/woozyc \
  --runtime-dir build/woozy-stdlib-native/native \
  --work-dir /tmp/stat-bytes
```

`python3 selfhost/tests/stat_bytes/test_runner.py` checks raw filename arguments,
timeout output retention, and rejection of incomplete process cleanup. Commands
use the corpus runner's process-group timeout; a child that outlives its parent
while holding a pipe causes failure rather than a passing result.

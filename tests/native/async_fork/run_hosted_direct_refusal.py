#!/usr/bin/env python3
"""Direct libc fork with a foreign executor must Panic and record why."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "foreign-direct"], capture_output=True,
                        timeout=60)
stderr = result.stderr.decode("utf-8", "backslashreplace")
ok = (result.returncode == 101 and
      "LANG_ASYNC_FORK_REFUSED:executor-on-another-thread" in stderr and
      "LANG_NATIVE_PANIC:v1:runtime-handle-misuse" in stderr)
if not ok:
    print(f"unexpected refusal result: exit={result.returncode} stderr={stderr!r}")
    raise SystemExit(1)
print("passed: hosted direct libc fork refusal marker and Panic")

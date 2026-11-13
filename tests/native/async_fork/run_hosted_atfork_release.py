#!/usr/bin/env python3
"""Run one hosted atfork final-release mode with a process-group timeout.

The worker thread takes the last Task reference while a user prepare/parent
handler joins it. A deadlock must be killed as a group, and the actual stdout,
stderr, timeout flag and exit status are retained in the report.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("mode")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    started = time.time()
    process = subprocess.Popen(
        [str(args.binary), args.mode], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True)
    timeout = False
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
        exit_code = process.returncode
    except subprocess.TimeoutExpired:
        timeout = True
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        exit_code = process.returncode
    report = {
        "mode": args.mode,
        "argv": [str(args.binary), args.mode],
        "elapsed_s": round(time.time() - started, 6),
        "timeout": timeout,
        "exit": exit_code,
        "stdout": stdout,
        "stderr": stderr,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    ok = (not timeout and exit_code == 0 and
          "user-handler-returned" in stderr and
          "worker-release-returned" in stderr and
          "fork-returned" in stderr)
    print(json.dumps({"status": "passed" if ok else "failed",
                      "mode": args.mode, "exit": exit_code,
                      "timeout": timeout, "report": str(args.out)}))
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()

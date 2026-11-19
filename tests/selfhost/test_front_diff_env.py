#!/usr/bin/env python3
"""Small tests for the front_diff evidence environment allowlist."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import front_diff  # noqa: E402


class RecordedEnvTests(unittest.TestCase):
    def environment(self) -> dict[str, str]:
        return {
            "PATH": "/usr/local/bin:/usr/bin:/bin",
            "LC_ALL": "C",
            "PYTHONDONTWRITEBYTECODE": "1",
            "TMPDIR": "/tmp/evidence",
            "LD_LIBRARY_PATH": "/toolchains/llvm/lib:/toolchains/libxml2/usr/lib",
            "WOOZY_TOOLCHAIN_ROOT": "/toolchains",
            "SH04_TRAP_LOG": "/tmp/evidence/runtime-traps.log",
            "ANTHROPIC_API_KEY": "sk-test-not-a-real-credential",
            "DEEPSEEK_API_KEY": "sk-other-not-a-real-credential",
            "HOME": "/home/test",
            "UNLISTED_RUNTIME_NOTE": "must-not-be-recorded",
        }

    def expected_subset(self, env: dict[str, str]) -> dict[str, str]:
        return {key: env[key] for key in front_diff.RECORDED_ENV_KEYS}

    def test_allowlist_only_and_input_unchanged(self) -> None:
        env = self.environment()
        before = copy.deepcopy(env)
        recorded = front_diff.recorded_env(env)

        self.assertEqual(env, before)  # the real subprocess env is not mutated
        self.assertIsNot(recorded, env)
        self.assertEqual(recorded, self.expected_subset(env))
        self.assertEqual(set(recorded), set(front_diff.RECORDED_ENV_KEYS))
        for key in ("PATH", "LC_ALL", "PYTHONDONTWRITEBYTECODE", "TMPDIR",
                    "LD_LIBRARY_PATH", "WOOZY_TOOLCHAIN_ROOT", "SH04_TRAP_LOG"):
            self.assertEqual(recorded[key], env[key])
        for key in ("ANTHROPIC_API_KEY", "DEEPSEEK_API_KEY", "HOME",
                    "UNLISTED_RUNTIME_NOTE"):
            self.assertNotIn(key, recorded)
        self.assertNotIn("sk-test-not-a-real-credential", recorded.values())
        self.assertNotIn("sk-other-not-a-real-credential", recorded.values())

    def test_case_record_omits_credentials_and_keeps_other_fields(self) -> None:
        env = self.environment()
        record = {
            "case": "selfhost/example.wz",
            "group": "selfhost",
            "input": {"path": "/main/selfhost/example.wz", "sha256": "a" * 64},
            "argv": ["/bin/oracle", "/main/selfhost/example.wz"],
            "cwd": "/work",
            "env": env,
            "timeout_seconds": 60.0,
            "exit": 0,
            "timed_out": False,
            "stdout": {"path": "/work/out", "sha256": "b" * 64, "bytes": 2},
            "stderr": {"path": "/work/err", "sha256": "c" * 64, "bytes": 0},
            "protocol": "oracle",
            "validation": "passed",
            "nested": {"kept": [1, 2, 3]},
        }
        before = copy.deepcopy(record)
        with tempfile.TemporaryDirectory(prefix="front-diff-env-test-") as tmp:
            path = pathlib.Path(tmp) / "case.json"
            front_diff.write_case_record(path, record)
            raw = path.read_text(encoding="utf-8")
            parsed = json.loads(raw)

        for secret in ("ANTHROPIC_API_KEY", "DEEPSEEK_API_KEY",
                       "sk-test-not-a-real-credential",
                       "sk-other-not-a-real-credential",
                       "UNLISTED_RUNTIME_NOTE", "must-not-be-recorded"):
            self.assertNotIn(secret, raw)
        self.assertEqual(parsed["env"], self.expected_subset(env))
        self.assertEqual({key: value for key, value in parsed.items()
                          if key != "env"},
                         {key: value for key, value in before.items()
                          if key != "env"})
        self.assertEqual(record, before)  # write_case_record does not mutate input

    def test_wz_runtime_subset_keeps_known_items(self) -> None:
        runtime_env = {
            "PATH": "/work/runtime-traps",
            "SH04_TRAP_LOG": "/work/runtime-traps-o0.log",
            "LC_ALL": "C",
            "ANTHROPIC_API_KEY": "sk-test-not-a-real-credential",
        }
        recorded = front_diff.recorded_env(runtime_env)
        self.assertEqual(recorded, {
            "PATH": "/work/runtime-traps",
            "LC_ALL": "C",
            "SH04_TRAP_LOG": "/work/runtime-traps-o0.log",
        })
        self.assertNotIn("ANTHROPIC_API_KEY", recorded)


if __name__ == "__main__":
    unittest.main(verbosity=2)

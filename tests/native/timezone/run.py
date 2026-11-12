#!/usr/bin/env python3
"""Independent, reproducible tests for std/timezone.

The driver only starts the supplied woozyc and the two test fixtures.  It does
not rebuild the compiler, does not modify the frozen fixture, and does not
write outside ``--out``.  Real tzdata is read from an explicit zoneinfo tree
(default /usr/share/zoneinfo); Python's ZoneInfo.from_file is always given the
same bytes that the Woozy fixture loads.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo, available_timezones, reset_tzpath

import tzif

UTC = timezone.utc
EPOCH_NAIVE = datetime(1970, 1, 1)
EPOCH_UTC = datetime(1970, 1, 1, tzinfo=UTC)
MAX_REPORT_FAILURES = 500
SELECTED_ZONES = [
    "America/New_York",
    "Europe/London",
    "Australia/Sydney",
    "Australia/Lord_Howe",
    "Pacific/Apia",
    "Europe/Dublin",
    "Europe/Amsterdam",
    "Africa/Monrovia",
    "America/Santiago",
    "Pacific/Chatham",
]
YEAR_SAMPLES = [1900, 1970, 2026, 2050, 2400]
ALL_TRANSITION_YEARS = [
    1900,
    1970,
    2024,
    2025,
    2026,
    2027,
    2038,
    2050,
    2099,
    2100,
    2399,
    2400,
]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256(path.read_bytes())


def naive_ts(value: datetime) -> int:
    return int((value - EPOCH_NAIVE).total_seconds())


def python_local(instant: int, zone: ZoneInfo) -> tuple[str, str]:
    local = datetime.fromtimestamp(instant, zone)
    offset = int(local.utcoffset().total_seconds())
    daylight = 1 if local.dst() else 0
    abbreviation = local.tzname()
    return (
        f"{local.year},{local.month},{local.day},{local.hour},"
        f"{local.minute},{local.second},{offset},{daylight},{abbreviation}",
        abbreviation,
    )


def python_candidates(zone: ZoneInfo, local: datetime) -> list[tuple[int, str]]:
    """Return exact UTC candidates that round-trip to the naive local time.

    fold=0 and fold=1 are both tried and independently round-tripped.  A
    default fold=0 result is never treated as proof that a time is unambiguous.
    """
    found: dict[int, str] = {}
    for fold in (0, 1):
        aware = local.replace(tzinfo=zone, fold=fold)
        instant = int((aware - EPOCH_UTC).total_seconds())
        back = datetime.fromtimestamp(instant, zone)
        if back.replace(tzinfo=None) == local and back.fold == fold:
            found[instant] = back.tzname()
    return sorted(found.items())


def local_expectation(
    zone: ZoneInfo, local: datetime, resolution: str
) -> tuple[str, str]:
    """Return (fixture line, classification) for a from_local request."""
    raw = python_candidates(zone, local)
    # A type designated -00 is explicitly unspecified in RFC 9636.  The Root
    # API chooses OutOfRange for it, so it cannot contribute a candidate.
    candidates = [(instant, name) for instant, name in raw if name != "-00"]
    if not raw:
        return "nonexistent", "nonexistent"
    if not candidates:
        return "out-of-range", "unknown-local-time"
    candidates.sort()
    if len(candidates) > 1:
        if resolution == "reject":
            return f"ambiguous:{candidates[0][0]},{candidates[-1][0]}", "ambiguous"
        if resolution == "earlier":
            return str(candidates[0][0]), "ambiguous-earlier"
        return str(candidates[-1][0]), "ambiguous-later"
    return str(candidates[0][0]), "unique"


# ---------------------------------------------------------------------------
# Synthetic TZif fixtures


@dataclass
class SyntheticFixture:
    name: str
    data: bytes
    mode: str  # python, footer, no-footer-transition, minus00
    spec: tzif.Footer | None = None
    parsed: tzif.ParsedTzif | None = None


def rule_m(month: int, week: int, weekday: int, hours: int = 2) -> tzif.Rule:
    return tzif.Rule(2, month, week, weekday, hours * 3600)


def rule_n(day: int, hours: int = 2) -> tzif.Rule:
    return tzif.Rule(0, day, 0, 0, hours * 3600)


def rule_j(day: int, hours: int = 2) -> tzif.Rule:
    return tzif.Rule(1, day, 0, 0, hours * 3600)


def make_synthetic(out: Path) -> tuple[dict[str, SyntheticFixture], dict[str, tuple[bytes, str]]]:
    out.mkdir(parents=True, exist_ok=True)
    valid: dict[str, SyntheticFixture] = {}
    invalid: dict[str, tuple[bytes, str]] = {}

    def add_valid(
        name: str,
        data: bytes,
        mode: str = "python",
        spec: tzif.Footer | None = None,
    ) -> None:
        parsed = tzif.parse_tzif(data)
        valid[name] = SyntheticFixture(name, data, mode, spec, parsed)
        (out / name).write_bytes(data)

    def add_footer(
        name: str,
        types: list[tuple[int, int, str]],
        footer: str,
        spec: tzif.Footer,
        version: int = 2,
    ) -> None:
        add_valid(
            name,
            tzif.make_tzif(version, types=types, footer=footer),
            "footer",
            spec,
        )

    add_valid("fixed_v1", tzif.make_tzif(0, types=[(0, 0, "FIX")], footer=None))
    add_valid(
        "fixed_v2",
        tzif.make_tzif(2, types=[(3600, 0, "ONE")], footer="ONE-1"),
    )
    add_valid(
        "post2038_v2",
        tzif.make_tzif(
            2,
            transitions=[2147483648],
            type_indexes=[1],
            types=[(0, 0, "AAA"), (3600, 0, "AAA")],
            footer="AAA-1",
            first=((), [], [(0, 0, "AAA")], (), (0,), (0,)),
        ),
    )
    add_valid(
        "trans_v1",
        tzif.make_tzif(
            0,
            transitions=[0],
            type_indexes=[1],
            types=[(0, 0, "AAA"), (3600, 0, "BBB")],
            footer=None,
        ),
        "no-footer-transition",
    )
    add_valid(
        "nofooter",
        tzif.make_tzif(
            2,
            transitions=[0],
            type_indexes=[1],
            types=[(0, 0, "AAA"), (3600, 0, "BBB")],
            footer="",
            first=((), [], [(0, 0, "AAA")], (), (0,), (0,)),
        ),
        "no-footer-transition",
    )
    add_valid(
        "nofooter_fixed",
        tzif.make_tzif(2, types=[(0, 0, "AAA")], footer=""),
    )
    add_valid("minus00", tzif.make_tzif(2, types=[(0, 0, "-00")], footer=""), "minus00")
    add_valid(
        "minus00_trans",
        tzif.make_tzif(
            2,
            transitions=[0],
            type_indexes=[1],
            types=[(0, 0, "AAA"), (0, 0, "-00")],
            footer="<-00>0",
            first=((), [], [(0, 0, "AAA")], (), (0,), (0,)),
        ),
        "minus00",
    )
    add_valid(
        "minus00_footer",
        tzif.make_tzif(2, types=[(0, 0, "-00")], footer="<-00>0"),
        "minus00",
    )
    std_dst = [(0, 0, "STD"), (3600, 1, "DST")]
    add_footer(
        "footer_m",
        std_dst,
        "STD0DST,M3.2.0/2,M11.1.0/2",
        tzif.Footer(0, "STD", 3600, "DST", rule_m(3, 2, 0), rule_m(11, 1, 0)),
    )
    add_footer(
        "southern",
        std_dst,
        "STD0DST,M10.1.0/2,M4.1.0/3",
        tzif.Footer(0, "STD", 3600, "DST", rule_m(10, 1, 0), rule_m(4, 1, 0, 3)),
    )
    add_footer(
        "v3_extended",
        std_dst,
        "STD0DST,M3.2.0/-2,M11.1.0/26",
        tzif.Footer(
            0, "STD", 3600, "DST", rule_m(3, 2, 0, -2), rule_m(11, 1, 0, 26)
        ),
        version=3,
    )
    add_footer(
        "allyear",
        [(-10800, 0, "XXX"), (-14400, 1, "EDT")],
        "XXX3EDT4,0/0,J365/23",
        tzif.Footer(
            -10800,
            "XXX",
            -14400,
            "EDT",
            rule_n(0, 0),
            rule_j(365, 23),
        ),
    )
    add_footer(
        "footer_j",
        std_dst,
        "STD0DST,J60/2,J300/2",
        tzif.Footer(0, "STD", 3600, "DST", rule_j(60), rule_j(300)),
    )
    add_footer(
        "footer_n",
        std_dst,
        "STD0DST,60/2,300/2",
        tzif.Footer(0, "STD", 3600, "DST", rule_n(60), rule_n(300)),
    )
    add_footer(
        "footer_default",
        std_dst,
        "STD0DST,M3.2.0,M11.1.0",
        tzif.Footer(0, "STD", 3600, "DST", rule_m(3, 2, 0), rule_m(11, 1, 0)),
    )
    add_footer(
        "footer_stdonly",
        [(-18000, 0, "STD")],
        "STD5",
        tzif.Footer(-18000, "STD", -18000, "STD", rule_n(0), rule_n(0), False),
    )
    add_footer(
        "footer_angle",
        [(-10800, 0, "-03"), (-7200, 1, "-02")],
        "<-03>3<-02>,M3.5.0/-2,M10.5.0/-1",
        tzif.Footer(
            -10800,
            "-03",
            -7200,
            "-02",
            rule_m(3, 5, 0, -2),
            rule_m(10, 5, 0, -1),
        ),
        version=3,
    )
    add_footer(
        "footer_ireland",
        [(3600, 0, "IST"), (0, 1, "GMT")],
        "IST-1GMT0,M10.5.0,M3.5.0/1",
        tzif.Footer(
            3600,
            "IST",
            0,
            "GMT",
            rule_m(10, 5, 0),
            rule_m(3, 5, 0, 1),
        ),
    )
    add_footer(
        "footer_v3_neg",
        std_dst,
        "STD0DST,M3.2.0/-167,M11.1.0/167",
        tzif.Footer(
            0,
            "STD",
            3600,
            "DST",
            rule_m(3, 2, 0, -167),
            rule_m(11, 1, 0, 167),
        ),
        version=3,
    )
    add_footer(
        "v4_min",
        [(0, 0, "AAA")],
        "AAA0",
        tzif.Footer(0, "AAA", 0, "AAA", rule_n(0), rule_n(0), False),
        version=4,
    )

    # Deliberately malformed files.  Most are byte mutations of one valid
    # base so the failing offset is stable and easy to inspect.
    base = tzif.make_tzif(2, types=[(0, 0, "AAA")], footer="AAA0")
    parsed = tzif.parse_tzif(base)
    second_header = parsed.second_header_offset
    assert second_header is not None
    type_offset = parsed.second_type_offset
    chars_offset = parsed.second_chars_offset
    assert type_offset is not None and chars_offset is not None
    char_count = parsed.second_counts[5]  # type: ignore[index]

    def patch_counts(data: bytes, field: int, value: int) -> bytes:
        mutable = bytearray(data)
        offset = second_header + 20 + field * 4
        mutable[offset : offset + 4] = value.to_bytes(4, "big")
        return bytes(mutable)

    def add_invalid(name: str, data: bytes, expected: str = "invalid-data") -> None:
        invalid[name] = (data, expected)
        (out / name).write_bytes(data)

    # Structurally valid TZif files with leap records are intentionally
    # rejected by the Root API (its Timespec is POSIX seconds).
    add_invalid(
        "leap_v1",
        tzif.make_tzif(0, types=[(0, 0, "UTC")], footer=None, leaps=[(0, 1)]),
        "leap-clock",
    )
    add_invalid(
        "leap_v2",
        tzif.make_tzif(2, types=[(0, 0, "UTC")], footer="UTC0", leaps=[(0, 1)]),
        "leap-clock",
    )

    add_invalid("bad_magic", b"TZif"[:3] + b"X" + base[4:])
    add_invalid("bad_version_1", base[:4] + b"1" + base[5:])
    add_invalid("bad_version_5", base[:4] + b"5" + base[5:])
    add_invalid("bad_reserved", base[:5] + b"\x01" + base[6:])
    add_invalid("truncated_header", base[:30])
    add_invalid("truncated_block", base[:60])
    add_invalid("truncated_footer", base[:-1])
    add_invalid("footer_extra", base + b"X")
    add_invalid("footer_nul", base[:-1] + b"\x00\n")
    add_invalid("footer_high", base[:-1] + b"\x80\n")
    for name, field, value in [
        ("utcnt_bad", 0, 2),
        ("stdcnt_bad", 1, 2),
        ("typecnt_zero", 4, 0),
        ("typecnt_257", 4, 257),
        ("charcnt_zero", 5, 0),
        ("timecnt_huge", 3, 0xFFFFFFFF),
    ]:
        add_invalid(name, patch_counts(base, field, value))
    second_version = bytearray(base)
    second_version[second_header + 4] = ord("3")
    add_invalid("second_version_mismatch", bytes(second_version))

    one_type = [(0, 0, "AAA")]
    first_placeholder = ((), [], one_type, (), (0,), (0,))
    for name, transitions in [
        ("nonincrease_equal", [0, 0]),
        ("nonincrease_decrease", [3600, 0]),
    ]:
        add_invalid(
            name,
            tzif.make_tzif(
                2,
                transitions=transitions,
                type_indexes=[0, 0],
                types=one_type,
                footer="AAA0",
                first=first_placeholder,
            ),
        )
    add_invalid(
        "bad_typeindex",
        tzif.make_tzif(
            2,
            transitions=[0],
            type_indexes=[1],
            types=one_type,
            footer="AAA0",
            first=first_placeholder,
        ),
    )
    isdst = bytearray(base)
    isdst[type_offset + 4] = 2
    add_invalid("bad_isdst", bytes(isdst))
    bad_offset = bytearray(base)
    bad_offset[type_offset : type_offset + 4] = (-2147483648).to_bytes(4, "big", signed=True)
    add_invalid("bad_offset_min", bytes(bad_offset))
    bad_abbr_index = bytearray(base)
    bad_abbr_index[type_offset + 5] = 9
    add_invalid("bad_abbr_index", bytes(bad_abbr_index))
    unterminated = bytearray(base)
    unterminated[chars_offset + char_count - 1] = ord("X")
    add_invalid("abbr_unterminated", bytes(unterminated))
    add_invalid(
        "bad_std_ut",
        tzif.make_tzif(2, types=one_type, footer="AAA0", std=[0], ut=[1]),
    )
    add_invalid(
        "bad_std_value",
        tzif.make_tzif(2, types=one_type, footer="AAA0", std=[2], ut=[0]),
    )

    footer_cases = [
        ("footer_shortname", 2, "AA0"),
        ("footer_bad_month", 2, "AAA0BBB,M13.1.0,M11.1.0"),
        ("footer_bad_week", 2, "AAA0BBB,M3.0.0,M11.1.0"),
        ("footer_bad_dow", 2, "AAA0BBB,M3.1.7,M11.1.0"),
        ("footer_J0", 2, "AAA0BBB,J0/2,J365/2"),
        ("footer_day366", 2, "AAA0BBB,366/2,0/2"),
        ("footer_hour25_v2", 2, "AAA0BBB,M3.2.0/25,M11.1.0/2"),
        ("footer_hour168_v3", 3, "AAA0BBB,M3.2.0/168,M11.1.0/2"),
        ("footer_neg_v2", 2, "AAA0BBB,M3.2.0/-1,M11.1.0/2"),
        ("footer_off25", 2, "AAA25BBB,M3.2.0/2,M11.1.0/2"),
        ("footer_missing_comma", 2, "AAA0BBB,M3.2.0/2M11.1.0/2"),
        ("footer_missing_rule", 2, "AAA0BBB,M3.2.0/2,"),
    ]
    for name, version, footer in footer_cases:
        add_invalid(
            name,
            tzif.make_tzif(
                version,
                types=[(0, 0, "AAA"), (3600, 1, "BBB")],
                footer=footer,
            ),
        )
    add_invalid("v2_missing_footer", base[:-6])
    add_invalid("v2_one_newline", base[:-1])

    # RFC 9636 section 3.3: a non-empty footer MUST agree with the final
    # transition's time type.  AAA is +01:00; BBB3 is -03:00.
    add_invalid(
        "inconsistent_footer",
        tzif.make_tzif(
            2,
            transitions=[0],
            type_indexes=[1],
            types=[(0, 0, "AAA"), (3600, 0, "AAA")],
            footer="BBB3",
            first=first_placeholder,
        ),
    )
    return valid, invalid


# ---------------------------------------------------------------------------
# Command execution and reporting


class Driver:
    def __init__(self, out: Path, env: dict[str, str]):
        self.out = out
        self.logs = out / "logs"
        self.cases = out / "cases"
        self.logs.mkdir(parents=True, exist_ok=True)
        self.cases.mkdir(parents=True, exist_ok=True)
        self.env = env
        self.failures: list[dict] = []
        self.batches: list[dict] = []

    def command(
        self,
        name: str,
        argv: list[str],
        *,
        timeout: float,
        cwd: Path | None = None,
        input_text: str | None = None,
    ) -> dict:
        argv_text = [str(item) for item in argv]
        input_bytes = b"" if input_text is None else input_text.encode()
        input_path = self.logs / f"{name}.stdin"
        stdout_path = self.logs / f"{name}.stdout"
        stderr_path = self.logs / f"{name}.stderr"
        input_path.write_bytes(input_bytes)
        try:
            result = subprocess.run(
                argv_text,
                input=input_bytes,
                capture_output=True,
                cwd=str(cwd) if cwd is not None else None,
                env=self.env,
                timeout=timeout,
            )
            status = "exited"
            exit_code = result.returncode
            stdout = result.stdout
            stderr = result.stderr
        except subprocess.TimeoutExpired as error:
            status = "timeout"
            exit_code = None
            stdout = error.stdout or b""
            stderr = error.stderr or b""
        except OSError as error:
            status = "launch_failed"
            exit_code = None
            stdout = b""
            stderr = str(error).encode()
        stdout_path.write_bytes(stdout)
        stderr_path.write_bytes(stderr)
        record = {
            "argv": argv_text,
            "cwd": str(cwd) if cwd is not None else None,
            "status": status,
            "exit": exit_code,
            "timeout_seconds": timeout,
            "input_bytes": len(input_bytes),
            "stdout_bytes": len(stdout),
            "stderr_bytes": len(stderr),
            "stdout_sha256": sha256(stdout),
            "stderr_sha256": sha256(stderr),
            "input_log": str(input_path),
            "stdout_log": str(stdout_path),
            "stderr_log": str(stderr_path),
        }
        if len(stdout) <= 4096:
            record["stdout_text"] = stdout.decode("utf-8", "backslashreplace")
        if len(stderr) <= 4096:
            record["stderr_text"] = stderr.decode("utf-8", "backslashreplace")
        return record

    def run_batch(
        self,
        batch_id: str,
        argv: list[str],
        lines: list[str],
        expected: list[str],
        *,
        timeout: float,
        cwd: Path | None = None,
        executable: str | None = None,
    ) -> None:
        if len(lines) != len(expected):
            raise RuntimeError(f"{batch_id}: line/expected length mismatch")
        argv = [executable or argv[0]] + argv[1:] if executable else argv
        command = self.command(
            batch_id,
            argv,
            timeout=timeout,
            cwd=cwd,
            input_text="\n".join(lines) + ("\n" if lines else ""),
        )
        actual = command["stdout_text"].splitlines() if "stdout_text" in command else []
        if "stdout_text" not in command:
            actual = (self.logs / f"{batch_id}.stdout").read_text(
                encoding="utf-8", errors="backslashreplace"
            ).splitlines()
        cases = []
        for index, (line, want) in enumerate(zip(lines, expected)):
            have = actual[index] if index < len(actual) else None
            status = "passed" if have == want else "failed"
            case = {
                "batch": batch_id,
                "index": index,
                "input": line,
                "expected": want,
                "actual": have,
                "status": status,
            }
            cases.append(case)
            if status == "failed":
                self.failures.append(case)
        for offset, extra in enumerate(actual[len(expected) :], start=len(expected)):
            case = {
                "batch": batch_id,
                "index": offset,
                "input": None,
                "expected": None,
                "actual": extra,
                "status": "failed",
            }
            cases.append(case)
            self.failures.append(case)
        case_path = self.cases / f"{batch_id}.jsonl"
        with case_path.open("w", encoding="utf-8") as stream:
            for case in cases:
                stream.write(json.dumps(case, ensure_ascii=False) + "\n")
        summary = {
            "id": batch_id,
            "command": command,
            "case_count": len(cases),
            "passed": sum(case["status"] == "passed" for case in cases),
            "failed": sum(case["status"] == "failed" for case in cases),
            "cases": str(case_path),
        }
        self.batches.append(summary)
        if command["status"] != "exited" or command["exit"] != 0:
            failure = {
                "batch": batch_id,
                "index": None,
                "input": None,
                "expected": f"command exit 0 ({len(cases)} cases)",
                "actual": f"{command['status']} exit={command['exit']}",
                "status": "failed",
            }
            self.failures.append(failure)


def patch_second_header_counts(data: bytes, parsed: tzif.ParsedTzif, field: int, value: int) -> bytes:
    """Small shared helper used by make_synthetic (kept for readability)."""
    assert parsed.second_header_offset is not None
    mutable = bytearray(data)
    offset = parsed.second_header_offset + 20 + field * 4
    mutable[offset : offset + 4] = value.to_bytes(4, "big")
    return bytes(mutable)


# ---------------------------------------------------------------------------
# Corpus generation


def utc_epoch_cases(
    zones: list[str], zoneinfo: Path
) -> tuple[list[str], list[str], dict[str, int]]:
    epoch_dates = [
        datetime(1900, 1, 15, 12, 34, 56),
        datetime(1900, 7, 15, 12, 34, 56),
        datetime(1970, 1, 15, 12, 34, 56),
        datetime(1970, 7, 15, 12, 34, 56),
        datetime(2026, 1, 15, 12, 34, 56),
        datetime(2026, 7, 15, 12, 34, 56),
        datetime(2050, 1, 15, 12, 34, 56),
        datetime(2050, 7, 15, 12, 34, 56),
        datetime(2400, 1, 15, 12, 34, 56),
        datetime(2400, 7, 15, 12, 34, 56),
        datetime(2038, 1, 19, 3, 14, 7),
        datetime(2038, 1, 19, 3, 14, 8),
    ]
    epochs = [naive_ts(value) for value in epoch_dates]
    lines: list[str] = []
    expected: list[str] = []
    counts: dict[str, int] = {}
    for name in zones:
        data = (zoneinfo / name).read_bytes()
        zone = ZoneInfo.from_file(io.BytesIO(data), key=name)
        for instant in epochs:
            text, abbreviation = python_local(instant, zone)
            expected.append("out-of-range" if abbreviation == "-00" else text)
            lines.append(f"utc|{name}|{instant}")
        counts[name] = len(epochs)
    return lines, expected, counts


def utc_transition_cases(
    zones: list[str], zoneinfo: Path
) -> tuple[list[str], list[str], dict[str, int]]:
    lines: list[str] = []
    expected: list[str] = []
    counts: dict[str, int] = {}
    for name in zones:
        data = (zoneinfo / name).read_bytes()
        parsed = tzif.parse_tzif(data)
        zone = ZoneInfo.from_file(io.BytesIO(data), key=name)
        instants: set[int] = {-2208988800, 0, 4102444800}
        for instant in parsed.transitions:
            instants.update((instant - 1, instant, instant + 1))
        for instant in sorted(instants):
            text, abbreviation = python_local(instant, zone)
            expected.append("out-of-range" if abbreviation == "-00" else text)
            lines.append(f"utc|{name}|{instant}")
        counts[name] = len(instants)
    return lines, expected, counts


def reverse_epoch_cases(
    zones: list[str], zoneinfo: Path
) -> tuple[list[str], list[str], dict[str, int]]:
    local_dates = []
    for year in YEAR_SAMPLES:
        for month, day in ((1, 15), (7, 15)):
            local_dates.append(datetime(year, month, day, 12, 34, 56))
    lines: list[str] = []
    expected: list[str] = []
    counts: dict[str, int] = {}
    for name in zones:
        data = (zoneinfo / name).read_bytes()
        zone = ZoneInfo.from_file(io.BytesIO(data), key=name)
        for local in local_dates:
            instant = naive_ts(local)
            for resolution in ("reject", "earlier", "later"):
                want, _classification = local_expectation(zone, local, resolution)
                lines.append(f"{resolution}|{name}|{instant}")
                expected.append(want)
        counts[name] = len(local_dates) * 3
    return lines, expected, counts


def reverse_selected_cases(
    selected: list[str], zoneinfo: Path
) -> tuple[list[str], list[str], dict[str, int]]:
    deltas = [
        -86400,
        -82800,
        -7200,
        -3601,
        -3600,
        -3599,
        -1801,
        -1800,
        -1799,
        -61,
        -60,
        -59,
        -31,
        -30,
        -29,
        -1,
        0,
        1,
        29,
        30,
        31,
        59,
        60,
        61,
        1799,
        1800,
        1801,
        3599,
        3600,
        3601,
        7200,
        82800,
        86400,
    ]
    seen: set[tuple[str, str, int]] = set()
    lines: list[str] = []
    expected: list[str] = []
    counts: dict[str, int] = {}
    for name in selected:
        data = (zoneinfo / name).read_bytes()
        parsed = tzif.parse_tzif(data)
        zone = ZoneInfo.from_file(io.BytesIO(data), key=name)
        local_values: set[datetime] = set()
        for instant in parsed.transitions:
            for base in (instant - 1, instant):
                try:
                    around = datetime.fromtimestamp(base, zone).replace(tzinfo=None)
                except (OverflowError, OSError, ValueError):
                    continue
                for delta in deltas:
                    local = around + timedelta(seconds=delta)
                    if 1 <= local.year <= 9999:
                        local_values.add(local)
        for local in sorted(local_values):
            instant = naive_ts(local)
            for resolution in ("reject", "earlier", "later"):
                key = (resolution, name, instant)
                if key in seen:
                    continue
                seen.add(key)
                want, _classification = local_expectation(zone, local, resolution)
                lines.append(f"{resolution}|{name}|{instant}")
                expected.append(want)
        counts[name] = len(local_values) * 3
    return lines, expected, counts


def data_parse_cases(
    zones: list[str],
    zoneinfo: Path,
    valid: dict[str, SyntheticFixture],
    invalid: dict[str, tuple[bytes, str]],
) -> tuple[list[str], list[str]]:
    lines: list[str] = []
    expected: list[str] = []
    for name in zones:
        lines.append(f"data|zoneinfo/{name}|0")
        expected.append(f"zoneinfo/{name}")
    right_root = zoneinfo / "right"
    if right_root.is_dir():
        for path in sorted(item for item in right_root.rglob("*") if item.is_file()):
            relative = path.relative_to(right_root).as_posix()
            lines.append(f"data|zoneinfo/right/{relative}|0")
            expected.append("leap-clock")
    for name in sorted(valid):
        lines.append(f"data|{name}|0")
        expected.append(name)
    for name in sorted(invalid):
        lines.append(f"data|{name}|0")
        expected.append(invalid[name][1])
    bad_names = [
        "/etc/passwd",
        "../etc/passwd",
        "a/../../b",
        "./fixed_v1",
        "fixed_v1/",
        "",
        "a//b",
        "..",
        "zoneinfo/../etc/passwd",
    ]
    for name in bad_names:
        lines.append(f"data|{name}|0")
        expected.append("invalid-name")
    lines.append("data|does-not-exist|0")
    expected.append("io-error")
    return lines, expected


def filename_cases() -> tuple[list[str], list[str]]:
    lines = [
        "utc|/etc/passwd|0",
        "utc|../UTC|0",
        "reject|/etc/passwd|0",
        "reject|../UTC|0",
    ]
    return lines, ["invalid-name"] * len(lines)


def loader_from_local_cases() -> tuple[list[str], list[str]]:
    lines = [
        "reject|fixed_v2|0",
        "reject|nofooter|-86400",
        "reject|nofooter|4102444800",
        "reject|trans_v1|4102444800",
        "reject|minus00_footer|0",
        "reject|minus00_trans|0",
    ]
    expected = [
        "-3600",
        "-86400",
        "out-of-range",
        "out-of-range",
        "out-of-range",
        "out-of-range",
    ]
    return lines, expected


def factory_minus00_cases() -> tuple[list[str], list[str]]:
    lines = [
        "utc|Factory|0",
        "reject|Factory|0",
        "earlier|Factory|0",
        "later|Factory|0",
    ]
    return lines, ["out-of-range"] * len(lines)


def synthetic_conversion_cases(
    valid: dict[str, SyntheticFixture]
) -> tuple[list[str], list[str]]:
    lines: list[str] = []
    expected: list[str] = []
    years = ALL_TRANSITION_YEARS
    months_days_hours = [
        (1, 1, 0),
        (1, 15, 12),
        (3, 1, 0),
        (3, 15, 12),
        (4, 1, 0),
        (4, 7, 0),
        (6, 1, 0),
        (7, 1, 0),
        (9, 1, 0),
        (10, 1, 0),
        (10, 15, 12),
        (11, 1, 0),
        (12, 31, 23),
    ]
    for name in sorted(valid):
        fixture = valid[name]
        instants: set[int] = set()
        if fixture.mode == "footer":
            assert fixture.spec is not None
            for year in years:
                for base in (
                    fixture.spec.transition_seconds(
                        year, fixture.spec.start, fixture.spec.standard_offset
                    ),
                    fixture.spec.transition_seconds(
                        year, fixture.spec.end, fixture.spec.summer_offset
                    ),
                ):
                    for delta in (-86400, -3601, -3600, -1, 0, 1, 3600, 3601, 86400):
                        instants.add(base + delta)
        if fixture.parsed is not None:
            for instant in fixture.parsed.transitions:
                instants.update((instant - 1, instant, instant + 1))
        for year in years:
            for month, day, hour in months_days_hours:
                try:
                    instants.add(naive_ts(datetime(year, month, day, hour)))
                except ValueError:
                    continue
        assert fixture.parsed is not None
        zone = None
        if fixture.mode in ("python", "minus00", "no-footer-transition"):
            try:
                zone = ZoneInfo.from_file(io.BytesIO(fixture.data), key=name)
            except Exception:
                zone = None
        for instant in sorted(instants):
            if fixture.mode == "footer":
                assert fixture.spec is not None
                want = tzif.format_local(instant, fixture.spec.type_at(instant))
            elif fixture.mode == "no-footer-transition":
                if fixture.parsed.transitions and instant >= fixture.parsed.transitions[-1]:
                    want = "out-of-range"
                elif zone is not None:
                    want = python_local(instant, zone)[0]
                else:
                    want = "out-of-range"
            elif fixture.mode == "minus00":
                if zone is None:
                    want = "out-of-range"
                else:
                    text, abbreviation = python_local(instant, zone)
                    want = "out-of-range" if abbreviation == "-00" else text
            else:
                if zone is None:
                    continue
                want = python_local(instant, zone)[0]
            lines.append(f"utc|{name}|{instant}")
            expected.append(want)
    return lines, expected


def synthetic_python_divergences(
    valid: dict[str, SyntheticFixture]
) -> list[dict]:
    """Record Python zoneinfo divergences from the independent footer oracle.

    Python 3.14 zoneinfo treats the POSIX zero-based day-of-year rule as
    1-based (and applies leap-day handling incorrectly).  tzcode's zdump and
    RFC 9636's zero-based definition agree with the oracle/Root.  This is
    evidence only; it is not counted as a Root failure.
    """
    divergences: list[dict] = []
    sample = [
        naive_ts(datetime(2024, 2, 29, 2, 0, 0)),
        naive_ts(datetime(2024, 3, 1, 2, 0, 0)),
        naive_ts(datetime(2025, 3, 1, 2, 0, 0)),
        naive_ts(datetime(2025, 3, 2, 2, 0, 0)),
        naive_ts(datetime(2025, 10, 27, 1, 0, 0)),
        naive_ts(datetime(2025, 10, 28, 1, 0, 0)),
    ]
    for name in sorted(valid):
        fixture = valid[name]
        if fixture.mode != "footer" or fixture.spec is None:
            continue
        try:
            zone = ZoneInfo.from_file(io.BytesIO(fixture.data), key=name)
        except Exception as error:
            divergences.append({"fixture": name, "error": repr(error)})
            continue
        for instant in sample:
            try:
                python_text = python_local(instant, zone)[0]
            except Exception as error:
                divergences.append(
                    {"fixture": name, "instant": instant, "python_error": repr(error)}
                )
                continue
            oracle_text = tzif.format_local(instant, fixture.spec.type_at(instant))
            if python_text != oracle_text:
                divergences.append(
                    {
                        "fixture": name,
                        "instant": instant,
                        "python": python_text,
                        "rfc_oracle": oracle_text,
                    }
                )
    return divergences[:40]


# ---------------------------------------------------------------------------
# Static ELF checks


def static_check(driver: Driver, label: str, program: Path) -> dict:
    readelf_headers = driver.command(
        f"{label}-readelf-headers", ["readelf", "-lW", str(program)], timeout=30
    )
    readelf_dynamic = driver.command(
        f"{label}-readelf-dynamic", ["readelf", "-dW", str(program)], timeout=30
    )
    nm_undefined = driver.command(
        f"{label}-nm-undefined",
        ["nm", "-u", "--format=posix", str(program)],
        timeout=30,
    )
    header_text = readelf_headers.get("stdout_text", "")
    dynamic_text = readelf_dynamic.get("stdout_text", "")
    nm_text = nm_undefined.get("stdout_text", "")
    strong_undefined = []
    for line in nm_text.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "U":
            strong_undefined.append(fields[0])
    passed = (
        readelf_headers["status"] == "exited"
        and readelf_headers["exit"] == 0
        and "INTERP" not in header_text
        and readelf_dynamic["status"] == "exited"
        and readelf_dynamic["exit"] == 0
        and "NEEDED" not in dynamic_text
        and nm_undefined["status"] == "exited"
        and nm_undefined["exit"] == 0
        and not strong_undefined
    )
    return {
        "label": label,
        "program": str(program),
        "program_sha256": sha256_file(program),
        "readelf_headers": readelf_headers,
        "readelf_dynamic": readelf_dynamic,
        "nm_undefined": nm_undefined,
        "strong_undefined": strong_undefined,
        "no_interp": "INTERP" not in header_text,
        "no_needed": "NEEDED" not in dynamic_text,
        "status": "passed" if passed else "failed",
    }


# ---------------------------------------------------------------------------
# Main


def find_cli_std_root(cli: Path) -> Path | None:
    """Find the std source root compiled into the supplied CLI, if declared."""
    build_ninja = cli.parent / "build.ninja"
    if build_ninja.is_file():
        for line in build_ninja.read_text(encoding="utf-8", errors="replace").splitlines():
            marker = r'LANG_STD_SOURCE_ROOT=\"'
            if marker in line:
                start = line.index(marker) + len(marker)
                end = line.find(r'\"', start)
                if end > start:
                    return Path(line[start:end])
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--zoneinfo", type=Path, default=Path("/usr/share/zoneinfo"))
    args = parser.parse_args()

    args.zoneinfo = args.zoneinfo.resolve()
    reset_tzpath([str(args.zoneinfo)])
    root = args.root.resolve()
    cli = args.cli.resolve()
    out = args.out.resolve()
    cli_std_root = find_cli_std_root(cli)
    if out.exists():
        if not out.is_dir() or any(out.iterdir()):
            print(f"refusing to overwrite non-empty --out: {out}", file=sys.stderr)
            return 2
    out.mkdir(parents=True, exist_ok=True)

    toolchain_lib = Path(
        "/home/qin/Work/language-dev-20261103/toolchains/libxml2-legacy-2.13.9/usr/lib"
    )
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(toolchain_lib) + ":" + env.get("LD_LIBRARY_PATH", "")

    report: dict = {
        "task": "independent std/timezone verification",
        "operation_id": "woozy-timezone-independent-tests-20261111",
        "status": "failed",
        "started_at": datetime.now(UTC).isoformat(),
        "environment": {
            "python": sys.version,
            "python_executable": sys.executable,
            "zoneinfo_module": getattr(__import__("zoneinfo"), "__file__", None),
            "python_tzpath": list(__import__("zoneinfo").TZPATH),
            "zoneinfo_tree": str(args.zoneinfo),
            "ld_library_path": env["LD_LIBRARY_PATH"],
            "cwd": os.getcwd(),
        },
        "builds": [],
        "static_checks": [],
        "batches": [],
        "failures": [],
        "known_issues": [],
        "not_checked": [
            "No non-Linux/non-x86_64 target was built or executed.",
            "Leap-second arithmetic is intentionally unsupported; right/ files are only checked for rejection.",
            "No exhaustive comparison of the posix/ subtree was added; available_timezones and right/ are covered.",
            "No fuzzing campaign; only the listed minimal malformed TZif cases were exercised.",
            "No Python backend comparison; this task verifies the native backend only.",
        ],
    }
    driver = Driver(out, env)
    valid: dict[str, SyntheticFixture] = {}
    invalid: dict[str, tuple[bytes, str]] = {}
    try:
        fixtures_dir = out / "fixtures"
        fixtures_dir.mkdir(exist_ok=True)
        for link_name, target in (("zoneinfo", args.zoneinfo), ("right", args.zoneinfo / "right")):
            link = fixtures_dir / link_name
            if not link.exists():
                try:
                    os.symlink(target, link)
                except OSError:
                    if target.is_dir():
                        shutil.copytree(target, link)
        valid, invalid = make_synthetic(fixtures_dir)
        report["synthetic_fixtures"] = {
            "valid": {
                name: {"sha256": sha256(value.data), "mode": value.mode}
                for name, value in sorted(valid.items())
            },
            "invalid": {
                name: {"sha256": sha256(value[0]), "expected": value[1]}
                for name, value in sorted(invalid.items())
            },
        }

        if args.zoneinfo != Path("/usr/share/zoneinfo"):
            report["environment"]["zoneinfo_override"] = str(args.zoneinfo)
        if not cli.is_file():
            raise RuntimeError(f"CLI not found: {cli}")

        cli_version = driver.command("cli-version", [str(cli), "--version"], timeout=30)
        report["cli"] = {
            "path": str(cli),
            "sha256": sha256_file(cli),
            "version": cli_version,
            "compiled_stdlib_source_root": str(cli_std_root) if cli_std_root else None,
        }
        report["environment"]["cli_stdlib_root"] = (
            str(cli_std_root) if cli_std_root else None
        )
        tzdata = args.zoneinfo / "tzdata.zi"
        if tzdata.is_file():
            report["tzdata"] = {
                "path": str(tzdata),
                "sha256": sha256_file(tzdata),
                "header": tzdata.read_text(encoding="utf-8", errors="replace").splitlines()[:3],
                "size": tzdata.stat().st_size,
            }

        zone_names = sorted(available_timezones())
        missing_zones = [
            name for name in zone_names if not (args.zoneinfo / name).is_file()
        ]
        if missing_zones:
            raise RuntimeError(
                "available_timezones names missing under --zoneinfo: "
                + ", ".join(missing_zones[:10])
            )
        report["coverage"] = {
            "available_timezone_count": len(zone_names),
            "available_timezones": zone_names,
        }

        # Record hashes of every real file used, plus generated fixtures and the
        # two fixture sources.  This makes the tzdata and parser inputs auditable.
        hash_files: dict[str, str] = {}
        for name in zone_names:
            path = args.zoneinfo / name
            hash_files[str(path)] = sha256_file(path)
        right_root = args.zoneinfo / "right"
        if right_root.is_dir():
            for path in sorted(item for item in right_root.rglob("*") if item.is_file()):
                hash_files[str(path)] = sha256_file(path)
        for name, fixture in valid.items():
            hash_files[str(fixtures_dir / name)] = sha256(fixture.data)
        for name, (data, _expected) in invalid.items():
            hash_files[str(fixtures_dir / name)] = sha256(data)
        for path in [
            root / "tests/native/timezone/project.toml",
            root / "tests/native/timezone/src/main.wz",
            root / "tests/native/timezone/fixture_loader/project.toml",
            root / "tests/native/timezone/fixture_loader/src/main.wz",
            root / "stdlib/timezone/timezone.wz",
        ]:
            if path.is_file():
                hash_files[str(path)] = sha256_file(path)
        if cli_std_root is not None:
            used_timezone = cli_std_root / "timezone/timezone.wz"
            if used_timezone.is_file():
                hash_files[str(used_timezone)] = sha256_file(used_timezone)
                report["stdlib_timezone_used_by_cli"] = {
                    "path": str(used_timezone),
                    "sha256": sha256_file(used_timezone),
                }
        (out / "timezone-hashes.json").write_text(
            json.dumps(hash_files, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        report["timezone_hashes_file"] = str(out / "timezone-hashes.json")
        report["timezone_hashes_count"] = len(hash_files)

        projects = [
            ("main", root / "tests/native/timezone/project.toml"),
            ("loader", root / "tests/native/timezone/fixture_loader/project.toml"),
        ]
        programs: dict[tuple[str, int], Path] = {}
        for label, project in projects:
            for optimization in (0, 2):
                build_dir = out / "build" / label / f"O{optimization}"
                build = driver.command(
                    f"build-{label}-O{optimization}",
                    [
                        str(cli),
                        "build",
                        "--project",
                        str(project),
                        "--backend",
                        "native",
                        "--out-dir",
                        str(build_dir),
                        "--opt",
                        str(optimization),
                        "--format",
                        "json",
                    ],
                    timeout=180,
                )
                program = build_dir / "program"
                build_record = {
                    "id": f"{label}-O{optimization}",
                    "project": str(project),
                    "optimization": optimization,
                    "build_dir": str(build_dir),
                    "command": build,
                    "program": str(program),
                    "program_exists": program.is_file(),
                }
                if program.is_file():
                    build_record["program_sha256"] = sha256_file(program)
                report["builds"].append(build_record)
                if build["status"] == "exited" and build["exit"] == 0 and program.is_file():
                    programs[(label, optimization)] = program
                else:
                    driver.failures.append(
                        {
                            "batch": f"build-{label}-O{optimization}",
                            "index": None,
                            "input": None,
                            "expected": "successful build with program",
                            "actual": f"{build['status']} exit={build['exit']}",
                            "status": "failed",
                        }
                    )

        for (label, optimization), program in sorted(programs.items()):
            static = static_check(driver, f"static-{label}-O{optimization}", program)
            report["static_checks"].append(static)
            if static["status"] != "passed":
                driver.failures.append(
                    {
                        "batch": f"static-{label}-O{optimization}",
                        "index": None,
                        "input": None,
                        "expected": "static ELF without INTERP/NEEDED/strong undefined symbols",
                        "actual": static["status"],
                        "status": "failed",
                    }
                )

        # All corpora are generated once per real executable; O0 and O2 must
        # produce identical bytes in the comparison.
        if ("main", 0) in programs and ("main", 2) in programs:
            epochs_lines, epochs_expected, epoch_counts = utc_epoch_cases(
                zone_names, args.zoneinfo
            )
            transition_lines, transition_expected, transition_counts = (
                utc_transition_cases(zone_names, args.zoneinfo)
            )
            reverse_lines, reverse_expected, reverse_counts = reverse_epoch_cases(
                zone_names, args.zoneinfo
            )
            selected_lines, selected_expected, selected_counts = (
                reverse_selected_cases(SELECTED_ZONES, args.zoneinfo)
            )
            data_lines, data_expected = data_parse_cases(
                zone_names, args.zoneinfo, valid, invalid
            )
            filename_lines, filename_expected = filename_cases()
            synthetic_lines, synthetic_expected = synthetic_conversion_cases(valid)
            report["coverage"].update(
                {
                    "epoch_utc_cases_per_zone": epoch_counts,
                    "transition_utc_cases_per_zone": transition_counts,
                    "reverse_epoch_cases_per_zone": reverse_counts,
                    "reverse_selected_cases_per_zone": selected_counts,
                    "selected_zones": SELECTED_ZONES,
                    "data_parse_cases": len(data_lines),
                    "synthetic_conversion_cases": len(synthetic_lines),
                }
            )
            report["python_oracle_divergences"] = synthetic_python_divergences(valid)
            report["oracle_notes"] = [
                "Real tzdata expectations use ZoneInfo.from_file on the exact bytes read from --zoneinfo.",
                "Synthetic footer-rule expectations use tests/native/timezone/tzif.py, an independent RFC 9636/POSIX evaluator, because Python 3.14 zoneinfo treats zero-based day-of-year n as 1-based.",
                "The n-rule divergence is recorded in python_oracle_divergences; tzcode zdump agrees with the independent oracle and Root on those transitions.",
                "The local-time reverse oracle tests both fold=0 and fold=1 and round-trips each candidate; it never treats fold=0 alone as unambiguous.",
            ]

            corpora = [
                (
                    "utc-all-zones-epochs",
                    epochs_lines,
                    epochs_expected,
                    120,
                    out,
                ),
                (
                    "utc-all-zones-transitions",
                    transition_lines,
                    transition_expected,
                    300,
                    out,
                ),
                (
                    "reverse-all-zones-epochs",
                    reverse_lines,
                    reverse_expected,
                    180,
                    out,
                ),
                (
                    "reverse-selected-transitions",
                    selected_lines,
                    selected_expected,
                    300,
                    out,
                ),
                ("data-parser", data_lines, data_expected, 120, out),
                ("filename-validation", filename_lines, filename_expected, 30, out),
                (
                    "factory-minus00",
                    *factory_minus00_cases(),
                    30,
                    out,
                ),
            ]
            for label, optimization in (("main", 0), ("main", 2)):
                program = programs[(label, optimization)]
                for batch_id, lines, expected, timeout, cwd in corpora:
                    driver.run_batch(
                        f"O{optimization}-{batch_id}",
                        [str(program)],
                        lines,
                        expected,
                        timeout=timeout,
                        cwd=cwd,
                        executable=str(program),
                    )
            # The loader fixture is used for synthetic file conversions.  Run
            # its own build too so O0/O2 compiler behavior is checked.
            for optimization in (0, 2):
                if ("loader", optimization) not in programs:
                    continue
                program = programs[("loader", optimization)]
                driver.run_batch(
                    f"O{optimization}-loader-synthetic-conversions",
                    [str(program)],
                    synthetic_lines,
                    synthetic_expected,
                    timeout=120,
                    cwd=out,
                    executable=str(program),
                )
                from_lines, from_expected = loader_from_local_cases()
                driver.run_batch(
                    f"O{optimization}-loader-from-local",
                    [str(program)],
                    from_lines,
                    from_expected,
                    timeout=30,
                    cwd=out,
                    executable=str(program),
                )
        else:
            for batch in (
                "utc-all-zones-epochs",
                "utc-all-zones-transitions",
                "reverse-all-zones-epochs",
                "reverse-selected-transitions",
                "data-parser",
                "filename-validation",
                "factory-minus00",
                "synthetic-conversions",
            ):
                report["batches"].append(
                    {
                        "id": batch,
                        "case_count": 0,
                        "passed": 0,
                        "failed": 0,
                        "status": "not_run",
                        "reason": "main fixture did not build",
                    }
                )

        # Compare O0 and O2 outputs byte-for-byte for each logical batch.
        by_batch_id = {batch["id"]: batch for batch in driver.batches}
        comparisons = []
        for batch_id, batch in by_batch_id.items():
            if not batch_id.startswith("O0-"):
                continue
            other_id = "O2-" + batch_id[3:]
            other = by_batch_id.get(other_id)
            if other is None:
                comparisons.append(
                    {"batch": batch_id[3:], "identical": False, "reason": "O2 batch missing"}
                )
                continue
            first_stdout = Path(batch["command"]["stdout_log"]).read_bytes()
            second_stdout = Path(other["command"]["stdout_log"]).read_bytes()
            identical = first_stdout == second_stdout
            item = {
                "batch": batch_id[3:],
                "identical": identical,
                "O0_stdout_sha256": sha256(first_stdout),
                "O2_stdout_sha256": sha256(second_stdout),
                "O0_exit": batch["command"]["exit"],
                "O2_exit": other["command"]["exit"],
            }
            comparisons.append(item)
            if not identical:
                driver.failures.append(
                    {
                        "batch": "O0/O2-comparison",
                        "index": None,
                        "input": batch_id[3:],
                        "expected": "identical stdout",
                        "actual": "different stdout",
                        "status": "failed",
                    }
                )
        report["o0_o2_comparison"] = comparisons

        # Locate the two contract failures directly, so Root gets a compact
        # reproducer even if the full corpus is large.  Aggregate repeated
        # timestamps instead of printing hundreds of equivalent rows.
        known_by_id: dict[str, dict] = {}

        def add_known(failure: dict, item: dict) -> None:
            entry = known_by_id.setdefault(
                item["id"],
                {
                    **item,
                    "occurrences": 0,
                    "examples": [],
                },
            )
            entry["occurrences"] += 1
            example = {
                "input": failure.get("input"),
                "expected": failure.get("expected"),
                "actual": failure.get("actual"),
            }
            if example not in entry["examples"] and len(entry["examples"]) < 6:
                entry["examples"].append(example)

        for failure in driver.failures:
            case_input = failure.get("input") or ""
            if case_input in (
                "data|inconsistent_footer|0",
                "utc|inconsistent_footer|0",
            ):
                add_known(
                    failure,
                    {
                        "id": "footer-not-consistent-with-last-type",
                        "rfc": "RFC 9636 section 3.3: a non-empty TZ string MUST be consistent with the last version 2+ transition.",
                        "source": "stdlib/timezone/timezone.wz:277 parses the footer but never compares it with the final transition type",
                        "fixture": str(fixtures_dir / "inconsistent_footer"),
                        "fixture_sha256": sha256((fixtures_dir / "inconsistent_footer").read_bytes()),
                        "minimal_counterexample": "TZif2 transition 0 -> +01:00 AAA, footer BBB3 (-03:00)",
                    },
                )
            if case_input.startswith(
                (
                    "utc|Factory|",
                    "reject|Factory|",
                    "earlier|Factory|",
                    "later|Factory|",
                    "utc|minus00_footer|",
                    "utc|minus00_trans|",
                    "reject|minus00_footer|",
                    "reject|minus00_trans|",
                    "earlier|minus00_footer|",
                    "earlier|minus00_trans|",
                    "later|minus00_footer|",
                    "later|minus00_trans|",
                )
            ):
                fixture_name = case_input.split("|")[1]
                if fixture_name == "Factory":
                    fixture = str(args.zoneinfo / "Factory")
                    fixture_sha = sha256_file(args.zoneinfo / "Factory")
                    counterexample = "real tzdata Factory file: no transitions, type 0 is -00, footer is <-00>0"
                else:
                    fixture = str(fixtures_dir / fixture_name)
                    fixture_sha = sha256((fixtures_dir / fixture_name).read_bytes())
                    counterexample = "TZif2 type 0 = -00, footer = <-00>0 (no transition)"
                add_known(
                    failure,
                    {
                        "id": "-00-footer-bypasses-unknown-local-time-check",
                        "rfc": "RFC 9636 section 3.2: a type designated -00 is a placeholder for unspecified local time.",
                        "source": "stdlib/timezone/timezone.wz:371-372 returns recurring_type before the :385 -00 check; :385 only covers data-block types",
                        "fixture": fixture,
                        "fixture_sha256": fixture_sha,
                        "minimal_counterexample": counterexample,
                    },
                )
        # Make the shortest real-world reproducer the first example for the
        # -00 issue, even though the failure corpus starts with older epochs.
        for entry in known_by_id.values():
            if entry["id"] != "-00-footer-bypasses-unknown-local-time-check":
                continue
            for wanted in ("utc|Factory|0", "reject|Factory|0", "utc|minus00_footer|0", "utc|minus00_trans|0"):
                match = next(
                    (
                        failure
                        for failure in driver.failures
                        if failure.get("input") == wanted
                    ),
                    None,
                )
                if match is None:
                    continue
                example = {
                    "input": match.get("input"),
                    "expected": match.get("expected"),
                    "actual": match.get("actual"),
                }
                if example not in entry["examples"]:
                    entry["examples"].insert(0, example)
            entry["examples"].sort(key=lambda item: len(item.get("input") or ""))
        report["known_issues"] = list(known_by_id.values())

        report["failures"] = driver.failures[:MAX_REPORT_FAILURES]
        report["failure_count"] = len(driver.failures)
        with (out / "failures.jsonl").open("w", encoding="utf-8") as stream:
            for failure in driver.failures:
                stream.write(json.dumps(failure, ensure_ascii=False) + "\n")
        report["failures_file"] = str(out / "failures.jsonl")
        report["batches"] = driver.batches
        report["status"] = "failed" if driver.failures else "passed"
    except Exception as error:
        report["fatal_error"] = repr(error)
        report["status"] = "failed"
        driver.failures.append(
            {
                "batch": "fatal",
                "index": None,
                "input": None,
                "expected": None,
                "actual": repr(error),
                "status": "failed",
            }
        )
    finally:
        report["finished_at"] = datetime.now(UTC).isoformat()
        report["failures"] = driver.failures[:MAX_REPORT_FAILURES]
        report["failure_count"] = len(driver.failures)
        report["batches"] = driver.batches
        try:
            with (out / "failures.jsonl").open("w", encoding="utf-8") as stream:
                for failure in driver.failures:
                    stream.write(json.dumps(failure, ensure_ascii=False) + "\n")
            report["failures_file"] = str(out / "failures.jsonl")
        except OSError:
            pass
        (out / "report.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(
            json.dumps(
                {
                    "status": report.get("status"),
                    "failure_count": report.get("failure_count", 0),
                    "known_issues": [item["id"] for item in report.get("known_issues", [])],
                    "report": str(out / "report.json"),
                },
                ensure_ascii=False,
            )
        )
    return 0 if report.get("status") == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())

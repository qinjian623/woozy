"""Deterministic TZif builders and a small RFC 9636 footer oracle.

This module is test-only.  It does not import or evaluate Woozy source.  The
builder is used to create minimal valid and deliberately malformed TZif files;
the footer oracle is an independent implementation of the POSIX TZ rules used
by the synthetic fixtures.  Real tzdata comparisons still use Python's
zoneinfo.ZoneInfo.from_file on the exact same bytes.
"""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
import struct
from typing import Iterable, Sequence


# ---------------------------------------------------------------------------
# TZif writer


def _designations(names: Sequence[str]) -> tuple[bytes, dict[str, int]]:
    out = bytearray()
    indexes: dict[str, int] = {}
    for name in names:
        if name not in indexes:
            indexes[name] = len(out)
            out += name.encode("ascii") + b"\0"
    return bytes(out), indexes


def _header(
    version: int,
    *,
    timecnt: int,
    typecnt: int,
    charcnt: int,
    leapcnt: int,
    isutcnt: int,
    isstdcnt: int,
) -> bytes:
    version_byte = 0 if version == 0 else ord(str(version))
    return (
        b"TZif"
        + bytes([version_byte])
        + b"\0" * 15
        + struct.pack(">6I", isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt)
    )


def _block(
    transitions: Sequence[int],
    type_indexes: Sequence[int],
    types: Sequence[tuple[int, int, str]],
    chars: bytes,
    names: dict[str, int],
    leaps: Sequence[tuple[int, int]],
    std: Sequence[int],
    ut: Sequence[int],
    width: int,
) -> bytes:
    out = bytearray()
    time_format = ">q" if width == 8 else ">i"
    leap_format = ">qi" if width == 8 else ">ii"
    for value in transitions:
        out += struct.pack(time_format, value)
    out += bytes(type_indexes)
    for offset, isdst, name in types:
        out += struct.pack(">iBB", offset, isdst, names[name])
    out += chars
    for occurrence, correction in leaps:
        out += struct.pack(leap_format, occurrence, correction)
    out += bytes(std)
    out += bytes(ut)
    return bytes(out)


def make_tzif(
    version: int = 2,
    *,
    transitions: Sequence[int] = (),
    type_indexes: Sequence[int] = (),
    types: Sequence[tuple[int, int, str]] = ((0, 0, "UTC"),),
    footer: str | None = "UTC0",
    leaps: Sequence[tuple[int, int]] = (),
    std: Sequence[int] | None = None,
    ut: Sequence[int] | None = None,
    first: tuple[
        Sequence[int],
        Sequence[int],
        Sequence[tuple[int, int, str]],
        Sequence[tuple[int, int]],
        Sequence[int] | None,
        Sequence[int] | None,
    ]
    | None = None,
) -> bytes:
    """Build a minimal TZif file.

    ``footer=None`` omits the footer entirely (used for malformed v2+ files);
    ``footer=""`` writes the required pair of newlines.  ``first`` overrides the
    version 1 block in a version 2+ file, which is needed for transitions after
    2038.  It is ``(transitions, type_indexes, types, leaps, std, ut)``.
    """
    if version not in (0, 2, 3, 4):
        raise ValueError("unsupported TZif version")
    if len(transitions) != len(type_indexes):
        raise ValueError("transition/type-index length mismatch")
    if std is None:
        std = [0] * len(types)
    if ut is None:
        ut = [0] * len(types)
    if len(std) not in (0, len(types)) or len(ut) not in (0, len(types)):
        raise ValueError("bad indicator lengths")

    names = [entry[2] for entry in types]
    chars, name_indexes = _designations(names)
    if version == 0:
        body = _block(
            transitions,
            type_indexes,
            types,
            chars,
            name_indexes,
            leaps,
            std,
            ut,
            4,
        )
        return _header(
            0,
            timecnt=len(transitions),
            typecnt=len(types),
            charcnt=len(chars),
            leapcnt=len(leaps),
            isutcnt=len(ut),
            isstdcnt=len(std),
        ) + body

    if first is None:
        first = (transitions, type_indexes, types, leaps, std, ut)
    (
        first_transitions,
        first_indexes,
        first_types,
        first_leaps,
        first_std,
        first_ut,
    ) = first
    if first_std is None:
        first_std = [0] * len(first_types)
    if first_ut is None:
        first_ut = [0] * len(first_types)
    first_names = [entry[2] for entry in first_types]
    first_chars, first_indexes_by_name = _designations(first_names)
    first_body = _block(
        first_transitions,
        first_indexes,
        first_types,
        first_chars,
        first_indexes_by_name,
        first_leaps,
        first_std,
        first_ut,
        4,
    )
    body = _block(
        transitions,
        type_indexes,
        types,
        chars,
        name_indexes,
        leaps,
        std,
        ut,
        8,
    )
    out = bytearray(
        _header(
            version,
            timecnt=len(first_transitions),
            typecnt=len(first_types),
            charcnt=len(first_chars),
            leapcnt=len(first_leaps),
            isutcnt=len(first_ut),
            isstdcnt=len(first_std),
        )
    )
    out += first_body
    out += _header(
        version,
        timecnt=len(transitions),
        typecnt=len(types),
        charcnt=len(chars),
        leapcnt=len(leaps),
        isutcnt=len(ut),
        isstdcnt=len(std),
    )
    out += body
    if footer is not None:
        out += b"\n" + footer.encode("ascii") + b"\n"
    return bytes(out)


# ---------------------------------------------------------------------------
# TZif reader used to locate transitions and to mutate generated files


@dataclass(frozen=True)
class ParsedTzif:
    version: int
    transitions: tuple[int, ...]
    type_indexes: tuple[int, ...]
    types: tuple[tuple[int, int, str], ...]
    chars: bytes
    footer: str | None
    leap_count: int
    second_header_offset: int | None
    second_block_offset: int | None
    second_type_offset: int | None
    second_chars_offset: int | None
    second_counts: tuple[int, int, int, int, int, int] | None


def _read_block(
    data: bytes, offset: int, width: int, counts: tuple[int, int, int, int, int, int]
) -> tuple[
    tuple[int, ...],
    tuple[int, ...],
    tuple[tuple[int, int, str], ...],
    bytes,
    int,
    int,
    int,
    int,
]:
    isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt = counts
    time_code = "q" if width == 8 else "i"
    times = (
        struct.unpack(f">{timecnt}{time_code}", data[offset : offset + width * timecnt])
        if timecnt
        else ()
    )
    offset += width * timecnt
    indexes = tuple(data[offset : offset + timecnt])
    offset += timecnt
    type_offset = offset
    raw_types = []
    for _ in range(typecnt):
        raw_types.append(struct.unpack(">iBB", data[offset : offset + 6]))
        offset += 6
    chars_offset = offset
    chars = data[offset : offset + charcnt]
    offset += charcnt
    offset += leapcnt * (width + 4) + isstdcnt + isutcnt
    types = []
    for gmtoff, isdst, name_index in raw_types:
        end = chars.find(b"\0", name_index)
        if end < 0:
            name = ""
        else:
            name = chars[name_index:end].decode("ascii", "replace")
        types.append((gmtoff, isdst, name))
    return (
        tuple(times),
        indexes,
        tuple(types),
        chars,
        offset,
        type_offset,
        chars_offset,
        leapcnt,
    )


def parse_tzif(data: bytes) -> ParsedTzif:
    """Read a valid-enough TZif file and expose block offsets for test mutations."""
    if data[:4] != b"TZif":
        raise ValueError("bad magic")
    version_byte = data[4]
    version = 0 if version_byte == 0 else version_byte - ord("0")
    if version not in (0, 2, 3, 4):
        raise ValueError("bad version")
    first_counts = struct.unpack(">6I", data[20:44])
    if version == 0:
        (
            transitions,
            indexes,
            types,
            chars,
            _end,
            type_offset,
            chars_offset,
            leap_count,
        ) = _read_block(data, 44, 4, first_counts)
        return ParsedTzif(
            version,
            transitions,
            indexes,
            types,
            chars,
            None,
            leap_count,
            None,
            None,
            type_offset,
            chars_offset,
            first_counts,
        )
    first_end = _read_block(data, 44, 4, first_counts)[4]
    second_header_offset = first_end
    if data[second_header_offset : second_header_offset + 4] != b"TZif":
        raise ValueError("missing second header")
    second_counts = struct.unpack(
        ">6I", data[second_header_offset + 20 : second_header_offset + 44]
    )
    second_block_offset = second_header_offset + 44
    (
        transitions,
        indexes,
        types,
        chars,
        end,
        type_offset,
        chars_offset,
        leap_count,
    ) = _read_block(data, second_block_offset, 8, second_counts)
    footer = None
    if end < len(data):
        if data[end] != 0x0A:
            raise ValueError("missing footer newline")
        end += 1
        footer_end = data.find(b"\n", end)
        if footer_end < 0:
            raise ValueError("unterminated footer")
        footer = data[end:footer_end].decode("ascii", "replace")
    return ParsedTzif(
        version,
        transitions,
        indexes,
        types,
        chars,
        footer,
        leap_count,
        second_header_offset,
        second_block_offset,
        type_offset,
        chars_offset,
        second_counts,
    )


def selected_data_type(parsed: ParsedTzif, instant: int) -> tuple[int, int, str]:
    """Select the data-block type before/at a timestamp, ignoring the footer."""
    if not parsed.transitions:
        return parsed.types[0]
    import bisect

    index = bisect.bisect_right(parsed.transitions, instant) - 1
    if index < 0:
        return parsed.types[0]
    return parsed.types[parsed.type_indexes[index]]


# ---------------------------------------------------------------------------
# Independent POSIX TZ-string footer oracle for synthetic fixtures


@dataclass(frozen=True)
class Rule:
    # kind: 0 = zero-based day n, 1 = Julian day Jn, 2 = M.m.w.d
    kind: int
    a: int
    b: int = 0
    c: int = 0
    seconds: int = 7200


@dataclass(frozen=True)
class Footer:
    standard_offset: int
    standard_name: str
    summer_offset: int
    summer_name: str
    start: Rule
    end: Rule
    daylight: bool = True

    def _is_leap(self, year: int) -> bool:
        return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)

    def _days_in_month(self, year: int, month: int) -> int:
        return [
            31,
            29 if self._is_leap(year) else 28,
            31,
            30,
            31,
            30,
            31,
            31,
            30,
            31,
            30,
            31,
        ][month - 1]

    @staticmethod
    def _weekday(value: datetime) -> int:
        # POSIX weekday: Sunday is 0.
        return (value.weekday() + 1) % 7

    def _rule_day(self, year: int, rule: Rule) -> datetime:
        if rule.kind == 0:
            return datetime(year, 1, 1) + timedelta(days=rule.a)
        if rule.kind == 1:
            day = rule.a - 1
            if self._is_leap(year) and rule.a >= 60:
                day += 1
            return datetime(year, 1, 1) + timedelta(days=day)
        first = datetime(year, rule.a, 1)
        month_day = (
            1
            + (rule.c - self._weekday(first) + 7) % 7
            + 7 * (rule.b - 1)
        )
        if month_day > self._days_in_month(year, rule.a):
            month_day -= 7
        return datetime(year, rule.a, month_day)

    def transition_seconds(self, year: int, rule: Rule, previous_offset: int) -> int:
        local = self._rule_day(year, rule) + timedelta(
            seconds=rule.seconds - previous_offset
        )
        return int((local - datetime(1970, 1, 1)).total_seconds())

    def type_at(self, instant: int) -> tuple[int, int, str]:
        if not self.daylight:
            return (self.standard_offset, 0, self.standard_name)
        year = datetime.fromtimestamp(instant, timezone.utc).year
        for candidate_year in (year - 1, year, year + 1):
            start = self.transition_seconds(
                candidate_year, self.start, self.standard_offset
            )
            end = self.transition_seconds(
                candidate_year, self.end, self.summer_offset
            )
            if start < end:
                if start <= instant < end:
                    return (self.summer_offset, 1, self.summer_name)
            else:
                end_next = self.transition_seconds(
                    candidate_year + 1, self.end, self.summer_offset
                )
                if start <= instant < end_next:
                    return (self.summer_offset, 1, self.summer_name)
        return (self.standard_offset, 0, self.standard_name)


def format_local(instant: int, local_type: tuple[int, int, str]) -> str:
    offset, daylight, abbreviation = local_type
    local = datetime.fromtimestamp(instant + offset, timezone.utc)
    return (
        f"{local.year},{local.month},{local.day},{local.hour},"
        f"{local.minute},{local.second},{offset},{daylight},{abbreviation}"
    )
